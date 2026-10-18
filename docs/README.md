# Docs

Working notes for the next time the cap misbehaves or a game update moves
the hook. The main `README.md` is the overview; these are the reference.

| Document | Covers |
| --- | --- |
| [limiter.md](limiter.md) | The one hook: the present call site, the mid-function stub, installing while the render thread runs, the pacing policy, the wait, foreground/background, VSync, what is measured, and the table of verified facts with sources. |
| [quirks.md](quirks.md) | FiveM and anti-cheat behaviour that shaped every design choice: what adhesive tolerates, MinHook under FiveM, loading the plugin. |
| [diagnostics.md](diagnostics.md) | The `debug` switch, what each log line means, how to tell whether the limiter is actually running, and the crash workflow. |
| [pause-menu-research.md](pause-menu-research.md) | Research notes (2026-10-18, not implemented): whether the limiter could be exposed inside the game's own pause menu under VSync, the candidate routes, their risks, and what is unverified. |
| [development.md](development.md) | Building, source layout, design rules, the menu window, and what to check after a game update. |

Conventions: offsets are bytes from the start of the object or image, game
build 3751 unless stated, and "Cfx" means the public citizenfx/fivem source.
