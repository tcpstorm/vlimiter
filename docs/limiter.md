# The limiter: the hook, the pacing, and why it is built this way

One game hook, one decision per frame. This document is the reference for
both, and for the installation step that differs from VLights.

## Where the frame is paced

GTA V's render thread ends every frame in a routine that calls the
platform's present function through a global pointer:

```
48 8B 05 xx xx xx xx    mov  rax, [rip + g_presentFn]
48 85 C0                test rax, rax
74 0C                   je   skip
8B 4D 50                mov  ecx, [rbp+0x50]        ; sync interval
8B ..                   mov  edx, ...               ; flags
FF D0                   call rax
```

FiveM's `rage-graphics-five` component writes its own `D3DPresent` into that
pointer (RenderHooks.cpp, "present hook function"), and `D3DPresent` is what
finally calls `IDXGISwapChain::Present`. FiveM locates the instruction with
the byte pattern

```
48 8B 05 ? ? ? ? 48 85 C0 74 0C 8B 4D 50 8B
```

and maintains that pattern across game builds; the plugin uses it unchanged
(`src/game/present.cpp`, `kPresentCallPattern`) and refuses to hook unless
it matches exactly once.

The plugin puts a MinHook `.text` detour on the `mov`. That is inside the
game image, on the render thread, once per frame, immediately before the
swapchain present. It is the same instant an external limiter (RTSS, the
driver's limiter) waits at, and it is not a hook on `Present`, which the
anti-cheat kills (quirks.md).

### A mid-function detour

Every other hook in VLights sits on a function's first instruction, so the
detour is an ordinary C++ function with the target's calling convention. This
one is in the middle of a function: any volatile register may hold a live
value and the stack is wherever the function left it. The entry point is
therefore a hand-written stub (bottom of `present.cpp`; `present_stub.asm`
for MSVC) that:

1. saves the flags, `rax rcx rdx r8 r9 r10 r11` and `xmm0`-`xmm5` (the
   Win64 volatile set) plus `rbx`, which it uses as a frame pointer;
2. aligns `rsp` to 16 and reserves 32 bytes of shadow space;
3. calls `vlimiter_before_present()`, ordinary C++;
4. restores everything and jumps to MinHook's trampoline, which is the
   relocated `mov rax, [rip+slot]` followed by a jump back to `test rax, rax`.

A branch can land exactly on the `mov` (it is a legal instruction boundary)
and finds the jump to the stub there; nothing can land inside the 5 bytes
the jump overwrote, because they were one instruction. The 2 remaining bytes
of the original 7-byte `mov` are never executed.

### Installing while the render thread runs

VLights installs its first hook from `DllMain` on a function only the main
thread calls, and the main thread is blocked in `LoadLibrary` at that point,
so the target cannot be executing. The present site is different: FiveM
loads `.asi` files during `DoGameLoad` while the render thread is drawing
the loading screen, so the target *is* running.

The patch is a 5-byte `jmp rel32`. The vendored MinHook never suspends
threads (FiveM stubs `CreateToolhelp32Snapshot`; quirks.md) and instead
writes the jump with a single aligned 8-byte store whenever the five bytes
fall inside one aligned 8-byte word, which x86-64 guarantees other cores
see atomically. The startup log reports whether that was the case:

```
present detour installed (thread 0x..., trampoline ..., jump written atomically, ...)
```

When the site straddles an 8-byte boundary the write is byte-wise, and a
core fetching that instruction at the same instant could in theory decode a
half-written jump. That is the standard risk every runtime hooking library
takes without thread suspension; the log line exists so a crash at startup
can be matched to it. Whether the site is aligned on a given build is only known from
that log line; nothing in the pattern guarantees it.

Cross-modifying code without a serialising instruction on the other core is
formally undefined on x86; in practice the fetched instruction is either
the old `mov` or the new `jmp`, both of which are valid continuations.

### What the startup log should show

```
VLimiter 0.1.0 starting (thread 0x..., game build 3751)
config (startup): enabled=1 fps_limit=60 ...
main module base=0x... size=0x...
present call site at 0x... (base+0x..., 8-byte-aligned); present routine slot at base+0x... -> 0x... (rage-graphics-five.dll+0x...)
present detour installed (thread 0x..., trampoline 0x..., jump written atomically, threads frozen via: none (not suspended))
menu: window ready (own D3D11 device, no game hooks)
```

The `present routine slot` line is the check that the pattern found the
right place: the pointer the game is about to call should live in
`rage-graphics-five.dll` (FiveM's `D3DPresent`). If it points into the game
executable itself, FiveM has moved its hook and the plugin's assumptions
need re-checking against `RenderHooks.cpp`.

## The pacing decision

`src/limiter/pacer.h`, pure arithmetic on `QueryPerformanceCounter` ticks,
tested on the host (`tests/test_pacer.cpp`):

- Frames go out on a fixed cadence: `next = previous target + interval`.
- A frame that arrives early waits until its target.
- A frame that arrives late (the game needed longer than the interval) goes
  out at once and the cadence restarts from now. No catch-up burst follows
  a slow frame.
- Interval 0 means unlimited and forgets the cadence, so switching the cap
  on again never waits for a stale target.

The wait itself (`WaitUntil` in `present.cpp`) is a high-resolution
waitable timer (`CREATE_WAITABLE_TIMER_HIGH_RESOLUTION`, Windows 10 1803+,
falls back to a standard timer) for everything but the last
`spin_margin_us`, then a `YieldProcessor` spin to the tick. The high-res
timer alone lands within a few hundred microseconds; the spin is what makes
the frame time flat on a graph. `spin_margin_us = 0` is sleep-only.

### Foreground and background

The worker thread (100 ms tick) asks `GetForegroundWindow` which process
owns the foreground and tells the limiter; the render thread only reads an
atomic. Our own menu window counts as the game having focus, so opening the
menu does not drop the measured rate to the background cap. FiveM already
holds a *minimised* game to 30 fps in `D3DPresent`; `background_fps` covers
the alt-tabbed and second-monitor cases FiveM leaves alone.

### Interaction with VSync

The detour runs before `D3DPresent`, which still passes the game's sync
interval to the swapchain. With VSync on, `Present(1)` blocks for the next
vertical blank after our wait, so the result is the lower of refresh rate
and cap, and a cap close to the refresh rate makes the two pacers fight
(frame times alternate). Either run VSync off and let the cap pace, or set
the cap clearly below the refresh rate.

## Measurement

Everything the menu shows is measured at the detour, on the render thread,
and published once per second through atomics:

| Figure | Meaning |
| --- | --- |
| fps | presents in the last one-second window |
| frame time | window length / presents (mean present-to-present) |
| ema | exponential moving average of present-to-present time, weight 1/8 |
| worst | longest present-to-present gap in the window |
| held | share of frames the limiter waited on, and the mean wait per frame |
| calls | detour invocations since start; climbs by one per frame |

With `debug = 1` the worker writes the same figures to the log every five
seconds (`pacing:` lines).

## Costs

Per frame: two `QueryPerformanceCounter` reads, a handful of atomic loads
and the stub's register save (about 40 instructions). No locks, no heap, no
logging on the render thread. The wait is the point, not a cost.

## Verified facts and their sources

| Fact | Source |
| --- | --- |
| The present call site pattern `48 8B 05 ? ? ? ? 48 85 C0 74 0C 8B 4D 50 8B` and that the rip-relative slot at +3 holds the present routine FiveM replaces with `D3DPresent` | Cfx `code/components/rage-graphics-five/src/RenderHooks.cpp`: `hook::put(hook::get_address<void*>(hook::get_pattern("48 8B 05 ? ? ? ? 48 85 C0 74 0C 8B 4D 50 8B", 3)), D3DPresent);` |
| `D3DPresent` is what calls `IDXGISwapChain::Present`, applies FiveM's vsync override and tearing flag, and limits a minimised game to 30 fps | same file, `void D3DPresent(int syncInterval, int flags)` |
| FiveM's own `LimitFrameTime` (timer wait to a target interval) exists only for its off-screen "reverse game data" swapchain and the minimised case; nothing in FiveM caps the normal game | same file, `LimitFrameTime`, `BufferBackedDXGISwapChain::Present` |
| `.asi` files load during `DoGameLoad`, main thread, unless `sv_pureLevel >= 2` | Cfx `code/components/asi-five/src/Component.cpp` |
| An `.asi` must carry an `FX_ASI_BUILD` resource typed with the numeric game build (2189+) | same `Component.cpp` |
| An `IDXGISwapChain::Present` hook and vtable writes get the game terminated by the anti-cheat; `.text` detours in the game image and a separate D3D11 window do not | VLights `docs/quirks.md`, observed |
| Unverified until the first in-game session: that the site is 8-byte aligned on every build, and that the detour's register save covers everything live at the site (xmm6-15 are callee-saved under Win64 and our C++ preserves them; the x87/MXCSR state is not touched) | this document; report the startup lines with the game build in any PR that touches the hook |
