#pragma once
#include "pch.h"
#include <string>
#include <fstream>
#include <sstream>

class Config
{
public:
    float cameraDistance = 8.0f;
    float step = 0.5f;
    float minDistance = 1.0f;
    float maxDistance = 30.0f;
    float smoothFactor = 0.08f;
    int   invertScroll = 0;
    int   debugLog     = 0;

    void Load()
    {
        std::string path = GetConfigPath();
        std::ifstream f(path);
        if (!f.good()) { f.close(); Save(); return; }

        std::string line;
        while (std::getline(f, line)) {
            auto ci = line.find(';');
            if (ci != std::string::npos) line = line.substr(0, ci);
            auto eq = line.find('=');
            if (eq == std::string::npos) continue;
            std::string key = Trim(line.substr(0, eq));
            std::string value = Trim(line.substr(eq + 1));
            if      (key == "camera_distance") TryParseFloat(value, cameraDistance);
            else if (key == "step")            TryParseFloat(value, step);
            else if (key == "min")             TryParseFloat(value, minDistance);
            else if (key == "max")             TryParseFloat(value, maxDistance);
            else if (key == "smooth_factor")   TryParseFloat(value, smoothFactor);
            else if (key == "invert_scroll")   TryParseInt(value, invertScroll);
            else if (key == "debug_log")       TryParseInt(value, debugLog);
        }
        Clamp(cameraDistance, 1.0f, 200.0f);
        Clamp(step, 0.1f, 10.0f);
        Clamp(minDistance, 1.0f, 10.0f);
        Clamp(maxDistance, 5.0f, 200.0f);
        Clamp(smoothFactor, 0.01f, 1.0f);
        if (invertScroll != 0) invertScroll = 1;
        if (debugLog != 0)     debugLog = 1;
    }

    void Save()
    {
        std::string path = GetConfigPath();
        auto slash = path.find_last_of("\\/");
        if (slash != std::string::npos)
            CreateDirectoryA(path.substr(0, slash).c_str(), nullptr);

        std::ofstream f(path);
        f << "; Elden Ring Camera Zoom Mod\n"
            << ";\n"
            << "; camera_distance  - zoom level saved on quit\n"
            << "; step             - distance change per scroll click\n"
            << "; min / max        - clamp range\n"
            << "; smooth_factor    - easing speed (0.01=very slow, 1.0=instant)\n"
            << "; invert_scroll    - 0 = scroll up zooms in, 1 = scroll up zooms out\n"
            << "; debug_log        - 0 = no log (default), 1 = write ERCameraZoom.log\n"
            << ";\n"
            << "camera_distance = " << cameraDistance << "\n"
            << "step            = " << step << "\n"
            << "min             = " << minDistance << "\n"
            << "max             = " << maxDistance << "\n"
            << "smooth_factor   = " << smoothFactor << "\n"
            << "invert_scroll   = " << invertScroll << "\n"
            << "debug_log       = " << debugLog << "\n";
    }

    std::string GetConfigPath()
    {
        char dllPath[MAX_PATH] = {};
        HMODULE hm = nullptr;
        GetModuleHandleExA(
            GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
            GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
            reinterpret_cast<LPCSTR>(&Config::GetDllPath), &hm);
        GetModuleFileNameA(hm, dllPath, MAX_PATH);
        std::string p(dllPath);
        auto slash = p.find_last_of("\\/");
        if (slash != std::string::npos) p = p.substr(0, slash + 1);
        return p + "config.ini";
    }

private:
    static void GetDllPath() {}

    static void TryParseFloat(const std::string& s, float& out) {
        try { out = std::stof(s); }
        catch (...) {}
    }
    static void TryParseInt(const std::string& s, int& out) {
        try { out = std::stoi(s); }
        catch (...) {}
    }
    static void Clamp(float& v, float lo, float hi) {
        if (v < lo) v = lo; if (v > hi) v = hi;
    }
    static std::string Trim(const std::string& s) {
        size_t a = s.find_first_not_of(" \t\r\n");
        size_t b = s.find_last_not_of(" \t\r\n");
        return a == std::string::npos ? "" : s.substr(a, b - a + 1);
    }
};
