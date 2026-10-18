// menu.cpp - the settings menu, as a separate always-on-top window.
//
// This deliberately does NOT hook the game's renderer. VLights, which this
// plugin is modelled on, once detoured IDXGISwapChain::Present and subclassed
// the game window to draw ImGui in the frame; FiveM's anti-cheat (adhesive)
// terminated the game about a minute after start, every time. So the plugin
// owns a small top-level window with its own D3D11 device and swapchain and
// renders Dear ImGui into that, exactly like ImGui's stock Win32 + DirectX 11
// example. It floats over the (borderless) game window, takes its own input,
// and touches nothing of the game's.
//
// Camera lock: the game reads the mouse through raw input registered for
// the whole process (with RIDEV_INPUTSINK it keeps receiving movement even
// when our window has focus, so the camera would spin while you drag a
// slider). While the menu is open the mouse is unregistered from raw input
// and the game's exact original registration is restored on close. That is
// an OS call on our own process, no game hook involved.
//
// Threads: MenuInit() (worker thread) spawns the UI thread, which owns the
// window, the device, the ImGui context and the message loop. MenuToggle()
// just posts a message to it.
#include "game/present.h"
#include "plugin/log.h"
#include "plugin/plugin.h"
#include "plugin/update.h"

#include <windows.h>
#include <shellapi.h>
#include <d3d11.h>
#include <dxgi.h>

#include "imgui.h"
#include "imgui_impl_dx11.h"
#include "imgui_impl_win32.h"

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <string>
#include <vector>

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

namespace
{
	// Avoid __uuidof so this builds identically under mingw and MSVC.
	const GUID kIID_ID3D11Texture2D = { 0x6f15aaf2, 0xd208, 0x4e89, { 0x9a, 0xb4, 0x48, 0x95, 0x35, 0xd3, 0x4f, 0x9c } };

	constexpr UINT WM_VLIMITER_TOGGLE = WM_APP + 1;
	constexpr int kWidth = 440;
	constexpr int kHeight = 600;
	constexpr DWORD kFrameMs = 33; // ~30 fps for the menu; it is a settings panel

	HWND g_hwnd = nullptr;
	std::atomic<bool> g_visible{ false };
	std::atomic<bool> g_ready{ false };
	std::atomic<bool> g_failed{ false };

	ID3D11Device* g_device = nullptr;
	ID3D11DeviceContext* g_context = nullptr;
	IDXGISwapChain* g_swapChain = nullptr;
	ID3D11RenderTargetView* g_rtv = nullptr;
	bool g_needResize = false;
	UINT g_resizeW = 0, g_resizeH = 0;

	// Menu-local copy of the config; pushed to the core on every change.
	vlimiter::Config g_menuCfg;
	bool g_menuCfgLoaded = false;
	std::string g_status;
	int g_refreshRate = 0; // of the monitor the game is on, read each time the menu opens

	// ------------------------------------------------------------ camera lock

	std::vector<RAWINPUTDEVICE> g_savedMouse;
	bool g_mouseSuspended = false;

	void SuspendGameMouse()
	{
		if (g_mouseSuspended)
			return;
		UINT n = 0;
		if (GetRegisteredRawInputDevices(nullptr, &n, sizeof(RAWINPUTDEVICE)) != 0 || n == 0)
		{
			vlimiter::Log("menu: no raw input devices registered in this process; camera lock not needed");
			return;
		}
		std::vector<RAWINPUTDEVICE> all(n);
		UINT got = GetRegisteredRawInputDevices(all.data(), &n, sizeof(RAWINPUTDEVICE));
		if (got == static_cast<UINT>(-1))
		{
			vlimiter::Log("menu: GetRegisteredRawInputDevices failed, error=%lu", GetLastError());
			return;
		}
		g_savedMouse.clear();
		for (UINT i = 0; i < got; ++i)
			if (all[i].usUsagePage == 0x01 && all[i].usUsage == 0x02) // generic desktop / mouse
				g_savedMouse.push_back(all[i]);
		if (g_savedMouse.empty())
		{
			vlimiter::Log("menu: the game has no raw mouse registration; camera lock not needed");
			return;
		}
		RAWINPUTDEVICE remove{};
		remove.usUsagePage = 0x01;
		remove.usUsage = 0x02;
		remove.dwFlags = RIDEV_REMOVE;
		remove.hwndTarget = nullptr;
		if (!RegisterRawInputDevices(&remove, 1, sizeof(remove)))
		{
			vlimiter::Log("menu: could not unregister raw mouse input, error=%lu", GetLastError());
			return;
		}
		g_mouseSuspended = true;
		vlimiter::Log("menu: camera locked (raw mouse input suspended; %u registration(s) saved, flags 0x%lX)",
			(unsigned)g_savedMouse.size(), (unsigned long)g_savedMouse[0].dwFlags);
	}

	void RestoreGameMouse()
	{
		if (!g_mouseSuspended)
			return;
		if (!RegisterRawInputDevices(g_savedMouse.data(), static_cast<UINT>(g_savedMouse.size()), sizeof(RAWINPUTDEVICE)))
			vlimiter::Log("menu: could not restore raw mouse input, error=%lu", GetLastError());
		else
			vlimiter::Log("menu: camera unlocked (raw mouse input restored)");
		g_mouseSuspended = false;
	}

	// ------------------------------------------------------------ D3D

	template <typename T>
	void SafeRelease(T*& p)
	{
		if (p)
		{
			p->Release();
			p = nullptr;
		}
	}

	bool CreateRenderTarget()
	{
		ID3D11Texture2D* back = nullptr;
		if (FAILED(g_swapChain->GetBuffer(0, kIID_ID3D11Texture2D, reinterpret_cast<void**>(&back))) || !back)
			return false;
		HRESULT hr = g_device->CreateRenderTargetView(back, nullptr, &g_rtv);
		back->Release();
		return SUCCEEDED(hr) && g_rtv != nullptr;
	}

	bool CreateDevice(HWND hwnd)
	{
		DXGI_SWAP_CHAIN_DESC sd{};
		sd.BufferCount = 2;
		sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
		sd.BufferDesc.RefreshRate.Numerator = 60;
		sd.BufferDesc.RefreshRate.Denominator = 1;
		sd.Flags = DXGI_SWAP_CHAIN_FLAG_ALLOW_MODE_SWITCH;
		sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
		sd.OutputWindow = hwnd;
		sd.SampleDesc.Count = 1;
		sd.Windowed = TRUE;
		sd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;

		D3D_FEATURE_LEVEL levels[] = { D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_0 };
		D3D_FEATURE_LEVEL got{};
		HRESULT hr = D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, levels, 2,
			D3D11_SDK_VERSION, &sd, &g_swapChain, &g_device, &got, &g_context);
		if (FAILED(hr))
		{
			hr = D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, levels, 2,
				D3D11_SDK_VERSION, &sd, &g_swapChain, &g_device, &got, &g_context);
		}
		if (FAILED(hr))
		{
			vlimiter::Log("menu: D3D11CreateDeviceAndSwapChain failed, hr=0x%08lX", (unsigned long)hr);
			return false;
		}
		return CreateRenderTarget();
	}

	void DestroyDevice()
	{
		SafeRelease(g_rtv);
		SafeRelease(g_swapChain);
		SafeRelease(g_context);
		SafeRelease(g_device);
	}

	// ------------------------------------------------------------ window

	// The game hides, clips and re-centres the OS cursor for its own window
	// every frame. Rather than fight that, the menu draws its own cursor
	// (ImGui software cursor), takes focus and pulls the pointer inside when
	// it opens, and releases any cursor clip while it is up.
	void FreeCursor()
	{
		ClipCursor(nullptr);
		while (ShowCursor(TRUE) < 0) {} // our thread's show count, in case it went negative
	}

	// Refresh rate of the monitor the game window sits on, for the "(monitor)"
	// preset. One EnumDisplaySettings per menu open; 0 if it cannot be read.
	void ReadRefreshRate(HWND game, HWND fallback)
	{
		g_refreshRate = 0;
		HMONITOR mon = MonitorFromWindow(game ? game : fallback, MONITOR_DEFAULTTOPRIMARY);
		MONITORINFOEXW mi{};
		mi.cbSize = sizeof(mi);
		DEVMODEW dm{};
		dm.dmSize = sizeof(dm);
		if (mon && GetMonitorInfoW(mon, &mi) && EnumDisplaySettingsW(mi.szDevice, ENUM_CURRENT_SETTINGS, &dm) && dm.dmDisplayFrequency > 1)
			g_refreshRate = static_cast<int>(dm.dmDisplayFrequency);
	}

	void ShowMenu(HWND hwnd)
	{
		HWND game = FindWindowW(L"grcWindow", nullptr);
		ReadRefreshRate(game, hwnd);
		RECT r{};
		if (game && GetWindowRect(game, &r))
			SetWindowPos(hwnd, HWND_TOPMOST, r.left + 40, r.top + 60, 0, 0, SWP_NOSIZE | SWP_NOACTIVATE);
		ShowWindow(hwnd, SW_SHOW);
		SetWindowPos(hwnd, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE);
		SetForegroundWindow(hwnd);
		SetActiveWindow(hwnd);
		SetFocus(hwnd);
		FreeCursor();
		RECT me{};
		if (GetWindowRect(hwnd, &me))
			SetCursorPos((me.left + me.right) / 2, (me.top + me.bottom) / 2);
		g_menuCfgLoaded = false;
		g_visible = true;
		SuspendGameMouse();
	}

	void HideMenu(HWND hwnd)
	{
		ShowWindow(hwnd, SW_HIDE);
		g_visible = false;
		RestoreGameMouse();
		if (HWND game = FindWindowW(L"grcWindow", nullptr))
			SetForegroundWindow(game); // give the game its input back
	}

	LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
	{
		if (ImGui_ImplWin32_WndProcHandler(hwnd, msg, wParam, lParam))
			return 1;

		switch (msg)
		{
		case WM_SIZE:
			if (wParam != SIZE_MINIMIZED)
			{
				g_needResize = true;
				g_resizeW = LOWORD(lParam);
				g_resizeH = HIWORD(lParam);
			}
			return 0;
		case WM_SYSCOMMAND:
			if ((wParam & 0xFFF0) == SC_KEYMENU) // no ALT menu
				return 0;
			break;
		case WM_SETCURSOR:
			if (LOWORD(lParam) == HTCLIENT)
			{
				SetCursor(nullptr); // ImGui draws the cursor over the client area
				return TRUE;
			}
			break;
		case WM_CLOSE:
			HideMenu(hwnd);
			return 0;
		case WM_VLIMITER_TOGGLE:
			if (g_visible.load())
				HideMenu(hwnd);
			else
				ShowMenu(hwnd);
			return 0;
		case WM_DESTROY:
			RestoreGameMouse();
			PostQuitMessage(0);
			return 0;
		default:
			break;
		}
		return DefWindowProcW(hwnd, msg, wParam, lParam);
	}

	// ------------------------------------------------------------ ImGui content

	void Push()
	{
		vlimiter::SetConfig(g_menuCfg);
		g_menuCfg = vlimiter::GetConfig(); // pick up clamped fields
	}

	void DrawMenu()
	{
		if (!g_menuCfgLoaded)
		{
			g_menuCfg = vlimiter::GetConfig();
			g_menuCfgLoaded = true;
		}
		vlimiter::Config& cfg = g_menuCfg;

		const ImGuiViewport* vp = ImGui::GetMainViewport();
		ImGui::SetNextWindowPos(vp->WorkPos);
		ImGui::SetNextWindowSize(vp->WorkSize);
		ImGuiWindowFlags flags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize
			| ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoBringToFrontOnFocus;

		if (ImGui::Begin("VLimiter", nullptr, flags))
		{
			ImGui::SeparatorText("VLimiter " VLIMITER_VERSION);
			{
				std::string latest, url;
				if (vlimiter::update::Available(latest, url))
				{
					ImGui::TextColored(ImVec4(1.f, 0.85f, 0.3f, 1.f), "Update available: %s (running " VLIMITER_VERSION ")", latest.c_str());
					ImGui::SameLine();
					if (ImGui::SmallButton("Open releases page"))
						ShellExecuteA(nullptr, "open", url.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
				}
			}

			const vlimiter::present::Stats st = vlimiter::present::GetStats();
			if (!st.installed)
				ImGui::TextColored(ImVec4(1.f, 0.4f, 0.4f, 1.f), "Hook not installed: nothing is being limited (see vlimiter.log).");

			bool changed = false;
			changed |= ImGui::Checkbox("Limit frame rate", &cfg.enabled);

			ImGui::SeparatorText("Limit");
			// Slider for the range people use (logarithmic, so 30-60-120-240 are
			// spaced out), a number field beside it for exact values up to 1000.
			// The slider works on a copy: a typed 1000 is not pulled back to 360
			// just by being drawn.
			{
				int slider = std::clamp(cfg.fpsLimit, 10, 360);
				ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - 130.f);
				if (ImGui::SliderInt("##fps_slider", &slider, 10, 360, "%d fps", ImGuiSliderFlags_Logarithmic | ImGuiSliderFlags_AlwaysClamp))
				{
					cfg.fpsLimit = slider;
					changed = true;
				}
				ImGui::SameLine();
				ImGui::SetNextItemWidth(70.f);
				int typed = cfg.fpsLimit;
				if (ImGui::InputInt("##fps_input", &typed, 0, 0, ImGuiInputTextFlags_EnterReturnsTrue))
				{
					cfg.fpsLimit = std::clamp(typed, 1, 1000);
					changed = true;
				}
				ImGui::SameLine();
				ImGui::TextDisabled("fps");
				if (cfg.fpsLimit == 0)
					ImGui::TextColored(ImVec4(1.f, 0.85f, 0.3f, 1.f), "fps_limit is 0 in the ini (no limit). Pick a value to cap.");
			}
			ImGui::TextDisabled("Presets:");
			const int presets[] = { 30, 60, 72, 90, 120, 144, 165, 240 };
			for (int p : presets)
			{
				ImGui::SameLine();
				char label[16];
				snprintf(label, sizeof(label), "%d", p);
				if (ImGui::SmallButton(label))
				{
					cfg.fpsLimit = p;
					changed = true;
				}
			}
			if (g_refreshRate > 0)
			{
				ImGui::SameLine();
				char label[32];
				snprintf(label, sizeof(label), "%d (monitor)", g_refreshRate);
				if (ImGui::SmallButton(label))
				{
					cfg.fpsLimit = g_refreshRate;
					changed = true;
				}
			}
			ImGui::TextWrapped("Frames per second while the game has focus. The game's VSync setting still applies on top.");

			bool separateBackground = cfg.backgroundFps > 0;
			if (ImGui::Checkbox("Lower limit while another window has focus", &separateBackground))
			{
				cfg.backgroundFps = separateBackground ? 30 : 0;
				changed = true;
			}
			if (separateBackground)
			{
				int slider = std::clamp(cfg.backgroundFps, 10, 240);
				ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - 130.f);
				if (ImGui::SliderInt("##bg_slider", &slider, 10, 240, "%d fps", ImGuiSliderFlags_Logarithmic | ImGuiSliderFlags_AlwaysClamp))
				{
					cfg.backgroundFps = slider;
					changed = true;
				}
				ImGui::SameLine();
				ImGui::SetNextItemWidth(70.f);
				int typed = cfg.backgroundFps;
				if (ImGui::InputInt("##bg_input", &typed, 0, 0, ImGuiInputTextFlags_EnterReturnsTrue))
				{
					cfg.backgroundFps = std::clamp(typed, 1, 1000);
					changed = true;
				}
				ImGui::SameLine();
				ImGui::TextDisabled("fps");
			}
			ImGui::TextWrapped("Lets a second monitor or an alt-tab cool the GPU down. Off = the same limit everywhere.");

			if (ImGui::CollapsingHeader("Precision"))
			{
				changed |= ImGui::SliderInt("Spin margin", &cfg.spinMarginUs, 0, 5000, "%d us", ImGuiSliderFlags_AlwaysClamp);
				ImGui::TextWrapped("Each wait sleeps on a timer and then spins for this long so the frame is released on the tick. "
					"More = steadier frame times at a little CPU on one core; 0 = sleep only.");
				ImGui::TextDisabled(st.timerHighRes ? "High-resolution timer in use." : "Standard timer in use (high-resolution unavailable).");
			}

			if (changed)
				Push();

			ImGui::SeparatorText("Status");
			if (st.installed)
			{
				ImGui::Text("Measured: %.1f fps, %.2f ms per frame (worst %.2f ms)", st.fps, st.frameMs, st.maxFrameMs);
				if (cfg.enabled)
					ImGui::Text("Limiter held %.0f%% of frames, %.2f ms each on average", st.waitShare * 100.0, st.waitMs);
				else
					ImGui::TextDisabled("Limiter off: frames pass straight through.");
				ImGui::TextDisabled("%s. %llu frames since start on render thread 0x%X.",
					st.background ? "Background limit in force" : "Game has focus", (unsigned long long)st.calls, st.renderThread);
			}
			ImGui::TextDisabled(g_mouseSuspended ? "Camera locked while this window is open." : "Camera not locked (no raw mouse registration found).");
			ImGui::TextDisabled("%s", vlimiter::update::Status());

			ImGui::Separator();
			if (ImGui::Button("Save to ini"))
			{
				std::string err;
				g_status = vlimiter::SaveConfigToDisk(err) ? "Saved." : ("Save failed: " + err);
			}
			ImGui::SameLine();
			if (ImGui::Button("Reload ini"))
			{
				if (vlimiter::ReloadConfigFromDisk("menu"))
				{
					g_menuCfgLoaded = false;
					g_status = "Reloaded from ini.";
				}
				else
				{
					g_status = "Reload failed (see log).";
				}
			}
			ImGui::SameLine();
			if (ImGui::Button("Close"))
				PostMessageW(g_hwnd, WM_VLIMITER_TOGGLE, 0, 0);

			if (!g_status.empty())
				ImGui::TextWrapped("%s", g_status.c_str());

			ImGui::TextDisabled("Changes apply to the next frame. Press the menu key again, or Close, to hide this window.");
		}
		ImGui::End();
	}

	void RenderFrame()
	{
		if (g_needResize && g_swapChain)
		{
			SafeRelease(g_rtv);
			g_swapChain->ResizeBuffers(0, g_resizeW, g_resizeH, DXGI_FORMAT_UNKNOWN, 0);
			CreateRenderTarget();
			g_needResize = false;
		}
		if (!g_rtv)
			return;

		ClipCursor(nullptr); // the game re-clips the cursor to its window; keep it free while we are up
		ImGui::GetIO().MouseDrawCursor = true;
		ImGui_ImplDX11_NewFrame();
		ImGui_ImplWin32_NewFrame();
		ImGui::NewFrame();
		DrawMenu();
		ImGui::Render();

		const float clear[4] = { 0.09f, 0.09f, 0.11f, 1.0f };
		g_context->OMSetRenderTargets(1, &g_rtv, nullptr);
		g_context->ClearRenderTargetView(g_rtv, clear);
		ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
		g_swapChain->Present(0, 0); // no vsync wait: never contend with the game's own swapchain
	}

	DWORD WINAPI UiThread(LPVOID)
	{
		WNDCLASSEXW wc{};
		wc.cbSize = sizeof(wc);
		wc.style = CS_CLASSDC;
		wc.lpfnWndProc = WndProc;
		wc.hInstance = GetModuleHandleW(nullptr);
		wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
		wc.lpszClassName = L"VLimiterMenu";
		if (!RegisterClassExW(&wc))
		{
			vlimiter::Log("menu: RegisterClassEx failed, error=%lu", GetLastError());
			g_failed = true;
			return 0;
		}

		g_hwnd = CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW, wc.lpszClassName, L"VLimiter",
			WS_POPUP | WS_CAPTION | WS_SYSMENU | WS_THICKFRAME, 100, 100, kWidth, kHeight,
			nullptr, nullptr, wc.hInstance, nullptr);
		if (!g_hwnd)
		{
			vlimiter::Log("menu: CreateWindowEx failed, error=%lu", GetLastError());
			g_failed = true;
			return 0;
		}

		if (!CreateDevice(g_hwnd))
		{
			DestroyDevice();
			DestroyWindow(g_hwnd);
			g_hwnd = nullptr;
			g_failed = true;
			return 0;
		}

		IMGUI_CHECKVERSION();
		ImGui::CreateContext();
		ImGuiIO& io = ImGui::GetIO();
		io.IniFilename = nullptr;
		io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
		io.ConfigFlags |= ImGuiConfigFlags_NoMouseCursorChange; // we handle WM_SETCURSOR ourselves
		io.MouseDrawCursor = true;                              // always a visible cursor inside the menu
		ImGui::StyleColorsDark();
		ImGui::GetStyle().WindowRounding = 0.f;
		ImGui::GetStyle().FrameRounding = 4.f;

		if (!ImGui_ImplWin32_Init(g_hwnd) || !ImGui_ImplDX11_Init(g_device, g_context))
		{
			vlimiter::Log("menu: ImGui backend init failed");
			g_failed = true;
			return 0;
		}

		g_ready = true;
		vlimiter::Log("menu: window ready (own D3D11 device, no game hooks)");

		MSG msg{};
		for (;;)
		{
			bool quit = false;
			while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE))
			{
				if (msg.message == WM_QUIT)
					quit = true;
				TranslateMessage(&msg);
				DispatchMessageW(&msg);
			}
			if (quit)
				break;

			if (g_visible.load())
			{
				RenderFrame();
				Sleep(kFrameMs);
			}
			else
			{
				WaitMessage(); // idle until toggled
			}
		}

		ImGui_ImplDX11_Shutdown();
		ImGui_ImplWin32_Shutdown();
		ImGui::DestroyContext();
		DestroyDevice();
		return 0;
	}
}

namespace vlimiter
{
	bool MenuInit()
	{
		HANDLE h = CreateThread(nullptr, 0, UiThread, nullptr, 0, nullptr);
		if (!h)
		{
			Log("menu: could not start UI thread, error=%lu", GetLastError());
			return false;
		}
		CloseHandle(h);
		return true;
	}

	void MenuToggle()
	{
		if (!g_ready.load() || g_failed.load() || !g_hwnd)
			return;
		PostMessageW(g_hwnd, WM_VLIMITER_TOGGLE, 0, 0);
	}

	bool MenuVisible()
	{
		return g_visible.load();
	}
}
