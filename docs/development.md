# Development

Building, the source layout, the design rules that keep the plugin alive
under FiveM, and what to check after a game update.

## Building

Docker Desktop only; nothing else is installed on the host. The plugin is
a plain Windows x64 DLL cross-compiled from a Debian container with
mingw-w64, CMake and Ninja.

```powershell
.\build.ps1            # -> .\dist\VLimiter.asi + vlimiter.ini
.\build.ps1 -Install   # also copies into %LOCALAPPDATA%\FiveM\FiveM.app\plugins
```

or directly:

```
docker build --target export -o dist .
```

The build first runs the pacing tests (`tests/test_pacer.cpp`) natively and
stops if they fail, then cross-compiles the `.asi` statically (no
`libstdc++`/`libgcc` DLL dependencies; the log prints the import table).
Plain CMake, so Visual Studio works too: the detour stub is GNU inline asm
under mingw and `src/hook/present_stub.asm` (MASM) under MSVC, selected by
`CMakeLists.txt`. MinHook and Dear ImGui are vendored under `third_party/`,
no network needed.

`.dockerignore` is an allowlist mirroring the Dockerfile's `COPY` lines, so
the build context holds only sources, resources, tests, vendored code and
the build files. Add a path to both when the Dockerfile starts copying
something new.

Installing while the game runs works by rename-then-copy (the next launch
picks the new file up). A leftover `FiveM_DumpServer` crash dialog keeps the
file open; close it first.

**Version.** The `VERSION` file at the repo root (`MAJOR.MINOR.PATCH`) is
the only place the version lives. CMake reads it into `project(VERSION)`,
generates `vlimiter/version.h` for the code, and fills the Windows version
resource (`res/version.rc.in`) from it; `build.ps1` prints the result.
Author and project URL come from the `VLIMITER_AUTHOR` / `VLIMITER_URL`
CMake cache variables. CI reads the same file, and a pull request into main
must raise it (CONTRIBUTING.md).

## CI and releases (GitHub Actions)

- `.github/workflows/ci.yml` runs on every pull request and on pushes to
  any branch but main: the same Docker build, tests included, with the
  `.asi` and ini uploaded as a run artifact. The `version bump` check fails
  a pull request into main whose `VERSION` is not higher than main's, when
  plugin-affecting files changed.
- `.github/workflows/release.yml` runs on every push to main: builds, reads
  `VERSION`, and if no `v<version>` tag exists yet creates the tag and a
  GitHub Release with `VLimiter.asi` and `vlimiter.ini` attached.

The shipped `vlimiter.ini` is what the plugin writes on first run. To
regenerate it after changing the config keys, load the built `.asi` from
PowerShell in an empty folder and copy the file it writes:

```powershell
[System.Runtime.InteropServices.NativeLibrary]::Load("$pwd\VLimiter.asi")
```

Outside the game the pattern does not match, the log says `inactive`, and
the ini is still written. That call is also the quickest way to check
whether Smart App Control will let the build load.

## Layout

```
src/main.cpp            DllMain, startup, hotkey / foreground worker thread
src/plugin/             config state + ini (config.*), logger (log.*),
                        the interface the hook, menu and hotkeys share
                        (plugin.h/.cpp), the release check (update.*)
src/limiter/pacer.h     the pacing policy: pure, header-only, unit-tested
src/game/present.*      the present-site detour: pattern, install, the
                        per-frame body, the wait, statistics, the GNU stub
src/hook/pattern.*      byte-pattern scanner, game build
src/hook/present_stub.asm   the MSVC version of the stub
src/ui/menu.cpp         the menu: own top-level window + D3D11 + ImGui, camera lock
res/VLimiter.rc         FX_ASI_BUILD resources (one per supported game build)
res/version.h.in        template for the generated version header
res/version.rc.in       Windows version info, filled from VERSION
VERSION                 the version, MAJOR.MINOR.PATCH
tests/                  host-native tests, run during the Docker build
docs/                   these notes
third_party/minhook     MinHook v1.3.4 + marked patches (quirks.md)
third_party/imgui       Dear ImGui v1.92.9b (core + dx11/win32 backends)
cmake/                  mingw-w64 toolchain file
Dockerfile, build.ps1   the build
```

## Design rules

- **One game detour, a `.text` detour in the game image**, found by Cfx's
  own byte pattern, installed only when that pattern matches exactly once.
  Never a vtable write, never a hook on `Present` or anything in a DirectX
  or Cfx module, never the immediate Direct3D context. The menu is its own
  window with its own device.
- **The render thread owns its state.** The per-frame body reads atomics
  the worker and menu threads write and touches nothing else shared: no
  locks, no allocation, no logging, no Win32 calls beyond the clock and the
  wait. Anything that needs a syscall (foreground check, logging) happens
  on the worker thread and is handed over as an atomic.
- **Policy is pure and tested.** `pacer.h` has no clock and no OS; a change
  to how frames are scheduled is a change to a header with a test.
- **A mid-function detour saves everything.** The stub preserves the whole
  Win64 volatile set and the flags; see limiter.md before changing it.
- **Say so when inactive.** The pattern missing, the hook failing or the
  timer being unavailable each produce one plain log line, and the menu
  shows a red line when nothing is limited.
- **Log what a future debugging session will need**, behind `debug`, and
  keep the always-on log to startup, hook installation, hotkeys and
  failures.

## Menu window

The menu is a small always-on-top window with its own D3D11 device and
swapchain, not an overlay drawn inside the game. VLights, this plugin's
parent, once detoured `IDXGISwapChain::Present` and subclassed the game
window to draw ImGui in the frame; the anti-cheat terminated the game about
a minute after start, every time (quirks.md). The window hooks nothing of
the game's. The camera is locked while it is open by removing the game's
raw-input mouse registration and restoring it on close; the cursor is drawn
by ImGui so it is always visible. Its own swapchain presents with sync
interval 0 so it never contends with the game's.

## After a game update

1. If there is no log at all, FiveM's own log in `FiveM.app\logs\` will say
   `does not claim to support game build N`: add
   `FX_ASI_BUILD N BEGIN "\0" END` to `res/VLimiter.rc` and rebuild.
2. If the log says the pattern matched 0 or more than 1 times, take the
   current pattern from Cfx's `RenderHooks.cpp` (search for `D3DPresent`
   near `hook::put(hook::get_address<void*>(hook::get_pattern(`) and update
   `kPresentCallPattern` in `src/game/present.cpp`. If the instruction
   shape changed (no longer a 7-byte rip-relative `mov rax`), the slot
   decoding in `Install()` and the stub's assumptions need a second look.
3. Check the `present routine slot` line names `rage-graphics-five.dll`.
4. Play a session with `debug = 1`: `pacing:` lines should show the cap,
   `held` near 100% when the game is faster than the cap, and `presents
   since start` climbing by the frame rate each second.
