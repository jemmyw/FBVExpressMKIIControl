.PHONY: all build test app smoke

all: build

build:
	swift build

test:
	sh scripts/check.sh

app:
	sh scripts/build-app.sh

smoke:
	swift build
	"$$(swift build --show-bin-path)/FBVApp" --snapshot capture/app-review/snapshots
