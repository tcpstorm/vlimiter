// plugin.cpp - config state shared by the hook, the menu and the hotkeys.
#include "plugin/plugin.h"
#include "game/present.h"
#include "plugin/log.h"

#include <windows.h>

extern "C" int mh_vlimiter_freeze_method; // from the patched MinHook

namespace
{
	SRWLOCK g_configLock = SRWLOCK_INIT;
	vlimiter::Config g_config;
	std::wstring g_configPath;

	void LogConfig(const vlimiter::Config& cfg, const char* why)
	{
		vlimiter::Log("config (%s): enabled=%d fps_limit=%d background_fps=%d spin_margin_us=%d menu_key=0x%X toggle_key=0x%X reload_key=0x%X update_check=%d debug=%d",
			why, cfg.enabled, cfg.fpsLimit, cfg.backgroundFps, cfg.spinMarginUs, cfg.menuKey, cfg.toggleKey, cfg.reloadKey, cfg.updateCheck, cfg.debug);
	}
}

namespace vlimiter
{
	void SetConfigPath(const std::wstring& path)
	{
		g_configPath = path;
	}

	const std::wstring& ConfigPath()
	{
		return g_configPath;
	}

	Config GetConfig()
	{
		AcquireSRWLockShared(&g_configLock);
		Config c = g_config;
		ReleaseSRWLockShared(&g_configLock);
		return c;
	}

	void SetConfig(const Config& in)
	{
		Config cfg = in;
		Sanitize(cfg);
		SetDebugLogging(cfg.debug);
		AcquireSRWLockExclusive(&g_configLock);
		g_config = cfg;
		ReleaseSRWLockExclusive(&g_configLock);
		present::Apply(cfg); // the render thread reads atomics, never this struct
	}

	bool ReloadConfigFromDisk(const char* why)
	{
		Config cfg;
		std::string report;
		if (!LoadConfig(g_configPath, cfg, report))
		{
			Log("config: could not open %ls (%s), keeping current values", g_configPath.c_str(), why);
			return false;
		}
		SetConfig(cfg);
		LogConfig(GetConfig(), why);
		if (!report.empty())
			Log("config warnings:\n%s", report.c_str());
		return true;
	}

	bool SaveConfigToDisk(std::string& error)
	{
		bool ok = SaveConfig(g_configPath, GetConfig(), error);
		Log(ok ? "config saved to %ls" : "config save to %ls failed", g_configPath.c_str());
		return ok;
	}

	void ToggleEnabled(const char* why)
	{
		Config cfg = GetConfig();
		cfg.enabled = !cfg.enabled;
		SetConfig(cfg);
		Log("limiter %s (%s)", cfg.enabled ? "enabled" : "disabled", why);
	}

	const char* LastFreezeMethod()
	{
		switch (mh_vlimiter_freeze_method)
		{
		case 1: return "Toolhelp";
		case 2: return "NtGetNextThread";
		case 0: return "none (not suspended)";
		default: return "unknown";
		}
	}
}
