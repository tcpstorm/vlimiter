# Contributing

VLimiter is a FiveM `.asi` that caps the frame rate by detouring one
instruction in the game's render path. That makes it a project where a
wrong offset crashes someone's game, so the rules below are about not
shipping that.

## Before you start

- Read `docs/limiter.md` and `docs/quirks.md`. The second lists what has
  already crashed the game once; do not rediscover it.
- Build with Docker only (`.\build.ps1`); nothing needs installing on the
  host. The build runs the tests first and fails on them.
- Test in-game. FiveM story mode is enough. A change to the hook or the
  stub is not done until it has survived a session of driving around with
  the menu open, the cap changed live, the game alt-tabbed and back.

## Versioning: one file

The version lives in exactly one place: the `VERSION` file at the repo
root, containing `MAJOR.MINOR.PATCH` and nothing else. CMake turns it into
`project(VERSION)`, generates `vlimiter/version.h` for the code, and fills
the Windows version resource; the release workflow reads it to name the
tag. Never put a version string anywhere else.

To make a release:

1. Bump `VERSION` in your branch. Patch for fixes and diagnostics, minor
   for a new capability (a new ini key that changes behaviour), major when
   settings or file names stop being compatible.
2. Open a pull request into `main`. CI runs two checks: `build` (the Docker
   build with tests) and `version bump` (the PR's `VERSION` must be higher
   than main's and its `v<version>` tag must not exist yet). Both are
   required to merge.
3. Merge. The push to `main` builds again and, because the tag is new,
   publishes a GitHub Release with `VLimiter.asi` and `vlimiter.ini`
   attached and the commit subjects since the previous tag as notes.

The bump check only applies when the plugin itself changes: anything under
`src/`, `res/`, `tests/`, `third_party/`, `cmake/`, or the build files
(`CMakeLists.txt`, `Dockerfile`, `build.ps1`, `vlimiter.ini`, `VERSION`).
Pull requests that touch only docs, `tools/`, the workflows, or the licence
are exempt and merge without a release.

## Pull requests

- One topic per PR.
- Commit messages describe the change and why; no attribution trailers or
  tool advertisements.
- If you touched the byte pattern, the stub, or the hook installation, say
  in the PR which game build you tested on and paste the startup lines from
  `vlimiter.log` (they show where the hook landed and how the jump was
  written).
- Update the docs in the same PR. New ini keys go in the ini template
  (`src/plugin/config.cpp`) and the README table; anything about the hook
  goes in `docs/limiter.md`.
- Regenerate the shipped `vlimiter.ini` when config keys change
  (`docs/development.md` shows how).

## Rules for game code

These are not preferences; each one is backed by a crash in `docs/quirks.md`
or by the analysis in `docs/limiter.md`.

- One detour, a `.text` code detour inside the game image, located by the
  byte pattern FiveM's own source uses, installed only when that pattern
  matches exactly once. No vtable writes, no `Present` hook, no hooks in
  DirectX or Cfx modules, no immediate Direct3D context calls.
- The per-frame path on the render thread does no locking, no allocation,
  no logging, and no Win32 calls beyond the clock and the pacing wait.
  Everything it needs from other threads arrives as an atomic.
- The mid-function stub preserves the complete Win64 volatile register set
  and the flags. Changing it means re-reading the instruction sequence at
  the site and testing in-game.
- Policy changes go into `src/limiter/pacer.h` with a test in
  `tests/test_pacer.cpp`.
- Add an `FX_ASI_BUILD` line to `res/VLimiter.rc` for each new game build,
  and re-check the byte pattern against Cfx's `RenderHooks.cpp`.

## Diagnosing a report

Ask for `vlimiter.log` with `debug = 1`. `docs/diagnostics.md` explains the
lines. For a crash, the offset in FiveM's dialog plus the startup lines
usually say whether it is the hook path, a torn install, or something else.
