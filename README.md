# Well of Souls — portable C reimplementation

A from-scratch C99 reimplementation of the 2008 game *Well of Souls* (Synthetic
Reality), targeting Linux and Windows through SDL2. The portable game and engine code
lives in `src/`; SDL2, Windows APIs and filesystem access are confined to
`src/platform/`. Reverse-engineering notes live in `REVERSE.md` and `docs/re/`.

## Game data and copyright

The original game is commercial, copyrighted software. **No game data, installer or
original binary is stored in this repository.** The engine reads the data at runtime
from a directory you supply:

```sh
tools/fetch_game_data.sh extracted          # from a local installer you own
WOS_INSTALLER_URL=... WOS_INSTALLER_SHA256=... tools/fetch_game_data.sh   # CI / pinned download
```

`assets/` (the installer) and `extracted/` (the unpacked data) are git-ignored. Supply
the data you legally own; the engine never embeds or redistributes it.

## Build

Requires CMake ≥ 3.16, a C99 compiler and SDL2.

```sh
tools/fetch_soundfont.sh                    # optional GPL-2 MIDI bank (pinned SHA-256)
cmake -S . -B build
cmake --build build -j"$(nproc)"            # -> build/wos
```

Windows (cross-compiled with a pinned llvm-mingw + SDL2 toolchain that the toolchain
file downloads on demand):

```sh
cmake -S . -B build-win -DCMAKE_TOOLCHAIN_FILE=cmake/mingw-w64.cmake
cmake --build build-win                     # -> build-win/wos.exe
```

Run with `build/wos --data extracted` (add `--headless` for the acceptance harness).

### Android

The Gradle build can provision the game data and soundfont for you. Supply the
installer (or a URL) and/or your own soundfont as Gradle properties (`-P`) or
environment variables; with an installer the data is unpacked during the build:

```sh
cd android
./gradlew assembleDebug -PwosInstaller=/path/to/WellOfSouls.exe
./gradlew assembleDebug -PwosInstallerUrl=https://... -PwosInstallerSha256=<hex>
./gradlew assembleDebug -PwosSoundfont=/path/to/font.sf2          # custom bank
./gradlew assembleDebug -PwosSoundfontUrl=https://... -PwosSoundfontSha256=<hex>
./gradlew assembleDebug -PwosSoundfontLicense=/path/to/LICENSE   # its license text
```

With no overrides it uses a pre-extracted `../../extracted` tree and the pinned
TimGM6mb bank. Unpacking the installer needs `mono` and `7z` on PATH. A custom
soundfont is staged under the fixed `TimGM6mb.sf2` name the engine loads at runtime.

**Runtime supply (the default).** The game data is copyrighted and is **not**
bundled in the APK by default (`-PwosBundleData=true` embeds it, or supply an
installer to the Gradle build as above). On first launch the app asks you to
supply the original `WellOfSouls.exe` installer — pick it with the system file
picker, or download it from a URL — and decodes it **on-device** with a native
Clickteam installer decoder (`src/platform/sdl2/cic_unpack.c`, validated
byte-exact against cicdec). The decoded game is stored in internal storage and
reused on later launches. The desktop build keeps the same decoder for host-side
unpacking but never links the codecs into the game binary.

**Runtime setup (one form).** A single "WoS Setup" launcher screen handles both inputs:
the game installer (pick or download a URL; stored as `installer-supplied.bin` and
decoded on-device) and, optionally, a soundfont (pick or download; stored as
`user-soundfont.sf2`). The soundfont URL is pre-filled with the official upstream
TimGM6mb bank. `find_soundfont()` prefers the supplied bank over the bundled one. The
game installer is a commercial title with no official free-download URL, so that field
is left for you to fill with a source you have the right to use.

## Tests

The self-tests and the replay suite are driven by the extracted retail data, so they
need `extracted/` present (see above).

```sh
cmake --build build -j"$(nproc)"            # builds wos, engine_selftest, world_selftest
ctest --test-dir build --output-on-failure  # self-tests (auto-skipped without data)
WOS_BUILD="$PWD/build" tests/replay_offline.sh
WOS_BUILD="$PWD/build" tests/replay_gaps.sh
```

The self-tests are `assert()`-driven, so the build forces `-UNDEBUG` for the test
translation units regardless of build type.

## Continuous integration

`.github/workflows/ci.yml` runs on every push and pull request:

- **Build (Linux)** and **Build (Windows, mingw cross)** — compile and link the game and
  the self-tests on both platforms. These need no secrets and always run.
- **Behavioural tests** — downloads the retail data (when the `WOS_INSTALLER_URL`
  repository secret is set), then runs the self-tests and the replay acceptance suite.
  Without that secret, or on a fork, this job compiles but skips the data-dependent
  steps, so the pipeline stays green and honest.

`.github/workflows/release.yml` publishes Linux and Windows binaries on a `v*` tag.
Release artifacts intentionally exclude the copyrighted data and the GPL-2 soundfont;
fetch the soundfont with `tools/fetch_soundfont.sh` and place it next to the binary.

## License

The project's own source is dedicated to the public domain under
[CC0-1.0](https://creativecommons.org/publicdomain/zero/1.0/) (see `LICENSE`).
Vendored third-party code keeps its own terms (`src/third_party/`): stb_image and
font8x8 are public domain, TinySoundFont is MIT, tml is zlib. The TimGM6mb soundfont
(fetched, not committed) is GPL-2. The original game data and installer are copyrighted
and are not distributed here.
