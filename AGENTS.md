# Well of Souls portable C port

Target: `extracted/Souls.exe` (Win32 PE, MSVC6 + MFC42, 2008-12-06, WoS A96), which is installed by
`assets/WellOfSouls.exe` (a Clickteam Install Creator installer). There is no `platforms/` file in the re skill for
Win32 PE. Addresses are Ghidra VAs in the `Souls.exe` image (ImageBase 0x400000), written as `0x00401234`.

## First step in a new session (Claude Code cloud containers)
Run `tools/bootstrap_cloud.sh` before anything else. A fresh container has no game data, no
build, no SDL2, no decompiler and no Wine; the script installs and builds all of it and is
idempotent, so it is also safe to re-run after a container restart. Use
`--no-oracle` / `--no-decomp` to skip the slow parts when a task does not need them.
- It installs apt packages (SDL2 dev, i386 Wine 9.0, mingw i686 gcc, Xvfb, 7z), downloads
  `assets/WellOfSouls.exe` and unpacks it with `tools/cic_reference.py` (mono is not available),
  builds `build/`, builds radare2 + r2ghidra from git and writes `work/decomp/all.c`, and
  provisions the oracle (`tools/oracle/fetch_wine.sh`, `tools/oracle/build.sh`).
- GitHub *release downloads* are blocked by the proxy (Ghidra, Kron4ek Wine, llvm-mingw);
  anonymous `git clone` of public repos works. Don't fight this: the script uses the
  alternatives above.
- Decompile a function on demand with `tools/r2/decomp.py <addr|label> ...`; Ghidra is not
  installed, so `tools/ghidra/query.sh` does not work in the cloud.
- Oracle runs: `tools/oracle/run.sh <dsc> <outdir>`, the full diff with
  `WOS_BUILD=$PWD/build tests/diff_oracle.sh`.
- If you add a dependency, add it to `tools/bootstrap_cloud.sh` in the same change.

## Layout
- `assets/`: original installer. READ-ONLY.
- `extracted/`: game files unpacked from the installer by `tools/extract_installer.sh` (gitignored, never edited).
- `src/platform/platform.h`: the only host boundary. `src/platform/sdl2/`: the SDL2 backend.
- `src/`: portable game code in C99. It MUST NOT include SDL, `windows.h`, `unistd.h`, `dirent.h` or any other OS header. Use the `plat_*` calls for all host needs.
- `src/third_party/`: vendored single-file libraries (stb_image, etc.). Leave them unedited.
- `tests/`: replay scripts and test drivers. `tests/replay_offline.sh` is the acceptance test.
- `tools/`: RE and extraction tooling. `tools/bootstrap_cloud.sh`: workspace setup. `tools/r2/`: radare2/r2ghidra decompiler. `tools/ghidra/`: headless Ghidra scripts (local installs only). `tools/oracle/`: the original under Wine.
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
