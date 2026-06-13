/*
 * Elden Ring Camera Zoom Mod
 * Controls: Alt + ScrollUp = zoom in, Alt + ScrollDown = zoom out
 *           (configurable via invert_scroll in config.ini)
 *
 * How it works:
 *   LockCamParam (166 rows) — handles on-foot and all normal riding camera.
 *   Code cave hook on movss xmm2,[rbx+160h] — fires every frame, captures
 *   rbx = live camera struct, writes our distance to +0x1B4/+0x1B8 each tick.
 *   This is the only thing that overrides the Torrent dash/gallop camera.
 *
 * Easing is computed in C++ (sustain loop, ~16ms ticks) and the result is
 * written into the cave's distance slot.  The cave itself does NO arithmetic —
 * it just copies that pre-computed value to the camera struct.  This avoids
 * clobbering XMM1 (which v3's in-cave lerp did), which caused camera instability
 * at close zoom distances.
 *
 * Params investigated and ruled out:
 *   RideParam           — all rows contain zeros; never contributed anything.
 *   DirectionCameraParam— only boolean flags (0x1 values); not a distance table.
 *   WorldChrMan chain   — pointer chain to cam struct was unreliable across
 *                         loading screens; replaced by the always-current hook.
 */

#include "pch.h"
#include "Config.h"
#include "Scanner.h"
#include <cstdio>
#include <cstdarg>
#include <cmath>
#include <string>
#include <thread>
#include <atomic>
#include <cstring>

// ─────────────────────────────────────────────────────────────────────────────
//  Logging — disabled by default, enable via config.ini: debug_log = 1
// ─────────────────────────────────────────────────────────────────────────────
static FILE* g_logFile = nullptr;
static bool g_logging = false;

static void Log(const char* fmt, ...)
{
    if (!g_logging) return;
    char buf[512];
    va_list args;
    va_start(args, fmt);
    vsnprintf_s(buf, sizeof(buf), _TRUNCATE, fmt, args);
    va_end(args);
    OutputDebugStringA(buf);
    if (g_logFile) { fputs(buf, g_logFile); fflush(g_logFile); }
}
static void OpenLog(const std::string& dir)
{
    if (!g_logging) return;
    fopen_s(&g_logFile, (dir + "ERCameraZoom.log").c_str(), "w");
}

// ─────────────────────────────────────────────────────────────────────────────
//  AOB patterns
// ─────────────────────────────────────────────────────────────────────────────
// CSRegulationManagerImp pointer
static const char* REG_PATTERN = "48 8B 0D ?? ?? ?? ?? 48 85 C9 74 0B 4C 8B C0 48 8B D7";
static const char* REG_MASK    = "xxx????xxxxxxxxxxx";

// Camera update function: movss xmm2,[rbx+160h] — rbx = live camera struct.
// Same AOB Hexington uses for aobGetFollowCam; fires every frame.
static const char* CAM_PATTERN = "F3 0F 10 93 60 01 00 00";
static const char* CAM_MASK    = "xxxxxxxx";

// ─────────────────────────────────────────────────────────────────────────────
//  Globals
// ─────────────────────────────────────────────────────────────────────────────
static uintptr_t          g_lockCamBase    = 0;
static uint32_t           g_lockCamRows    = 0;

static std::atomic<float> g_distance       { 8.0f };  // target (set by user input)
static float              g_smoothedDist   = 8.0f;    // current eased value (C++ only)
static float              g_minDist        = 1.0f;
static float              g_maxDist        = 1000.0f;
static float              g_step           = 1.0f;
static float              g_smoothFactor   = 0.08f;
static bool               g_invertScroll   = false;
static std::atomic<bool>  g_running        { true };
static HHOOK              g_mouseHook      = nullptr;
static HANDLE             g_hookThread     = nullptr;
static DWORD              g_hookTid        = 0;
static uintptr_t          g_modBase        = 0;
static Config             g_cfg;

// ─────────────────────────────────────────────────────────────────────────────
//  Code cave hook globals
//
//  Cave layout (72 bytes) — identical to v2, no arithmetic in shellcode:
//    +00  48 89 1D 39 00 00 00    MOV  [RIP+0x39], RBX     — save rbx → cave+0x40
//    +07  F3 0F 10 05 21 00 00 00 MOVSS XMM0,[RIP+0x21]    — load dist from cave+0x30
//    +0F  F3 0F 11 83 B4 01 00 00 MOVSS [RBX+0x1B4], XMM0  — force distance
//    +17  F3 0F 11 83 B8 01 00 00 MOVSS [RBX+0x1B8], XMM0  — force distance
//    +1F  F3 0F 10 93 60 01 00 00 MOVSS XMM2,[RBX+0x160]   — stolen instruction
//    +27  FF 25 0B 00 00 00       JMP  [RIP+0x0B]           — jump back (addr at cave+0x38)
//    +2D  90 90 90                NOP padding
//    +30  (4 bytes)  distance slot — C++ writes g_smoothedDist here each tick
//    +34  (4 bytes)  padding
//    +38  (8 bytes)  back address = hookAddr + 8
//    +40  (8 bytes)  cam struct slot (rbx written here by cave each frame)
//
//  Only XMM0 and XMM2 are touched; XMM2 is restored by the stolen instruction.
//  XMM1 is NOT touched (this was the v3 regression that caused close-range glitch).
// ─────────────────────────────────────────────────────────────────────────────
static uint8_t*  g_cave        = nullptr;
static uintptr_t g_camHookAddr = 0;
static uint8_t   g_origBytes[8]= {};

static constexpr size_t CAVE_DIST_SLOT = 0x30;
static constexpr size_t CAVE_BACK_SLOT = 0x38;
static constexpr size_t CAVE_RBX_SLOT  = 0x40;

// ─────────────────────────────────────────────────────────────────────────────
//  Safe memory helpers
// ─────────────────────────────────────────────────────────────────────────────
static bool SafeReadPtr(uintptr_t addr, uintptr_t& out)
{
    __try { out = *reinterpret_cast<uintptr_t*>(addr); return true; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
static bool SafeReadInt32(uintptr_t addr, int32_t& out)
{
    __try { out = *reinterpret_cast<int32_t*>(addr); return true; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
static bool SafeReadU16(uintptr_t addr, uint16_t& out)
{
    __try { out = *reinterpret_cast<uint16_t*>(addr); return true; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
static bool SafeWriteFloat(float* ptr, float val)
{
    __try { *ptr = val; return true; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

// ─────────────────────────────────────────────────────────────────────────────
//  Param name reader
// ─────────────────────────────────────────────────────────────────────────────
static bool ReadParamName(uintptr_t hdr, char* out, int len)
{
    __try {
        if (!*reinterpret_cast<uint32_t*>(hdr + 0x28)) return false;
        uintptr_t ptr = *reinterpret_cast<uintptr_t*>(hdr + 0x18);
        if (!ptr) return false;
        int n = 0;
        for (int i = 0; i < len - 1; ++i) {
            wchar_t wc = *reinterpret_cast<wchar_t*>(ptr + i * 2);
            if (!wc) break;
            out[n++] = (char)(wc & 0xFF);
        }
        out[n] = 0;
        return n > 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

// ─────────────────────────────────────────────────────────────────────────────
//  Find param tableBase by name
// ─────────────────────────────────────────────────────────────────────────────
static bool FindParamTableBase(uintptr_t regMgr, const char* name, uintptr_t& baseOut)
{
    uintptr_t ls = 0, le = 0;
    if (!SafeReadPtr(regMgr + 0x18, ls) || !ls) return false;
    if (!SafeReadPtr(regMgr + 0x20, le) || !le) return false;
    uintptr_t n = (le - ls) / 8;
    if (!n || n > 2000) return false;

    for (uintptr_t i = 0; i < n; ++i) {
        uintptr_t hdr = 0;
        if (!SafeReadPtr(ls + i * 8, hdr) || !hdr) continue;
        char buf[128] = {};
        if (!ReadParamName(hdr, buf, sizeof(buf))) continue;
        if (strcmp(buf, name) != 0) continue;

        uintptr_t p1 = 0, tb = 0;
        if (!SafeReadPtr(hdr + 0x80, p1) || !p1) return false;
        if (!SafeReadPtr(p1  + 0x80, tb)  || !tb)  return false;
        Log("[ERCam] %s idx=%llu tableBase=0x%llX\n",
            name, (unsigned long long)i, (unsigned long long)tb);
        baseOut = tb;
        return true;
    }
    return false;
}

static bool ResolveLockCamParam(uintptr_t regMgr)
{
    uintptr_t tb = 0;
    if (!FindParamTableBase(regMgr, "LockCamParam", tb)) return false;
    uint16_t r = 0;
    if (!SafeReadU16(tb + 0x0A, r) || !r || r > 2000) return false;
    g_lockCamBase = tb;
    g_lockCamRows = r;
    Log("[ERCam] LockCamParam: %u rows\n", r);
    return true;
}

// ─────────────────────────────────────────────────────────────────────────────
//  Write distance to all LockCamParam rows (camDistTarget at row+0x00)
// ─────────────────────────────────────────────────────────────────────────────
static void WriteAllParams(float dist)
{
    if (!g_lockCamBase || !g_lockCamRows) return;
    for (uint32_t i = 0; i < g_lockCamRows; i++) {
        int32_t off = 0;
        if (!SafeReadInt32(g_lockCamBase + 0x40 + i * 0x18 + 0x08, off)) continue;
        if (off <= 0 || (uint32_t)off > 0x400000) continue;
        SafeWriteFloat(reinterpret_cast<float*>(g_lockCamBase + (uint32_t)off), dist);
    }
}

// ─────────────────────────────────────────────────────────────────────────────
//  SetDistance — updates target; easing happens in the sustain loop
// ─────────────────────────────────────────────────────────────────────────────
static void SetDistance(float d)
{
    if (d < g_minDist) d = g_minDist;
    if (d > g_maxDist) d = g_maxDist;
    g_distance.store(d);
    WriteAllParams(d);
    Log("[ERCam] Distance: %.2f\n", d);
}

// ─────────────────────────────────────────────────────────────────────────────
//  Input — Alt + scroll wheel
// ─────────────────────────────────────────────────────────────────────────────
static LRESULT CALLBACK LowLevelMouseProc(int nCode, WPARAM wParam, LPARAM lParam)
{
    if (nCode == HC_ACTION && wParam == WM_MOUSEWHEEL) {
        if (GetAsyncKeyState(VK_MENU) & 0x8000) {
            short delta = (short)HIWORD(reinterpret_cast<MSLLHOOKSTRUCT*>(lParam)->mouseData);
            // Default: scroll up (delta > 0) = zoom IN (decrease distance).
            // invert_scroll = 1 flips this.
            bool scrollUp = (delta > 0);
            bool zoomIn   = g_invertScroll ? !scrollUp : scrollUp;
            SetDistance(g_distance.load() + (zoomIn ? -g_step : g_step));
            return 1;
        }
    }
    return CallNextHookEx(g_mouseHook, nCode, wParam, lParam);
}

static DWORD WINAPI HookThreadProc(LPVOID)
{
    g_mouseHook = SetWindowsHookEx(WH_MOUSE_LL, LowLevelMouseProc, nullptr, 0);
    if (!g_mouseHook) { Log("[ERCam] Mouse hook failed\n"); return 1; }
    Log("[ERCam] Mouse hook OK\n");
    MSG msg;
    while (g_running.load()) {
        if (MsgWaitForMultipleObjects(0, nullptr, FALSE, 100, QS_ALLINPUT) == WAIT_OBJECT_0)
            while (PeekMessageA(&msg, nullptr, 0, 0, PM_REMOVE)) {
                TranslateMessage(&msg); DispatchMessageA(&msg);
            }
    }
    if (g_mouseHook) { UnhookWindowsHookEx(g_mouseHook); g_mouseHook = nullptr; }
    return 0;
}

// ─────────────────────────────────────────────────────────────────────────────
//  Code cave — allocate RWX memory within ±1.75 GB of target address.
// ─────────────────────────────────────────────────────────────────────────────
static uint8_t* AllocNear(uintptr_t target, size_t bytes)
{
    SYSTEM_INFO si = {};
    GetSystemInfo(&si);
    const ptrdiff_t RANGE = 0x70000000LL;

    uintptr_t lo = (target > (uintptr_t)RANGE)
                 ? (target - RANGE)
                 : (uintptr_t)si.lpMinimumApplicationAddress;
    uintptr_t hi = target + RANGE;
    if (hi > (uintptr_t)si.lpMaximumApplicationAddress)
        hi = (uintptr_t)si.lpMaximumApplicationAddress;

    MEMORY_BASIC_INFORMATION mbi = {};
    uintptr_t addr = lo;
    while (addr + bytes < hi) {
        if (!VirtualQuery(reinterpret_cast<void*>(addr), &mbi, sizeof(mbi))) break;
        if (mbi.State == MEM_FREE && mbi.RegionSize >= bytes) {
            void* p = VirtualAlloc(reinterpret_cast<void*>(addr),
                                   bytes, MEM_COMMIT | MEM_RESERVE,
                                   PAGE_EXECUTE_READWRITE);
            if (p) return reinterpret_cast<uint8_t*>(p);
        }
        addr = reinterpret_cast<uintptr_t>(mbi.BaseAddress) + mbi.RegionSize;
    }
    return nullptr;
}

// ─────────────────────────────────────────────────────────────────────────────
//  InstallCamHook
//
//  Cave is the same as v2 — no arithmetic, no XMM1 clobber.
//  The C++ sustain loop updates CAVE_DIST_SLOT with the eased distance value.
//
//  RIP-relative offset verification:
//    MOV  target: RIP(+00 end) = cave+7,  data = cave+0x40, rel = 0x39  ✓
//    MOVSS src:   RIP(+07 end) = cave+0F, data = cave+0x30, rel = 0x21  ✓
//    JMP  target: RIP(+27 end) = cave+2D, data = cave+0x38, rel = 0x0B  ✓
// ─────────────────────────────────────────────────────────────────────────────
static bool InstallCamHook()
{
    uintptr_t hookAddr = Scanner::FindPattern(g_modBase, CAM_PATTERN, CAM_MASK);
    if (!hookAddr) { Log("[ERCam] Camera AOB not found\n"); return false; }
    g_camHookAddr = hookAddr;
    Log("[ERCam] Camera hook @ 0x%llX\n", (unsigned long long)hookAddr);

    g_cave = AllocNear(hookAddr, 80);
    if (!g_cave) { Log("[ERCam] AllocNear failed\n"); return false; }

    static const uint8_t CAVE_TEMPLATE[72] = {
        // +00  MOV [RIP+0x39], RBX  — save rbx → cave+0x40 each frame
        0x48, 0x89, 0x1D, 0x39, 0x00, 0x00, 0x00,
        // +07  MOVSS XMM0, [RIP+0x21]  — load eased distance from cave+0x30
        0xF3, 0x0F, 0x10, 0x05, 0x21, 0x00, 0x00, 0x00,
        // +0F  MOVSS [RBX+0x1B4], XMM0  — write distance to camera struct
        0xF3, 0x0F, 0x11, 0x83, 0xB4, 0x01, 0x00, 0x00,
        // +17  MOVSS [RBX+0x1B8], XMM0  — write distance to camera struct
        0xF3, 0x0F, 0x11, 0x83, 0xB8, 0x01, 0x00, 0x00,
        // +1F  MOVSS XMM2, [RBX+0x160]  — stolen original instruction
        0xF3, 0x0F, 0x10, 0x93, 0x60, 0x01, 0x00, 0x00,
        // +27  JMP QWORD PTR [RIP+0x0B]  — return to hookAddr+8
        0xFF, 0x25, 0x0B, 0x00, 0x00, 0x00,
        // +2D  NOP padding
        0x90, 0x90, 0x90,
        // +30  distance slot (4 bytes) — sustain loop writes g_smoothedDist here
        0x00, 0x00, 0x00, 0x00,
        // +34  padding
        0x00, 0x00, 0x00, 0x00,
        // +38  back address (8 bytes)
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        // +40  cam struct slot (8 bytes, filled by cave each frame)
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    };
    memcpy(g_cave, CAVE_TEMPLATE, sizeof(CAVE_TEMPLATE));

    // Initialise distance slot to current distance (no lerp on startup)
    float dist = g_distance.load();
    memcpy(g_cave + CAVE_DIST_SLOT, &dist, sizeof(float));

    uint64_t backAddr = hookAddr + 8;
    memcpy(g_cave + CAVE_BACK_SLOT, &backAddr, 8);

    memcpy(g_origBytes, reinterpret_cast<void*>(hookAddr), 8);

    int32_t rel = (int32_t)((uintptr_t)g_cave - (hookAddr + 5));
    DWORD old;
    VirtualProtect(reinterpret_cast<void*>(hookAddr), 8, PAGE_EXECUTE_READWRITE, &old);
    *reinterpret_cast<uint8_t*> (hookAddr)     = 0xE9;
    *reinterpret_cast<int32_t*> (hookAddr + 1) = rel;
    *reinterpret_cast<uint8_t*> (hookAddr + 5) = 0x90;
    *reinterpret_cast<uint8_t*> (hookAddr + 6) = 0x90;
    *reinterpret_cast<uint8_t*> (hookAddr + 7) = 0x90;
    VirtualProtect(reinterpret_cast<void*>(hookAddr), 8, old, &old);
    FlushInstructionCache(GetCurrentProcess(), reinterpret_cast<void*>(hookAddr), 8);

    Log("[ERCam] Cave @ 0x%llX  rel32=0x%X\n",
        (unsigned long long)(uintptr_t)g_cave, (unsigned)rel);
    return true;
}

static void RemoveCamHook()
{
    if (!g_camHookAddr) return;
    DWORD old;
    VirtualProtect(reinterpret_cast<void*>(g_camHookAddr), 8, PAGE_EXECUTE_READWRITE, &old);
    memcpy(reinterpret_cast<void*>(g_camHookAddr), g_origBytes, 8);
    VirtualProtect(reinterpret_cast<void*>(g_camHookAddr), 8, old, &old);
    FlushInstructionCache(GetCurrentProcess(), reinterpret_cast<void*>(g_camHookAddr), 8);
    if (g_cave) { VirtualFree(g_cave, 0, MEM_RELEASE); g_cave = nullptr; }
    g_camHookAddr = 0;
}

// ─────────────────────────────────────────────────────────────────────────────
//  Helpers
// ─────────────────────────────────────────────────────────────────────────────
static std::string GetDllDir(HMODULE hm)
{
    char path[MAX_PATH] = {};
    GetModuleFileNameA(hm, path, MAX_PATH);
    std::string s(path);
    auto sl = s.find_last_of("\\/");
    return sl != std::string::npos ? s.substr(0, sl + 1) : "";
}

// ─────────────────────────────────────────────────────────────────────────────
//  ModMain
// ─────────────────────────────────────────────────────────────────────────────
static HMODULE g_hModule = nullptr;
static void ModMain()
{
    Sleep(5000);

    std::string dir = GetDllDir(g_hModule);

    g_cfg.Load();
    g_logging = (g_cfg.debugLog != 0);
    OpenLog(dir);

    g_distance.store(g_cfg.cameraDistance);
    g_smoothedDist = g_cfg.cameraDistance;  // start at target — no startup lerp
    g_step         = g_cfg.step;
    g_minDist      = g_cfg.minDistance;
    g_maxDist      = g_cfg.maxDistance;
    g_smoothFactor = g_cfg.smoothFactor;
    g_invertScroll = (g_cfg.invertScroll != 0);

    Log("[ERCam] dist=%.2f step=%.2f min=%.2f max=%.2f smooth=%.3f invert=%d\n",
        g_distance.load(), g_step, g_minDist, g_maxDist,
        g_smoothFactor, g_cfg.invertScroll);

    g_modBase = reinterpret_cast<uintptr_t>(GetModuleHandleA("eldenring.exe"));
    if (!g_modBase) { Log("[ERCam] ERROR: eldenring.exe not found\n"); return; }

    uintptr_t hit = Scanner::FindPattern(g_modBase, REG_PATTERN, REG_MASK);
    if (!hit) { Log("[ERCam] ERROR: REG pattern not found\n"); return; }
    uintptr_t regMgrPtr = hit + 7 + *reinterpret_cast<int32_t*>(hit + 3);
    Log("[ERCam] RegMgrPtr @ 0x%llX\n", (unsigned long long)regMgrPtr);

    if (!InstallCamHook()) {
        Log("[ERCam] WARNING: camera hook failed — dash camera won't be fixed\n");
    }

    g_hookThread = CreateThread(nullptr, 0, HookThreadProc, nullptr, 0, &g_hookTid);

    for (int att = 0; att < 120 && g_running.load(); att++, Sleep(500)) {
        uintptr_t regMgr = 0;
        if (!SafeReadPtr(regMgrPtr, regMgr) || !regMgr) {
            if (att % 10 == 0) Log("[ERCam] waiting for RegMgr... (%d)\n", att);
            continue;
        }
        Log("[ERCam] RegMgr @ 0x%llX (att %d)\n", (unsigned long long)regMgr, att);
        if (ResolveLockCamParam(regMgr)) {
            WriteAllParams(g_distance.load());
            Log("[ERCam] SUCCESS — Lock:%u rows  Cave:%s\n",
                g_lockCamRows, g_cave ? "installed" : "MISSING");
            break;
        }
        if (att == 119) Log("[ERCam] ERROR: timed out waiting for params\n");
    }

    // ── Sustain loop ─────────────────────────────────────────────────────────
    // Every tick (~16ms):
    //   1. Ease g_smoothedDist toward g_distance by g_smoothFactor.
    //   2. Write g_smoothedDist into the cave's distance slot so the cave
    //      applies it to the camera struct on the next game frame.
    // Every ~100ms (6 ticks): re-write LockCamParam rows.
    int tick = 0;
    while (g_running.load())
    {
        Sleep(16);

        float target = g_distance.load();

        // Lerp smoothed distance toward target
        float delta = target - g_smoothedDist;
        g_smoothedDist += delta * g_smoothFactor;
        // Snap if close enough to avoid infinite asymptotic crawl
        if (fabsf(delta) < 0.002f)
            g_smoothedDist = target;

        // Push eased value into cave
        if (g_cave)
            memcpy(g_cave + CAVE_DIST_SLOT, &g_smoothedDist, sizeof(float));

        // Write LockCamParam every ~100ms
        if (++tick >= 6) {
            tick = 0;
            if (g_lockCamBase) {
                WriteAllParams(target);
            } else {
                uintptr_t regMgr = 0;
                if (SafeReadPtr(regMgrPtr, regMgr) && regMgr)
                    if (ResolveLockCamParam(regMgr))
                        WriteAllParams(target);
            }
        }
    }
}

// ─────────────────────────────────────────────────────────────────────────────
//  Shutdown
// ─────────────────────────────────────────────────────────────────────────────
static void Shutdown()
{
    g_running.store(false);
    if (g_hookTid) PostThreadMessageA(g_hookTid, WM_QUIT, 0, 0);
    if (g_hookThread) {
        WaitForSingleObject(g_hookThread, 500);
        CloseHandle(g_hookThread);
        g_hookThread = nullptr;
    }
    RemoveCamHook();
    g_cfg.cameraDistance = g_distance.load();
    g_cfg.Save();
    if (g_logFile) { fclose(g_logFile); g_logFile = nullptr; }
}

// ─────────────────────────────────────────────────────────────────────────────
//  DLL entry
// ─────────────────────────────────────────────────────────────────────────────
BOOL APIENTRY DllMain(HMODULE hModule, DWORD ul_reason, LPVOID)
{
    if (ul_reason == DLL_PROCESS_ATTACH) {
        g_hModule = hModule;
        DisableThreadLibraryCalls(hModule);
        std::thread(ModMain).detach();
    } else if (ul_reason == DLL_PROCESS_DETACH) {
        Shutdown();
    }
    return TRUE;
}
