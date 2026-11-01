#!/usr/bin/env bash
set +x
set -euo pipefail
umask 077
cd "$(dirname "$0")/.."

fail() { printf 'error: %s\n' "$1" >&2; exit 1; }
stage() { printf '%s\n' "$1"; }
[[ -n "${FBV_SIGNING_P12:-}" ]] || fail 'FBV_SIGNING_P12 must name a Developer ID Application P12 file'
[[ -n "${FBV_NOTARY_SECRET_ITEM:-}" ]] || fail 'FBV_NOTARY_SECRET_ITEM must name an op:// item'
[[ "$FBV_NOTARY_SECRET_ITEM" == op://* ]] || fail 'FBV_NOTARY_SECRET_ITEM must begin with op://'
[[ -f "$FBV_SIGNING_P12" ]] || fail 'signing P12 file not found'
for command in op openssl security xcrun ditto jq plutil codesign spctl lipo shasum make mkfifo; do
    command -v "$command" >/dev/null || fail "required command not found: $command"
done
[[ "$(uname -m)" == arm64 ]] || fail 'this release path requires an arm64 host'
plist=scripts/Info.plist
version=$(plutil -extract CFBundleShortVersionString raw -o - "$plist")
build=$(plutil -extract CFBundleVersion raw -o - "$plist")
bundle_id=$(plutil -extract CFBundleIdentifier raw -o - "$plist")
[[ "$version" == 1.0.1 && "$build" == 2 && "$bundle_id" == com.mixitone.fbv ]] || fail 'expected com.mixitone.fbv version 1.0.1 build 2'
release="dist/releases/v${version}-arm64"
mkdir -p dist/releases
[[ ! -e "$release" ]] || fail 'release output already exists; refusing overwrite'
lock="dist/releases/.v${version}-arm64.lock"
mkdir "$lock" 2>/dev/null || fail 'release already running or stale lock needs inspection'
work=''
key_dir=''
keychain=''
cleanup() {
    local result=$?
    unset apple_id certificate_password apple_password team_id keychain_password
    if [[ -n "$keychain" && -e "$keychain" ]]; then
        if security delete-keychain "$keychain" >/dev/null 2>&1; then
            rmdir "$key_dir" 2>/dev/null || true
        else
            printf 'error: temporary keychain cleanup failed; inspect %s\n' "$key_dir" >&2
            result=1
        fi
    elif [[ -n "$key_dir" ]]; then
        rmdir "$key_dir" 2>/dev/null || true
    fi
    rmdir "$lock" 2>/dev/null || true
    if [[ -n "$work" ]]; then
        printf 'Private release evidence: %s\n' "$work"
    fi
    exit "$result"
}
trap cleanup EXIT
trap 'exit 130' INT
trap 'exit 143' TERM
work=$(mktemp -d "$PWD/dist/releases/.notarize-v${version}.XXXXXX")
key_dir=$(mktemp -d "${TMPDIR:-/tmp}/fbv-signing.XXXXXX")
keychain="$key_dir/signing.keychain-db"

stage 'Building local release app and CLI'
make app >"$work/build.log" 2>&1 || fail 'build failed; inspect private build.log'
app="$work/payload/FBV.app"
cli="$work/payload/fbv"
# Stage only build outputs, not stale files left in a local developer bundle.
mkdir -p "$app/Contents/MacOS" "$app/Contents/Resources"
cp dist/FBV.app/Contents/MacOS/FBVApp "$app/Contents/MacOS/FBVApp"
cp dist/FBV.app/Contents/Info.plist "$app/Contents/Info.plist"
cp dist/FBV.app/Contents/Resources/AppIcon.icns "$app/Contents/Resources/AppIcon.icns"
cp dist/fbv "$cli"
cp LICENSE "$app/Contents/Resources/LICENSE"
cp LICENSE "$work/payload/LICENSE"
for binary in "$app/Contents/MacOS/FBVApp" "$cli"; do
    [[ "$(lipo -archs "$binary")" == arm64 ]] || fail 'release binaries must be arm64 only'
done

stage 'Loading signing credentials into a temporary keychain'
read_secret() {
    op read "${FBV_NOTARY_SECRET_ITEM%/}/$1" 2>/dev/null || fail '1Password read failed'
}
apple_id=$(read_secret username)
certificate_password=$(read_secret APPLE_CERTIFICATE_PASSWORD)
apple_password=$(read_secret APPLE_PASSWORD)
team_id=$(read_secret APPLE_TEAM_ID)
[[ -n "$apple_id" && -n "$certificate_password" && -n "$apple_password" && "$team_id" =~ ^[A-Z0-9]{10}$ ]] || fail 'missing credentials or invalid Apple team ID'
[[ "$apple_id$apple_password" != *$'\n'* && "$apple_id$apple_password" != *$'\r'* ]] || fail 'Apple credentials must be single-line values'
keychain_password=$(openssl rand -hex 24)
security create-keychain -p "$keychain_password" "$keychain" >/dev/null 2>&1 || fail 'keychain creation failed'
security set-keychain-settings -lut 21600 "$keychain" >/dev/null 2>&1 || fail 'keychain settings failed'
security unlock-keychain -p "$keychain_password" "$keychain" >/dev/null 2>&1 || fail 'keychain unlock failed'
security import "$FBV_SIGNING_P12" -k "$keychain" -P "$certificate_password" -T /usr/bin/codesign -T /usr/bin/security >/dev/null 2>&1 || fail 'P12 import failed'
unset certificate_password
security set-key-partition-list -S apple-tool:,apple:,codesign: -s -k "$keychain_password" "$keychain" >/dev/null 2>&1 || fail 'key partition access failed'
unset keychain_password
identities=$(security find-identity -v -p codesigning "$keychain" 2>/dev/null) || fail 'identity lookup failed'
identity=$(printf '%s\n' "$identities" | awk -v team="($team_id)\"" '/Developer ID Application:/ && index($0, team) { print $2 }')
[[ "$identity" =~ ^[A-Fa-f0-9]{40}$ ]] || fail 'expected exactly one Developer ID Application identity for the configured team'
unset identities

stage 'Signing and verifying app and standalone CLI'
verify_signature() {
    codesign --verify --deep --strict "$1" > /dev/null 2>&1 || fail 'signature verification failed'
    local details
    details=$(codesign -d --verbose=4 "$1" 2>&1) || fail 'signature inspection failed'
    [[ "$details" == *"TeamIdentifier=$team_id"* && "$details" == *"(runtime)"* && "$details" == *"Authority=Developer ID Application:"* ]] || fail 'signature team, identity or hardened runtime mismatch'
    codesign -d --entitlements :- "$1" >"$work/entitlements.plist" 2>/dev/null || fail 'entitlements inspection failed'
    if [[ -s "$work/entitlements.plist" ]]; then
        plutil -lint "$work/entitlements.plist" >/dev/null || fail 'invalid entitlements output'
        local debug
        debug=$(plutil -extract com.apple.security.get-task-allow raw -o - "$work/entitlements.plist" 2>/dev/null || true)
        [[ "$debug" != true ]] || fail 'debug entitlement found'
    fi
}
for target in "$cli" "$app"; do
    codesign --force --sign "$identity" --keychain "$keychain" --options runtime --timestamp "$target" >/dev/null 2>&1 || fail 'Developer ID signing failed'
    verify_signature "$target"
done
ditto -c -k "$work/payload" "$work/submission.zip"

redact_notary_output() {
    local line
    while IFS= read -r line || [[ -n "$line" ]]; do
        line=${line//"$apple_password"/[REDACTED]}
        line=${line//"$apple_id"/[REDACTED]}
        printf '%s\n' "$line"
    done
}
run_notary() {
    local record=$1 output result=0 stderr_pid
    shift
    # A FIFO keeps unredacted diagnostics off disk and permits waiting on macOS Bash 3.2.
    mkfifo "$work/$record.stderr-pipe"
    redact_notary_output <"$work/$record.stderr-pipe" >"$work/$record.stderr" &
    stderr_pid=$!
    output=$(xcrun notarytool "$@" --apple-id "$apple_id" --password "$apple_password" --team-id "$team_id" --output-format json 2>"$work/$record.stderr-pipe") || result=$?
    wait "$stderr_pid" || fail 'diagnostic capture failed'
    rm "$work/$record.stderr-pipe"
    printf '%s\n' "$output" | redact_notary_output >"$work/$record.json"
    printf '%s\n' "$result" >"$work/$record.exit-status"
    return "$result"
}

stage 'Submitting once to Apple; evidence is retained on failure'
submit_status=0
run_notary submission submit "$work/submission.zip" || submit_status=$?
submission_id=$(jq -r '.id // empty' "$work/submission.json" 2>/dev/null || true)
if [[ "$submission_id" =~ ^[A-Fa-f0-9-]{36}$ ]]; then
    printf '%s\n' "$submission_id" >"$work/submission-id"
    printf 'Submission ID: %s\n' "$submission_id"
else
    fail 'no valid submission ID returned; inspect private evidence before any retry'
fi
[[ "$submit_status" == 0 ]] || fail 'upload returned an error; retained submission ID must be inspected before any retry'

stage 'Waiting for the saved Apple submission'
wait_status=0
run_notary wait wait "$submission_id" || wait_status=$?
submission_status=$(jq -r '.status // empty' "$work/wait.json" 2>/dev/null || true)
if [[ "$wait_status" != 0 || "$submission_status" != Accepted ]]; then
    run_notary notary-log log "$submission_id" || true
    fail 'notarisation not accepted or wait failed; inspect submission evidence, do not resubmit automatically'
fi
unset apple_id apple_password team_id
stage 'Stapling and checking Gatekeeper'
xcrun stapler staple "$app" >"$work/staple.log" 2>&1 || fail 'stapling failed'
xcrun stapler validate "$app" >"$work/staple-validation.log" 2>&1 || fail 'staple validation failed'
codesign --verify --deep --strict "$app" >"$work/signature-validation.log" 2>&1 || fail 'stapled signature verification failed'
spctl --assess --type execute "$app" >"$work/gatekeeper.log" 2>&1 || fail 'Gatekeeper rejected app'
stage 'Packaging verified release'
mkdir "$work/final"
ditto "$app" "$work/final/FBV.app"
cp "$cli" "$work/final/fbv"
cp "$work/payload/LICENSE" "$work/final/LICENSE"
archive="FBV-${version}-arm64.zip"
ditto -c -k "$work/payload" "$work/final/$archive"
(cd "$work/final" && shasum -a 256 "$archive" > SHA256SUMS)
[[ ! -e "$release" ]] || fail 'release output appeared during build; refusing overwrite'
mv "$work/final" "$release"
stage "Accepted release: $release (app, CLI, ZIP and SHA256SUMS)"
