#include "plugin/config.h"

#include <windows.h>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <fstream>

namespace vlimiter
{
	static std::string KeyText(int vk)
	{
		if (vk >= VK_F1 && vk <= VK_F24)
			return "F" + std::to_string(vk - VK_F1 + 1);
		char buf[16];
		snprintf(buf, sizeof(buf), "0x%X", vk);
		return buf;
	}

	static std::string Bool(bool b)
	{
		return b ? "1" : "0";
	}

	void Sanitize(Config& cfg)
	{
		cfg.fpsLimit = std::clamp(cfg.fpsLimit, 0, 1000);
		cfg.backgroundFps = std::clamp(cfg.backgroundFps, 0, 1000);
		cfg.spinMarginUs = std::clamp(cfg.spinMarginUs, 0, 20000);
		cfg.reloadKey = std::max(cfg.reloadKey, 0);
		cfg.menuKey = std::max(cfg.menuKey, 0);
		cfg.toggleKey = std::max(cfg.toggleKey, 0);
	}

	std::string RenderIni(const Config& cfg)
	{
		std::string s;
		s += "# VLimiter - config\n";
		s += "# Lives next to VLimiter.asi. Use the in-game menu (menu key) and Save,\n";
		s += "# or edit this file and press the reload key / restart the game.\n";
		s += "\n";
		s += "# enabled: master switch. 0 = frames pass through untouched.\n";
		s += "enabled = " + Bool(cfg.enabled) + "\n";
		s += "\n";
		s += "# fps_limit: frames per second while the game window has focus. 0 = no\n";
		s += "# limit. The game's own VSync setting still applies on top: with VSync on\n";
		s += "# you get whichever of the two is lower.\n";
		s += "fps_limit = " + std::to_string(cfg.fpsLimit) + "\n";
		s += "# background_fps: limit while another window has focus (alt-tabbed, on a\n";
		s += "# second monitor). 0 = same as fps_limit. FiveM itself already holds a\n";
		s += "# minimised game to 30.\n";
		s += "background_fps = " + std::to_string(cfg.backgroundFps) + "\n";
		s += "\n";
		s += "# spin_margin_us: each wait is a timer sleep followed by a short spin for\n";
		s += "# the last spin_margin_us microseconds, which is what makes the cap\n";
		s += "# precise. Larger = steadier frame times, slightly more CPU on one core;\n";
		s += "# 0 = sleep only (lowest CPU, frame times wobble by up to a millisecond).\n";
		s += "spin_margin_us = " + std::to_string(cfg.spinMarginUs) + "\n";
		s += "\n";
		s += "# update_check: once at startup, ask api.github.com for the latest release\n";
		s += "# and show a notice in the menu if it is newer. Nothing is downloaded or\n";
		s += "# installed, nothing else is sent. 0 = never contact anything.\n";
		s += "update_check = " + Bool(cfg.updateCheck) + "\n";
		s += "\n";
		s += "# Hotkeys: F1-F24, or a hex/decimal virtual-key code. 0 disables.\n";
		s += "# menu_key toggles the in-game menu. toggle_key switches the limiter on and\n";
		s += "# off. reload_key re-reads this file. F8 is FiveM's console and F1-F3 the\n";
		s += "# Rockstar Editor, so avoid those.\n";
		s += "menu_key = " + KeyText(cfg.menuKey) + "\n";
		s += "toggle_key = " + KeyText(cfg.toggleKey) + "\n";
		s += "reload_key = " + KeyText(cfg.reloadKey) + "\n";
		s += "\n";
		s += "# debug: write a frame-pacing summary (fps, frame time, wait time, hook\n";
		s += "# calls) to vlimiter.log every few seconds. Leave at 0 unless investigating.\n";
		s += "debug = " + Bool(cfg.debug) + "\n";
		return s;
	}

	std::string DefaultIniText()
	{
		return RenderIni(Config{});
	}

	bool SaveConfig(const std::wstring& path, const Config& cfg, std::string& error)
	{
		FILE* f = _wfopen(path.c_str(), L"w");
		if (!f)
		{
			error = "could not open config file for writing";
			return false;
		}
		std::string text = RenderIni(cfg);
		bool ok = fputs(text.c_str(), f) >= 0;
		fclose(f);
		if (!ok)
			error = "write failed";
		return ok;
	}

	static std::string Trim(const std::string& s)
	{
		size_t a = 0, b = s.size();
		while (a < b && std::isspace(static_cast<unsigned char>(s[a])))
			++a;
		while (b > a && std::isspace(static_cast<unsigned char>(s[b - 1])))
			--b;
		return s.substr(a, b - a);
	}

	static std::string Lower(std::string s)
	{
		for (auto& c : s)
			c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
		return s;
	}

	static bool ParseBool(const std::string& v, bool& out)
	{
		std::string s = Lower(v);
		if (s == "1" || s == "true" || s == "yes" || s == "on") { out = true; return true; }
		if (s == "0" || s == "false" || s == "no" || s == "off") { out = false; return true; }
		return false;
	}

	static bool ParseInt(const std::string& v, int& out)
	{
		char* end = nullptr;
		long i = std::strtol(v.c_str(), &end, 0);
		if (!end || end == v.c_str() || *end != '\0')
			return false;
		out = static_cast<int>(i);
		return true;
	}

	static bool ParseKey(const std::string& v, int& out)
	{
		std::string s = Lower(Trim(v));
		if (s.size() >= 2 && s[0] == 'f')
		{
			int n = 0;
			if (ParseInt(s.substr(1), n) && n >= 1 && n <= 24)
			{
				out = VK_F1 + (n - 1);
				return true;
			}
			return false;
		}
		return ParseInt(s, out);
	}

	bool LoadConfig(const std::wstring& path, Config& out, std::string& report)
	{
		std::ifstream in(path.c_str());
		if (!in)
			return false;

		Config cfg; // start from defaults so missing keys are well-defined
		std::string line;
		int lineNo = 0;
		while (std::getline(in, line))
		{
			++lineNo;
			std::string t = Trim(line);
			if (t.empty() || t[0] == '#' || t[0] == ';')
				continue;
			size_t eq = t.find('=');
			if (eq == std::string::npos)
			{
				report += "line " + std::to_string(lineNo) + ": expected key = value\n";
				continue;
			}
			std::string key = Lower(Trim(t.substr(0, eq)));
			std::string val = Trim(t.substr(eq + 1));

			bool ok = true;
			if (key == "enabled")             ok = ParseBool(val, cfg.enabled);
			else if (key == "fps_limit")      ok = ParseInt(val, cfg.fpsLimit);
			else if (key == "background_fps") ok = ParseInt(val, cfg.backgroundFps);
			else if (key == "spin_margin_us") ok = ParseInt(val, cfg.spinMarginUs);
			else if (key == "update_check")   ok = ParseBool(val, cfg.updateCheck);
			else if (key == "menu_key")       ok = ParseKey(val, cfg.menuKey);
			else if (key == "toggle_key")     ok = ParseKey(val, cfg.toggleKey);
			else if (key == "reload_key")     ok = ParseKey(val, cfg.reloadKey);
			else if (key == "debug")          ok = ParseBool(val, cfg.debug);
			else
			{
				report += "line " + std::to_string(lineNo) + ": unknown key '" + key + "'\n";
				continue;
			}
			if (!ok)
				report += "line " + std::to_string(lineNo) + ": bad value for '" + key + "': '" + val + "'\n";
		}

		Sanitize(cfg);
		out = cfg;
		return true;
	}

	bool EnsureDefaultConfig(const std::wstring& path)
	{
		if (GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES)
			return true;
		FILE* f = _wfopen(path.c_str(), L"w");
		if (!f)
			return false;
		fputs(DefaultIniText().c_str(), f);
		fclose(f);
		return true;
	}
}
