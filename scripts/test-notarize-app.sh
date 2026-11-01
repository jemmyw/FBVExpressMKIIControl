#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
work=$(mktemp -d "${TMPDIR:-/tmp}/fbv-release-test.XXXXXX")
cleanup() {
    local result=$?
    if [[ "$result" != 0 ]]; then
        printf 'FAIL: offline release test; captured stage output follows\n' >&2
        cat "$work"/*.log >&2 2>/dev/null || true
    fi
    rm -rf "$work"
    exit "$result"
}
trap cleanup EXIT
mkdir -p "$work/bin" "$work/project/scripts" "$work/tmp"
cp scripts/notarize-app.sh scripts/Info.plist "$work/project/scripts/"
cp LICENSE "$work/project/LICENSE"
touch "$work/fake.p12"
cat >"$work/bin/fake" <<'STUB'
#!/bin/bash
set -eu
case "${0##*/}" in
    make)
        mkdir -p dist/FBV.app/Contents/MacOS dist/FBV.app/Contents/Resources
        printf icon >dist/FBV.app/Contents/Resources/AppIcon.icns
        printf stale >dist/FBV.app/Contents/stale-executable
        printf stale >dist/FBV.app/Contents/Resources/LICENSE
        printf app >dist/FBV.app/Contents/MacOS/FBVApp
        cp scripts/Info.plist dist/FBV.app/Contents/Info.plist
        printf cli >dist/fbv
        ;;
    lipo) printf 'arm64\n' ;;
    op)
        case "$2" in
            */username) printf 'test@example.invalid' ;;
            */APPLE_TEAM_ID) printf 'ABCDEFGHIJ' ;;
            *) printf 'FAKE-SECRET-DO-NOT-LOG[*]' ;;
        esac
        ;;
    security)
        case "$1" in
            create-keychain) touch "${@: -1}"; printf '%s\n' "${@: -1}" >"$TEST_ROOT/keychain-path" ;;
            delete-keychain) rm "$2"; printf cleaned >"$TEST_ROOT/cleaned" ;;
            find-identity)
                team=ABCDEFGHIJ
                [[ "$TEST_CASE" != wrong-team ]] || team=ZZZZZZZZZZ
                printf '  1) AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA "Developer ID Application: Test (%s)"\n' "$team"
                ;;
            list-keychains) exit 99 ;;
        esac
        ;;
    codesign)
        case "$1" in
            --force)
                target=${@: -1}
                cmp LICENSE "${target%/*}/LICENSE"
                if [[ "$target" == *.app ]]; then
                    cmp LICENSE "$target/Contents/Resources/LICENSE"
                    test ! -e "$target/Contents/stale-executable"
                fi
                printf '%s\n' "$target" >>"$TEST_ROOT/signed"
                ;;
            -d)
                if [[ "$2" == --verbose=4 ]]; then
                    printf 'Authority=Developer ID Application: Test (ABCDEFGHIJ)\nTeamIdentifier=ABCDEFGHIJ\nflags=0x10000(runtime)\n' >&2
                fi
                ;;
        esac
        ;;
    xcrun)
        if [[ "$1 $2" == 'notarytool submit' ]]; then
            printf submit >>"$TEST_ROOT/submitted"
            for argument in "$@"; do
                [[ "$argument" != --wait ]] || exit 99
            done
            printf '{"id":"12345678-1234-1234-1234-123456789abc","status":"In Progress"}\n'
        elif [[ "$1 $2" == 'notarytool wait' ]]; then
            printf wait >>"$TEST_ROOT/waited"
            records=(dist/releases/.notarize-*/submission-id)
            test "$(cat "${records[0]}")" == "$3"
            if [[ "$TEST_CASE" == timeout ]]; then
                printf 'wait timed out: test@example.invalid FAKE-SECRET-DO-NOT-LOG[*]\n' >&2
                exit 1
            fi
            status=Accepted
            [[ "$TEST_CASE" != rejected ]] || status=Invalid
            printf '{"id":"%s","status":"%s"}\n' "$3" "$status"
        elif [[ "$1 $2" == 'notarytool log' ]]; then
            printf '{"status":"Invalid","message":"FAKE-SECRET-DO-NOT-LOG[*] test@example.invalid"}\n'
        elif [[ "$1 $2" == 'stapler staple' ]]; then
            printf stapled >"$3/ticket"
        elif [[ "$1 $2" == 'stapler validate' ]]; then
            test -f "$3/ticket"
        fi
        ;;
    spctl) test -f "${@: -1}/ticket" ;;
esac
STUB
chmod +x "$work/bin/fake"
for command in make lipo op security codesign xcrun spctl; do
    ln -s fake "$work/bin/$command"
done
export PATH="$work/bin:$PATH"
export FBV_SIGNING_P12="$work/fake.p12" FBV_NOTARY_SECRET_ITEM=op://Test/Signing
export TEST_ROOT="$work" TMPDIR="$work/tmp"
cd "$work/project"
for TEST_CASE in success rejected timeout wrong-team; do
    export TEST_CASE
    rm -rf dist
    rm -f "$work/cleaned" "$work/signed" "$work/submitted" "$work/waited"
    result=0
    bash scripts/notarize-app.sh >"$work/$TEST_CASE.log" 2>&1 || result=$?
    release=dist/releases/v1.0.1-arm64
    test -f "$work/cleaned"
    test ! -e "$(cat "$work/keychain-path")"
    test ! -e dist/releases/.v1.0.1-arm64.lock
    if [[ "$TEST_CASE" == success ]]; then
        test "$result" -eq 0
        test "$(wc -l <"$work/signed" | tr -d ' ')" -eq 2
        test -f "$release/FBV.app/ticket"
        test ! -e "$release/FBV.app/Contents/stale-executable"
        test -f "$release/fbv"
        (cd "$release" && shasum -a 256 -c SHA256SUMS)
        cmp LICENSE "$release/LICENSE"
        cmp LICENSE "$release/FBV.app/Contents/Resources/LICENSE"
        unzip -p "$release/FBV-1.0.1-arm64.zip" LICENSE >"$work/archive-license"
        cmp LICENSE "$work/archive-license"
        unzip -p "$release/FBV-1.0.1-arm64.zip" FBV.app/Contents/Resources/LICENSE >"$work/app-license"
        cmp LICENSE "$work/app-license"
        unzip -l "$release/FBV-1.0.1-arm64.zip" >"$work/archive.txt"
        if rg -q 'stale-executable' "$work/archive.txt"; then exit 1; fi
        rg -q 'FBV.app/ticket' "$work/archive.txt"
        rg -q ' fbv$' "$work/archive.txt"
        if bash scripts/notarize-app.sh >"$work/refusal.log" 2>&1; then
            printf 'FAIL: release overwrite allowed\n'; exit 1
        fi
        test "$(cat "$work/submitted")" == submit
        test "$(cat "$work/waited")" == wait
        rg -q 'refusing overwrite' "$work/refusal.log"
    else
        test "$result" -ne 0
        test ! -e "$release"
        if [[ "$TEST_CASE" == wrong-team ]]; then
            test ! -e "$work/submitted"
            test ! -e "$work/waited"
        else
            test "$(cat "$work/submitted")" == submit
            test "$(cat "$work/waited")" == wait
            evidence=(dist/releases/.notarize-*)
            test -s "${evidence[0]}/submission.json"
            test "$(cat "${evidence[0]}/submission-id")" == 12345678-1234-1234-1234-123456789abc
            test -s "${evidence[0]}/notary-log.json"
            if [[ "$TEST_CASE" == timeout ]]; then
                test "$(cat "${evidence[0]}/wait.exit-status")" == 1
                test -z "$(cat "${evidence[0]}/wait.json")"
                rg -q 'wait timed out: \[REDACTED\] \[REDACTED\]' "${evidence[0]}/wait.stderr"
            fi
        fi
    fi
    if rg -q -F -e 'FAKE-SECRET-DO-NOT-LOG' -e 'test@example.invalid' "$work/$TEST_CASE.log" dist; then
        printf 'FAIL: secret leaked\n'; exit 1
    fi
    printf 'PASS: %s (temporary keychain cleaned)\n' "$TEST_CASE"
done
printf 'Offline release packaging, refusal, failure evidence and cleanup checks passed.\n'
