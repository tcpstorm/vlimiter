// present.h - the one game hook: a detour at the render thread's present call
// site, where the frame is paced before it reaches IDXGISwapChain::Present.
//
// Why there and not on Present itself: FiveM's anti-cheat terminates the game
// on an IDXGISwapChain::Present hook (docs/quirks.md). The game, however,
// calls the platform present routine through a function pointer from one
// place in its own code, and FiveM's public source names that place with a
// byte pattern (RenderHooks.cpp, "present hook function"). A MinHook .text
// detour on that instruction, inside the game image, runs on the render
// thread once per frame, right before the swapchain present, which is the
// same moment every external limiter waits at.
#pragma once

#include "plugin/config.h"

#include <cstdint>

namespace vlimiter::present
{
	struct Stats
	{
		bool installed = false;
		uint64_t calls = 0;          // detour invocations since start
		double fps = 0.0;            // presents per second over the last window
		double frameMs = 0.0;        // mean present-to-present time over the last window
		double frameEmaMs = 0.0;     // smoothed present-to-present time (EMA, 1/8)
		double maxFrameMs = 0.0;     // worst frame in the last window
		double waitMs = 0.0;         // mean time the limiter held each frame in the last window
		double waitShare = 0.0;      // 0..1 share of frames that were held
		bool background = false;     // the background limit is in force
		uint32_t renderThread = 0;   // thread id the detour runs on
		bool timerHighRes = false;   // CREATE_WAITABLE_TIMER_HIGH_RESOLUTION available
	};

	// Locates the present call site through Cfx's public byte pattern and
	// detours it. Call from DllMain. Returns false if the plugin should stay
	// inactive (the log says why).
	bool Install();
	bool Installed();

	// Pushes the limits to the atomics the render thread reads.
	void Apply(const Config& cfg);

	// Told by the worker thread whether another process has the foreground.
	void SetBackground(bool background);

	Stats GetStats();
}
