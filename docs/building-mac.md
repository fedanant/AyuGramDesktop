## Build instructions for macOS

### Prepare folder

Choose a folder for the future build, for example **/Users/user/TBuild**. It will be named ***BuildPath*** in the rest of this document. All commands will be launched from Terminal.

**Note about disk space:** The full build process will require approximately **55 GB** of free space. This includes:
- **~35 GB** for libraries (when building for both x64 and arm64 architectures)
- **~20 GB** for the compiled Telegram app (in the `out` folder)

### Clone source code and prepare libraries

Go to ***BuildPath*** and run

    /bin/bash -c "$(curl -fsSL https://raw.githubusercontent.com/Homebrew/install/HEAD/install.sh)"
    brew install git automake libtool cmake wget pkg-config gnu-tar ninja nasm meson

    sudo xcode-select -s /Applications/Xcode.app/Contents/Developer

    git clone --recursive https://github.com/AyuGram/AyuGramDesktop.git tdesktop
    ./tdesktop/Telegram/build/prepare/mac.sh

### Building the project

Go to ***BuildPath*/tdesktop/Telegram** and run

    ./configure.sh -D TDESKTOP_API_ID=2040 -D TDESKTOP_API_HASH=b18441a1ff607e10a989891a5462e627

Then launch Xcode, open ***BuildPath*/tdesktop/out/Telegram.xcodeproj** and build for Debug / Release.

### GitHub Actions release builds

The release workflow prepares universal dependencies in `prepare-macos`, then
builds `x86_64` and `arm64` in separate `build-macos-arch` jobs. Each build has
its own six-hour time budget. The `build-macos` job combines the app bundles
and produces `AyuGram-macOS.dmg` for both Intel and Apple Silicon.

Dependencies are passed between jobs as a compressed tar artifact, preserving
executable permissions and symlinks. An exact toolchain-specific cache can skip
preparation on later runs, but a cache miss or eviction does not make app jobs
rebuild the libraries. The archive retains installed Qt under `Libraries/local`
and excludes the redundant `Libraries/qt_*` source/build tree. It is intended
for configuring and compiling the app, not for rerunning `prepare.py`.

Intermediate dependency and app archives are retained for three days. Rerun
the whole workflow if those artifacts have expired. Bundle assembly checks
both architectures in every Mach-O file before packaging the DMG; missing
files, differing bundle versions, or missing architecture slices fail the job.

For a manual macOS verification run, enable `macos_only` and disable
`publish_release`. This uploads the DMG as a workflow artifact without creating
or replacing a GitHub Release. Push-triggered builds still build both platforms
and publish as before.
