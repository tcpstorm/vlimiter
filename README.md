# VLimiter

A FiveM plugin that caps your frame rate from inside the game. No RivaTuner,
no driver control panel, no third-party overlay: one `.asi` in the plugins
folder, a hotkey for a small settings window, and the cap follows you from
server to server.

Client-side only. It touches nothing but the moment each finished frame is
handed to the screen, and it changes nothing in the game's files or
settings. Story mode and servers that allow plugins both work.

## Why

GTA V has no frame limit of its own; VSync is the only brake it offers, and
that ties you to the monitor's refresh rate. People cap FiveM to keep the
GPU cool and quiet, to hold a steady frame time instead of a bouncing one,
or to keep a laptop off its fans. Until now that meant an external tool
sitting over the game.

## Install

1. Download `VLimiter.asi` from the
   [Releases page](https://github.com/tcpstorm/vlimiter/releases) (or
   build it, see below).
2. Put it in `FiveM.app\plugins\`. Create the folder if it does not exist.
   On most installs that is `%LOCALAPPDATA%\FiveM\FiveM.app\plugins`.
3. Start FiveM. The plugin writes its settings file (`vlimiter.ini`) and a
   log next to itself on first run, and caps at 60 fps until you change it.

**Servers.** A server decides whether plugins load. Anything with
`sv_pureLevel` below 2 allows them; you can see the value under `vars` at
`http://<server>:30120/info.json`. Whether a server's *rules* allow a
client-side limiter is your call, not the plugin's.

## Use

Press **F4** in-game for the menu. It floats over the game as its own small
window; the camera is locked while it is open so the mouse does not move
your view.

- **Limit frame rate** switches the cap on and off.
- **FPS**: the cap while the game has focus. A slider covers 10 to 360
  (log scale, so the common values are easy to hit), the number field
  beside it takes anything from 1 to 1000, and the presets include 30,
  60, 72, 90, 120, 144, 165, 240 and your monitor's refresh rate.
- **Lower limit while another window has focus**: a second cap for when
  you alt-tab to a browser or click onto a second monitor.
- **Precision**: the spin margin, for people who watch frame-time graphs.
  The default is right for almost everyone.
- **Status**: the frame rate actually measured at the present call, the
  frame time and its worst case over the last second, and how much of
  each frame the limiter held.
- **Save** writes the ini; **Reload** re-reads it.

Changes take effect on the next frame. The menu key, a toggle key for the
cap and a reload key can all be set in the ini.

The menu also tells you when a newer release is out, with a button to the
Releases page. That is the only network access the plugin has: one request
to GitHub at startup, off with `update_check = 0`, and it never installs
anything on its own.

**VSync.** The game's own VSync setting stays in charge of tearing. With
VSync on you get whichever is lower: the refresh rate or your cap. With
VSync off the cap alone sets the pace, which is the usual way to run it.

## Settings

All of these live in `vlimiter.ini` next to the plugin. The file is
commented; the essentials:

| Key | Default | What it does |
| --- | --- | --- |
| `enabled` | `1` | Master switch. `0` lets every frame through. |
| `fps_limit` | `60` | The cap while the game has focus. `0` = no limit. |
| `background_fps` | `30` | The cap while another window has focus. `0` = same as `fps_limit`. |
| `spin_margin_us` | `1000` | How much of each wait is spent spinning for precision rather than sleeping. `0` = sleep only. |
| `menu_key` | `F4` | Opens the menu. `0` disables. |
| `toggle_key` | `0` | Switches the cap on and off. Off by default; `F6`, `F11` or a key code to enable. |
| `reload_key` | `0` | Re-reads the ini. |
| `update_check` | `1` | Once at startup, ask GitHub whether a newer release exists and say so in the menu. `0` = never contact anything. |
| `debug` | `0` | A pacing summary line in the log every five seconds. |

Hotkeys take `F1`-`F24` or a virtual-key code. Avoid F8 (FiveM's console)
and F1-F3 (the Rockstar Editor).

## How it works, briefly

The game hands each finished frame to the screen from one place in its own
code, through a function pointer that FiveM fills in. The plugin finds that
instruction with the same byte pattern FiveM's public source uses and puts
a small detour on it. Every frame, on the render thread, the detour works
out when this frame is allowed to go out, waits until then (a timer for the
bulk, a short spin for the last stretch so the release is on the tick), and
lets the game continue into its normal present. Nothing is hooked in
DirectX, no vtable is written, and the game's files and settings are
untouched. `docs/limiter.md` has the detail, and `docs/quirks.md` says why
it is built that way.

## Something off?

Set `debug = 1`, restart, play for a minute, and read `vlimiter.log` next
to the plugin. The startup lines say whether the hook landed and where; the
pacing lines say what frame rate it measured and how long it held each
frame. `docs/diagnostics.md` explains every line. Set `debug` back to `0`
afterwards.

## Build it yourself

Only Docker Desktop is needed; nothing else gets installed.

```powershell
.\build.ps1            # -> .\dist\VLimiter.asi
.\build.ps1 -Install   # also copies it into the FiveM plugins folder
```

Details, source layout, and how every fact the plugin relies on was
verified are in `docs/development.md`. Versioning, releases, and the rules
for changes to game hooks are in `CONTRIBUTING.md`.

## Relation to VLights

VLimiter is built on the same foundation as
[VLights](https://github.com/tcpstorm/vlights): the same build, menu window,
config and logging code, the same patched MinHook, and the same rules for
what may be hooked under FiveM's anti-cheat. The two plugins are
independent and can be installed together.

## Contributing and reports

Crash, wrong behaviour, or an idea: open an issue; the templates ask for
what each kind of report needs. Changes are welcome as pull requests, see
`CONTRIBUTING.md`. Everyone taking part is expected to follow the
`CODE_OF_CONDUCT.md`; anything security-related goes through `SECURITY.md`.

## Licence

MIT, see `LICENSE`. Bundles MinHook (BSD 2-Clause) and Dear ImGui (MIT)
under `third_party/`.
