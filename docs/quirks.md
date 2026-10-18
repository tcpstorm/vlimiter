# Quirks: FiveM, the anti-cheat, and the engine

Everything here cost at least one crash or one wasted evening, most of it
during VLights, which this plugin inherits its foundation from. Check this
list before adding a hook or a check.

## What the anti-cheat (adhesive) tolerates

Terminates the game ("Early-exit trap", ~8-60 s after the offence):

- an `IDXGISwapChain::Present` hook. This is the reason the limiter sits on
  the game's own present call site instead (limiter.md), and the reason the
  menu is a separate window rather than an overlay;
- writing to a vtable in the game's read-only data.

Tolerated so far (VLights, several releases):

- MinHook `.text` detours inside the game image;
- creating Direct3D objects on the game's device from a worker thread;
- a separate top-level window with its own D3D11 device for the menu.

Not tried before VLimiter: a detour on an instruction the render thread is
executing at install time (limiter.md, "Installing while the render thread
runs"). Not tried at all: hooking FiveM's heap-generated code, hooking
inside Cfx DLLs, immediate-context calls on the game's device.

## ReShade and the menu window

ReShade (loaded by FiveM from `plugins/dxgi.dll`) detours
`D3D11CreateDeviceAndSwapChain` for the whole process and binds a full
effect runtime, with its own numbered ini, to every swap chain it sees.
With a hardware device for the menu, the menu window was created before
the game's swap chain and so received the primary `ReShade.ini`; the game
was pushed to `ReShade3.ini` (VLights' menu took `ReShade2.ini`),
ReShade's first-run tutorial and Home-key overlay opened inside the menu,
and on one user's machine the menu's very first present (the line after
`menu: camera locked` in the log) crashed the game.

ReShade skips `D3D_DRIVER_TYPE_WARP` and `D3D_DRIVER_TYPE_REFERENCE`
devices ("Skipping device because the driver type is ...") and then the
swap chain created on them ("Skipping swap chain because it was created
without a proxy Direct3D device"). The menu therefore always uses a WARP
device. Signs it worked: the ReShade log carries both lines, no
`ReShade2.ini` appears on a fresh start, and Home no longer opens ReShade
inside the menu. ReShade's input detours (`RegisterRawInputDevices`,
`PeekMessage`, `SetCursorPos`, `ClipCursor`) still see the menu's calls,
unlike FiveM's IAT hooks, but with no runtime bound to the window they do
nothing with them.

## FiveM's own frame pacing

FiveM removes the game's OS yields around present ("present sleeper" is
stubbed to `ret`, a "dumb render thread sleep" flag is cleared; Cfx
`RenderHooks.cpp`), so without a cap the game runs the render thread flat
out. FiveM's `D3DPresent` holds a *minimised* game to 30 fps with a timer
wait of its own and otherwise never limits. Its `LimitFrameTime` helper is
used only there and for the off-screen "reverse game data" swapchain.

## MinHook under FiveM

FiveM stubs `CreateToolhelp32Snapshot` (fails with no error code); stock
MinHook reports that as `MH_ERROR_MEMORY_ALLOC` and refuses to hook. The
vendored copy (`third_party/minhook`) is patched: `MH_TOLERATE_FREEZE_FAILURE`,
`mh_vlimiter_allow_suspend = 0` (never suspend threads), an `NtGetNextThread`
fallback behind the flag, and the 5-byte jump written as one aligned 8-byte
store whenever it fits inside an aligned word
(`mh_vlimiter_last_patch_atomic` says whether it did). The present site's
only caller is the render thread, which is running during install, so the
atomic store is what stands between the plugin and a torn instruction fetch.

## Loading the plugin

- FiveM refuses an `.asi` that does not declare the running game build:
  `res/VLimiter.rc` has one `FX_ASI_BUILD <build>` resource per supported
  build (2189..3889). Add one when a build lands.
- `sv_pureLevel >= 2` blocks all plugins; `sv_scriptHookAllowed` is
  irrelevant (no SHV natives used).
- Windows Smart App Control can block a freshly built, unsigned `.asi` on
  first load ("Couldn't load VLimiter.asi", Code Integrity event 3077);
  its verdict flips on the next launch. Loading the file through
  `NativeLibrary::Load` in PowerShell (development.md) checks that.
- The crash dialog helper `FiveM_DumpServer` keeps the `.asi` open after a
  crash; close it before installing. Installing by rename-then-copy works
  while the game runs (the next launch picks it up).

## The menu window and the game's input

- The game registers raw mouse input for the whole process with
  `RIDEV_INPUTSINK`, so the camera would spin while dragging a slider. The
  menu unregisters the mouse from raw input while open and restores the
  exact original registration on close.
- The game re-clips and hides the OS cursor every frame; the menu draws its
  own cursor and releases the clip each frame it is visible.
- FiveM IAT-hooks `GetForegroundWindow`, `SetFocus`, `ClipCursor` and
  friends *for the game module*; a plugin's own imports are untouched, which
  is why the worker thread's foreground check sees the real foreground
  window.
