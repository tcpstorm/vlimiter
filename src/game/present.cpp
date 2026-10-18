// present.cpp - see present.h.
//
// The detour sits in the middle of a function, on the instruction
//
//     48 8B 05 xx xx xx xx    mov rax, [rip + g_presentFn]
//     48 85 C0                test rax, rax
//     74 0C                   je  skip
//     8B 4D 50                mov ecx, [rbp+0x50]      ; sync interval
//     8B ...                  mov edx, ...             ; flags
//     FF D0                   call rax                 ; -> FiveM's D3DPresent -> IDXGISwapChain::Present
//
// so unlike a hook on a function's first instruction it cannot assume the
// calling convention: every volatile register may hold a live value. The entry
// point is therefore a hand-written stub (bottom of this file) that saves the
// flags, the volatile integer registers and xmm0-5, aligns the stack, calls
// vlimiter_before_present(), restores everything and jumps to MinHook's
// trampoline, which executes the relocated mov and continues at test rax.
//
// Everything the render thread touches per frame is either its own (the
// pacer, the frame-time window, the waitable timer) or an atomic the worker
// and menu threads write (the limits). No locks, no allocations, no logging
// on the hot path.
#include "game/present.h"
#include "hook/pattern.h"
#include "limiter/pacer.h"
#include "plugin/log.h"
#include "plugin/plugin.h"

#include <windows.h>
#include <MinHook.h>

#include <atomic>
#include <cstdint>
#include <cstring>

#ifndef CREATE_WAITABLE_TIMER_HIGH_RESOLUTION
#define CREATE_WAITABLE_TIMER_HIGH_RESOLUTION 0x00000002
#endif

extern "C"
{
	// Where the stub continues after our code: MinHook's trampoline for the
	// detoured instruction. Written once, before the hook is enabled.
	void* vlimiter_present_trampoline = nullptr;
	// The hand-written entry point (asm at the bottom, or present_stub.asm for MSVC).
	void vlimiter_present_stub();
	// Called by the stub with a clean, aligned stack and nothing live in the
	// volatile registers.
	void vlimiter_before_present();
	// From the patched MinHook: 1 if the last enable wrote its jump atomically.
	extern int mh_vlimiter_last_patch_atomic;
}

namespace vlimiter::present
{
	namespace
	{
		// Cfx code/components/rage-graphics-five/src/RenderHooks.cpp:
		//   hook::put(hook::get_address<void*>(hook::get_pattern("48 8B 05 ? ? ? ? 48 85 C0 74 0C 8B 4D 50 8B", 3)), D3DPresent);
		// Cfx maintains the pattern across builds; a miss here means the
		// plugin stays inactive and says so.
		constexpr const char* kPresentCallPattern = "48 8B 05 ? ? ? ? 48 85 C0 74 0C 8B 4D 50 8B";

		std::atomic<bool> g_installed{ false };
		uintptr_t g_imageBase = 0;
		size_t g_imageSize = 0;

		// --- written by other threads, read per frame ---
		std::atomic<bool> g_enabled{ false };
		std::atomic<bool> g_background{ false };
		std::atomic<int64_t> g_intervalFg{ 0 };
		std::atomic<int64_t> g_intervalBg{ 0 };
		std::atomic<int64_t> g_spinTicks{ 0 };

		// --- render thread only ---
		Pacer g_pacer;
		Ema g_frameEma;
		int64_t g_lastPresent = 0;
		HANDLE g_timer = nullptr;
		bool g_timerTried = false;
		bool g_timerHighRes = false;
		int64_t g_winStart = 0;
		uint64_t g_winFrames = 0;
		uint64_t g_winWaits = 0;
		int64_t g_winWaitTicks = 0;
		int64_t g_winMaxFrame = 0;

		// --- published by the render thread, read by menu / worker ---
		std::atomic<uint64_t> g_calls{ 0 };
		std::atomic<uint32_t> g_renderThread{ 0 };
		std::atomic<bool> g_pubTimerHighRes{ false };
		std::atomic<int64_t> g_pubFpsMilli{ 0 };     // fps * 1000
		std::atomic<int64_t> g_pubFrameUs{ 0 };
		std::atomic<int64_t> g_pubFrameEmaUs{ 0 };
		std::atomic<int64_t> g_pubMaxFrameUs{ 0 };
		std::atomic<int64_t> g_pubWaitUs{ 0 };
		std::atomic<int64_t> g_pubWaitShareMilli{ 0 }; // held frames / frames * 1000

		int64_t Frequency()
		{
			static int64_t f = [] { LARGE_INTEGER li; QueryPerformanceFrequency(&li); return static_cast<int64_t>(li.QuadPart); }();
			return f;
		}

		int64_t Now()
		{
			LARGE_INTEGER li;
			QueryPerformanceCounter(&li);
			return static_cast<int64_t>(li.QuadPart);
		}

		int64_t TicksToUs(int64_t t) { return t * 1000000 / Frequency(); }

		void EnsureTimer()
		{
			if (g_timerTried)
				return;
			g_timerTried = true;
			g_timer = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
			if (g_timer)
			{
				g_timerHighRes = true;
			}
			else
			{
				g_timer = CreateWaitableTimerExW(nullptr, nullptr, 0, TIMER_ALL_ACCESS);
				if (!g_timer)
					Log("present: no waitable timer (error %lu); waits will spin", GetLastError());
				else
					Log("present: high-resolution waitable timer unavailable (error %lu); using a standard one, set spin_margin_us higher if frame times wobble", GetLastError());
			}
			g_pubTimerHighRes = g_timerHighRes;
		}

		// Holds the thread until `target`: a timer wait for the bulk, then a
		// spin for the last spin_margin_us so the release is on the tick.
		void WaitUntil(int64_t target)
		{
			EnsureTimer();
			const int64_t spin = g_spinTicks.load(std::memory_order_relaxed);
			for (;;)
			{
				const int64_t now = Now();
				const int64_t remaining = target - now;
				if (remaining <= 0)
					return;
				if (g_timer && remaining > spin)
				{
					const int64_t sleepTicks = remaining - spin;
					LARGE_INTEGER due;
					due.QuadPart = -(sleepTicks * 10000000 / Frequency()); // relative, 100 ns units
					if (due.QuadPart < 0 && SetWaitableTimer(g_timer, &due, 0, nullptr, nullptr, FALSE))
					{
						WaitForSingleObject(g_timer, 100); // the bound is a safety net; the timer fires first
						continue;
					}
				}
				YieldProcessor();
			}
		}

		void Publish(int64_t now)
		{
			const int64_t elapsed = now - g_winStart;
			if (elapsed <= 0 || g_winFrames == 0)
				return;
			g_pubFpsMilli = g_winFrames * 1000 * Frequency() / elapsed;
			g_pubFrameUs = TicksToUs(elapsed / static_cast<int64_t>(g_winFrames));
			g_pubFrameEmaUs = TicksToUs(g_frameEma.Value());
			g_pubMaxFrameUs = TicksToUs(g_winMaxFrame);
			g_pubWaitUs = TicksToUs(g_winWaitTicks / static_cast<int64_t>(g_winFrames));
			g_pubWaitShareMilli = static_cast<int64_t>(g_winWaits * 1000 / g_winFrames);
		}
	}

	bool Install()
	{
		if (!GetMainModuleRange(g_imageBase, g_imageSize))
		{
			Log("could not read main module headers");
			return false;
		}
		Log("main module base=0x%llX size=0x%llX", (unsigned long long)g_imageBase, (unsigned long long)g_imageSize);

		Pattern pat;
		if (!ParsePattern(kPresentCallPattern, pat))
		{
			Log("bad pattern text");
			return false;
		}
		auto hits = FindPattern(pat, g_imageBase, g_imageSize, 2);
		if (hits.size() != 1)
		{
			Log("pattern '%s' matched %u times (need exactly 1); not hooking. The game build may have changed; check Cfx RenderHooks.cpp (\"present hook function\") for the current pattern.",
				kPresentCallPattern, (unsigned)hits.size());
			return false;
		}

		uint8_t* site = reinterpret_cast<uint8_t*>(hits[0]);
		// Decode the rip-relative operand: the slot the game reads its present
		// routine from, and what is in it right now. Under FiveM that is
		// rage-graphics-five.dll's D3DPresent; logging the owner module is the
		// check that this is the site we think it is.
		int32_t disp = 0;
		memcpy(&disp, site + 3, sizeof(disp));
		void** slot = reinterpret_cast<void**>(site + 7 + disp);
		void* fn = nullptr;
		SIZE_T got = 0;
		if (!ReadProcessMemory(GetCurrentProcess(), slot, &fn, sizeof(fn), &got) || got != sizeof(fn))
			fn = nullptr;
		wchar_t owner[MAX_PATH] = L"?";
		uintptr_t ownerBase = 0;
		if (fn)
		{
			HMODULE mod = nullptr;
			if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, static_cast<LPCWSTR>(fn), &mod) && mod)
			{
				GetModuleFileNameW(mod, owner, MAX_PATH);
				ownerBase = reinterpret_cast<uintptr_t>(mod);
			}
		}
		const wchar_t* ownerName = owner;
		for (const wchar_t* p = owner; *p; ++p)
			if (*p == L'\\' || *p == L'/')
				ownerName = p + 1;
		Log("present call site at 0x%llX (base+0x%llX, %s-aligned); present routine slot at base+0x%llX -> %p (%ls+0x%llX)",
			(unsigned long long)site, (unsigned long long)(reinterpret_cast<uintptr_t>(site) - g_imageBase),
			(reinterpret_cast<uintptr_t>(site) & 7) == 0 ? "8-byte" : "not 8-byte",
			(unsigned long long)(reinterpret_cast<uintptr_t>(slot) - g_imageBase), fn, ownerName,
			(unsigned long long)(fn && ownerBase ? reinterpret_cast<uintptr_t>(fn) - ownerBase : 0));
		if (!fn)
			Log("present routine slot is empty at this point; FiveM normally fills it before any frame is drawn");

		MH_STATUS st = MH_Initialize();
		if (st != MH_OK && st != MH_ERROR_ALREADY_INITIALIZED)
		{
			Log("MH_Initialize failed: %s", MH_StatusToString(st));
			return false;
		}
		st = MH_CreateHook(site, reinterpret_cast<void*>(&vlimiter_present_stub), &vlimiter_present_trampoline);
		if (st == MH_OK)
			st = MH_EnableHook(site);
		if (st != MH_OK)
		{
			Log("hooking the present call site failed: %s", MH_StatusToString(st));
			return false;
		}
		g_installed = true;
		Log("present detour installed (thread 0x%lX, trampoline %p, jump written %s, threads frozen via: %s)",
			GetCurrentThreadId(), vlimiter_present_trampoline, mh_vlimiter_last_patch_atomic ? "atomically" : "byte-wise", LastFreezeMethod());
		return true;
	}

	bool Installed()
	{
		return g_installed.load();
	}

	void Apply(const Config& cfg)
	{
		const int64_t f = Frequency();
		g_intervalFg = IntervalTicks(cfg.fpsLimit, f);
		g_intervalBg = IntervalTicks(cfg.backgroundFps > 0 ? cfg.backgroundFps : cfg.fpsLimit, f);
		g_spinTicks = static_cast<int64_t>(cfg.spinMarginUs) * f / 1000000;
		g_enabled = cfg.enabled;
	}

	void SetBackground(bool background)
	{
		g_background.store(background, std::memory_order_relaxed);
	}

	Stats GetStats()
	{
		Stats s;
		s.installed = g_installed.load();
		s.calls = g_calls.load(std::memory_order_relaxed);
		s.fps = g_pubFpsMilli.load(std::memory_order_relaxed) / 1000.0;
		s.frameMs = g_pubFrameUs.load(std::memory_order_relaxed) / 1000.0;
		s.frameEmaMs = g_pubFrameEmaUs.load(std::memory_order_relaxed) / 1000.0;
		s.maxFrameMs = g_pubMaxFrameUs.load(std::memory_order_relaxed) / 1000.0;
		s.waitMs = g_pubWaitUs.load(std::memory_order_relaxed) / 1000.0;
		s.waitShare = g_pubWaitShareMilli.load(std::memory_order_relaxed) / 1000.0;
		s.background = g_background.load(std::memory_order_relaxed);
		s.renderThread = g_renderThread.load(std::memory_order_relaxed);
		s.timerHighRes = g_pubTimerHighRes.load(std::memory_order_relaxed);
		return s;
	}
}

// The per-frame body. Render thread, once per present, before the swapchain
// present. Nothing here may block on anything but the pacing wait itself.
extern "C" void vlimiter_before_present()
{
	using namespace vlimiter;
	using namespace vlimiter::present;

	g_calls.fetch_add(1, std::memory_order_relaxed);
	if (g_renderThread.load(std::memory_order_relaxed) == 0)
		g_renderThread.store(GetCurrentThreadId(), std::memory_order_relaxed);

	int64_t now = Now();

	int64_t interval = 0;
	if (g_enabled.load(std::memory_order_relaxed))
		interval = g_background.load(std::memory_order_relaxed) ? g_intervalBg.load(std::memory_order_relaxed) : g_intervalFg.load(std::memory_order_relaxed);

	const int64_t target = g_pacer.Arrive(now, interval);
	if (target > now)
	{
		WaitUntil(target);
		const int64_t after = Now();
		g_winWaits++;
		g_winWaitTicks += after - now;
		now = after;
	}

	if (g_lastPresent)
	{
		const int64_t ft = now - g_lastPresent;
		g_frameEma.Add(ft);
		if (ft > g_winMaxFrame)
			g_winMaxFrame = ft;
	}
	g_lastPresent = now;
	g_winFrames++;

	if (g_winStart == 0)
	{
		g_winStart = now;
	}
	else if (now - g_winStart >= Frequency())
	{
		Publish(now);
		g_winStart = now;
		g_winFrames = 0;
		g_winWaits = 0;
		g_winWaitTicks = 0;
		g_winMaxFrame = 0;
	}
}

// ---------------------------------------------------------------- the stub
//
// Mid-function detour entry. Preserves: flags, rax, rcx, rdx, r8-r11 (the
// volatile integer registers), xmm0-xmm5 (the volatile vector registers) and
// rbx (used here as the frame pointer, so it is saved too). rsp is realigned
// to 16 for the call with 32 bytes of shadow space. Then jumps to MinHook's
// trampoline, i.e. the relocated `mov rax, [rip+slot]` followed by a jump back
// to `test rax, rax` in the game.
#if defined(__GNUC__)
asm(R"ASM(
	.text
	.globl vlimiter_present_stub
vlimiter_present_stub:
	pushfq
	push %rax
	push %rcx
	push %rdx
	push %r8
	push %r9
	push %r10
	push %r11
	push %rbx
	mov %rsp, %rbx
	and $-16, %rsp
	sub $0x80, %rsp
	movups %xmm0, 0x20(%rsp)
	movups %xmm1, 0x30(%rsp)
	movups %xmm2, 0x40(%rsp)
	movups %xmm3, 0x50(%rsp)
	movups %xmm4, 0x60(%rsp)
	movups %xmm5, 0x70(%rsp)
	call vlimiter_before_present
	movups 0x20(%rsp), %xmm0
	movups 0x30(%rsp), %xmm1
	movups 0x40(%rsp), %xmm2
	movups 0x50(%rsp), %xmm3
	movups 0x60(%rsp), %xmm4
	movups 0x70(%rsp), %xmm5
	mov %rbx, %rsp
	pop %rbx
	pop %r11
	pop %r10
	pop %r9
	pop %r8
	pop %rdx
	pop %rcx
	pop %rax
	popfq
	jmp *vlimiter_present_trampoline(%rip)
)ASM");
#elif !defined(_MSC_VER)
#error "vlimiter_present_stub needs GNU inline asm or src/hook/present_stub.asm (MSVC)"
#endif
