<!-- One topic per pull request. See CONTRIBUTING.md. -->

## What this changes

<!-- What, and why. Link the issue if there is one. -->

## Kind of change

- [ ] Plugin code (`src/`, `res/`, `tests/`, build files): `VERSION` is bumped and the merge will publish a release
- [ ] Docs, tooling, workflows only: no version bump

## If the byte pattern, the stub, or the hook installation changed

- Game build tested on:
- Startup lines from `vlimiter.log` (`present call site ...` and `present detour installed ...`):

```
```

- Tested in-game: a session with the menu open, the cap changed live, alt-tabbed and back, no crash

## Checklist

- [ ] Builds with `.\build.ps1` (tests run inside the build)
- [ ] Docs updated in the same PR (ini template in `src/plugin/config.cpp`, README table, `docs/limiter.md` for anything about the hook)
- [ ] Shipped `vlimiter.ini` regenerated if config keys changed
- [ ] No attribution trailers or tool advertisements in commits
