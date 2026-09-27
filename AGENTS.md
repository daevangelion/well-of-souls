# Well of Souls portable C port

Target: `extracted/Souls.exe` (Win32 PE, MSVC6 + MFC42, 2008-12-06, WoS A96), which is installed by
`assets/WellOfSouls.exe` (a Clickteam Install Creator installer). There is no `platforms/` file in the re skill for
Win32 PE. Addresses are Ghidra VAs in the `Souls.exe` image (ImageBase 0x400000), written as `0x00401234`.

## Layout
- `assets/`: original installer. READ-ONLY.
- `extracted/`: game files unpacked from the installer by `tools/extract_installer.sh` (gitignored, never edited).
- `src/platform/platform.h`: the only host boundary. `src/platform/sdl2/`: the SDL2 backend.
- `src/`: portable game code in C99. It MUST NOT include SDL, `windows.h`, `unistd.h`, `dirent.h` or any other OS header. Use the `plat_*` calls for all host needs.
- `src/third_party/`: vendored single-file libraries (stb_image, etc.). Leave them unedited.
- `tests/`: replay scripts and test drivers. `tests/replay_offline.sh` is the acceptance test.
- `tools/`: RE and extraction tooling. `tools/ghidra/`: headless Ghidra scripts.
- `work/`: scratch space (Ghidra project, decompiler dump `work/decomp/all.c`). Gitignored.
- `REVERSE.md`: findings, format notes and the task list. `labels.csv`: `addr,name,comment` for Souls.exe.
- `docs/architecture_port.md`: C module graph, mapping from original to port, and deviations.

## Build
- `cmake -S . -B build && cmake --build build -j4` builds the Linux binary at `build/wos`.
- `cmake -S . -B build-win -DCMAKE_TOOLCHAIN_FILE=cmake/mingw-w64.cmake && cmake --build build-win` builds the Windows cross build.
- Code compiles as C99 with `-Wall -Wextra`. Treat new warnings in our code as defects.

## Runtime
- `build/wos --data extracted [--headless] [--replay FILE] [--log FILE] [--seed N] [--max-frames N]`
- Headless acceptance test: `SDL_VIDEODRIVER=dummy tests/replay_offline.sh`.
- The game runs a fixed 60 Hz simulation step. In replay mode the step ignores the wall clock, so runs are deterministic.
- Structured log lines are `EVT <name> key=value ...`, one per line. Tests assert on these lines.

## Port rules (from the re skill, weighted toward working code)
- Working code first, but never fake the result: every log checkpoint must be emitted by the real code path.
- Take names from labels.csv/REVERSE.md where known. Use neutral names otherwise.
- Keep the integer math the original uses where it is known.
- Record behaviour deviations from the original (e.g. MFC dialogs redrawn as in-framebuffer UI) in docs/architecture_port.md.
- Use one owner per file when agents work in parallel. Do not reformat files you don't own.
