// config.h - the ini settings and their file format.
#pragma once

#include <string>

namespace vlimiter
{
	struct Config
	{
		bool enabled = true;       // master switch; off = frames pass through untouched
		int fpsLimit = 60;         // frames per second while the game has focus; 0 = no limit
		int backgroundFps = 30;    // limit while another window has focus; 0 = same as fps_limit
		int spinMarginUs = 1000;   // last part of each wait done by spinning instead of sleeping
		int reloadKey = 0;         // re-read the ini; 0 disables
		int menuKey = 0x73;        // VK_F4 toggles the in-game menu; 0 disables
		int toggleKey = 0;         // enable/disable the limiter; 0 disables
		bool updateCheck = true;   // one GET of the latest GitHub release at startup; notice in the menu
		bool debug = false;        // a frame-pacing summary line in the log every few seconds
	};

	// Clamps every field to its valid range.
	void Sanitize(Config& cfg);

	// Full ini text (with comments) for the given values. DefaultIniText()
	// is RenderIni(Config{}).
	std::string RenderIni(const Config& cfg);
	std::string DefaultIniText();

	// Writes RenderIni(cfg) to path.
	bool SaveConfig(const std::wstring& path, const Config& cfg, std::string& error);

	// Parses key = value lines. Unknown keys and bad values are reported in
	// `report` (one line each) but never abort loading; missing keys keep
	// their defaults. Returns false only if the file could not be opened.
	bool LoadConfig(const std::wstring& path, Config& out, std::string& report);

	// Writes DefaultIniText() to path if the file does not exist.
	bool EnsureDefaultConfig(const std::wstring& path);
}
