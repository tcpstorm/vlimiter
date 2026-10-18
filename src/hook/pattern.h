#pragma once
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace vlimiter
{
	struct Pattern
	{
		std::vector<uint8_t> bytes;
		std::vector<bool> mask; // true = byte must match, false = wildcard
	};

	// "25 00 0C ? ? 49 8B 06" style. Returns false on a malformed token.
	bool ParsePattern(const std::string& text, Pattern& out);

	// Base and SizeOfImage of the main executable module (the game image).
	bool GetMainModuleRange(uintptr_t& base, size_t& size);

	// Game build parsed from the main module name (FiveM_b3751_GTAProcess.exe),
	// 0 if it cannot be read.
	int GameBuild();


	// Scans committed, readable pages in [start, start+size). Stops after
	// maxResults hits so uniqueness can be checked cheaply.
	std::vector<uintptr_t> FindPattern(const Pattern& p, uintptr_t start, size_t size, size_t maxResults);
}
