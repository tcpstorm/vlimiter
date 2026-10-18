# VLimiter: guidance for coding agents

Shared, committed, tool-agnostic. Personal settings go in `CLAUDE.local.md`
(gitignored), which `CLAUDE.md` imports after this file.

## What this is

A FiveM `.asi` plugin (Windows x64 DLL, C++17) that caps the game's frame
rate from inside the game, by detouring the render thread's present call
site. Read `docs/limiter.md` first; `docs/quirks.md` lists what has already
crashed the game under FiveM (mostly learnt in the sibling project VLights,
whose foundation this reuses), and `CONTRIBUTING.md` has the versioning and
release rules.

## How to work here

- Build with Docker only: `.\build.ps1` (or `docker build --target export
  -o dist .`). Nothing is installed on the host. The build runs the tests
  first.
- The version lives in `VERSION` and nowhere else. Bump it for any change
  to the plugin; docs-only changes leave it alone.
- Never add attribution trailers, tool advertisements, or generated-by
  notes to commits, pull requests, or code.
- Game hooks follow the rules in `CONTRIBUTING.md` ("Rules for game code")
  and `docs/development.md` ("Design rules"): one `.text` detour in the
  game image, no vtable writes, no `Present` hook, nothing in DirectX or
  Cfx modules, nothing shared on the render thread but atomics.
- The pacing policy is `src/limiter/pacer.h` and has a test. Change the
  test with the policy.
- When the cap misbehaves, do not guess: `debug = 1` and read the `pacing:`
  lines (`docs/diagnostics.md`).
- Check FiveM's public source (citizenfx/fivem, `RenderHooks.cpp` in
  `rage-graphics-five`) before touching the pattern or the assumptions
  about the present site.
- Update the docs in the same change: ini keys go in the template in
  `src/plugin/config.cpp` and the README table; regenerate the shipped
  `vlimiter.ini` afterwards (`docs/development.md`).
