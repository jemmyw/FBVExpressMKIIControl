.PHONY: all build test app notarize test-release smoke

all: build

build:
	swift build

test:
	sh scripts/check.sh

app:
	sh scripts/build-app.sh

notarize:
	bash scripts/notarize-app.sh

test-release:
	bash scripts/test-notarize-app.sh

smoke:
	swift build
	"$$(swift build --show-bin-path)/FBVApp" --snapshot capture/app-review/snapshots
