#pragma once
#include "pch.h"
#include <vector>
#include <string>
#include <sstream>
#include <iomanip>

// Scans the .text section of a loaded module for a byte pattern.
// Pattern format: "F3 0F 10 05 ?? ?? ?? ??" where ?? is a wildcard.
// Mask format:    "xxxx????"  where x = match, ? = skip

namespace Scanner
{
    inline uintptr_t FindPattern(uintptr_t moduleBase,
                                  const char* patternStr,
                                  const char* mask)
    {
        // Get module size from PE header
        auto* dos  = reinterpret_cast<IMAGE_DOS_HEADER*>(moduleBase);
        auto* nt   = reinterpret_cast<IMAGE_NT_HEADERS*>(moduleBase + dos->e_lfanew);
        size_t size = nt->OptionalHeader.SizeOfImage;

        // Parse hex pattern string into byte array
        std::vector<BYTE> pattern;
        {
            std::istringstream ss(patternStr);
            std::string token;
            while (ss >> token) {
                if (token == "??") pattern.push_back(0x00);
                else pattern.push_back(static_cast<BYTE>(std::stoul(token, nullptr, 16)));
            }
        }

        size_t patLen = pattern.size();
        size_t maskLen = strlen(mask);
        if (patLen == 0 || maskLen == 0) return 0;

        const BYTE* data = reinterpret_cast<const BYTE*>(moduleBase);

        for (size_t i = 0; i + patLen <= size; ++i) {
            bool found = true;
            for (size_t j = 0; j < patLen && j < maskLen; ++j) {
                if (mask[j] == 'x' && data[i + j] != pattern[j]) {
                    found = false;
                    break;
                }
            }
            if (found) return moduleBase + i;
        }
        return 0;
    }
}
