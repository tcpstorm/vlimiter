// main.cpp - DllMain, startup, and the hotkey / housekeeping thread.
//
// Load order in FiveM: the asi-five component LoadLibrary()s every .asi in
// FiveM.app/plugins/ from its DoGameLoad step, on the main thread, unless
// the server's sv_pureLevel is >= 2 (code/components/asi-five/src/Component.cpp).
// By then FiveM's own render hooks are in place (its D3DPresent sits in the
// slot our detour site reads) and the render thread is drawing the loading
// screen.
//
// The present detour is installed synchronously inside DllMain. Its target
// is executing on the render thread at the time, so MinHook is told not to
// suspend anything (FiveM stubs the Toolhelp snapshot anyway) and the jump
// is written as one aligned 8-byte store whenever the site allows it; the
// log says which. See docs/limiter.md, "Installing while the render thread
// runs".
#include "game/present.h"
#include "hook/pattern.h"
#include "plugin/config.h"
#include "plugin/log.h"
#include "plugin/plugin.h"
#include "plugin/update.h"

#include <windows.h>

#include <string>

namespace
{
	HMODULE g_module = nullptr;

	std::wstring PluginDir()
	{
		wchar_t buf[MAX_PATH];
		DWORD n = GetModuleFileNameW(g_module, buf, MAX_PATH);
		std::wstring path(buf, n);
		size_t slash = path.find_last_of(L"\\/");
		return (slash == std::wstring::npos) ? L"." : path.substr(0, slash);
	}

	// True when no window of this process has the foreground: alt-tabbed
	// away, clicked onto another monitor, or no foreground window at all.
	// Our own menu window counts as the game having focus.
	bool ProcessInBackground()
	{
		HWND fg = GetForegroundWindow();
		if (!fg)
			return true;
		DWORD pid = 0;
		GetWindowThreadProcessId(fg, &pid);
		return pid != GetCurrentProcessId();
	}

	bool Pressed(int vk, bool& wasDown)
	{
		if (vk <= 0)
			return false;
		const bool down = (GetAsyncKeyState(vk) & 0x8000) != 0;
		const bool pressed = down && !wasDown;
		wasDown = down;
		return pressed;
	}

	DWORD WINAPI WorkerThread(LPVOID)
	{
		if (vlimiter::GetConfig().menuKey > 0)
		{
			if (!vlimiter::MenuInit())
				vlimiter::Log("menu unavailable; ini + hotkeys still work");
		}
		else
		{
			vlimiter::Log("menu disabled (menu_key = 0)");
		}

		if (vlimiter::GetConfig().updateCheck)
			vlimiter::update::Start();

		bool reloadWasDown = false, menuWasDown = false, toggleWasDown = false;
		bool wasBackground = false;
		ULONGLONG lastDebugLine = GetTickCount64();
		for (;;)
		{
			Sleep(100);
			const vlimiter::Config cfg = vlimiter::GetConfig();

			const bool background = ProcessInBackground();
			if (background != wasBackground)
			{
				vlimiter::present::SetBackground(background);
				vlimiter::LogDebug("focus: %s", background ? "another process has the foreground; background limit in force" : "game has the foreground");
				wasBackground = background;
			}

			if (Pressed(cfg.reloadKey, reloadWasDown))
				vlimiter::ReloadConfigFromDisk("hotkey");
			if (Pressed(cfg.toggleKey, toggleWasDown))
				vlimiter::ToggleEnabled("hotkey");
			if (Pressed(cfg.menuKey, menuWasDown))
				vlimiter::MenuToggle();

			if (cfg.debug && GetTickCount64() - lastDebugLine >= 5000)
			{
				lastDebugLine = GetTickCount64();
				const vlimiter::present::Stats st = vlimiter::present::GetStats();
				vlimiter::LogDebug("pacing: %.1f fps, frame %.2f ms (ema %.2f, max %.2f), held %.0f%% of frames for %.2f ms avg, %llu presents since start, render thread 0x%X, %s",
					st.fps, st.frameMs, st.frameEmaMs, st.maxFrameMs, st.waitShare * 100.0, st.waitMs, (unsigned long long)st.calls, st.renderThread,
					st.background ? "background" : "foreground");
			}
		}
	}

	void Startup()
	{
		const std::wstring dir = PluginDir();
		vlimiter::SetConfigPath(dir + L"\\vlimiter.ini");
		vlimiter::LogInit(dir + L"\\vlimiter.log");
		vlimiter::Log("VLimiter " VLIMITER_VERSION " starting (thread 0x%lX, game build %d)", GetCurrentThreadId(), vlimiter::GameBuild());

		if (!vlimiter::EnsureDefaultConfig(vlimiter::ConfigPath()))
			vlimiter::Log("could not write default config to %ls", vlimiter::ConfigPath().c_str());
		vlimiter::ReloadConfigFromDisk("startup");

		if (!vlimiter::present::Install())
			vlimiter::Log("inactive: no frame limiting this session (the menu still opens and says so)");

		HANDLE h = CreateThread(nullptr, 0, WorkerThread, nullptr, 0, nullptr);
		if (h)
			CloseHandle(h);
		else
			vlimiter::Log("could not start worker thread; hotkeys and menu disabled");
	}
}

extern "C" BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, LPVOID)
{
	if (reason == DLL_PROCESS_ATTACH)
	{
		g_module = instance;
		DisableThreadLibraryCalls(instance);
		Startup();
	}
	return TRUE;
}
