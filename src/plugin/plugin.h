// plugin.h - the plugin core's interface: config state and the menu entry
// points. Implemented in plugin.cpp (core) and ui/menu.cpp (menu).
#pragma once

#include "plugin/config.h"
#include <vlimiter/version.h>

#include <cstdint>
#include <string>

namespace vlimiter
{
	// --- plugin.cpp ---
	void SetConfigPath(const std::wstring& path);
	const std::wstring& ConfigPath();
	Config GetConfig();                        // snapshot by value
	void SetConfig(const Config& cfg);         // sanitise, store, push to the limiter
	bool ReloadConfigFromDisk(const char* why);
	bool SaveConfigToDisk(std::string& error);
	void ToggleEnabled(const char* why);       // flip `enabled`, keep everything else
	const char* LastFreezeMethod();            // how MinHook froze threads on the last enable

	// --- ui/menu.cpp ---
	// Starts the menu's own window + UI thread (no game hooks). Call once from
	// a worker thread, never from DllMain.
	bool MenuInit();
	void MenuToggle();
	bool MenuVisible();
}
