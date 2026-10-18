# Diagnostics

Everything goes to `vlimiter.log` next to the `.asi`, truncated at each
start. The always-on lines cover startup, hook installation, config changes
and failures; `debug = 1` adds a pacing summary every five seconds.

## Is it running?

Three checks, in order:

1. **The log exists.** If there is none, FiveM did not load the plugin:
   `FiveM.app\logs\CitizenFX_log_*.log` will say `does not claim to support
   game build N` (add the build to `res/VLimiter.rc`) or the server runs
   `sv_pureLevel 2`.
2. **`present detour installed`** appears in the startup lines. If instead
   the log says `pattern ... matched 0 times` the game build changed the
   code around the present call; see development.md, "After a game update".
3. **The measured fps in the menu moves** when you change the cap, and
   `frames since start` climbs. If the count stays at zero the detour is in
   place but never called: the site is not the per-frame present on this
   build. That has not happened yet and would be worth an issue with the
   startup lines attached.

## Startup lines

```
VLimiter 0.1.0 starting (thread 0x2C20, game build 3751)
```
Version and the game build parsed from the process name.

```
config (startup): enabled=1 fps_limit=60 background_fps=30 spin_margin_us=1000 menu_key=0x73 ...
config warnings:
line 12: unknown key 'fps'
```
The values in force. Warnings list ini lines that were ignored; the rest of
the file still loads.

```
main module base=0x7FF74AE50000 size=0x3FFA800
present call site at 0x7FF7... (base+0x..., 8-byte-aligned); present routine slot at base+0x... -> 0x7FF9... (rage-graphics-five.dll+0x...)
```
Where the pattern landed and what the game is about to call from there.
The owner module should be `rage-graphics-five.dll` (FiveM's `D3DPresent`).
`not 8-byte-aligned` is informational: see the next line.

```
present detour installed (thread 0x2C20, trampoline 0x..., jump written atomically, threads frozen via: none (not suspended))
```
The hook is live. `jump written atomically` means the 5-byte jump was
stored as one aligned 8-byte word; `byte-wise` means it was not, which is
only a concern if the game crashed in the same instant (limiter.md).

```
pattern '48 8B 05 ? ? ? ? 48 85 C0 74 0C 8B 4D 50 8B' matched 0 times (need exactly 1); not hooking. ...
inactive: no frame limiting this session (the menu still opens and says so)
```
The plugin found nothing it trusts and did nothing. The menu shows a red
line to the same effect.

```
present: high-resolution waitable timer unavailable (error N); using a standard one, set spin_margin_us higher if frame times wobble
```
Windows older than 10 1803, or a restricted environment. Waits still work;
the spin covers the timer's coarseness if `spin_margin_us` is raised to
2000-3000.

```
menu: window ready (own D3D11 device, no game hooks)
menu: camera locked (raw mouse input suspended; 1 registration(s) saved, flags 0x100)
```
The menu thread is up; the second line appears each time the menu opens.

## Pacing lines (`debug = 1`)

```
pacing: 60.0 fps, frame 16.67 ms (ema 16.66, max 17.10), held 100% of frames for 9.84 ms avg, 18234 presents since start, render thread 0x1F3C, foreground
focus: another process has the foreground; background limit in force
```

- `fps`, `frame`: presents in the last second and the mean gap between them.
- `ema`: the smoothed frame time; should sit on the cap's interval.
- `max`: the worst gap in the window. One-off spikes are streaming and
  shader compiles, not the limiter; a *regular* pattern of long/short
  frames with VSync on means the cap and the refresh rate are fighting
  (limiter.md, "Interaction with VSync").
- `held N%`: the share of frames the limiter waited on. 100% means the game
  is faster than the cap everywhere; 0% means the game cannot reach the
  cap, and the limiter is idle.
- `presents since start`: should climb by roughly `fps` per second.

`focus:` lines mark every foreground change the worker thread saw.

## Hotkeys and the menu

Each hotkey logs when it fires (`limiter disabled (hotkey)`, `config
(hotkey): ...`). A key that does nothing is usually bound to something
else: F8 is FiveM's console, F1-F3 the Rockstar Editor, and VLights, if
installed, uses F9 and F10 by default.

## Crash dumps

FiveM's crash dialog shows a module and offset. If the module is
`VLimiter.asi`, the offset can be resolved against a symbol build:
configure with `-DCMAKE_BUILD_TYPE=RelWithDebInfo` in the Dockerfile's
second step and drop the `-s` from the link options in `CMakeLists.txt`,
then `x86_64-w64-mingw32-objdump -d` the result and look up the offset. A
crash whose stack has `vlimiter_present_stub` or
`vlimiter_before_present` frames is in the hook path; one in the game's
module within a few instructions of the present site is a torn install
(limiter.md), and the startup line will have said `byte-wise`.

For a report, paste the whole dialog text and attach the log; the startup
lines are what identifies the build and the hook placement.
