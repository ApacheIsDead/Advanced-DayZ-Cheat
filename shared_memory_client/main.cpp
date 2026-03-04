#define _CRT_SECURE_NO_WARNINGS
#include "readradar.cpp"
/*
*  PACKETLOSS v1.28.8 — Single-Thread Overlay + Card UI
*  Menu pages: Visuals | Aimbot | Radar | Filter | Exploits | Config
*  ESP: Corner boxes, weapon-in-hand display, dark tag items
*  Aimbot: Bone selector, magic bullet, mouse aim, prediction
*  Radar: Circular with trails, color presets
*  + Weapon swap detection (prevents stale ammo pointer crash)
*/

#include <iomanip>
#include <sstream>
#include "Common.h"
#include "utils_um.h"
#include "main.h"
#include <conio.h>
#include "aimbot.h"
#include <cstdint>
#include "offsets.h"
#include "math.h"
#include "d3d11.h"
#include <cstdio>
#include <tchar.h>
#include <string>
#include <windows.h>
#include <tlhelp32.h>
#include <vector>
#include <cmath>
#include <cstring>
#include <algorithm>
#include <iostream>
#include <unordered_map>
#include <windowsx.h>
#include <functional>
#include <mutex>
#include <thread>
#include <atomic>
#include <array>
#include <set>

using std::string;

#include <gdiplus.h>
#pragma comment(lib, "gdiplus.lib")

static ULONG_PTR g_gdipToken = 0;

#include <dwmapi.h>
#pragma comment(lib, "Dwmapi.lib")

// ═══ DEBUG: Crash handler — keeps console open on crash ═══
static LONG WINAPI CrashHandler(EXCEPTION_POINTERS* ep) {
    printf("\n\n======== PACKETLOSS CRASH REPORT ========\n");
    printf("Exception: 0x%08X\n", ep->ExceptionRecord->ExceptionCode);
    printf("Address:   %p\n", ep->ExceptionRecord->ExceptionAddress);
    if (ep->ExceptionRecord->ExceptionCode == 0xC0000005) {
        printf("AV %s addr: %p\n",
            ep->ExceptionRecord->ExceptionInformation[0] ? "WRITE" : "READ",
            (void*)ep->ExceptionRecord->ExceptionInformation[1]);
    }
    printf("RIP: %p  RSP: %p\n",
        (void*)ep->ContextRecord->Rip, (void*)ep->ContextRecord->Rsp);
    printf("=========================================\n");
    fflush(stdout);
    printf("\nPress any key to close...\n");
    _getch();
    return EXCEPTION_EXECUTE_HANDLER;
}

// ═══════════════════════════════════════════════════════════════
//  1.28 OFFSETS
// ═══════════════════════════════════════════════════════════════
namespace Off {
    constexpr uintptr_t World = 0xF4B050;
    constexpr uintptr_t Camera = 0x1B8;
    constexpr uintptr_t CamPos = 0x2C;
    constexpr uintptr_t CamRight = 0x8;
    constexpr uintptr_t CamUp = 0x14;
    constexpr uintptr_t CamForward = 0x20;
    constexpr uintptr_t ProjX = 0xD0;
    constexpr uintptr_t NearBase = 0xF48;
    constexpr uintptr_t NearCount = 0xF50;
    constexpr uintptr_t FarBase = 0x1090;
    constexpr uintptr_t FarCount = 0x1098;
    constexpr uintptr_t ItemTable = 0x2060;
    constexpr uintptr_t ItemTableSize = 0x2068;
    constexpr uintptr_t VisualState = 0x1C8;
    constexpr uintptr_t GetCoord = 0x2C;
    constexpr uintptr_t PlayerSkeleton = 0x7E8;
    constexpr uintptr_t ZombieSkeleton = 0x678;
    constexpr uintptr_t AnimClassOff = 0xB0;
    constexpr uintptr_t MatrixClassOff = 0xBF0;
    constexpr uintptr_t NetworkID = 0x6E4;
    constexpr uintptr_t Bullets = 0xE00;
    constexpr uintptr_t BulletCount = 0xE08;
    constexpr uintptr_t Inventory = 0x658;    // used by hands/weapon reading (works)
    constexpr uintptr_t InvHands = 0x1B0;
    constexpr uintptr_t AmmoType1 = 0x6B0;
    constexpr uintptr_t AmmoType2 = 0x20;
    constexpr uintptr_t InitSpeed = 0x364;
    constexpr uintptr_t AirFriction = 0x3B4;

    // ── Inventory walking (1.28) ──
    // Entity base shifted (+0xB0 from 2022) but internal InventoryComponent layout unchanged.
    // INVENTORY_CARGO stayed at 0x148 (confirmed), so ATTACHMENTS/SIZE also stayed.
    constexpr uintptr_t ItemInventory = 0x660;   // entity → InventoryComponent (was 0x5B0 in 2022)
    constexpr uintptr_t CargoGrid = 0x148;        // inv → CargoGrid ptr (confirmed 1.28)
    constexpr uintptr_t CargoItemList = 0x38;     // CargoGrid → item ptr array (confirmed 1.28)
    constexpr uintptr_t AttachStart = 0x150;      // inv → attachment slot array base (0x150 since 2022)
    constexpr uintptr_t AttachSize = 0x15C;       // inv → attachment slot count (0x15C since 2022)
    constexpr int AttachSlotStride = 0x10;        // sizeof(slot_t) { u32 id; u32 pad; u64 item_ptr; }
    constexpr int AttachSlotItemOff = 0x8;        // offset of item_ptr within slot_t

}

// ═══════════════════════════════════════════════════════════════
//  FORWARD DECL
// ═══════════════════════════════════════════════════════════════
inline float dot(const vector3& a, const vector3& b);
inline vector3 normalize(const vector3& v);
void PumpUIOnce();
void changeFov(float fov, int dayzid);

// ═══════════════════════════════════════════════════════════════
//  GLOBALS
// ═══════════════════════════════════════════════════════════════
const std::string process_name = "DayZ_x64.exe";
uintptr_t base = 0;
uintptr_t client = 0;
int processId1 = 0;

std::atomic<bool> g_espEnabled{ false };
std::atomic<bool> g_radarEnabled{ false };
std::atomic<bool> g_showBones{ false };
std::atomic<bool> g_showZombieBones{ false };
std::atomic<bool> g_showItems{ false };
std::atomic<bool> g_fovOnly{ false };
std::atomic<bool> g_espPlayers{ true };
std::atomic<bool> g_espZombies{ true };
std::atomic<bool> g_snapPlayers{ true };       // player snaplines
std::atomic<bool> g_snapZombies{ false };      // zombie snaplines
static float g_skeleDist = 200.f;              // skeleton render max distance
static float g_maxZombieRender = 30.f;             // max zombies drawn per frame
static COLORREF g_fovCircleColor = RGB(0, 170, 255); // FOV circle color
static float g_brightness = 1.0f;                     // gamma brightness (1.0 = default, 0.5-3.0 range)
static bool g_gammaModified = false;
static WORD g_originalGamma[3][256] = {};              // backup of original gamma ramp

static void ApplyBrightness(float gamma) {
    HDC hdc = GetDC(nullptr);
    if (!hdc) return;
    if (!g_gammaModified) {
        GetDeviceGammaRamp(hdc, g_originalGamma);
        g_gammaModified = true;
    }
    WORD ramp[3][256];
    for (int i = 0; i < 256; i++) {
        float val = powf(i / 255.0f, 1.0f / gamma) * 65535.0f;
        WORD v = (WORD)(std::min)(65535.0f, (std::max)(0.0f, val));
        ramp[0][i] = ramp[1][i] = ramp[2][i] = v;
    }
    SetDeviceGammaRamp(hdc, ramp);
    ReleaseDC(nullptr, hdc);
}

static void RestoreBrightness() {
    if (g_gammaModified) {
        HDC hdc = GetDC(nullptr);
        if (hdc) { SetDeviceGammaRamp(hdc, g_originalGamma); ReleaseDC(nullptr, hdc); }
        g_gammaModified = false;
    }
}
std::atomic<bool> g_radPlayers{ true };
std::atomic<bool> g_radZombies{ true };
std::atomic<bool> g_radTrails{ false };  // movement breadcrumb trails
std::atomic<bool> g_overlayRunning{ false };
std::atomic<bool> g_shutdownAll{ false };
std::atomic<bool> g_silentAim{ false };
std::atomic<bool> g_showLandmarks{ false };
std::atomic<bool> g_lmCities{ true };
std::atomic<bool> g_lmTowns{ true };
std::atomic<bool> g_lmMilitary{ true };
std::atomic<bool> g_showWells{ false };
std::atomic<bool> g_mouseAim{ false };
std::atomic<bool> g_railgunAim{ false };    // railgun = mouse aim + forced 50000 m/s
std::atomic<bool> g_aimPrediction{ true };  // velocity leading
std::atomic<bool> g_noRecoil{ false };      // anti-recoil mouse pull
std::atomic<bool> g_noGrass{ false };       // disable grass rendering
static bool g_noGrassApplied = false;
std::atomic<bool> g_laserFire{ false };     // speed hack toggle (separate from aim)
std::atomic<bool> g_raidMode{ false };      // raid mode — bullet looping
std::atomic<bool> g_mortarMode{ false };    // mortar mode — XYZ bullet teleport
static float g_fovRadiusF = 300.f;         // ESP FOV filter radius (slider)

// ── Bullet tracer state ──
std::atomic<bool> g_bulletTracers{ false };
static COLORREF g_tracerColor = RGB(255, 200, 40);   // tracer line color
struct TracerEntry { uintptr_t ptr; vector3 prev; vector3 cur; int lastSeen; bool hasPrev; };
static TracerEntry g_tracers[128];
static int g_tracerCount = 0;

// ── Loot teleport state ──
std::atomic<bool> g_lootTP{ false };
static float g_lootTPRange = 50.f;         // max item distance for TP (meters)

// ── Remote loot state ──
std::atomic<bool> g_remoteLoot{ false };
static float g_remoteLootRange = 100.f;    // max item distance (meters)
static float g_remoteLootTime = 5.0f;      // seconds to hold at item
static std::atomic<bool> g_remoteLootActive{ false };
static std::atomic<uintptr_t> g_remoteLootPlayerVS{ 0 }; // playerPtr via 0x2960 chain
static std::atomic<uintptr_t> g_remoteLootCamPtr{ 0 };  // camera ptr for view lock
static vector3 g_remoteLootTarget = { 0,0,0 };
static vector3 g_remoteLootSavedPos = { 0,0,0 };
static DWORD g_remoteLootStartMs = 0;
static std::string g_remoteLootItemName;

// ── Freecam state ──
std::atomic<bool> g_freecam{ false };
static std::atomic<bool> g_freecamActive{ false };
static vector3 g_freecamPos = { 0,0,0 };
static float g_freecamSpeedSlow = 0.15f;
static float g_freecamSpeedNormal = 1.0f;
static float g_freecamSpeedFast = 8.0f;
static int g_freecamHotkeyIdx = 3;         // index into hotkey table (default F4)
struct HotkeyEntry { int vk; const char* name; };
static const HotkeyEntry g_freecamHotkeys[] = {
    { VK_F1, "F1" }, { VK_F2, "F2" }, { VK_F3, "F3" }, { VK_F4, "F4" },
    { VK_F5, "F5" }, { VK_F6, "F6" }, { VK_F7, "F7" }, { VK_F8, "F8" },
    { 0x47, "G" }, { 0x56, "V" }, { 0x58, "X" }, { 0x59, "Y" },
};
static const int g_freecamHotkeyCount = sizeof(g_freecamHotkeys) / sizeof(g_freecamHotkeys[0]);
// Cached addresses for the write thread (updated by main loop)
static std::atomic<uintptr_t> g_freecamCamPtr{ 0 };
static std::atomic<uintptr_t> g_freecamPlayerVS{ 0 };

// ── Raid mode state ──
static bool g_raidPointSet = false;
static vector3 g_raidPoint = { 0,0,0 };
static vector3 g_raidDir = { 0,0,0 };      // locked forward direction at placement
static float g_raidDist = 10.f;            // distance from camera to place point (1-50m)
static float g_raidSpeed = 4000.f;         // bullet speed for raiding (high = more damage per crossing)
static float g_raidOscDist = 50.f;         // oscillation distance each side of wall (meters)
static int g_raidMaxLoops = 1000;           // max oscillations per bullet

// Raid bullet tracking — bullets we're actively looping
struct RaidBullet {
    uintptr_t ptr;
    uintptr_t bvs;     // cached VisualState
    int phase;          // 0=posA, 1=posB alternating
    int loops;          // number of oscillations done
    int startTick;
};
static RaidBullet g_raidBullets[16];
static int g_raidBulletCount = 0;
static bool raidBulletTracked(uintptr_t ptr) {
    for (int i = 0; i < g_raidBulletCount; i++) if (g_raidBullets[i].ptr == ptr) return true;
    return false;
}

// ── Mortar mode — teleport bullets to fixed XYZ ──
static bool g_mortarSet = false;
static vector3 g_mortarTarget = { 0,0,0 };
static float g_mortarSpeed = 25000.f;    // bullet speed for mortar (500-50000)
// Mortar dialog state
static HWND g_mortarDlg = nullptr;
static HWND g_mortarEdX = nullptr, g_mortarEdY = nullptr, g_mortarEdZ = nullptr;
static bool g_mortarPendingSet = false;
static vector3 g_mortarPendingPos = { 0,0,0 };

// Shared state — local player cache (used by aim)
static uintptr_t cachedLocalPlayer = 0;
static int localPlayerTick = 0;

static bool g_isAdmin = false; // checked once at startup

static bool CheckAdminRights() {
    BOOL isAdmin = FALSE;
    PSID adminGroup = NULL;
    SID_IDENTIFIER_AUTHORITY ntAuth = SECURITY_NT_AUTHORITY;
    if (AllocateAndInitializeSid(&ntAuth, 2, SECURITY_BUILTIN_DOMAIN_RID,
        DOMAIN_ALIAS_RID_ADMINS, 0, 0, 0, 0, 0, 0, &adminGroup)) {
        CheckTokenMembership(NULL, adminGroup, &isAdmin);
        FreeSid(adminGroup);
    }
    return isAdmin != FALSE;
}



// ── Slider-driven values (Aimbot page) ──
static float g_aimFovF = 120.f;    // aim FOV radius pixels (30-300)
static float g_silentRange = 500.f;     // silent aim max range (5-1000m)
static float g_mouseRange = 500.f;    // mouse aim max range (5-500m)
static float g_mouseSmooth = 3.0f;     // mouse smoothing (1-10)
static float g_bulletSpeed = 25000.f;  // init speed (500-50000)
static float g_leadFactor = 0.6f;     // prediction lead in meters (~2ft default) (0-2.0)
static float g_recoilPull = 1.5f;    // anti-recoil pull-down strength (0.5-5.0)

// Aim bone selector (mouse aimbot)
static int g_aimBoneChoice = 2; // 0=HEAD,1=NECK,2=SPINE,3=PELVIS (legacy/default)
static int g_mbBoneChoice = 0;  // magic bullet bone (default HEAD)
static int g_maBoneChoice = 2;  // mouse aim bone (default SPINE)
static int g_rgBoneChoice = 0;  // railgun bone (default HEAD)
static const char* g_boneChoiceNames[] = { "Head", "Neck", "Spine", "Pelvis" };
static constexpr int NUM_BONE_CHOICES = 4;
static constexpr float RAILGUN_SPEED = 50000.f; // railgun forced speed

// ── Custom Waypoints ──
struct Waypoint { char name[32]; float x, y, z; };
static std::vector<Waypoint> g_waypoints;
static constexpr int MAX_WAYPOINTS = 10;
static vector3 g_lastCamPos = { 0,0,0 }; // updated each overlay frame for waypoint use

// ── Custom Waypoint Dialog ──
static HWND g_wpDlg = nullptr;
static HWND g_wpEdName = nullptr, g_wpEdX = nullptr, g_wpEdY = nullptr, g_wpEdZ = nullptr;
static bool g_wpPendingAdd = false;
static Waypoint g_wpPendingWP = {};

static LRESULT CALLBACK WaypointDlgProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
    case WM_CREATE: {
        HFONT f = CreateFontA(14, 0, 0, 0, FW_NORMAL, 0, 0, 0, ANSI_CHARSET, 0, 0, CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, "Segoe UI");
        // Labels
        HWND lbl1 = CreateWindowA("STATIC", "Name:", WS_CHILD | WS_VISIBLE, 12, 14, 50, 20, hwnd, 0, (HINSTANCE)GetWindowLongPtr(hwnd, GWLP_HINSTANCE), 0);
        HWND lbl2 = CreateWindowA("STATIC", "X:", WS_CHILD | WS_VISIBLE, 12, 44, 20, 20, hwnd, 0, (HINSTANCE)GetWindowLongPtr(hwnd, GWLP_HINSTANCE), 0);
        HWND lbl3 = CreateWindowA("STATIC", "Y:", WS_CHILD | WS_VISIBLE, 12, 74, 20, 20, hwnd, 0, (HINSTANCE)GetWindowLongPtr(hwnd, GWLP_HINSTANCE), 0);
        HWND lbl4 = CreateWindowA("STATIC", "Z:", WS_CHILD | WS_VISIBLE, 12, 104, 20, 20, hwnd, 0, (HINSTANCE)GetWindowLongPtr(hwnd, GWLP_HINSTANCE), 0);
        // Edit controls
        g_wpEdName = CreateWindowExA(WS_EX_CLIENTEDGE, "EDIT", "", WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL, 70, 12, 160, 22, hwnd, (HMENU)101, (HINSTANCE)GetWindowLongPtr(hwnd, GWLP_HINSTANCE), 0);
        g_wpEdX = CreateWindowExA(WS_EX_CLIENTEDGE, "EDIT", "0", WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL, 70, 42, 160, 22, hwnd, (HMENU)102, (HINSTANCE)GetWindowLongPtr(hwnd, GWLP_HINSTANCE), 0);
        g_wpEdY = CreateWindowExA(WS_EX_CLIENTEDGE, "EDIT", "0", WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL, 70, 72, 160, 22, hwnd, (HMENU)103, (HINSTANCE)GetWindowLongPtr(hwnd, GWLP_HINSTANCE), 0);
        g_wpEdZ = CreateWindowExA(WS_EX_CLIENTEDGE, "EDIT", "0", WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL, 70, 102, 160, 22, hwnd, (HMENU)104, (HINSTANCE)GetWindowLongPtr(hwnd, GWLP_HINSTANCE), 0);
        // Buttons
        HWND btnOK = CreateWindowA("BUTTON", "OK", WS_CHILD | WS_VISIBLE | BS_DEFPUSHBUTTON, 50, 136, 80, 28, hwnd, (HMENU)IDOK, (HINSTANCE)GetWindowLongPtr(hwnd, GWLP_HINSTANCE), 0);
        HWND btnCan = CreateWindowA("BUTTON", "Cancel", WS_CHILD | WS_VISIBLE, 150, 136, 80, 28, hwnd, (HMENU)IDCANCEL, (HINSTANCE)GetWindowLongPtr(hwnd, GWLP_HINSTANCE), 0);
        // Set font on all
        HWND ctrls[] = { lbl1,lbl2,lbl3,lbl4,g_wpEdName,g_wpEdX,g_wpEdY,g_wpEdZ,btnOK,btnCan };
        for (auto c : ctrls) SendMessage(c, WM_SETFONT, (WPARAM)f, TRUE);
        SetFocus(g_wpEdName);
        return 0;
    }
    case WM_COMMAND:
        if (LOWORD(wParam) == IDOK) {
            // Cache values before destroying
            char buf[32];
            GetWindowTextA(g_wpEdName, g_wpPendingWP.name, sizeof(g_wpPendingWP.name));
            GetWindowTextA(g_wpEdX, buf, sizeof(buf)); g_wpPendingWP.x = (float)atof(buf);
            GetWindowTextA(g_wpEdY, buf, sizeof(buf)); g_wpPendingWP.y = (float)atof(buf);
            GetWindowTextA(g_wpEdZ, buf, sizeof(buf)); g_wpPendingWP.z = (float)atof(buf);
            if (g_wpPendingWP.name[0] == '\0') snprintf(g_wpPendingWP.name, 32, "WP%d", (int)g_waypoints.size() + 1);
            g_wpPendingAdd = true;
            DestroyWindow(hwnd); return 0;
        }
        if (LOWORD(wParam) == IDCANCEL) { g_wpPendingAdd = false; DestroyWindow(hwnd); return 0; }
        return 0;
    case WM_DESTROY:
        g_wpDlg = nullptr; return 0;
    }
    return DefWindowProc(hwnd, msg, wParam, lParam);
}

static void ShowWaypointDialog() {
    if (g_wpDlg) { SetForegroundWindow(g_wpDlg); return; } // already open
    static ATOM wpa = 0;
    HINSTANCE hi = GetModuleHandle(nullptr);
    if (!wpa) {
        WNDCLASSEXA wc = {}; wc.cbSize = sizeof(wc); wc.lpfnWndProc = WaypointDlgProc;
        wc.hInstance = hi; wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
        wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
        wc.lpszClassName = "WPDlg"; wpa = RegisterClassExA(&wc);
    }
    g_wpPendingAdd = false;
    g_wpDlg = CreateWindowExA(WS_EX_TOPMOST | WS_EX_TOOLWINDOW, "WPDlg", "Add Custom Waypoint",
        WS_POPUP | WS_CAPTION | WS_SYSMENU, 200, 200, 250, 200, nullptr, nullptr, hi, nullptr);
    if (g_wpDlg) { ShowWindow(g_wpDlg, SW_SHOW); UpdateWindow(g_wpDlg); }
}

static void CheckPendingWaypoint() {
    if (g_wpPendingAdd && (int)g_waypoints.size() < MAX_WAYPOINTS) {
        g_waypoints.push_back(g_wpPendingWP);
        g_wpPendingAdd = false;
    }
    // Pump dialog messages if open
    if (g_wpDlg) {
        MSG msg;
        while (PeekMessageA(&msg, g_wpDlg, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg); DispatchMessageA(&msg);
        }
    }
}

// ── Create death marker waypoint at last known position ──
extern std::atomic<bool> g_deathMarkers;
static void AddDeathMarker(vector3 pos, const char* reason) {
    if (!g_deathMarkers.load()) return;
    if ((int)g_waypoints.size() >= MAX_WAYPOINTS) return;
    if (fabsf(pos.x) < 1.f && fabsf(pos.z) < 1.f) return; // invalid pos
    Waypoint wp = {};
    snprintf(wp.name, sizeof(wp.name), "DEATH %d", (int)g_waypoints.size() + 1);
    wp.x = pos.x; wp.y = pos.y; wp.z = pos.z;
    g_waypoints.push_back(wp);
    printf("[DEATH-MARKER] %s at (%.0f, %.0f, %.0f)\n", reason, pos.x, pos.y, pos.z);
    fflush(stdout);
}

// ── Mortar Dialog ──
static LRESULT CALLBACK MortarDlgProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
    case WM_CREATE: {
        HFONT f = CreateFontA(14, 0, 0, 0, FW_NORMAL, 0, 0, 0, ANSI_CHARSET, 0, 0, CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, "Segoe UI");
        CreateWindowA("STATIC", "X:", WS_CHILD | WS_VISIBLE, 12, 14, 20, 20, hwnd, 0, (HINSTANCE)GetWindowLongPtr(hwnd, GWLP_HINSTANCE), 0);
        CreateWindowA("STATIC", "Y:", WS_CHILD | WS_VISIBLE, 12, 44, 20, 20, hwnd, 0, (HINSTANCE)GetWindowLongPtr(hwnd, GWLP_HINSTANCE), 0);
        CreateWindowA("STATIC", "Z:", WS_CHILD | WS_VISIBLE, 12, 74, 20, 20, hwnd, 0, (HINSTANCE)GetWindowLongPtr(hwnd, GWLP_HINSTANCE), 0);
        g_mortarEdX = CreateWindowExA(WS_EX_CLIENTEDGE, "EDIT", "0", WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL, 40, 12, 180, 22, hwnd, (HMENU)201, (HINSTANCE)GetWindowLongPtr(hwnd, GWLP_HINSTANCE), 0);
        g_mortarEdY = CreateWindowExA(WS_EX_CLIENTEDGE, "EDIT", "0", WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL, 40, 42, 180, 22, hwnd, (HMENU)202, (HINSTANCE)GetWindowLongPtr(hwnd, GWLP_HINSTANCE), 0);
        g_mortarEdZ = CreateWindowExA(WS_EX_CLIENTEDGE, "EDIT", "0", WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL, 40, 72, 180, 22, hwnd, (HMENU)203, (HINSTANCE)GetWindowLongPtr(hwnd, GWLP_HINSTANCE), 0);
        HWND btnOK = CreateWindowA("BUTTON", "Set Target", WS_CHILD | WS_VISIBLE | BS_DEFPUSHBUTTON, 30, 106, 100, 28, hwnd, (HMENU)IDOK, (HINSTANCE)GetWindowLongPtr(hwnd, GWLP_HINSTANCE), 0);
        HWND btnCan = CreateWindowA("BUTTON", "Cancel", WS_CHILD | WS_VISIBLE, 140, 106, 80, 28, hwnd, (HMENU)IDCANCEL, (HINSTANCE)GetWindowLongPtr(hwnd, GWLP_HINSTANCE), 0);
        HWND ctrls[] = { g_mortarEdX, g_mortarEdY, g_mortarEdZ, btnOK, btnCan };
        for (auto c : ctrls) SendMessage(c, WM_SETFONT, (WPARAM)f, TRUE);
        SetFocus(g_mortarEdX);
        return 0;
    }
    case WM_COMMAND:
        if (LOWORD(wParam) == IDOK) {
            char buf[32];
            GetWindowTextA(g_mortarEdX, buf, sizeof(buf)); g_mortarPendingPos.x = (float)atof(buf);
            GetWindowTextA(g_mortarEdY, buf, sizeof(buf)); g_mortarPendingPos.y = (float)atof(buf);
            GetWindowTextA(g_mortarEdZ, buf, sizeof(buf)); g_mortarPendingPos.z = (float)atof(buf);
            g_mortarPendingSet = true;
            DestroyWindow(hwnd); return 0;
        }
        if (LOWORD(wParam) == IDCANCEL) { g_mortarPendingSet = false; DestroyWindow(hwnd); return 0; }
        return 0;
    case WM_DESTROY:
        g_mortarDlg = nullptr; return 0;
    }
    return DefWindowProc(hwnd, msg, wParam, lParam);
}

static void ShowMortarDialog() {
    if (g_mortarDlg) { SetForegroundWindow(g_mortarDlg); return; }
    static ATOM mda = 0;
    HINSTANCE hi = GetModuleHandle(nullptr);
    if (!mda) {
        WNDCLASSEXA wc = {}; wc.cbSize = sizeof(wc); wc.lpfnWndProc = MortarDlgProc;
        wc.hInstance = hi; wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
        wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
        wc.lpszClassName = "MortarDlg"; mda = RegisterClassExA(&wc);
    }
    g_mortarPendingSet = false;
    g_mortarDlg = CreateWindowExA(WS_EX_TOPMOST | WS_EX_TOOLWINDOW, "MortarDlg", "Set Mortar Target (X, Y, Z)",
        WS_POPUP | WS_CAPTION | WS_SYSMENU, 200, 200, 240, 170, nullptr, nullptr, hi, nullptr);
    if (g_mortarDlg) { ShowWindow(g_mortarDlg, SW_SHOW); UpdateWindow(g_mortarDlg); }
}

static void CheckPendingMortar() {
    if (g_mortarPendingSet) {
        g_mortarTarget = g_mortarPendingPos;
        g_mortarSet = true;
        g_mortarPendingSet = false;
        printf("[MORTAR] Target set: (%.1f, %.1f, %.1f)\n", g_mortarTarget.x, g_mortarTarget.y, g_mortarTarget.z);
        fflush(stdout);
    }
    if (g_mortarDlg) {
        MSG msg;
        while (PeekMessageA(&msg, g_mortarDlg, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg); DispatchMessageA(&msg);
        }
    }
}

// Speed hack needs re-apply when bullet speed slider changes
static bool  g_speedHackDone = false;
static float g_lastAppliedSpeed = 0.f;
static int   g_lastSpeedWriteTick = 0; // throttle writes to avoid driver freeze

// ── Write queue — drained from overlay thread (driver is single-request, no concurrent calls) ──
struct WriteReq { uintptr_t addr; char data[16]; int size; int pid; };
static WriteReq g_pendingWrites[32];
static int g_writeHead = 0, g_writeTail = 0;

template <typename T>
static void queueWrite(uintptr_t addr, T val, int pid) {
    int next = (g_writeHead + 1) % 32;
    if (next == g_writeTail) return; // full, drop
    WriteReq& req = g_pendingWrites[g_writeHead];
    req.addr = addr; req.size = sizeof(T); req.pid = pid;
    memcpy(req.data, &val, sizeof(T));
    g_writeHead = next;
}

// Call once per overlay frame — processes up to maxPerFrame queued speed writes
static void drainWrites(int maxPerFrame = 1) {
    int done = 0;
    while (g_writeTail != g_writeHead && done < maxPerFrame) {
        WriteReq& req = g_pendingWrites[g_writeTail];
        if (req.size == 4) {
            float v; memcpy(&v, req.data, 4);
            write_dedicated<float>(req.addr, v, req.pid);
        }
        else if (req.size == 12) {
            vector3 v; memcpy(&v, req.data, 12);
            write_dedicated<vector3>(req.addr, v, req.pid);
        }
        g_writeTail = (g_writeTail + 1) % 32;
        done++;
    }
}
// write_dedicated<T> and write_async<T> defined in comms (utils_um)
// OpenWriteChannel() defined in comms — called from main()

// ── Batch read helper — uses OP_READ_BATCH via read_batch() in comms ──
struct BatchCtx {
    static constexpr int MAX_ENTRIES = 200;
    static constexpr int MAX_RESULT_BYTES = 3840;

    BatchEntry entries[MAX_ENTRIES];
    uint16_t offsets[MAX_ENTRIES];
    int count = 0;
    int totalBytes = 0;
    uint8_t results[MAX_RESULT_BYTES];
    bool ok = false;

    void reset() { count = 0; totalBytes = 0; ok = false; }

    int add(uintptr_t addr, int size) {
        if (count >= MAX_ENTRIES || totalBytes + size > MAX_RESULT_BYTES) return -1;
        offsets[count] = (uint16_t)totalBytes;
        entries[count] = { addr, (uint32_t)size };
        totalBytes += size;
        return count++;
    }

    bool execute(int procId) {
        ok = false;
        if (count == 0) { ok = true; return true; }
        void* res = read_batch(entries, count, procId);
        if (!res) {
            memset(results, 0, totalBytes);
            return false;
        }
        memcpy(results, res, totalBytes);
        ok = true;
        return true;
    }

    template<typename T>
    T get(int idx) {
        T val{};
        if (idx >= 0 && idx < count && ok)
            memcpy(&val, results + offsets[idx], sizeof(T));
        return val;
    }
};

static uintptr_t g_cachedHands = 0;    // weapon swap detection

// Bullet tracking
static uintptr_t g_tpdBullets[64];
static int g_tpdCount = 0;
static inline bool bulletAlreadyTpd(uintptr_t addr) {
    for (int i = 0; i < g_tpdCount; i++) if (g_tpdBullets[i] == addr) return true;
    return false;
}
static inline void trackBullet(uintptr_t addr) {
    if (g_tpdCount < 64) g_tpdBullets[g_tpdCount++] = addr;
}

// ── Target tagging (lock-on) ──
static uintptr_t g_taggedTarget = 0;       // entity ptr of tagged player
static bool g_taggedIsZombie = false;       // track if tagged entity is zombie
static int g_taggedBone = 2;                // bone to aim at on tagged target (2=SPINE2)
static int g_tagTick = 0;                   // tick when tagged
static vector3 g_taggedLastPos = { 0,0,0 }; // last known position of tagged target
std::atomic<bool> g_deathMarkers{ true };    // create waypoint when tagged target dies

// ── Velocity tracking for prediction (EMA smoothed) ──
struct VelocityEntry { vector3 lastPos; vector3 velocity; vector3 smoothVel; int lastTick; bool valid; };
static std::unordered_map<uintptr_t, VelocityEntry> g_velocityMap;

// ── Entity + camera cache for dead reckoning during driver stalls ──
// Forward-declare bone types (full definitions later in BONES section)
struct Matrix3x4 { float m[12]; };
struct BonePos { vector3 pos; bool valid; };
enum PlayerBone {
    PB_HEAD = 0, PB_NECK, PB_SPINE2, PB_SPINE1, PB_PELVIS,
    PB_L_SHOULDER, PB_R_SHOULDER, PB_L_ELBOW, PB_R_ELBOW,
    PB_L_HAND, PB_R_HAND, PB_L_HIP, PB_R_HIP,
    PB_L_KNEE, PB_R_KNEE, PB_L_FOOT, PB_R_FOOT, PB_MAX
};
struct CachedEntEntry {
    uintptr_t ptr, visualState;
    vector3 pos, headPos, velocity;
    float dist;
    bool isPlayer, isZombie, isItem, hasBones;
    int catIdx;
    std::string name, weaponName;
    std::vector<std::string> inventory;
    BonePos bones[PB_MAX];
    int lastTick;
};
struct CachedCamera {
    uintptr_t world, camPtr;
    vector3 camPos, camRight, camUp, camForward, localPos;
    float projD1, projD2;
    int lastTick;
    bool valid;
};
static std::unordered_map<uintptr_t, CachedEntEntry> g_entityCache;
static CachedCamera g_camCache = {};
static int g_lastFreshTick = 0;

// ── Player inventory ESP ──
std::atomic<bool> g_showPlayerInv{ false };   // toggle: show inventory list under players
static std::unordered_map<uintptr_t, std::pair<int, std::vector<std::string>>> g_invCache; // ptr->{tick, items}

// Tick rate constants — overlay runs at Sleep(2) ≈ ~500fps
// Use these for all time-based logic so timers don't break if Sleep changes
static constexpr int TPS = 500;              // approximate ticks per second
static constexpr int T_HALF_S = TPS / 2;     // 250  — 0.5s
static constexpr int T_1S = TPS;             // 500  — 1s
static constexpr int T_2S = TPS * 2;         // 1000 — 2s
static constexpr int T_3S = TPS * 3;         // 1500 — 3s
static constexpr int T_5S = TPS * 5;         // 2500 — 5s
static constexpr int T_10S = TPS * 10;       // 5000 — 10s
static constexpr int T_30S = TPS * 30;       // 15000— 30s

// Weapon name cache — read once on first sight, refresh every ~30s
struct WeaponCacheEntry { std::string name; int lastTick; };
static std::unordered_map<uintptr_t, WeaponCacheEntry> g_weaponCache;
static constexpr int WEAPON_REFRESH_TICKS = T_30S;

static std::string g_espFilterText;
static int g_menuPage = 0;   // 0=ESP, 1=Aimbot, 2=Radar, 3=Filter

// ── ESP Colors (user-customizable) ──
static COLORREF g_colPlayerBox = RGB(180, 130, 255);
static COLORREF g_colZombieBox = RGB(255, 80, 40);
static COLORREF g_colSnapLine = RGB(140, 80, 220);
static COLORREF g_colBone = RGB(0, 220, 180);
static COLORREF g_colHeadDot = RGB(255, 60, 60);

// Color picker state
struct ColorEntry { const char* label; COLORREF* color; };
static int g_colorPickerOpen = -1; // -1 = closed, 0-4 = ESP, 5-13 = item cats, 14 = FOV, 15 = tracers
static HBITMAP g_wheelBmp = nullptr;
static const int WHEEL_R = 56; // color wheel radius

// ── Item categories ──
enum ItemCat { CAT_WEAPON = 0, CAT_AMMO, CAT_MEDICAL, CAT_FOOD, CAT_CLOTHING, CAT_BACKPACK, CAT_ATTACHMENT, CAT_TOOL, CAT_OTHER, CAT_COUNT };
static const char* g_catNames[] = { "WEAPONS","AMMO/MAGS","MEDICAL","FOOD/DRINK","CLOTHING","BACKPACKS","ATTACHMENTS","TOOLS","OTHER" };
static COLORREF g_catColors[] = {
    RGB(255,80,80),RGB(255,180,60),RGB(80,220,120),RGB(180,140,255),
    RGB(120,180,220),RGB(200,160,100),RGB(160,160,220),RGB(180,200,140),RGB(100,110,125)
};

static int CategorizeItem(const std::string& n) {
    // Weapons
    if (n.find("Rifle") != std::string::npos || n.find("Pistol") != std::string::npos ||
        n.find("Shotgun") != std::string::npos || n.find("Launcher") != std::string::npos ||
        n.find("AKM") != std::string::npos || n.find("AK101") != std::string::npos ||
        n.find("AK74") != std::string::npos || n.find("M4A1") != std::string::npos ||
        n.find("Mosin") != std::string::npos || n.find("SVD") != std::string::npos ||
        n.find("VSS") != std::string::npos || n.find("FAL") != std::string::npos ||
        n.find("UMP") != std::string::npos || n.find("MP5") != std::string::npos ||
        n.find("Saiga") != std::string::npos || n.find("Vaiga") != std::string::npos ||
        n.find("CR527") != std::string::npos || n.find("CR61") != std::string::npos ||
        n.find("Blaze") != std::string::npos || n.find("Repeater") != std::string::npos ||
        n.find("Glock") != std::string::npos || n.find("FX45") != std::string::npos ||
        n.find("Deagle") != std::string::npos || n.find("Magnum") != std::string::npos ||
        n.find("Mlock") != std::string::npos || n.find("P1") != std::string::npos ||
        n.find("Revolver") != std::string::npos || n.find("Derringer") != std::string::npos ||
        n.find("BK18") != std::string::npos || n.find("BK43") != std::string::npos ||
        n.find("Pioneer") != std::string::npos || n.find("Sporter") != std::string::npos ||
        n.find("Trumpet") != std::string::npos || n.find("SSG82") != std::string::npos ||
        n.find("VSD") != std::string::npos || n.find("M70") != std::string::npos ||
        n.find("Scout") != std::string::npos || n.find("LeMas") != std::string::npos ||
        n.find("MKII") != std::string::npos || n.find("Longhorn") != std::string::npos ||
        n.find("_Base") != std::string::npos) return CAT_WEAPON;
    // Ammo/Mags
    if (n.find("Ammo_") != std::string::npos || n.find("Mag_") != std::string::npos ||
        n.find("Clip_") != std::string::npos || n.find("SpeedLoader") != std::string::npos ||
        n.find("CMAGx") != std::string::npos || n.find("Rnd") != std::string::npos) return CAT_AMMO;
    // Medical
    if (n.find("Bandage") != std::string::npos || n.find("Morphine") != std::string::npos ||
        n.find("Epinephrine") != std::string::npos || n.find("Saline") != std::string::npos ||
        n.find("BloodBag") != std::string::npos || n.find("Tetracycline") != std::string::npos ||
        n.find("Codeine") != std::string::npos || n.find("Vitamin") != std::string::npos ||
        n.find("Charcoal") != std::string::npos || n.find("Splint") != std::string::npos ||
        n.find("Defibrillator") != std::string::npos || n.find("BloodTest") != std::string::npos ||
        n.find("Syringe") != std::string::npos || n.find("Injection") != std::string::npos ||
        n.find("SurgicalGloves") != std::string::npos || n.find("DisinfectantSpray") != std::string::npos ||
        n.find("IodineTincture") != std::string::npos || n.find("PainkillerTablets") != std::string::npos) return CAT_MEDICAL;
    // Food/Drink
    if (n.find("Can") != std::string::npos || n.find("Apple") != std::string::npos ||
        n.find("Pear") != std::string::npos || n.find("Plum") != std::string::npos ||
        n.find("Kiwi") != std::string::npos || n.find("Tomato") != std::string::npos ||
        n.find("Pepper") != std::string::npos || n.find("Zucchini") != std::string::npos ||
        n.find("Potato") != std::string::npos || n.find("Mushroom") != std::string::npos ||
        n.find("Rice") != std::string::npos || n.find("Cereal") != std::string::npos ||
        n.find("Tuna") != std::string::npos || n.find("Sardine") != std::string::npos ||
        n.find("Bacon") != std::string::npos || n.find("Steak") != std::string::npos ||
        n.find("Fat") != std::string::npos || n.find("WaterBottle") != std::string::npos ||
        n.find("Canteen") != std::string::npos || n.find("Soda") != std::string::npos ||
        n.find("Drink") != std::string::npos || n.find("Powdered") != std::string::npos ||
        n.find("Honey") != std::string::npos || n.find("Worm") != std::string::npos ||
        n.find("Berry") != std::string::npos || n.find("PetBottle") != std::string::npos) return CAT_FOOD;
    // Backpacks
    if (n.find("Bag") != std::string::npos || n.find("Pack") != std::string::npos ||
        n.find("Pouch") != std::string::npos || n.find("Case") != std::string::npos ||
        n.find("Drysack") != std::string::npos) return CAT_BACKPACK;
    // Attachments
    if (n.find("Suppressor") != std::string::npos || n.find("Silencer") != std::string::npos ||
        n.find("Scope") != std::string::npos || n.find("Optic") != std::string::npos ||
        n.find("Handguard") != std::string::npos || n.find("Buttstock") != std::string::npos ||
        n.find("Bayonet") != std::string::npos || n.find("Compensator") != std::string::npos ||
        n.find("RailAtt") != std::string::npos || n.find("Flashlight") != std::string::npos ||
        n.find("Light_") != std::string::npos || n.find("ACOG") != std::string::npos ||
        n.find("PSO") != std::string::npos || n.find("PUScopeOptic") != std::string::npos ||
        n.find("Hunting") != std::string::npos || n.find("_Bttstck") != std::string::npos ||
        n.find("_Hndgrd") != std::string::npos) return CAT_ATTACHMENT;
    // Clothing
    if (n.find("Jacket") != std::string::npos || n.find("Pants") != std::string::npos ||
        n.find("Shirt") != std::string::npos || n.find("Boots") != std::string::npos ||
        n.find("Shoes") != std::string::npos || n.find("Gloves") != std::string::npos ||
        n.find("Hat") != std::string::npos || n.find("Helmet") != std::string::npos ||
        n.find("Vest") != std::string::npos || n.find("Hoodie") != std::string::npos ||
        n.find("Coat") != std::string::npos || n.find("Armband") != std::string::npos ||
        n.find("Balaclava") != std::string::npos || n.find("Mask") != std::string::npos ||
        n.find("Gorka") != std::string::npos || n.find("Ghillie") != std::string::npos ||
        n.find("Ushanka") != std::string::npos || n.find("Beanie") != std::string::npos ||
        n.find("Sneakers") != std::string::npos || n.find("Dress") != std::string::npos) return CAT_CLOTHING;
    // Tools
    if (n.find("Knife") != std::string::npos || n.find("Axe") != std::string::npos ||
        n.find("Hatchet") != std::string::npos || n.find("Saw") != std::string::npos ||
        n.find("Pliers") != std::string::npos || n.find("Lock") != std::string::npos ||
        n.find("Tent") != std::string::npos || n.find("Barrel") != std::string::npos ||
        n.find("Crate") != std::string::npos || n.find("Rope") != std::string::npos ||
        n.find("Battery") != std::string::npos || n.find("Duct") != std::string::npos ||
        n.find("Nails") != std::string::npos || n.find("Shovel") != std::string::npos ||
        n.find("Wrench") != std::string::npos || n.find("Compass") != std::string::npos ||
        n.find("Map") != std::string::npos || n.find("Radio") != std::string::npos ||
        n.find("Binoculars") != std::string::npos || n.find("NVG") != std::string::npos ||
        n.find("Rangefinder") != std::string::npos || n.find("HandSaw") != std::string::npos ||
        n.find("SledgeHammer") != std::string::npos || n.find("Crowbar") != std::string::npos ||
        n.find("Screwdriver") != std::string::npos || n.find("Pickaxe") != std::string::npos ||
        n.find("FishingRod") != std::string::npos || n.find("Matchbox") != std::string::npos ||
        n.find("Lighter") != std::string::npos) return CAT_TOOL;
    return CAT_OTHER;
}

struct CatItem { std::string name; int cat; };

// ── Nearby item table (Filter page) ──
static std::mutex g_itemMtx;
static std::vector<CatItem> g_nearbyItems;
static std::set<std::string> g_selectedItems;
static int g_filterScroll = 0;
static const int FILTER_ROW_H = 24;
static const int FILTER_MAX_VISIBLE = 14;

// ── Hit Log ──
struct HitEntry { int tick; float dist; std::string bone; std::string method; DWORD timestamp; };
static std::vector<HitEntry> g_hitLog;
static const int MAX_HIT_LOG = 12;
static int g_totalHits = 0;
static uintptr_t g_lastBulletTPTarget = 0;
static int g_lastBulletTPTick = 0;
static bool g_pendingHitCheck = false;

std::thread g_overlayThread;

// ═══════════════════════════════════════════════════════════════
//  SLIDER SYSTEM
// ═══════════════════════════════════════════════════════════════
struct SliderDef {
    const char* label;
    float* val;
    float lo, hi, step;
    COLORREF col;
    const char* unit;
    int decimals;
};
static std::vector<SliderDef> g_aimSliders;
static int g_sliderDrag = -1;

static const int SL_H = 46;
static const int SL_LABEL_H = 16;
static const int SL_TRACK_Y = 30;
static const int SL_TRACK_H = 6;
static const int SL_KNOB_R = 7;
static const int SL_PAD = 20;

// ═══════════════════════════════════════════════════════════════
//  LANDMARKS
// ═══════════════════════════════════════════════════════════════
enum LandmarkCat { LM_CITY = 0, LM_TOWN = 1, LM_MILI = 2 };
struct Landmark { const char* name; float x, z; int cat; };
static const Landmark g_landmarks[] = {
    {"Cherno",6649,2476,LM_CITY},{"Elektro",10348,2154,LM_CITY},{"Berezino",12431,9716,LM_CITY},
    {"Solnechny",13323,6050,LM_CITY},{"Svetlo",14052,13335,LM_CITY},{"Novodmitrovsk",11366,14360,LM_CITY},
    {"Severograd",8014,12732,LM_CITY},{"Novo Petrovka",3437,13007,LM_CITY},{"Zelenogorsk",2760,5320,LM_CITY},
    {"Stary Sobor",6090,7750,LM_CITY},{"Krasnostav",11127,12220,LM_CITY},{"Vybor",3784,8937,LM_CITY},
    {"Gorka",9558,8886,LM_TOWN},{"Novy Sobor",7073,7633,LM_TOWN},{"Kabanino",5316,8624,LM_TOWN},
    {"Polyana",10576,8032,LM_TOWN},{"Dubrovka",10420,9936,LM_TOWN},{"Kamyshovo",12198,3438,LM_TOWN},
    {"Kamenka",1887,2240,LM_TOWN},{"Komarovo",3643,2409,LM_TOWN},{"Balota",4523,2418,LM_TOWN},
    {"Dolina",11260,6623,LM_TOWN},{"Staroye",10148,5460,LM_TOWN},{"Mogilevka",7532,5078,LM_TOWN},
    {"Lopatino",2720,10042,LM_TOWN},{"Myshkino",2071,7367,LM_TOWN},{"Pavlovo",1710,3883,LM_TOWN},
    {"Pustoshka",3016,7866,LM_TOWN},
    {"NWAF",4474,10239,LM_MILI},{"NEAF",12054,12467,LM_MILI},{"Balota AF",4468,2356,LM_MILI},
    {"Tisy Base",1550,14122,LM_MILI},{"Kamensk Base",7861,14472,LM_MILI},{"Green Mtn",3706,5985,LM_MILI},
    {"Devils Castle",6900,11475,LM_MILI},{"Troitskoe",8726,13300,LM_MILI},{"Prison Island",2680,1198,LM_MILI},
    {"Skalisty Isl",13691,2989,LM_MILI},{"Rify",13838,11219,LM_MILI},{"Pavlovo Mil",2076,3682,LM_MILI},
    {"Myshkino Tents",1870,7258,LM_MILI},{"Zeleno Mil",2420,5228,LM_MILI},
};
static constexpr int NUM_LANDMARKS = sizeof(g_landmarks) / sizeof(g_landmarks[0]);

// ═══════════════════════════════════════════════════════════════
//  WELL PUMPS
// ═══════════════════════════════════════════════════════════════
struct WellPos { float x, z; };
static const WellPos g_wells[] = {
    {593.1f,5287.2f},{1161.7f,10004.2f},{1201.9f,8765.3f},{1460.3f,11951.2f},
    {1672.9f,3843.0f},{1914.5f,2231.6f},{1991.3f,7307.8f},{2215.1f,11098.6f},
    {2550.5f,6369.2f},{2715.8f,5292.6f},{2752.3f,9985.0f},{2995.2f,7824.3f},
    {3347.1f,3910.2f},{3435.8f,13052.6f},{3654.9f,2464.5f},{3738.8f,8957.8f},
    {4371.4f,4657.9f},{4402.5f,2483.4f},{4483.1f,6425.2f},{4798.2f,6826.4f},
    {4900.3f,5678.1f},{4904.2f,13050.7f},{4982.0f,15134.5f},{5407.0f,8566.8f},
    {5816.2f,13572.7f},{5863.0f,4815.9f},{5972.8f,10311.7f},{6039.2f,3257.4f},
    {6095.2f,7738.7f},{6297.7f,12666.7f},{6515.3f,2307.5f},{6541.5f,6090.2f},
    {6643.5f,14375.7f},{7080.8f,4308.8f},{7152.9f,7702.9f},{7215.6f,6994.0f},
    {7535.5f,5138.3f},{7576.6f,13467.5f},{8080.8f,10916.7f},{8065.3f,12637.2f},
    {8140.4f,3226.5f},{8191.8f,11580.7f},{8429.0f,6675.2f},{8469.9f,13964.7f},
    {8595.6f,11930.1f},{9164.5f,3895.7f},{9384.6f,14551.1f},{9464.4f,8823.9f},
    {9478.2f,13803.4f},{9661.4f,6562.0f},{9800.3f,2216.4f},{9958.3f,10386.1f},
    {10127.5f,5484.5f},{10424.1f,2215.9f},{10426.2f,9826.8f},{10685.3f,8065.5f},
    {10763.4f,14364.7f},{11001.1f,12417.1f},{11193.9f,6573.5f},{11266.9f,5484.0f},
    {11544.8f,14798.8f},{11902.6f,9149.7f},{12051.5f,3593.8f},{12090.8f,7217.7f},
    {12103.0f,13718.8f},{12255.2f,10626.8f},{12315.5f,10938.4f},{12442.6f,9563.0f},
    {12719.9f,14577.7f},{12876.5f,4437.4f},{12888.8f,5649.1f},{12956.3f,8026.3f},
    {12920.1f,10145.4f},{13360.7f,12866.2f},{13391.9f,6276.5f},{13462.8f,2940.9f},
    {13512.0f,14039.1f},{13800.4f,13297.8f},{14074.6f,15031.7f},
};
static constexpr int NUM_WELLS = sizeof(g_wells) / sizeof(g_wells[0]);

// ═══════════════════════════════════════════════════════════════
//  XOR + MATH + STRINGS
// ═══════════════════════════════════════════════════════════════
constexpr char XOR_KEY = 0xAA;
template <size_t N> constexpr std::array<char, N> xor_encrypt(const char(&s)[N]) {
    std::array<char, N> e{}; for (size_t i = 0; i < N; ++i)e[i] = s[i] ^ XOR_KEY; return e;
}
inline std::string xor_decrypt(const char* e, size_t l) { std::string d; d.reserve(l); for (size_t i = 0; i < l; ++i)d += e[i] ^ XOR_KEY; return d; }
#define XORS(s) []()->std::string{constexpr auto e=xor_encrypt(s);return xor_decrypt(e.data(),e.size()-1);}()

inline float dot(const vector3& a, const vector3& b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
inline vector3 normalize(const vector3& v) { float l = sqrtf(dot(v, v)); if (l < 0.0001f)return{ 0,0,0 }; return{ v.x / l,v.y / l,v.z / l }; }
inline vector3 cross(const vector3& a, const vector3& b) { return{ a.y * b.z - a.z * b.y,a.z * b.x - a.x * b.z,a.x * b.y - a.y * b.x }; }

static bool strContainsCI(const std::string& haystack, const std::string& needle) {
    if (needle.empty()) return true;
    auto it = std::search(haystack.begin(), haystack.end(), needle.begin(), needle.end(),
        [](char a, char b) { return toupper(a) == toupper(b); });
    return it != haystack.end();
}

struct CharBlock64 { char c[64]; };
struct CharBlock128 { char c[128]; };

static std::string ReadRemoteChars(uintptr_t addr, size_t maxLen, int d) {
    if (!addr || addr < 0x10000 || maxLen == 0 || maxLen > 256) return "";
    std::string r;
    if (maxLen <= 64) {
        CharBlock64 blk = read<CharBlock64>(addr, d);
        r.reserve(maxLen);
        for (size_t i = 0; i < maxLen; i++) {
            char c = blk.c[i];
            if (c == '\0' || c < 0x20 || c > 0x7E) break;
            r += c;
        }
    }
    else if (maxLen <= 128) {
        CharBlock128 blk = read<CharBlock128>(addr, d);
        r.reserve(maxLen);
        for (size_t i = 0; i < maxLen; i++) {
            char c = blk.c[i];
            if (c == '\0' || c < 0x20 || c > 0x7E) break;
            r += c;
        }
    }
    else {
        // Fallback for >128 (rare)
        r.reserve(maxLen);
        for (size_t off = 0; off < maxLen; off += 64) {
            CharBlock64 blk = read<CharBlock64>(addr + off, d);
            for (size_t i = 0; i < 64 && (off + i) < maxLen; i++) {
                char c = blk.c[i];
                if (c == '\0' || c < 0x20 || c > 0x7E) return r;
                r += c;
            }
        }
    }
    return r;
}
static std::string ReadArmaString(uintptr_t p, int d) {
    if (!p || p < 0x10000)return""; int l = read<int>(p + 0x8, d); if (l <= 0 || l > 128)return"";
    return ReadRemoteChars(p + 0x10, (size_t)l, d);
}
static std::string ReadEnfusionString(uintptr_t stringClassPtr, int d) {
    if (!stringClassPtr || stringClassPtr < 0x10000000ULL)return"";
    return ReadRemoteChars(stringClassPtr + 0x10, 64, d);
}
static std::string ReadItemTypeName(uintptr_t ent, int d) {
    uintptr_t ti = read<uintptr_t>(ent + 0x180, d);
    if (ti && ti > 0x10000000ULL) {
        uintptr_t on = read<uintptr_t>(ti + 0x70, d);
        if (on && on > 0x10000000ULL) { std::string n = ReadEnfusionString(on, d); if (!n.empty() && n.length() > 1)return n; }
        uintptr_t cn = read<uintptr_t>(ti + 0xA8, d);
        if (cn && cn > 0x10000000ULL) { std::string n = ReadEnfusionString(cn, d); if (!n.empty() && n.length() > 1)return n; }
    }
    uintptr_t ht = read<uintptr_t>(ent + 0xA8, d);
    if (ht && ht > 0x10000000ULL) {
        uintptr_t on = read<uintptr_t>(ht + 0x70, d);
        if (on && on > 0x10000000ULL) { std::string n = ReadEnfusionString(on, d); if (!n.empty() && n.length() > 1)return n; }
    }
    return "";
}

static std::string ReadWeaponInHand(uintptr_t playerEnt, int d) {
    uintptr_t inv = read<uintptr_t>(playerEnt + Off::Inventory, d);
    if (!inv || inv < 0x10000000ULL) return "";
    uintptr_t hands = read<uintptr_t>(inv + Off::InvHands, d);
    if (!hands || hands < 0x10000000ULL) return "";
    // Only return name if item has ammo chain (= it's a firearm)
    uintptr_t ammo1 = read<uintptr_t>(hands + Off::AmmoType1, d);
    if (!ammo1 || ammo1 < 0x10000000ULL) return "";
    return ReadItemTypeName(hands, d);
}

// ── Read all items attached to a player (worn + cargo) ──
// DayZ 1.28 Enfusion inventory structure:
//   entity + 0x660 → InventoryComponent
//   InventoryComponent has:
//     +0x1B0 → hands item (EntityAI*)
//     +0x138 → attachment slot array base (slot_t*)
//     +0x140 → attachment slot count (u32)
//   slot_t is { u32 id; u32 pad; u64 item_ptr; } = 0x10 bytes
//
//   Each attached item (clothing etc.) has its own inventory:
//     item + 0x660 → sub-InventoryComponent
//     sub-inv + 0x148 → CargoGrid ptr
//     CargoGrid + 0x38 → item pointer array
//     CargoGrid + 0x40 → item count
//
// We read: hands item, all attachment slots (worn gear),
// and optionally one level of cargo inside each attachment.
static std::vector<std::string> ReadPlayerInventory(uintptr_t playerEnt, int d) {
    std::vector<std::string> result;

    uintptr_t inv = read<uintptr_t>(playerEnt + Off::ItemInventory, d);
    if (!inv || inv < 0x10000000ULL) return result;

    // 1) Hands item
    uintptr_t hands = read<uintptr_t>(inv + Off::InvHands, d);
    if (hands && hands > 0x10000000ULL && hands < 0x7FFFFFFFFFFFF000ULL) {
        std::string n = ReadItemTypeName(hands, d);
        if (!n.empty() && n.length() > 1) result.push_back("[H] " + n);
    }

    // 2) Walk attachment slots (worn gear: vest, backpack, clothing, etc.)
    //    slot_t { u32 id; u32 pad; u64 item_ptr; } stride=0x10
    uintptr_t slotBase = read<uintptr_t>(inv + Off::AttachStart, d);
    uint32_t slotCount = read<uint32_t>(inv + Off::AttachSize, d);

    // Sanity: clamp to reasonable range
    if (slotCount > 24) slotCount = 0;
    if (!slotBase || slotBase < 0x10000000ULL) slotCount = 0;

    std::vector<uintptr_t> attachedItems; // save for cargo scan

    for (uint32_t i = 0; i < slotCount && result.size() < 20; i++) {
        uintptr_t slotAddr = slotBase + i * Off::AttachSlotStride;
        uintptr_t itemPtr = read<uintptr_t>(slotAddr + Off::AttachSlotItemOff, d);
        if (!itemPtr || itemPtr < 0x10000000ULL || itemPtr > 0x7FFFFFFFFFFFF000ULL) continue;
        if (itemPtr == hands) continue; // already listed

        // Validate: must have readable TypeInfo at +0x180
        uintptr_t ti = read<uintptr_t>(itemPtr + 0x180, d);
        if (!ti || ti < 0x10000000ULL) continue;

        std::string n = ReadItemTypeName(itemPtr, d);
        if (n.empty() || n.length() <= 1 || n == "Item") continue;

        result.push_back(n);
        attachedItems.push_back(itemPtr);
    }

    // 3) One level of cargo inside each attachment (items inside backpack, vest, etc.)
    for (uintptr_t attachEnt : attachedItems) {
        if (result.size() >= 20) break;

        uintptr_t subInv = read<uintptr_t>(attachEnt + Off::ItemInventory, d);
        if (!subInv || subInv < 0x10000000ULL) continue;

        uintptr_t cargoGrid = read<uintptr_t>(subInv + Off::CargoGrid, d);
        if (!cargoGrid || cargoGrid < 0x10000000ULL) continue;

        uintptr_t itemList = read<uintptr_t>(cargoGrid + Off::CargoItemList, d);
        if (!itemList || itemList < 0x10000000ULL) continue;

        // No confirmed count offset for cargo — iterate until null, max 32
        for (uint32_t j = 0; j < 32 && result.size() < 20; j++) {
            uintptr_t cItem = read<uintptr_t>(itemList + j * 0x8, d);
            if (!cItem || cItem < 0x10000000ULL || cItem > 0x7FFFFFFFFFFFF000ULL) continue;

            uintptr_t cti = read<uintptr_t>(cItem + 0x180, d);
            if (!cti || cti < 0x10000000ULL) continue;

            std::string cn = ReadItemTypeName(cItem, d);
            if (cn.empty() || cn.length() <= 1 || cn == "Item") continue;

            // Dedupe
            bool dup = false;
            for (auto& r : result) {
                // Compare without [H] prefix
                std::string cmp = r;
                if (cmp.substr(0, 4) == "[H] ") cmp = cmp.substr(4);
                if (cmp == cn) { dup = true; break; }
            }
            if (!dup) result.push_back("  " + cn); // indented = cargo item
        }
    }

    return result;
}

// ═══════════════════════════════════════════════════════════════
//  BONES (types declared above CachedEntEntry, indices + links here)
// ═══════════════════════════════════════════════════════════════
static const int g_humanBoneIdx[PB_MAX] = { 24,21,20,19,0,61,94,63,97,65,99,1,9,4,12,6,14 };
static const int g_zombieBoneIdx[PB_MAX] = { 21,19,16,15,0,24,56,25,59,27,60,1,9,4,12,6,13 };
struct BoneLink { int from; int to; };
static const BoneLink g_boneLinks[] = {
    {PB_NECK,PB_SPINE2},{PB_SPINE2,PB_SPINE1},{PB_SPINE1,PB_PELVIS},
    {PB_NECK,PB_L_SHOULDER},{PB_L_SHOULDER,PB_L_ELBOW},{PB_L_ELBOW,PB_L_HAND},
    {PB_NECK,PB_R_SHOULDER},{PB_R_SHOULDER,PB_R_ELBOW},{PB_R_ELBOW,PB_R_HAND},
    {PB_PELVIS,PB_L_HIP},{PB_L_HIP,PB_L_KNEE},{PB_L_KNEE,PB_L_FOOT},
    {PB_PELVIS,PB_R_HIP},{PB_R_HIP,PB_R_KNEE},{PB_R_KNEE,PB_R_FOOT},
};
static constexpr int NUM_BONE_LINKS = sizeof(g_boneLinks) / sizeof(g_boneLinks[0]);

static bool g_boneDbgDone = false;

// ═══════════════════════════════════════════════════════════════
//  GAME FRAME — FULLY BATCHED (~12 driver calls instead of ~300)
// ═══════════════════════════════════════════════════════════════
struct EntityData {
    uintptr_t ptr, visualState;
    vector3 pos, headPos;
    float dist;
    bool isPlayer, isZombie, isItem;
    int catIdx;  // item category index for colored ESP (0-8, -1 if n/a)
    std::string name;
    std::string weaponName;
    std::vector<std::string> inventory;  // player inventory items (if g_showPlayerInv)
    BonePos bones[PB_MAX];
    bool hasBones;
};
struct FrameData {
    bool valid; uintptr_t world, camPtr;
    vector3 camPos, camRight, camUp, camForward;
    float projD1, projD2;
    vector3 localPos;
    std::vector<EntityData> entities;
};

static FrameData ReadGameFrame(int dayzid, int screenW, int screenH, int tick) {
    FrameData f = {}; f.valid = false;
    static BatchCtx B;

    // ── PHASE 0: World pointer (single read) ──
    f.world = read<uintptr_t>(base + Off::World, dayzid);
    if (!f.world) return f;

    // ── PHASE 1: Camera ptr + entity list metadata (5 reads → 1 batch) ──
    B.reset();
    int _cam = B.add(f.world + Off::Camera, 8);
    int _nBase = B.add(f.world + Off::NearBase, 8);
    int _nCnt = B.add(f.world + Off::NearCount, 4);
    int _fBase = B.add(f.world + Off::FarBase, 8);
    int _fCnt = B.add(f.world + Off::FarCount, 4);
    if (!B.execute(dayzid)) return f;

    uintptr_t cam = B.get<uintptr_t>(_cam);
    if (!cam) return f;
    f.camPtr = cam;
    uintptr_t nBase = B.get<uintptr_t>(_nBase);
    int nCount = (std::max)(0, (std::min)(B.get<int>(_nCnt), 512));
    uintptr_t fBase = B.get<uintptr_t>(_fBase);
    int fCount = (std::max)(0, (std::min)(B.get<int>(_fCnt), 512));

    // ── PHASE 2: Camera vectors + projection (5 reads → 1 batch) ──
    B.reset();
    int _cPos = B.add(cam + Off::CamPos, 12);
    int _cR = B.add(cam + Off::CamRight, 12);
    int _cU = B.add(cam + Off::CamUp, 12);
    int _cF = B.add(cam + Off::CamForward, 12);
    int _pX = B.add(cam + Off::ProjX, 4);
    if (!B.execute(dayzid)) return f;

    f.camPos = B.get<vector3>(_cPos);
    f.camRight = normalize(B.get<vector3>(_cR));
    f.camUp = normalize(B.get<vector3>(_cU));
    f.camForward = normalize(B.get<vector3>(_cF));
    f.projD1 = B.get<float>(_pX);
    f.projD2 = f.projD1 * ((float)screenW / (float)screenH);
    f.localPos = f.camPos;

    bool wantBones = g_showBones.load() || g_showZombieBones.load();
    bool wantItems = g_showItems.load();

    // ── PHASE 3: Entity pointers (batched, chunked at 200) ──
    int totalList = nCount + fCount;
    std::vector<uintptr_t> allEnts;
    allEnts.reserve(totalList);

    for (int off = 0; off < totalList; off += BatchCtx::MAX_ENTRIES) {
        int chunk = (std::min)(totalList - off, BatchCtx::MAX_ENTRIES);
        B.reset();
        int sl[200];
        for (int i = 0; i < chunk; i++) {
            int gi = off + i;
            uintptr_t addr = (gi < nCount)
                ? nBase + gi * 8
                : fBase + (gi - nCount) * 8;
            sl[i] = B.add(addr, 8);
        }
        B.execute(dayzid);
        for (int i = 0; i < chunk; i++) {
            uintptr_t e = B.get<uintptr_t>(sl[i]);
            if (e && e > 0x10000000ULL) allEnts.push_back(e);
        }
    }

    // ── PHASE 4: VisualState + skeleton pointers (3×8B per ent → 66 per batch) ──
    struct EMeta { uintptr_t ptr, vs, zsk, psk; };
    std::vector<EMeta> metas;
    metas.reserve(allEnts.size());

    for (size_t off = 0; off < allEnts.size(); off += 66) {
        size_t end = (std::min)(off + (size_t)66, allEnts.size());
        int n = (int)(end - off);
        B.reset();
        struct S3 { int vs, zsk, psk; };
        S3 s3[66];
        for (int i = 0; i < n; i++) {
            s3[i].vs = B.add(allEnts[off + i] + Off::VisualState, 8);
            s3[i].zsk = B.add(allEnts[off + i] + Off::ZombieSkeleton, 8);
            s3[i].psk = B.add(allEnts[off + i] + Off::PlayerSkeleton, 8);
        }
        B.execute(dayzid);
        for (int i = 0; i < n; i++) {
            EMeta m;
            m.ptr = allEnts[off + i];
            m.vs = B.get<uintptr_t>(s3[i].vs);
            m.zsk = B.get<uintptr_t>(s3[i].zsk);
            m.psk = B.get<uintptr_t>(s3[i].psk);
            if (m.vs && m.vs > 0x10000000ULL) metas.push_back(m);
        }
    }

    // ── PHASE 5: Positions — filter by distance (200 per batch) ──
    struct PassedEnt { size_t metaIdx; vector3 pos; float dist; bool isZ, isP; };
    std::vector<PassedEnt> passed;
    passed.reserve(metas.size());

    for (size_t off = 0; off < metas.size(); off += 200) {
        size_t end = (std::min)(off + (size_t)200, metas.size());
        int n = (int)(end - off);
        B.reset();
        int psl[200];
        for (int i = 0; i < n; i++)
            psl[i] = B.add(metas[off + i].vs + Off::GetCoord, 12);
        B.execute(dayzid);
        for (int i = 0; i < n; i++) {
            vector3 pos = B.get<vector3>(psl[i]);
            float dx = pos.x - f.camPos.x, dy = pos.y - f.camPos.y, dz = pos.z - f.camPos.z;
            float dist = sqrtf(dx * dx + dy * dy + dz * dz);
            if (dist < 0.5f || dist > 2900.f) continue;

            auto& m = metas[off + i];
            bool isZ = (m.zsk && m.zsk > 0x10000000ULL);
            bool isP = (!isZ && m.psk && m.psk > 0x10000000ULL);
            if (!isZ && !isP) continue;

            PassedEnt pe;
            pe.metaIdx = off + i;
            pe.pos = pos; pe.dist = dist; pe.isZ = isZ; pe.isP = isP;
            passed.push_back(pe);
        }
    }

    // ── Build EntityData for all passed entities ──
    f.entities.reserve(passed.size());
    std::vector<size_t> passedToEntity(passed.size());

    for (size_t i = 0; i < passed.size(); i++) {
        auto& pe = passed[i];
        auto& m = metas[pe.metaIdx];
        EntityData ed = {};
        ed.ptr = m.ptr; ed.visualState = m.vs;
        ed.pos = pe.pos;
        ed.headPos = { pe.pos.x, pe.pos.y + 1.7f, pe.pos.z };
        ed.dist = pe.dist;
        ed.isPlayer = pe.isP; ed.isZombie = pe.isZ;
        ed.isItem = false; ed.hasBones = false;
        passedToEntity[i] = f.entities.size();
        f.entities.push_back(std::move(ed));
    }

    // ── PHASE 6-8: Batched bone reading ──
    if (wantBones) {
        struct BoneCandidate {
            size_t passedIdx;
            uintptr_t skelPtr;
            bool isZombie;
        };
        std::vector<BoneCandidate> boneCands;
        for (size_t i = 0; i < passed.size(); i++) {
            if (passed[i].dist >= 500.f) continue;
            auto& m = metas[passed[i].metaIdx];
            BoneCandidate bc;
            bc.passedIdx = i;
            bc.isZombie = passed[i].isZ;
            bc.skelPtr = bc.isZombie ? m.zsk : m.psk;
            if (bc.skelPtr && bc.skelPtr > 0x10000000ULL)
                boneCands.push_back(bc);
        }

        if (!boneCands.empty()) {
            // Step A: animClass ptrs (200 per batch)
            std::vector<uintptr_t> animClasses(boneCands.size(), 0);
            for (size_t off = 0; off < boneCands.size(); off += 200) {
                size_t end = (std::min)(off + (size_t)200, boneCands.size());
                int n = (int)(end - off);
                B.reset();
                int acsl[200];
                for (int i = 0; i < n; i++)
                    acsl[i] = B.add(boneCands[off + i].skelPtr + Off::AnimClassOff, 8);
                B.execute(dayzid);
                for (int i = 0; i < n; i++)
                    animClasses[off + i] = B.get<uintptr_t>(acsl[i]);
            }

            // Step B: matrixClass ptrs
            std::vector<uintptr_t> matrixClasses(boneCands.size(), 0);
            {
                B.reset();
                int mcsl[200]; int mcCount = 0;
                size_t mcMap[200];
                for (size_t i = 0; i < boneCands.size() && mcCount < 200; i++) {
                    if (animClasses[i] && animClasses[i] > 0x10000000ULL) {
                        mcMap[mcCount] = i;
                        mcsl[mcCount] = B.add(animClasses[i] + Off::MatrixClassOff, 8);
                        mcCount++;
                    }
                }
                B.execute(dayzid);
                for (int i = 0; i < mcCount; i++)
                    matrixClasses[mcMap[i]] = B.get<uintptr_t>(mcsl[i]);
            }

            // One-time bone debug
            if (!g_boneDbgDone && !boneCands.empty()) {
                for (size_t i = 0; i < boneCands.size(); i++) {
                    if (matrixClasses[i] && matrixClasses[i] > 0x10000000ULL) {
                        g_boneDbgDone = true;
                        auto& m = metas[passed[boneCands[i].passedIdx].metaIdx];
                        printf("\n[BONE] entity=%p sk=%p ac=%p mc=%p\n",
                            (void*)m.ptr, (void*)boneCands[i].skelPtr,
                            (void*)animClasses[i], (void*)matrixClasses[i]);
                        break;
                    }
                }
            }

            // Step C: Entity matrices + bone locals (18 entries per ent → 11 per batch)
            const int BONE_CHUNK = 11;
            for (size_t off = 0; off < boneCands.size(); off += BONE_CHUNK) {
                size_t end = (std::min)(off + (size_t)BONE_CHUNK, boneCands.size());
                B.reset();

                struct BoneSlots { int matSlot; int localSlots[PB_MAX]; bool valid; };
                BoneSlots bsl[11];

                for (size_t i = off; i < end; i++) {
                    int li = (int)(i - off);
                    uintptr_t mc = matrixClasses[i];
                    bsl[li].valid = (mc && mc > 0x10000000ULL);
                    if (!bsl[li].valid) continue;

                    auto& m = metas[passed[boneCands[i].passedIdx].metaIdx];
                    bsl[li].matSlot = B.add(m.vs + 0x8, 48);

                    const int* boneIdx = boneCands[i].isZombie ? g_zombieBoneIdx : g_humanBoneIdx;
                    for (int b = 0; b < PB_MAX; b++)
                        bsl[li].localSlots[b] = B.add(mc + 0x54 + boneIdx[b] * sizeof(Matrix3x4), 12);
                }
                B.execute(dayzid);

                for (size_t i = off; i < end; i++) {
                    int li = (int)(i - off);
                    if (!bsl[li].valid) continue;

                    Matrix3x4 em = B.get<Matrix3x4>(bsl[li].matSlot);
                    if (isnan(em.m[9]) || isnan(em.m[10]) || isnan(em.m[11])) continue;
                    if (fabsf(em.m[9]) < 0.01f && fabsf(em.m[10]) < 0.01f && fabsf(em.m[11]) < 0.01f) continue;

                    size_t eIdx = passedToEntity[boneCands[i].passedIdx];
                    EntityData& ed = f.entities[eIdx];
                    bool anyValid = false;

                    for (int b = 0; b < PB_MAX; b++) {
                        vector3 local = B.get<vector3>(bsl[li].localSlots[b]);
                        if (isnan(local.x) || isnan(local.y) || isnan(local.z)) {
                            ed.bones[b].valid = false; continue;
                        }
                        ed.bones[b].pos.x = em.m[0] * local.x + em.m[3] * local.y + em.m[6] * local.z + em.m[9];
                        ed.bones[b].pos.y = em.m[1] * local.x + em.m[4] * local.y + em.m[7] * local.z + em.m[10];
                        ed.bones[b].pos.z = em.m[2] * local.x + em.m[5] * local.y + em.m[8] * local.z + em.m[11];
                        float bx = ed.bones[b].pos.x - em.m[9];
                        float by = ed.bones[b].pos.y - em.m[10];
                        float bz = ed.bones[b].pos.z - em.m[11];
                        ed.bones[b].valid = (fabsf(bx) < 5.0f && fabsf(by) < 5.0f && fabsf(bz) < 5.0f);
                        if (ed.bones[b].valid) anyValid = true;
                    }
                    ed.hasBones = anyValid;
                }
            }
        }
    }

    // ── PHASE 9: Weapon in hand (cached, individual reads) ──
    for (auto& ed : f.entities) {
        if (!ed.isPlayer || ed.dist >= 800.f) continue;
        auto wit = g_weaponCache.find(ed.ptr);
        if (wit == g_weaponCache.end() || (tick - wit->second.lastTick) > WEAPON_REFRESH_TICKS) {
            std::string wn = ReadWeaponInHand(ed.ptr, dayzid);
            g_weaponCache[ed.ptr] = { wn, tick };
            ed.weaponName = std::move(wn);
        }
        else {
            ed.weaponName = wit->second.name;
        }
    }

    // ── PHASE 9b: Player inventory (cached, refresh every ~4s) ──
    if (g_showPlayerInv.load()) {
        static constexpr int INV_REFRESH = 120; // ~4s at 30fps read rate
        for (auto& ed : f.entities) {
            if (!ed.isPlayer || ed.dist >= 300.f) continue;
            ed.catIdx = -1; // players don't have item category
            auto ic = g_invCache.find(ed.ptr);
            if (ic == g_invCache.end() || (tick - ic->second.first) > INV_REFRESH) {
                try {
                    auto items = ReadPlayerInventory(ed.ptr, dayzid);
                    g_invCache[ed.ptr] = { tick, items };
                    ed.inventory = std::move(items);
                }
                catch (...) {
                    ed.inventory.clear();
                }
            }
            else {
                ed.inventory = ic->second.second;
            }
        }
    }

    // ── PHASE 10-12: Item table (batched) ──
    if (wantItems) {
        B.reset();
        int _iTable = B.add(f.world + Off::ItemTable, 8);
        int _iCount = B.add(f.world + Off::ItemTableSize, 4);
        B.execute(dayzid);
        uintptr_t itemTable = B.get<uintptr_t>(_iTable);
        int rawItemCount = B.get<int>(_iCount);
        int itemCount = (std::max)(0, (std::min)(rawItemCount, 2048));

        std::set<std::string> uniqueNames;
        std::vector<CatItem> catItems;

        static std::unordered_map<uintptr_t, std::string> s_itemNameCache;
        static int s_itemCacheTick = 0;
        if (tick - s_itemCacheTick > T_10S) { s_itemNameCache.clear(); s_itemCacheTick = tick; }

        if (itemTable && itemTable > 0x10000000ULL && itemCount > 0) {
            // Step A: Batch read flags + entity ptrs (2 per item → 100 per batch)
            struct ItemEntry { uint32_t flag; uintptr_t ent; };
            std::vector<ItemEntry> itemEntries;
            itemEntries.reserve(itemCount);

            for (int off = 0; off < itemCount; off += 100) {
                int chunk = (std::min)(itemCount - off, 100);
                B.reset();
                struct IS { int flag, ent; };
                IS isl[100];
                for (int i = 0; i < chunk; i++) {
                    uintptr_t base2 = itemTable + (off + i) * 0x18;
                    isl[i].flag = B.add(base2, 4);
                    isl[i].ent = B.add(base2 + 0x8, 8);
                }
                B.execute(dayzid);
                for (int i = 0; i < chunk; i++) {
                    uint32_t fl = B.get<uint32_t>(isl[i].flag);
                    uintptr_t en = B.get<uintptr_t>(isl[i].ent);
                    if (fl == 1 && en && en > 0x10000000ULL)
                        itemEntries.push_back({ fl, en });
                }
            }

            // Step B: Batch read VS ptrs
            struct ItemVS { uintptr_t ent, vs; };
            std::vector<ItemVS> itemVSes;
            itemVSes.reserve(itemEntries.size());

            for (size_t off = 0; off < itemEntries.size(); off += 200) {
                size_t end = (std::min)(off + (size_t)200, itemEntries.size());
                int n = (int)(end - off);
                B.reset();
                int vsl[200];
                for (int i = 0; i < n; i++)
                    vsl[i] = B.add(itemEntries[off + i].ent + Off::VisualState, 8);
                B.execute(dayzid);
                for (int i = 0; i < n; i++) {
                    uintptr_t vs = B.get<uintptr_t>(vsl[i]);
                    if (vs && vs > 0x10000000ULL)
                        itemVSes.push_back({ itemEntries[off + i].ent, vs });
                }
            }

            // Step C: Batch read positions
            struct ItemPos { uintptr_t ent, vs; vector3 pos; float dist; };
            std::vector<ItemPos> nearItems;

            for (size_t off = 0; off < itemVSes.size(); off += 200) {
                size_t end = (std::min)(off + (size_t)200, itemVSes.size());
                int n = (int)(end - off);
                B.reset();
                int psl2[200];
                for (int i = 0; i < n; i++)
                    psl2[i] = B.add(itemVSes[off + i].vs + Off::GetCoord, 12);
                B.execute(dayzid);
                for (int i = 0; i < n; i++) {
                    vector3 pos = B.get<vector3>(psl2[i]);
                    float dx2 = pos.x - f.camPos.x, dy2 = pos.y - f.camPos.y, dz2 = pos.z - f.camPos.z;
                    float dist2 = sqrtf(dx2 * dx2 + dy2 * dy2 + dz2 * dz2);
                    if (dist2 < 0.5f || dist2 > 150.f) continue;
                    nearItems.push_back({ itemVSes[off + i].ent, itemVSes[off + i].vs, pos, dist2 });
                }
            }

            // Step D: Names + build entities — DEBUG + DATA RACE FIX
            // FIX: Snapshot filter+selection under mutex (prevents race with menu thread)
            std::string filterSnap;
            std::set<std::string> selectedSnap;
            {
                std::lock_guard<std::mutex> lk(g_itemMtx);
                filterSnap = g_espFilterText;
                selectedSnap = g_selectedItems;
            }
            int itemsNamed = 0, itemsFailed = 0;
            for (auto& item : nearItems) {
                std::string itemName;
                auto ncIt = s_itemNameCache.find(item.ent);
                if (ncIt != s_itemNameCache.end()) {
                    itemName = ncIt->second;
                }
                else {
                    try { itemName = ReadItemTypeName(item.ent, dayzid); }
                    catch (...) {
                        printf("[ITEM-DBG] EXCEPTION in ReadItemTypeName ent=%p\n", (void*)item.ent);
                        fflush(stdout); itemName = ""; itemsFailed++;
                    }
                    if (itemName.empty()) itemName = "Item";
                    s_itemNameCache[item.ent] = itemName;
                }
                itemsNamed++;
                try {
                    if (uniqueNames.insert(itemName).second)
                        catItems.push_back({ itemName, CategorizeItem(itemName) });
                }
                catch (...) {
                    printf("[ITEM-DBG] EXCEPTION in CategorizeItem\n"); fflush(stdout);
                }
                if (!filterSnap.empty() && !strContainsCI(itemName, filterSnap)) continue;
                if (!selectedSnap.empty() && selectedSnap.find(itemName) == selectedSnap.end()) continue;
                EntityData ed = {};
                ed.ptr = item.ent; ed.visualState = item.vs; ed.pos = item.pos;
                ed.headPos = { item.pos.x, item.pos.y + 0.3f, item.pos.z };
                ed.dist = item.dist; ed.isPlayer = false; ed.isZombie = false;
                ed.isItem = true; ed.hasBones = false; ed.name = itemName;
                ed.catIdx = CategorizeItem(itemName);
                f.entities.push_back(std::move(ed));
            }
        }
        std::sort(catItems.begin(), catItems.end(), [](const CatItem& a, const CatItem& b) {
            if (a.cat != b.cat) return a.cat < b.cat;
            return a.name < b.name;
            });
        {
            std::lock_guard<std::mutex> lk(g_itemMtx);
            g_nearbyItems = std::move(catItems);
        }
    }
    f.valid = true; return f;
}


// ── Cache management ──
static bool IsFrameFresh(const FrameData& f) {
    // Reject if camera position is at origin (driver returned zeros)
    if (fabsf(f.camPos.x) < 1.f && fabsf(f.camPos.z) < 1.f) return false;
    return f.valid;
}

static void UpdateCache(const FrameData& f, int tick) {
    // Camera
    g_camCache.world = f.world;
    g_camCache.camPtr = f.camPtr;
    g_camCache.camPos = f.camPos;
    g_camCache.camRight = f.camRight;
    g_camCache.camUp = f.camUp;
    g_camCache.camForward = f.camForward;
    g_camCache.projD1 = f.projD1;
    g_camCache.projD2 = f.projD2;
    g_camCache.localPos = f.localPos;
    g_camCache.lastTick = tick;
    g_camCache.valid = true;

    // Mark all cache entries as potentially stale
    for (auto& kv : g_entityCache) kv.second.lastTick = kv.second.lastTick; // no-op, age naturally

    // Update from fresh entities
    for (auto& ent : f.entities) {
        auto& ce = g_entityCache[ent.ptr];
        // Compute velocity from position delta
        if (ce.lastTick > 0 && (tick - ce.lastTick) < T_1S && ce.lastTick != tick) {
            float dt = (float)(tick - ce.lastTick);
            if (dt > 0.f) {
                ce.velocity.x = (ent.pos.x - ce.pos.x) / dt;
                ce.velocity.y = (ent.pos.y - ce.pos.y) / dt;
                ce.velocity.z = (ent.pos.z - ce.pos.z) / dt;
            }
        }
        else if (ce.lastTick == 0) {
            ce.velocity = { 0,0,0 };
        }
        ce.ptr = ent.ptr; ce.visualState = ent.visualState;
        ce.pos = ent.pos; ce.headPos = ent.headPos;
        ce.dist = ent.dist;
        ce.isPlayer = ent.isPlayer; ce.isZombie = ent.isZombie; ce.isItem = ent.isItem;
        ce.catIdx = ent.catIdx;
        ce.name = ent.name; ce.weaponName = ent.weaponName;
        ce.inventory = ent.inventory;
        ce.hasBones = ent.hasBones;
        if (ent.hasBones) memcpy(ce.bones, ent.bones, sizeof(ce.bones));
        ce.lastTick = tick;
    }
    g_lastFreshTick = tick;

    // Prune old entries (>3 seconds)
    if (tick % T_1S == 0) {
        for (auto it = g_entityCache.begin(); it != g_entityCache.end();) {
            if (tick - it->second.lastTick > T_3S) it = g_entityCache.erase(it);
            else ++it;
        }
    }
}

static FrameData BuildCachedFrame(int tick, int screenW, int screenH) {
    FrameData f = {};
    if (!g_camCache.valid) { f.valid = false; return f; }

    f.valid = true;
    f.world = g_camCache.world;
    f.camPtr = g_camCache.camPtr;
    f.camPos = g_camCache.camPos;
    f.camRight = g_camCache.camRight;
    f.camUp = g_camCache.camUp;
    f.camForward = g_camCache.camForward;
    f.projD1 = g_camCache.projD1;
    f.projD2 = g_camCache.projD2;
    f.localPos = g_camCache.localPos;

    for (auto& kv : g_entityCache) {
        auto& ce = kv.second;
        int age = tick - ce.lastTick;
        if (age > T_2S) continue; // >2s stale, drop

        EntityData ed = {};
        ed.ptr = ce.ptr; ed.visualState = ce.visualState;

        // Dead reckoning: extrapolate position by velocity * age
        float dt = (float)age;
        ed.pos.x = ce.pos.x + ce.velocity.x * dt;
        ed.pos.y = ce.pos.y + ce.velocity.y * dt;
        ed.pos.z = ce.pos.z + ce.velocity.z * dt;
        float headOff = ce.isItem ? 0.3f : (ce.isZombie ? 1.2f : 1.7f);
        ed.headPos = { ed.pos.x, ed.pos.y + headOff, ed.pos.z };

        // Recompute distance from camera
        float dx = ed.pos.x - f.camPos.x, dy = ed.pos.y - f.camPos.y, dz = ed.pos.z - f.camPos.z;
        ed.dist = sqrtf(dx * dx + dy * dy + dz * dz);
        if (ed.dist < 0.5f || ed.dist > 2900.f) continue;

        ed.isPlayer = ce.isPlayer; ed.isZombie = ce.isZombie; ed.isItem = ce.isItem;
        ed.catIdx = ce.catIdx;
        ed.name = ce.name; ed.weaponName = ce.weaponName;
        ed.inventory = ce.inventory;
        ed.hasBones = ce.hasBones;
        if (ce.hasBones) memcpy(ed.bones, ce.bones, sizeof(ce.bones));
        // Extrapolate bone positions too
        if (ce.hasBones && age > 0) {
            for (int b = 0; b < PB_MAX; b++) {
                if (!ed.bones[b].valid) continue;
                ed.bones[b].pos.x += ce.velocity.x * dt;
                ed.bones[b].pos.y += ce.velocity.y * dt;
                ed.bones[b].pos.z += ce.velocity.z * dt;
            }
        }
        f.entities.push_back(std::move(ed));
    }
    return f;
}

// ═══════════════════════════════════════════════════════════════
//  WORLD-TO-SCREEN + FOV
// ═══════════════════════════════════════════════════════════════

static bool W2S(vector3 pos, int& sx, int& sy, const FrameData & f, int sw, int sh) {
    vector3 d = { pos.x - f.camPos.x,pos.y - f.camPos.y,pos.z - f.camPos.z };
    float dl = sqrtf(d.x * d.x + d.y * d.y + d.z * d.z);
    if (dl < 0.01f)return false;
    vector3 dir = { d.x / dl,d.y / dl,d.z / dl };
    if (dir.x * f.camForward.x + dir.y * f.camForward.y + dir.z * f.camForward.z < 0.01f)return false;
    float x = d.x * f.camRight.x + d.y * f.camRight.y + d.z * f.camRight.z;
    float y = d.x * f.camUp.x + d.y * f.camUp.y + d.z * f.camUp.z;
    float z = d.x * f.camForward.x + d.y * f.camForward.y + d.z * f.camForward.z;
    sx = (int)((sw * 0.5f) * (1.0f + (x / z) * f.projD1));
    sy = (int)((sh * 0.5f) * (1.0f - (y / z) * f.projD2));
    //sx = (int)((sw * 0.5f) * (1.0f + (x / f.projD1 / z)));
    //sy = (int)((sh * 0.5f) * (1.0f - (y / f.projD2 / z)));
    return true;
}
static bool InFovCircle(int sx, int sy, int sw, int sh) {
    float dx = (float)(sx - sw / 2), dy = (float)(sy - sh / 2);
    return(dx * dx + dy * dy) <= (g_fovRadiusF * g_fovRadiusF);
}
inline void PumpUIOnce() { MSG m; while (PeekMessage(&m, nullptr, 0, 0, PM_REMOVE)) { TranslateMessage(&m); DispatchMessage(&m); } }

// ═══════════════════════════════════════════════════════════════
//  RADAR CONFIG
// ═══════════════════════════════════════════════════════════════
struct ColorConfig { COLORREF player = RGB(255, 90, 90), zombie = RGB(255, 30, 30), other = RGB(160, 160, 160), self = RGB(255, 220, 60), arrow = RGB(220, 220, 255); };
static ColorConfig colors;
static COLORREF playerPresets[] = { RGB(255,90,90),RGB(100,150,255),RGB(80,220,80),RGB(200,100,255),RGB(255,180,60) };
static COLORREF zombiePresets[] = { RGB(255,30,30),RGB(180,50,220),RGB(60,200,60),RGB(255,120,180),RGB(220,80,0) };
static COLORREF otherPresets[] = { RGB(160,160,160),RGB(120,120,255),RGB(200,200,100),RGB(100,255,220),RGB(220,100,220) };
static COLORREF selfPresets[] = { RGB(255,220,60),RGB(255,140,0),RGB(100,255,100),RGB(180,100,255),RGB(60,180,255) };
static COLORREF arrowPresets[] = { RGB(220,220,255),RGB(255,200,100),RGB(100,255,255),RGB(255,100,255),RGB(255,255,100) };
static int playerPI = 0, zombiePI = 0, otherPI = 0, selfPI = 0, arrowPI = 0;

// ═══════════════════════════════════════════════════════════════
//  OVERLAY THREAD — with velocity prediction
// ═══════════════════════════════════════════════════════════════
static LRESULT CALLBACK RadarWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    if (msg == WM_LBUTTONDOWN) {
        int cx = LOWORD(lParam), cy = HIWORD(lParam);
        const int sY = 355, lh = 20; if (cx >= 10 && cx <= 330) {
            int r = cy - sY; if (r >= 0 && r < 5 * lh) {
                switch (r / lh) {
                case 0:playerPI = (playerPI + 1) % _countof(playerPresets); colors.player = playerPresets[playerPI]; break;
                case 1:zombiePI = (zombiePI + 1) % _countof(zombiePresets); colors.zombie = zombiePresets[zombiePI]; break;
                case 2:otherPI = (otherPI + 1) % _countof(otherPresets); colors.other = otherPresets[otherPI]; break;
                case 3:selfPI = (selfPI + 1) % _countof(selfPresets); colors.self = selfPresets[selfPI]; break;
                case 4:arrowPI = (arrowPI + 1) % _countof(arrowPresets); colors.arrow = arrowPresets[arrowPI]; break;
                }
            }
        }return 0;
    }
    return DefWindowProc(hwnd, msg, wParam, lParam);
}

// ═══════════════════════════════════════════════════════════════
//  GDI+ RENDERING HELPERS — antialiased, gradient, glow
// ═══════════════════════════════════════════════════════════════
static thread_local Gdiplus::Graphics* g_gfx = nullptr; // per-thread, set each frame in WM_PAINT / ESP

static inline Gdiplus::Color GdipCol(COLORREF c, BYTE a = 255) {
    return Gdiplus::Color(a, GetRValue(c), GetGValue(c), GetBValue(c));
}

// Antialiased filled + stroked rounded rect
static void GdipRoundRect(int x, int y, int w, int h, int r, COLORREF fill, COLORREF stroke, int strokeW = 1) {
    if (!g_gfx) return;
    Gdiplus::GraphicsPath path;
    int d = r * 2;
    path.AddArc(x, y, d, d, 180, 90);
    path.AddArc(x + w - d - 1, y, d, d, 270, 90);
    path.AddArc(x + w - d - 1, y + h - d - 1, d, d, 0, 90);
    path.AddArc(x, y + h - d - 1, d, d, 90, 90);
    path.CloseFigure();
    Gdiplus::SolidBrush fb(GdipCol(fill));
    g_gfx->FillPath(&fb, &path);
    if (strokeW > 0) {
        Gdiplus::Pen sp(GdipCol(stroke), (Gdiplus::REAL)strokeW);
        g_gfx->DrawPath(&sp, &path);
    }
}

// Vertical gradient rounded rect
static void GdipGradientRoundRect(int x, int y, int w, int h, int r, COLORREF top, COLORREF bot, COLORREF stroke = 0, int strokeW = 0) {
    if (!g_gfx) return;
    Gdiplus::GraphicsPath path;
    int d = r * 2;
    path.AddArc(x, y, d, d, 180, 90);
    path.AddArc(x + w - d - 1, y, d, d, 270, 90);
    path.AddArc(x + w - d - 1, y + h - d - 1, d, d, 0, 90);
    path.AddArc(x, y + h - d - 1, d, d, 90, 90);
    path.CloseFigure();
    Gdiplus::LinearGradientBrush gb(Gdiplus::Point(x, y), Gdiplus::Point(x, y + h), GdipCol(top), GdipCol(bot));
    g_gfx->FillPath(&gb, &path);
    if (strokeW > 0 && stroke) {
        Gdiplus::Pen sp(GdipCol(stroke), (Gdiplus::REAL)strokeW);
        g_gfx->DrawPath(&sp, &path);
    }
}

// Glow effect: multiple expanded rounded rects with decreasing alpha
static void GdipGlow(int x, int y, int w, int h, int r, COLORREF color, int spread = 6, BYTE peakAlpha = 30) {
    if (!g_gfx) return;
    for (int i = spread; i > 0; i--) {
        BYTE a = (BYTE)(peakAlpha * (spread - i + 1) / spread);
        Gdiplus::GraphicsPath path;
        int d2 = (r + i) * 2;
        int ex = x - i, ey = y - i, ew = w + i * 2, eh = h + i * 2;
        path.AddArc(ex, ey, d2, d2, 180, 90);
        path.AddArc(ex + ew - d2, ey, d2, d2, 270, 90);
        path.AddArc(ex + ew - d2, ey + eh - d2, d2, d2, 0, 90);
        path.AddArc(ex, ey + eh - d2, d2, d2, 90, 90);
        path.CloseFigure();
        Gdiplus::Pen gp(GdipCol(color, a), 1.0f);
        g_gfx->DrawPath(&gp, &path);
    }
}

// Antialiased filled pill (capsule)
static void GdipPill(int x, int y, int w, int h, COLORREF fill) {
    if (!g_gfx) return;
    Gdiplus::GraphicsPath path;
    int r = h;
    path.AddArc(x, y, r, r, 90, 180);
    path.AddArc(x + w - r, y, r, r, 270, 180);
    path.CloseFigure();
    Gdiplus::SolidBrush fb(GdipCol(fill));
    g_gfx->FillPath(&fb, &path);
}

// Antialiased filled circle
static void GdipCircle(int cx, int cy, int r, COLORREF fill, COLORREF border = 0, int borderW = 0) {
    if (!g_gfx) return;
    Gdiplus::SolidBrush fb(GdipCol(fill));
    g_gfx->FillEllipse(&fb, cx - r, cy - r, r * 2, r * 2);
    if (borderW > 0) {
        Gdiplus::Pen bp(GdipCol(border), (Gdiplus::REAL)borderW);
        g_gfx->DrawEllipse(&bp, cx - r, cy - r, r * 2, r * 2);
    }
}

// Antialiased line
static void GdipLine(int x1, int y1, int x2, int y2, COLORREF color, float width = 1.0f) {
    if (!g_gfx) return;
    Gdiplus::Pen p(GdipCol(color), width);
    g_gfx->DrawLine(&p, x1, y1, x2, y2);
}

// Horizontal gradient (for sidebar or header)
static void GdipGradientH(int x, int y, int w, int h, COLORREF left, COLORREF right) {
    if (!g_gfx) return;
    Gdiplus::LinearGradientBrush gb(Gdiplus::Point(x, y), Gdiplus::Point(x + w, y), GdipCol(left), GdipCol(right));
    g_gfx->FillRectangle(&gb, x, y, w, h);
}

// Vertical gradient (for sidebar or header)
static void GdipGradientV(int x, int y, int w, int h, COLORREF top, COLORREF bot) {
    if (!g_gfx) return;
    Gdiplus::LinearGradientBrush gb(Gdiplus::Point(x, y), Gdiplus::Point(x, y + h), GdipCol(top), GdipCol(bot));
    g_gfx->FillRectangle(&gb, x, y, w, h);
}

// Radial glow at a point (for logo, LEDs, active indicators)
static void GdipRadialGlow(int cx, int cy, int r, COLORREF color, BYTE peakAlpha = 50) {
    if (!g_gfx) return;
    for (int i = r; i > 0; i -= 2) {
        BYTE a = (BYTE)(peakAlpha * i / r);
        Gdiplus::SolidBrush fb(GdipCol(color, a));
        g_gfx->FillEllipse(&fb, cx - i, cy - i, i * 2, i * 2);
    }
}
void RunOverlays(int dayzid) {
    printf("[overlay] Single thread started.\n");
    const int SW = 1920, SH = 1080;
    int screenWidth = GetSystemMetrics(SM_CXSCREEN);

    // ESP WINDOW
    WNDCLASSEXA wcE = {}; wcE.cbSize = sizeof(wcE); wcE.lpfnWndProc = DefWindowProcA;
    wcE.hInstance = GetModuleHandleA(nullptr); wcE.lpszClassName = "ESP_OVL";
    RegisterClassExA(&wcE);
    HWND espH = CreateWindowExA(WS_EX_TOPMOST | WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_TOOLWINDOW,
        "ESP_OVL", "", WS_POPUP, 0, 0, SW, SH, nullptr, nullptr, wcE.hInstance, nullptr);
    const COLORREF CLR = RGB(255, 0, 255);
    SetLayeredWindowAttributes(espH, CLR, 0, LWA_COLORKEY);
    HFONT espF = CreateFontA(12, 0, 0, 0, FW_NORMAL, 0, 0, 0, ANSI_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, "Times New Roman");
    HFONT espFB = espF; HFONT espFI = espF;
    HFONT espFItem = CreateFontA(14, 0, 0, 0, FW_SEMIBOLD, 0, 0, 0, ANSI_CHARSET, 0, 0, CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, "Segoe UI");
    HFONT espFItemDist = CreateFontA(11, 0, 0, 0, FW_NORMAL, 0, 0, 0, ANSI_CHARSET, 0, 0, CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, "Segoe UI");

    // RADAR WINDOW
    WNDCLASSEXA wcR = {}; wcR.cbSize = sizeof(wcR); wcR.style = CS_HREDRAW | CS_VREDRAW;
    wcR.lpfnWndProc = RadarWndProc; wcR.hInstance = GetModuleHandleA(NULL);
    wcR.hCursor = LoadCursor(NULL, IDC_HAND); wcR.lpszClassName = "RADAR_OVL";
    RegisterClassExA(&wcR);
    const int RW = 340, RH = 400;
    HWND radH = CreateWindowExA(WS_EX_TOPMOST | WS_EX_LAYERED, "RADAR_OVL", "", WS_POPUP,
        screenWidth - RW - 40, 60, RW, RH, NULL, NULL, wcR.hInstance, NULL);
    SetLayeredWindowAttributes(radH, 0, 195, LWA_ALPHA);
    HFONT radF = CreateFontA(14, 0, 0, 0, FW_NORMAL, 0, 0, 0, ANSI_CHARSET, 0, 0, CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, "Segoe UI");

    // BEARING BAR
    const int BW = 300, BH = 26;
    HWND barH = CreateWindowExA(WS_EX_TOPMOST | WS_EX_LAYERED | WS_EX_TRANSPARENT,
        "STATIC", "", WS_POPUP, (screenWidth - BW) / 2, 20, BW, BH, NULL, NULL, GetModuleHandleA(NULL), NULL);
    SetLayeredWindowAttributes(barH, 0, 180, LWA_ALPHA);

    const COLORREF COL_CROSS = RGB(0, 220, 80);
    const COLORREF COL_WELL = RGB(80, 180, 255);

    HFONT espFL = CreateFontA(12, 0, 0, 0, FW_NORMAL, 0, 0, 0, ANSI_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, "Times New Roman");

    std::unordered_map<uintptr_t, std::pair<float, float>> espPrev;
    std::unordered_map<uintptr_t, vector3> radPrev;
    std::unordered_map<uintptr_t, std::vector<vector3>> radTrails; // breadcrumb history per player
    static const int MAX_TRAIL_POINTS = 60;
    vector3 lastLocal = { 0,0,0 }; bool hasLast = false;
    vector3 moveDir = { 0,0,0 }; bool hasDir = false;
    float radScale = 0.075f;
    int tick = 0;
    MSG msg{};

    while (!g_shutdownAll.load()) {
        while (PeekMessageA(&msg, nullptr, 0, 0, PM_REMOVE)) { TranslateMessage(&msg); DispatchMessageA(&msg); }
        if (GetAsyncKeyState(VK_END) & 1) { g_shutdownAll = true; break; }
        if (GetAsyncKeyState(VK_UP) & 0x8000)radScale = (std::min)(radScale + 0.015f, 0.18f);
        if (GetAsyncKeyState(VK_DOWN) & 0x8000)radScale = (std::max)(radScale - 0.015f, 0.025f);

        bool espOn = g_espEnabled.load(), radOn = g_radarEnabled.load(), aimOn = g_silentAim.load();

        // Apply brightness/gamma if changed
        {
            static float lastBright = 1.0f; float b = g_brightness;
            if (fabsf(b - lastBright) > 0.01f) { ApplyBrightness(b); lastBright = b; }
        }

        ShowWindow(espH, espOn ? SW_SHOWNOACTIVATE : SW_HIDE);
        ShowWindow(radH, radOn ? SW_SHOWNOACTIVATE : SW_HIDE);
        ShowWindow(barH, radOn ? SW_SHOWNOACTIVATE : SW_HIDE);
        if (!espOn && !radOn && !aimOn && !g_mouseAim.load() && !g_railgunAim.load() && !g_laserFire.load() && !g_raidMode.load() && !g_noRecoil.load() && !g_mortarMode.load() && !g_bulletTracers.load() && !g_lootTP.load() && !g_remoteLoot.load() && !g_freecam.load() && !g_noGrass.load() && !g_noGrassApplied) { drainWrites(1); Sleep(50); tick++; continue; }

        FrameData rawFrame = ReadGameFrame(dayzid, SW, SH, tick);
        bool freshData = IsFrameFresh(rawFrame);
        FrameData frame;
        if (freshData) {
            UpdateCache(rawFrame, tick);
            frame = std::move(rawFrame);
        }
        else {
            // Driver stall or bad read — build frame from cache with dead reckoning
            frame = BuildCachedFrame(tick, SW, SH);
        }
        if (!frame.valid) { drainWrites(1); Sleep(2); tick++; continue; }
        g_lastCamPos = frame.camPos; // store for waypoint creation

        if (hasLast) {
            vector3 d = { frame.localPos.x - lastLocal.x,0,frame.localPos.z - lastLocal.z };
            float l = sqrtf(d.x * d.x + d.z * d.z); if (l > 0.02f) { moveDir = { d.x / l,0,d.z / l }; hasDir = true; }
        }
        // Always use camera forward for facing direction (XZ plane)
        float fwdLen = sqrtf(frame.camForward.x * frame.camForward.x + frame.camForward.z * frame.camForward.z);
        if (fwdLen > 0.001f) {
            moveDir = { frame.camForward.x / fwdLen, 0, frame.camForward.z / fwdLen };
            hasDir = true;
        }
        lastLocal = frame.localPos; hasLast = true;

        bool fovFilter = g_fovOnly.load();
        bool espShowPlayers = g_espPlayers.load();
        bool espShowZombies = g_espZombies.load();
        bool radShowPlayers = g_radPlayers.load();
        bool radShowZombies = g_radZombies.load();

        // ── Update velocity tracking for all entities (EMA smoothed) — only on fresh data ──
        if (freshData) {
            const float velAlpha = 0.3f; // smoothing factor: lower=smoother, higher=more responsive
            for (auto& ent : frame.entities) {
                if (!ent.isPlayer && !ent.isZombie)continue;
                auto it = g_velocityMap.find(ent.ptr);
                if (it != g_velocityMap.end()) {
                    int dt = tick - it->second.lastTick;
                    if (dt > 0 && dt < T_1S) {
                        float invDt = (float)dt;
                        vector3 rawVel;
                        rawVel.x = (ent.pos.x - it->second.lastPos.x) / invDt;
                        rawVel.y = (ent.pos.y - it->second.lastPos.y) / invDt;
                        rawVel.z = (ent.pos.z - it->second.lastPos.z) / invDt;
                        // EMA smooth to reduce jitter
                        it->second.smoothVel.x = velAlpha * rawVel.x + (1.f - velAlpha) * it->second.smoothVel.x;
                        it->second.smoothVel.y = velAlpha * rawVel.y + (1.f - velAlpha) * it->second.smoothVel.y;
                        it->second.smoothVel.z = velAlpha * rawVel.z + (1.f - velAlpha) * it->second.smoothVel.z;
                        it->second.velocity = it->second.smoothVel;
                        it->second.valid = true;
                    }
                    it->second.lastPos = ent.pos;
                    it->second.lastTick = tick;
                }
                else {
                    VelocityEntry ve;
                    ve.lastPos = ent.pos; ve.velocity = { 0,0,0 }; ve.smoothVel = { 0,0,0 }; ve.lastTick = tick; ve.valid = false;
                    g_velocityMap[ent.ptr] = ve;
                }
            }
        } // end freshData guard
        // Clean stale entries every ~5 seconds (always, not just fresh)
        if (tick % T_5S == 0) {
            for (auto it = g_velocityMap.begin(); it != g_velocityMap.end();) {
                if (tick - it->second.lastTick > T_5S)it = g_velocityMap.erase(it);
                else ++it;
            }
            for (auto it = g_weaponCache.begin(); it != g_weaponCache.end();) {
                if (tick - it->second.lastTick > WEAPON_REFRESH_TICKS * 2)it = g_weaponCache.erase(it);
                else ++it;
            }
        }

        // ══════════════════════════════
        //  AIM SYSTEMS — target finding + prediction + bullet teleport
        // ══════════════════════════════
        vector3 aimTarget = { 0,0,0 };
        vector3 silentTarget = { 0,0,0 }; // un-led position for bullet TP
        bool hasAimTarget = false;
        static bool aimDbgDone = false;
        int aimFovI = (int)g_aimFovF;

        // Find local player — needed by aim systems, loot TP, and other features
        if (!cachedLocalPlayer || (tick - localPlayerTick > T_2S)) {
            float bestCamDist = 5.0f;
            for (auto& ent : frame.entities) {
                if (!ent.isPlayer)continue;
                float cdx = ent.pos.x - frame.camPos.x, cdy = ent.pos.y - frame.camPos.y, cdz = ent.pos.z - frame.camPos.z;
                float cdist = sqrtf(cdx * cdx + cdy * cdy + cdz * cdz);
                if (cdist < bestCamDist) { bestCamDist = cdist; cachedLocalPlayer = ent.ptr; localPlayerTick = tick; }
            }
        }

        if (g_silentAim.load() || g_mouseAim.load() || g_railgunAim.load() || g_laserFire.load() || g_raidMode.load() || g_mortarMode.load()) {

            // Determine active range based on which mode
            float activeRange = g_silentAim.load() ? g_silentRange : g_mouseRange;
            if (g_silentAim.load() && (g_mouseAim.load() || g_railgunAim.load())) activeRange = (std::max)(g_silentRange, g_mouseRange);
            if (g_railgunAim.load() && !g_silentAim.load() && !g_mouseAim.load()) activeRange = g_mouseRange;

            float bestDist2D = 99999.f;
            float targetWorldDist = 0.f;
            int scrCX = SW / 2, scrCY = SH / 2;
            uintptr_t bestEntPtr = 0;

            // ── TAG SYSTEM: Middle mouse to tag/untag ──
            // When raid mode is active, MMB sets/clears raid point instead
            if (GetAsyncKeyState(VK_MBUTTON) & 1) {
                if (g_raidMode.load()) {
                    if (g_raidPointSet) {
                        g_raidPointSet = false;
                        g_raidBulletCount = 0; // clear active loops
                        g_speedHackDone = false; g_lastAppliedSpeed = 0; // force speed restore
                        printf("[RAID] Point cleared\n");
                    }
                    else {
                        // Place raid point along camera forward at g_raidDist
                        g_raidPoint.x = frame.camPos.x + frame.camForward.x * g_raidDist;
                        g_raidPoint.y = frame.camPos.y + frame.camForward.y * g_raidDist;
                        g_raidPoint.z = frame.camPos.z + frame.camForward.z * g_raidDist;
                        // Lock the oscillation direction (perpendicular to wall surface = camera forward at placement)
                        float fl = sqrtf(frame.camForward.x * frame.camForward.x + frame.camForward.y * frame.camForward.y + frame.camForward.z * frame.camForward.z);
                        if (fl > 0.001f) { g_raidDir = { frame.camForward.x / fl, frame.camForward.y / fl, frame.camForward.z / fl }; }
                        else { g_raidDir = { 0,0,1 }; }
                        g_raidPointSet = true;
                        g_speedHackDone = false; g_lastAppliedSpeed = 0; // force raid speed apply
                        printf("[RAID] Point set at (%.1f, %.1f, %.1f) dist=%.1fm\n",
                            g_raidPoint.x, g_raidPoint.y, g_raidPoint.z, g_raidDist);
                    }
                }
                else if (g_taggedTarget) {
                    // Untag
                    printf("[TAG] Untagged %p\n", (void*)g_taggedTarget);
                    g_taggedTarget = 0;
                }
                else {
                    // Tag the closest player OR zombie to crosshair
                    float bestTag2D = 99999.f;
                    uintptr_t bestTagPtr = 0;
                    bool bestTagIsZ = false;
                    vector3 bestTagPos = { 0,0,0 };
                    for (size_t ti = 0; ti < frame.entities.size(); ti++) {
                        EntityData& te = frame.entities[ti];
                        if (!te.isPlayer && !te.isZombie) continue;
                        if (te.ptr == cachedLocalPlayer) continue;
                        if (te.isPlayer && !espShowPlayers) continue;
                        if (te.isZombie && !espShowZombies) continue;
                        if (te.dist > g_silentRange) continue;
                        int tsx, tsy;
                        if (!W2S(te.pos, tsx, tsy, frame, SW, SH)) continue;
                        float td = sqrtf((float)((tsx - scrCX) * (tsx - scrCX) + (tsy - scrCY) * (tsy - scrCY)));
                        if (td < bestTag2D) { bestTag2D = td; bestTagPtr = te.ptr; bestTagIsZ = te.isZombie; bestTagPos = te.pos; }
                    }
                    if (bestTagPtr) {
                        g_taggedTarget = bestTagPtr;
                        g_taggedIsZombie = bestTagIsZ;
                        g_taggedLastPos = bestTagPos;
                        g_tagTick = tick;
                        printf("[TAG] Tagged %s %p\n", bestTagIsZ ? "ZOMBIE" : "PLAYER", (void*)g_taggedTarget);
                    }
                }
            }

            // ── TAGGED TARGET: resolve position each frame ──
            bool tagActive = false;
            if (g_taggedTarget && g_silentAim.load()) {
                bool tagFound = false;
                for (size_t ti = 0; ti < frame.entities.size(); ti++) {
                    EntityData& te = frame.entities[ti];
                    if (te.ptr != g_taggedTarget) continue;
                    if (te.dist > g_silentRange) { break; } // out of range, skip but don't untag yet
                    tagFound = true;

                    // Track last known position for death marker
                    g_taggedLastPos = te.pos;

                    // Get bone position for tagged target
                    vector3 tagBone;
                    static const int boneMap2[] = { PB_HEAD, PB_NECK, PB_SPINE2, PB_PELVIS };
                    int prefB = boneMap2[g_mbBoneChoice]; // tagged target uses MB bone
                    if (te.hasBones && te.bones[prefB].valid) {
                        tagBone = te.bones[prefB].pos;
                    }
                    else if (te.hasBones && te.bones[PB_SPINE2].valid) {
                        tagBone = te.bones[PB_SPINE2].pos;
                    }
                    else {
                        tagBone = { te.pos.x, te.pos.y + 1.2f, te.pos.z };
                    }

                    // Apply velocity prediction if enabled
                    vector3 tagLed = tagBone;
                    if (g_aimPrediction.load() && g_leadFactor > 0.01f) {
                        auto vit = g_velocityMap.find(te.ptr);
                        if (vit != g_velocityMap.end() && vit->second.valid) {
                            tagLed.x += vit->second.velocity.x * g_leadFactor * (float)TPS;
                            tagLed.y += vit->second.velocity.y * g_leadFactor * (float)TPS;
                            tagLed.z += vit->second.velocity.z * g_leadFactor * (float)TPS;
                        }
                    }

                    silentTarget = tagBone;
                    aimTarget = tagLed;
                    targetWorldDist = te.dist;
                    hasAimTarget = true;
                    bestEntPtr = te.ptr;
                    tagActive = true;
                    g_tagTick = tick; // refresh
                    break;
                }
                // Auto-untag if not seen for ~5 seconds
                if (!tagFound && (tick - g_tagTick > T_5S)) {
                    printf("[TAG] Lost target %p, untagging\n", (void*)g_taggedTarget);
                    AddDeathMarker(g_taggedLastPos, "Tag lost");
                    g_taggedTarget = 0;
                }
            }

            // ── NORMAL AIM TARGETING (skip if tag is active) ──
            if (!tagActive) {
                for (auto& ent : frame.entities) {
                    if (!ent.isPlayer && !ent.isZombie)continue;
                    if (ent.ptr == cachedLocalPlayer)continue;
                    if (ent.dist > activeRange)continue;
                    // Only aim at entities the overlay is actually showing
                    if (ent.isPlayer && !espShowPlayers) continue;
                    if (ent.isZombie && !espShowZombies) continue;

                    vector3 targetBone;
                    const char* aimSrc = "GUESS";

                    // Preferred bone from selector — per aim mode
                    static const int boneMap[] = { PB_HEAD, PB_NECK, PB_SPINE2, PB_PELVIS };
                    // Pick bone based on active mode priority: MB > MA > RG
                    int activeBone = g_maBoneChoice;
                    if (g_silentAim.load()) activeBone = g_mbBoneChoice;
                    else if (g_railgunAim.load()) activeBone = g_rgBoneChoice;
                    int prefBone = boneMap[activeBone];
                    if (ent.hasBones && ent.bones[prefBone].valid) {
                        targetBone = ent.bones[prefBone].pos;
                        aimSrc = g_boneChoiceNames[activeBone];
                    }
                    // Fallback chain if preferred bone invalid
                    else if (ent.hasBones && ent.bones[PB_SPINE2].valid) { targetBone = ent.bones[PB_SPINE2].pos; aimSrc = "SPINE2*"; }
                    else if (ent.hasBones && ent.bones[PB_NECK].valid) { targetBone = ent.bones[PB_NECK].pos; aimSrc = "NECK*"; }
                    else if (ent.hasBones && ent.bones[PB_PELVIS].valid) { targetBone = ent.bones[PB_PELVIS].pos; aimSrc = "PELVIS*"; }
                    else {
                        float chestH = ent.isZombie ? 0.9f : 1.2f;
                        targetBone = { ent.pos.x,ent.pos.y + chestH,ent.pos.z };
                    }

                    // ── Velocity prediction: only for mouse aim (silent aim TPs directly) ──
                    vector3 targetRaw = targetBone; // save un-led position for silent aim
                    vector3 targetLed = targetBone; // led version for mouse aim
                    if ((g_mouseAim.load() || g_railgunAim.load()) && g_aimPrediction.load() && g_leadFactor > 0.01f) {
                        auto vit = g_velocityMap.find(ent.ptr);
                        if (vit != g_velocityMap.end() && vit->second.valid) {
                            vector3 vel = vit->second.velocity;
                            float speed = sqrtf(vel.x * vel.x + vel.z * vel.z);
                            if (speed > 0.005f) {
                                float leadFrames = g_leadFactor / (speed + 0.001f);
                                leadFrames = (std::min)(leadFrames, (float)T_HALF_S);
                                targetLed.x += vel.x * leadFrames;
                                targetLed.y += vel.y * leadFrames;
                                targetLed.z += vel.z * leadFrames;
                            }
                        }
                    }

                    // FOV check ALWAYS on raw position (never prediction-led)
                    int tsx, tsy;
                    if (!W2S(targetRaw, tsx, tsy, frame, SW, SH))continue;
                    float dx2 = (float)(tsx - scrCX), dy2 = (float)(tsy - scrCY);
                    float dist2D = sqrtf(dx2 * dx2 + dy2 * dy2);
                    if (dist2D > (float)aimFovI)continue;
                    // If ESP FOV filter is on, also require entity inside ESP FOV circle
                    if (fovFilter && !InFovCircle(tsx, tsy, SW, SH)) continue;
                    if (dist2D < bestDist2D) {
                        bestDist2D = dist2D;
                        aimTarget = targetLed;         // led position for mouse aim
                        silentTarget = targetRaw;      // raw position for bullet TP
                        targetWorldDist = ent.dist;
                        hasAimTarget = true;
                        bestEntPtr = ent.ptr;

                        static uintptr_t lastAimPtr = 0;
                        if (ent.ptr != lastAimPtr) {
                            lastAimPtr = ent.ptr;
                            auto vit2 = g_velocityMap.find(ent.ptr);
                            bool hasVel = vit2 != g_velocityMap.end() && vit2->second.valid;
                            printf("[AIM] -> %s @ %.0fm | src=%s bones=%s pred=%s\n",
                                ent.isPlayer ? "PLAYER" : "ZOMBIE", ent.dist, aimSrc,
                                ent.hasBones ? "YES" : "NO",
                                (hasVel && g_aimPrediction.load()) ? "LEADING" : "OFF");
                        }
                    }
                }
            } // end if (!tagActive)

            // One-shot debug
            if (freshData && !aimDbgDone) {
                aimDbgDone = true;
                uintptr_t bt = read<uintptr_t>(frame.world + Off::Bullets, dayzid);
                int bc = read<int>(frame.world + Off::BulletCount, dayzid);
                printf("\n[AIM] Bullet table=%p count=%d silentRange=%.0fm mouseRange=%.0fm fov=%dpx\n",
                    (void*)bt, bc, g_silentRange, g_mouseRange, aimFovI);
                printf("[AIM] Local player: %p  prediction=%s  lead=%.1fm\n",
                    (void*)cachedLocalPlayer, g_aimPrediction.load() ? "ON" : "OFF", g_leadFactor);
            }

            // ── SPEED HACK — with weapon swap detection (only on fresh reads) ──
            bool wantSpeed = g_silentAim.load() || g_laserFire.load() || g_railgunAim.load() || (g_raidMode.load() && g_raidPointSet) || (g_mortarMode.load() && g_mortarSet);
            float targetSpeed = g_bulletSpeed;
            if (g_raidMode.load() && g_raidPointSet) targetSpeed = g_raidSpeed;
            else if (g_mortarMode.load() && g_mortarSet) targetSpeed = g_mortarSpeed;
            if (g_railgunAim.load() && !(g_raidMode.load() && g_raidPointSet) && !(g_mortarMode.load() && g_mortarSet)) targetSpeed = RAILGUN_SPEED;
            if (freshData && cachedLocalPlayer && wantSpeed) {
                uintptr_t inv = read<uintptr_t>(cachedLocalPlayer + Off::Inventory, dayzid);
                uintptr_t hands = 0;
                if (inv && inv > 0x10000000ULL)
                    hands = read<uintptr_t>(inv + Off::InvHands, dayzid);

                // Weapon swap detection: if hands pointer changed, validate new item
                if (hands != g_cachedHands) {
                    g_cachedHands = hands;
                    if (hands && hands > 0x10000000ULL) {
                        // Walk FULL ammo chain to confirm firearm (not magazine/ammo item)
                        // Magazines have AmmoType1 but NOT a valid AmmoType2 chain
                        uintptr_t testAmmo1 = read<uintptr_t>(hands + Off::AmmoType1, dayzid);
                        uintptr_t testAmmo2 = 0;
                        if (testAmmo1 && testAmmo1 > 0x10000000ULL)
                            testAmmo2 = read<uintptr_t>(testAmmo1 + Off::AmmoType2, dayzid);
                        if (testAmmo2 && testAmmo2 > 0x10000000ULL) {
                            g_speedHackDone = false; // full chain valid = firearm
                            printf("[SPD] Weapon swap -> firearm (chain valid), re-applying speed\n");
                        }
                        else {
                            g_speedHackDone = true; // chain breaks = magazine/ammo/non-firearm
                            printf("[SPD] Weapon swap -> non-firearm (chain breaks at %s), skipping\n",
                                (!testAmmo1 || testAmmo1 < 0x10000000ULL) ? "AmmoType1" : "AmmoType2");
                        }
                    }
                    else {
                        g_speedHackDone = true; // empty hands
                    }
                }

                if ((!g_speedHackDone && (tick - g_lastSpeedWriteTick) > T_HALF_S) ||
                    (fabsf(targetSpeed - g_lastAppliedSpeed) > 1.f && (tick - g_lastSpeedWriteTick) > T_HALF_S)) {
                    if (hands && hands > 0x10000000ULL) {
                        uintptr_t ammo1 = read<uintptr_t>(hands + Off::AmmoType1, dayzid);
                        if (!ammo1 || ammo1 < 0x10000000ULL) { g_speedHackDone = true; }
                        else {
                            uintptr_t ammo2 = read<uintptr_t>(ammo1 + Off::AmmoType2, dayzid);
                            if (!ammo2 || ammo2 < 0x10000000ULL) { g_speedHackDone = true; }
                            else {
                                write_async<float>(ammo2 + Off::InitSpeed, targetSpeed, dayzid);
                                g_speedHackDone = true;
                                g_lastAppliedSpeed = targetSpeed;
                                g_lastSpeedWriteTick = tick;
                                printf("[SPD] InitSpeed -> %.0f m/s%s\n", targetSpeed,
                                    (g_raidMode.load() && g_raidPointSet) ? " (RAID)" : "");
                                fflush(stdout);
                            }
                        }
                    }
                }
            }

            // ── MOUSE AIMBOT — move mouse toward predicted target when RMB held ──
            if ((g_mouseAim.load() || g_railgunAim.load()) && hasAimTarget) {
                // Enforce mouse range separately
                if (targetWorldDist <= g_mouseRange) {
                    int atx, aty;
                    if (W2S(aimTarget, atx, aty, frame, SW, SH)) {
                        if (GetAsyncKeyState(VK_RBUTTON) & 0x8000) {
                            int scrCX2 = SW / 2, scrCY2 = SH / 2;
                            float deltaX = (float)(atx - scrCX2), deltaY = (float)(aty - scrCY2);
                            float dist2scr = sqrtf(deltaX * deltaX + deltaY * deltaY);
                            // Dead zone: don't move when within 3px of target (prevents oscillation)
                            if (dist2scr > 3.0f) {
                                // Scale smoothing based on distance — slower as we get closer
                                float dynSmooth = g_mouseSmooth;
                                if (dist2scr < 20.f)dynSmooth = g_mouseSmooth * 1.5f; // extra smooth when close
                                float moveX = deltaX / dynSmooth;
                                float moveY = deltaY / dynSmooth;
                                // Clamp to at least 1px move if we're outside dead zone
                                // but DON'T force movement when delta is tiny (prevents jitter)
                                if (fabsf(moveX) < 1.f && fabsf(deltaX) > 3.f)moveX = (deltaX > 0) ? 1.f : -1.f;
                                if (fabsf(moveY) < 1.f && fabsf(deltaY) > 3.f)moveY = (deltaY > 0) ? 1.f : -1.f;
                                mouse_event(MOUSEEVENTF_MOVE, (DWORD)(int)moveX, (DWORD)(int)moveY, 0, 0);
                            }
                        }
                    }
                }
            }

            // ── SILENT AIM — bullet TP ──
            // Uses write_dedicated: goes through write channel, never blocks read pipeline
            if (freshData && g_silentAim.load() && hasAimTarget && targetWorldDist <= g_silentRange) {
                uintptr_t bulletTable = read<uintptr_t>(frame.world + Off::Bullets, dayzid);
                int bulletCount = read<int>(frame.world + Off::BulletCount, dayzid);
                int tpdThisFrame = 0;
                if (bulletTable && bulletTable > 0x10000000ULL && bulletCount > 0 && bulletCount < 512) {
                    for (int i = 0; i < bulletCount && tpdThisFrame < 8; i++) {
                        uintptr_t bullet = read<uintptr_t>(bulletTable + i * 0x8, dayzid);
                        if (!bullet || bullet < 0x10000000ULL)continue;
                        if (bulletAlreadyTpd(bullet))continue;
                        if (raidBulletTracked(bullet))continue; // don't steal raid bullets
                        uintptr_t bvs = read<uintptr_t>(bullet + Off::VisualState, dayzid);
                        if (!bvs || bvs < 0x10000000ULL)continue;

                        // Teleport via dedicated write channel
                        write_dedicated<vector3>(bvs + Off::GetCoord, silentTarget, dayzid);
                        trackBullet(bullet);
                        tpdThisFrame++;
                        // Track for hit log
                        g_lastBulletTPTarget = bestEntPtr;
                        g_lastBulletTPTick = tick;
                        g_pendingHitCheck = true;
                        printf("[AIM] bullet %p -> (%.1f,%.1f,%.1f) dist=%.0fm\n",
                            (void*)bullet, silentTarget.x, silentTarget.y, silentTarget.z, targetWorldDist);
                        fflush(stdout);
                    }
                }
                else { g_tpdCount = 0; }
            }

            // ── MORTAR MODE — teleport bullets to fixed XYZ target ──
            if (freshData && g_mortarMode.load() && g_mortarSet) {
                uintptr_t bulletTable = read<uintptr_t>(frame.world + Off::Bullets, dayzid);
                int bulletCount = read<int>(frame.world + Off::BulletCount, dayzid);
                int mortarTpd = 0;
                if (bulletTable && bulletTable > 0x10000000ULL && bulletCount > 0 && bulletCount < 512) {
                    for (int i = 0; i < bulletCount && mortarTpd < 8; i++) {
                        uintptr_t bullet = read<uintptr_t>(bulletTable + i * 0x8, dayzid);
                        if (!bullet || bullet < 0x10000000ULL) continue;
                        if (bulletAlreadyTpd(bullet)) continue;
                        if (raidBulletTracked(bullet)) continue;
                        uintptr_t bvs = read<uintptr_t>(bullet + Off::VisualState, dayzid);
                        if (!bvs || bvs < 0x10000000ULL) continue;
                        write_dedicated<vector3>(bvs + Off::GetCoord, g_mortarTarget, dayzid);
                        trackBullet(bullet);
                        mortarTpd++;
                        printf("[MORTAR] bullet %p -> (%.1f,%.1f,%.1f)\n",
                            (void*)bullet, g_mortarTarget.x, g_mortarTarget.y, g_mortarTarget.z);
                        fflush(stdout);
                    }
                }
                else { g_tpdCount = 0; }
            }

            // ── RAID MODE — bullet looping through raid point ──
            if (freshData && g_raidMode.load() && g_raidPointSet) {
                uintptr_t bulletTable = read<uintptr_t>(frame.world + Off::Bullets, dayzid);
                int bulletCount = read<int>(frame.world + Off::BulletCount, dayzid);

                if (bulletTable && bulletTable > 0x10000000ULL && bulletCount > 0 && bulletCount < 512) {
                    // Build set of bullets still in table for cleanup
                    static uintptr_t tableBullets[512];
                    int tblCount = 0;
                    for (int i = 0; i < bulletCount && tblCount < 512; i++) {
                        uintptr_t b = read<uintptr_t>(bulletTable + i * 0x8, dayzid);
                        if (b && b > 0x10000000ULL) tableBullets[tblCount++] = b;
                    }

                    // Find new bullets to start looping (not already tracked, not in normal tpd list)
                    for (int i = 0; i < tblCount && g_raidBulletCount < 16; i++) {
                        uintptr_t bullet = tableBullets[i];
                        if (bulletAlreadyTpd(bullet)) continue;
                        if (raidBulletTracked(bullet)) continue;
                        uintptr_t bvs = read<uintptr_t>(bullet + Off::VisualState, dayzid);
                        if (!bvs || bvs < 0x10000000ULL) continue;
                        // New bullet — start looping
                        RaidBullet& rb = g_raidBullets[g_raidBulletCount++];
                        rb.ptr = bullet;
                        rb.bvs = bvs;
                        rb.phase = 0;
                        rb.loops = 0;
                        rb.startTick = tick;
                    }

                    // Oscillate all active raid bullets
                    // Use locked direction from placement time (perpendicular to wall)
                    float oscillateOffset = g_raidOscDist;
                    vector3 posA = { g_raidPoint.x + g_raidDir.x * oscillateOffset,
                                     g_raidPoint.y + g_raidDir.y * oscillateOffset,
                                     g_raidPoint.z + g_raidDir.z * oscillateOffset };
                    vector3 posB = { g_raidPoint.x - g_raidDir.x * oscillateOffset,
                                     g_raidPoint.y - g_raidDir.y * oscillateOffset,
                                     g_raidPoint.z - g_raidDir.z * oscillateOffset };

                    for (int ri = 0; ri < g_raidBulletCount; ri++) {
                        RaidBullet& rb = g_raidBullets[ri];

                        // Check if bullet still in table
                        bool inTable = false;
                        for (int t = 0; t < tblCount; t++) {
                            if (tableBullets[t] == rb.ptr) { inTable = true; break; }
                        }

                        if (!inTable || rb.loops >= g_raidMaxLoops) {
                            // Done — remove from raid tracking
                            trackBullet(rb.ptr); // mark as done so silent aim doesn't pick it up
                            g_raidBullets[ri] = g_raidBullets[--g_raidBulletCount];
                            ri--; continue;
                        }

                        // Burst-write: oscillate multiple times per frame
                        // Server evaluates at its own tick rate, more writes = more chances to catch ticks
                        int burstCount = 8; // 8 oscillations per frame = ~480/sec at 60fps
                        for (int b = 0; b < burstCount && rb.loops < g_raidMaxLoops; b++) {
                            vector3 writePos = (rb.phase == 0) ? posA : posB;
                            write_dedicated<vector3>(rb.bvs + Off::GetCoord, writePos, dayzid);
                            rb.phase = 1 - rb.phase;
                            rb.loops++;
                        }
                    }
                }
                else {
                    // No bullets — prune stale raid entries
                    g_raidBulletCount = 0;
                }
            }

            // ── HIT LOG — check if targeted entity disappeared ──
            if (g_pendingHitCheck && g_lastBulletTPTarget) {
                int elapsed = tick - g_lastBulletTPTick;
                if (elapsed > (T_HALF_S / 2) && elapsed < T_5S) { // check between ~0.25s and ~5s
                    bool found = false;
                    for (size_t ei = 0; ei < frame.entities.size(); ei++) {
                        if (frame.entities[ei].ptr == g_lastBulletTPTarget) { found = true; break; }
                    }
                    if (!found) {
                        g_totalHits++;
                        HitEntry he;
                        he.tick = tick;
                        he.dist = targetWorldDist;
                        he.bone = g_boneChoiceNames[g_mbBoneChoice];
                        he.method = "MB";
                        he.timestamp = GetTickCount();
                        g_hitLog.push_back(he);
                        if ((int)g_hitLog.size() > MAX_HIT_LOG)
                            g_hitLog.erase(g_hitLog.begin());
                        printf("[HIT] #%d confirmed! Entity %p gone after %d ticks\n",
                            g_totalHits, (void*)g_lastBulletTPTarget, elapsed);
                        // Auto-untag if tagged target was killed
                        if (g_taggedTarget == g_lastBulletTPTarget) {
                            printf("[TAG] Tagged target killed, untagging\n");
                            AddDeathMarker(g_taggedLastPos, "Kill confirmed");
                            g_taggedTarget = 0;
                        }
                        g_pendingHitCheck = false;
                        g_lastBulletTPTarget = 0;
                    }
                }
                if (elapsed >= T_5S) { // timeout, entity survived or we lost tracking
                    g_pendingHitCheck = false;
                    g_lastBulletTPTarget = 0;
                }
            }
        }

        // ── NO RECOIL — pull mouse down while firing ──
        if (g_noRecoil.load() && (GetAsyncKeyState(VK_LBUTTON) & 0x8000)) {
            mouse_event(MOUSEEVENTF_MOVE, 0, (DWORD)(int)g_recoilPull, 0, 0);
        }

        // ── NO GRASS — write 0 to grass render distance ──
        if (freshData && g_noGrass.load() && !g_noGrassApplied) {
            write_dedicated<ULONG>(frame.world + 0xC00, 0x00000000, dayzid);
            g_noGrassApplied = true;
            printf("[NOGRASS] Disabled grass\n"); fflush(stdout);
        }
        if (freshData && !g_noGrass.load() && g_noGrassApplied) {
            write_dedicated<ULONG>(frame.world + 0xC00, 0x41480000, dayzid); // 12.5f default
            g_noGrassApplied = false;
            printf("[NOGRASS] Restored grass\n"); fflush(stdout);
        }

        // ── BULLET TRACERS — read bullet positions for visualization ──
        if (freshData && g_bulletTracers.load()) {
            uintptr_t bt = read<uintptr_t>(frame.world + Off::Bullets, dayzid);
            int bc = read<int>(frame.world + Off::BulletCount, dayzid);
            // Mark all existing tracers as unseen this frame
            bool seen[128] = {};
            if (bt && bt > 0x10000000ULL && bc > 0 && bc < 512) {
                BatchCtx TB;
                int bSlots[512]; uintptr_t bPtrs[512]; int validB = 0;
                // Step 1: read bullet pointers
                for (int off = 0; off < bc; off += 200) {
                    int chunk = (std::min)(bc - off, 200);
                    TB.reset();
                    int sl[200];
                    for (int i = 0; i < chunk; i++) sl[i] = TB.add(bt + (off + i) * 8, 8);
                    TB.execute(dayzid);
                    for (int i = 0; i < chunk; i++) {
                        uintptr_t bp = TB.get<uintptr_t>(sl[i]);
                        if (bp && bp > 0x10000000ULL && validB < 512) bPtrs[validB++] = bp;
                    }
                }
                // Step 2: read VS ptrs
                struct BVS { uintptr_t ptr, vs; };
                BVS bvsList[512]; int bvsCount = 0;
                for (int off = 0; off < validB; off += 200) {
                    int chunk = (std::min)(validB - off, 200);
                    TB.reset();
                    int sl[200];
                    for (int i = 0; i < chunk; i++) sl[i] = TB.add(bPtrs[off + i] + Off::VisualState, 8);
                    TB.execute(dayzid);
                    for (int i = 0; i < chunk; i++) {
                        uintptr_t vs = TB.get<uintptr_t>(sl[i]);
                        if (vs && vs > 0x10000000ULL) bvsList[bvsCount++] = { bPtrs[off + i], vs };
                    }
                }
                // Step 3: read positions
                for (int off = 0; off < bvsCount; off += 200) {
                    int chunk = (std::min)(bvsCount - off, 200);
                    TB.reset();
                    int sl[200];
                    for (int i = 0; i < chunk; i++) sl[i] = TB.add(bvsList[off + i].vs + Off::GetCoord, 12);
                    TB.execute(dayzid);
                    for (int i = 0; i < chunk; i++) {
                        vector3 pos = TB.get<vector3>(sl[i]);
                        uintptr_t bp = bvsList[off + i].ptr;
                        // Update or insert into tracer array
                        bool found = false;
                        for (int t = 0; t < g_tracerCount; t++) {
                            if (g_tracers[t].ptr == bp) {
                                g_tracers[t].prev = g_tracers[t].cur;
                                g_tracers[t].cur = pos;
                                g_tracers[t].lastSeen = tick;
                                g_tracers[t].hasPrev = true;
                                seen[t] = true;
                                found = true; break;
                            }
                        }
                        if (!found && g_tracerCount < 128) {
                            g_tracers[g_tracerCount] = { bp, pos, pos, tick, false };
                            seen[g_tracerCount] = true;
                            g_tracerCount++;
                        }
                    }
                }
            }
            // Expire tracers not seen for 60 ticks (~1s)
            for (int t = g_tracerCount - 1; t >= 0; t--) {
                if (tick - g_tracers[t].lastSeen > 60) {
                    g_tracers[t] = g_tracers[--g_tracerCount];
                }
            }
        }

        // ── FREECAM — handle input, update position (writes done by FreecamWriteThread) ──
        if (g_freecam.load() && (GetAsyncKeyState(g_freecamHotkeys[g_freecamHotkeyIdx].vk) & 1)) {
            if (!g_freecamActive.load()) {
                g_freecamPos = frame.camPos;
                // Cache addresses for write thread
                g_freecamCamPtr = frame.camPtr;
                // Resolve playerPtr via world+0x2960 chain (same as reference code)
                uintptr_t lpPtr = read<uintptr_t>(frame.world + 0x2960, dayzid);
                if (lpPtr && lpPtr > 0x10000000ULL) {
                    uintptr_t p2 = read<uintptr_t>(lpPtr + 0x8, dayzid);
                    if (p2 && p2 > 0x10000000ULL) {
                        uintptr_t pVS = read<uintptr_t>(p2 + 0x120, dayzid);
                        if (pVS && pVS > 0x10000000ULL) g_freecamPlayerVS = pVS;
                    }
                }
                g_freecamActive = true;
                printf("[FREECAM] Active — pos (%.1f, %.1f, %.1f)\n",
                    g_freecamPos.x, g_freecamPos.y, g_freecamPos.z);
                fflush(stdout);
            }
            else {
                g_freecamActive = false;
                g_freecamCamPtr = 0;
                g_freecamPlayerVS = 0;
                printf("[FREECAM] Deactivated\n"); fflush(stdout);
            }
        }
        if (!g_freecam.load() && g_freecamActive.load()) {
            g_freecamActive = false; g_freecamCamPtr = 0; g_freecamPlayerVS = 0;
        }
        if (g_freecamActive.load()) {
            // Update cached cam ptr each fresh frame
            if (freshData && frame.camPtr) g_freecamCamPtr = frame.camPtr;
            // Re-resolve playerPtr via world+0x2960 chain periodically
            if (freshData) {
                uintptr_t lpPtr = read<uintptr_t>(frame.world + 0x2960, dayzid);
                if (lpPtr && lpPtr > 0x10000000ULL) {
                    uintptr_t p2 = read<uintptr_t>(lpPtr + 0x8, dayzid);
                    if (p2 && p2 > 0x10000000ULL) {
                        uintptr_t pVS = read<uintptr_t>(p2 + 0x120, dayzid);
                        if (pVS && pVS > 0x10000000ULL) g_freecamPlayerVS = pVS;
                    }
                }
            }

            // Speed: Shift=fast, Ctrl=slow
            float spd = g_freecamSpeedNormal;
            if (GetAsyncKeyState(VK_LSHIFT) & 0x8000) spd = g_freecamSpeedFast;
            else if (GetAsyncKeyState(VK_LCONTROL) & 0x8000) spd = g_freecamSpeedSlow;

            vector3 fwd = frame.camForward;
            vector3 rgt = frame.camRight;
            vector3 up = frame.camUp;

            if (GetAsyncKeyState('W') & 0x8000) {
                g_freecamPos.x += fwd.x * spd; g_freecamPos.y += fwd.y * spd; g_freecamPos.z += fwd.z * spd;
            }
            if (GetAsyncKeyState('S') & 0x8000) {
                g_freecamPos.x -= fwd.x * spd; g_freecamPos.y -= fwd.y * spd; g_freecamPos.z -= fwd.z * spd;
            }
            if (GetAsyncKeyState('D') & 0x8000) {
                g_freecamPos.x += rgt.x * spd; g_freecamPos.y += rgt.y * spd; g_freecamPos.z += rgt.z * spd;
            }
            if (GetAsyncKeyState('A') & 0x8000) {
                g_freecamPos.x -= rgt.x * spd; g_freecamPos.y -= rgt.y * spd; g_freecamPos.z -= rgt.z * spd;
            }
            if (GetAsyncKeyState(VK_SPACE) & 0x8000) {
                g_freecamPos.x += up.x * spd; g_freecamPos.y += up.y * spd; g_freecamPos.z += up.z * spd;
            }
            if (GetAsyncKeyState('C') & 0x8000) {
                g_freecamPos.x -= up.x * spd; g_freecamPos.y -= up.y * spd; g_freecamPos.z -= up.z * spd;
            }

            // Inline writes from main loop too (reference writes from both threads)
            uintptr_t cam = g_freecamCamPtr.load();
            uintptr_t pvs = g_freecamPlayerVS.load();
            vector3 pos = g_freecamPos;
            if (cam && cam > 0x10000000ULL)
                write_dedicated<vector3>(cam + Off::CamPos, pos, dayzid);
            if (pvs && pvs > 0x10000000ULL)
                write_dedicated<vector3>(pvs + Off::GetCoord, pos, dayzid);
        }

        // ── LOOT TELEPORT — press T to TP nearest item to player ──
        if (freshData && g_lootTP.load() && cachedLocalPlayer && (GetAsyncKeyState(0x54) & 1)) {
            float bestDist = g_lootTPRange;
            uintptr_t bestVS = 0;
            std::string bestName;
            for (auto& ent : frame.entities) {
                if (!ent.isItem) continue;
                if (ent.dist < bestDist) {
                    bestDist = ent.dist;
                    bestVS = ent.visualState;
                    bestName = ent.name;
                }
            }
            if (bestVS && bestVS > 0x10000000ULL) {
                vector3 dest = {
                    frame.localPos.x + frame.camForward.x * 1.0f,
                    frame.localPos.y,
                    frame.localPos.z + frame.camForward.z * 1.0f
                };
                write_dedicated<vector3>(bestVS + Off::GetCoord, dest, dayzid);
                printf("[LOOT-TP] %s -> feet (was %.0fm)\n", bestName.c_str(), bestDist);
                fflush(stdout);
            }
        }

        // ── REMOTE LOOT — press Y to TP yourself to nearest item, hold, then snap back ──
        if (freshData && g_remoteLoot.load() && !g_remoteLootActive.load() && (GetAsyncKeyState(0x59) & 1)) {
            // Find nearest item
            float bestDist = g_remoteLootRange;
            vector3 bestPos = { 0,0,0 };
            std::string bestName;
            bool found = false;
            for (auto& ent : frame.entities) {
                if (!ent.isItem) continue;
                if (ent.dist < bestDist) {
                    bestDist = ent.dist;
                    bestPos = ent.pos;
                    bestName = ent.name;
                    found = true;
                }
            }
            if (found) {
                // Resolve playerPtr via 0x2960 chain
                uintptr_t lpPtr = read<uintptr_t>(frame.world + 0x2960, dayzid);
                if (lpPtr && lpPtr > 0x10000000ULL) {
                    uintptr_t p2 = read<uintptr_t>(lpPtr + 0x8, dayzid);
                    if (p2 && p2 > 0x10000000ULL) {
                        uintptr_t pVS = read<uintptr_t>(p2 + 0x120, dayzid);
                        if (pVS && pVS > 0x10000000ULL) {
                            // Save current position
                            g_remoteLootSavedPos = frame.localPos;
                            g_remoteLootTarget = bestPos;
                            g_remoteLootItemName = bestName;
                            g_remoteLootPlayerVS = pVS;
                            g_remoteLootCamPtr = frame.camPtr;
                            g_remoteLootStartMs = GetTickCount();
                            g_remoteLootActive = true;
                            printf("[REMOTE-LOOT] %s at %.0fm — holding %.1fs\n",
                                bestName.c_str(), bestDist, g_remoteLootTime);
                            fflush(stdout);
                        }
                    }
                }
            }
        }
        // Allow early cancel with Y while active
        if (g_remoteLootActive.load() && (GetAsyncKeyState(0x59) & 1)) {
            uintptr_t pvs = g_remoteLootPlayerVS.load();
            uintptr_t cam = g_remoteLootCamPtr.load();
            if (pvs && pvs > 0x10000000ULL)
                write_dedicated<vector3>(pvs + Off::GetCoord, g_remoteLootSavedPos, dayzid);
            if (cam && cam > 0x10000000ULL)
                write_dedicated<vector3>(cam + Off::CamPos, g_remoteLootSavedPos, dayzid);
            g_remoteLootActive = false;
            g_remoteLootPlayerVS = 0;
            g_remoteLootCamPtr = 0;
            printf("[REMOTE-LOOT] Cancelled — restored position\n"); fflush(stdout);
        }

        // ══════════════════════════════
        //  RENDER ESP
        // ══════════════════════════════
        if (espOn) {
            HDC hdc = GetDC(espH); HDC mem = CreateCompatibleDC(hdc);
            HBITMAP bmp = CreateCompatibleBitmap(hdc, SW, SH);
            SelectObject(mem, bmp); SelectObject(mem, espF);
            RECT rr = { 0,0,SW,SH }; HBRUSH cb = CreateSolidBrush(CLR); FillRect(mem, &rr, cb); DeleteObject(cb);
            SetBkMode(mem, TRANSPARENT); SetTextColor(mem, g_colPlayerBox);

            // ── GDI+ overlay rendering (antialiased ESP) ──
            Gdiplus::Graphics espGfx(mem);
            espGfx.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
            espGfx.SetTextRenderingHint(Gdiplus::TextRenderingHintClearTypeGridFit);
            g_gfx = &espGfx;

            // Crosshair (GDI+ antialiased)
            int cx = SW / 2, cy = SH / 2;
            if (g_gfx) {
                Gdiplus::Pen cp(GdipCol(COL_CROSS), 1.0f);
                g_gfx->DrawLine(&cp, cx - 8, cy, cx - 3, cy);
                g_gfx->DrawLine(&cp, cx + 3, cy, cx + 8, cy);
                g_gfx->DrawLine(&cp, cx, cy - 8, cx, cy - 3);
                g_gfx->DrawLine(&cp, cx, cy + 3, cx, cy + 8);
            }
            else {
                HPEN crossPen = CreatePen(PS_SOLID, 1, COL_CROSS); SelectObject(mem, crossPen);
                MoveToEx(mem, cx - 8, cy, NULL); LineTo(mem, cx - 3, cy);
                MoveToEx(mem, cx + 3, cy, NULL); LineTo(mem, cx + 8, cy);
                MoveToEx(mem, cx, cy - 8, NULL); LineTo(mem, cx, cy - 3);
                MoveToEx(mem, cx, cy + 3, NULL); LineTo(mem, cx, cy + 8);
                DeleteObject(crossPen);
            }


            // ── Hit feed (top-right) ──
            if (!g_hitLog.empty()) {
                SelectObject(mem, espFItem);
                int hfx = SW - 220, hfy = 30;
                // Header
                char hfHdr[48]; snprintf(hfHdr, sizeof(hfHdr), "HITS: %d", g_totalHits);
                SetTextColor(mem, RGB(255, 60, 60));
                TextOutA(mem, hfx, hfy, hfHdr, (int)strlen(hfHdr));
                hfy += 18;
                SelectObject(mem, espFItemDist);
                for (int hi = (int)g_hitLog.size() - 1; hi >= 0; hi--) {
                    HitEntry& he = g_hitLog[hi];
                    int age = tick - he.tick;
                    if (age > T_10S) continue; // fade after ~10s
                    int alpha = 255 - age * 255 / T_10S;
                    if (alpha < 40) alpha = 40;
                    char hfLine[64]; snprintf(hfLine, sizeof(hfLine), ">> HIT  %.0fm  [%s]", he.dist, he.bone.c_str());
                    SetTextColor(mem, RGB(alpha, alpha * 40 / 255, alpha * 40 / 255));
                    TextOutA(mem, hfx, hfy, hfLine, (int)strlen(hfLine));
                    hfy += 16;
                }
                SelectObject(mem, espF); SetTextColor(mem, g_colPlayerBox);
            }

            // ── Bullet tracers ──
            if (g_bulletTracers.load() && g_tracerCount > 0) {
                for (int t = 0; t < g_tracerCount; t++) {
                    if (!g_tracers[t].hasPrev) continue;
                    int x1t, y1t, x2t, y2t;
                    bool v1 = W2S(g_tracers[t].prev, x1t, y1t, frame, SW, SH);
                    bool v2 = W2S(g_tracers[t].cur, x2t, y2t, frame, SW, SH);
                    if (!v1 && !v2) continue;
                    // Clamp off-screen endpoints to screen edge
                    if (!v1) { x1t = (std::max)(0, (std::min)(SW, x1t)); y1t = (std::max)(0, (std::min)(SH, y1t)); }
                    if (!v2) { x2t = (std::max)(0, (std::min)(SW, x2t)); y2t = (std::max)(0, (std::min)(SH, y2t)); }
                    // Fade based on age
                    int age = tick - g_tracers[t].lastSeen;
                    BYTE alpha = (BYTE)(std::max)(40, 220 - age * 4);
                    if (g_gfx) {
                        Gdiplus::Pen tp(GdipCol(g_tracerColor, alpha), 1.5f);
                        g_gfx->DrawLine(&tp, x1t, y1t, x2t, y2t);
                        // Bright head dot at current position
                        if (v2) {
                            Gdiplus::SolidBrush hb(GdipCol(g_tracerColor, alpha));
                            g_gfx->FillEllipse(&hb, x2t - 2, y2t - 2, 4, 4);
                        }
                    }
                    else {
                        HPEN tp2 = CreatePen(PS_SOLID, 1, g_tracerColor);
                        SelectObject(mem, tp2);
                        MoveToEx(mem, x1t, y1t, NULL); LineTo(mem, x2t, y2t);
                        DeleteObject(tp2);
                    }
                }
            }

            // FOV circle (GDI+ antialiased)
            if (fovFilter) {
                int fovR = (int)g_fovRadiusF;
                if (g_gfx) {
                    Gdiplus::Pen fp(GdipCol(g_fovCircleColor, 140), 1.0f);
                    fp.SetDashStyle(Gdiplus::DashStyleDot);
                    g_gfx->DrawEllipse(&fp, cx - fovR, cy - fovR, fovR * 2, fovR * 2);
                }
                else {
                    HPEN fovPen = CreatePen(PS_DOT, 1, g_fovCircleColor);
                    SelectObject(mem, fovPen); SelectObject(mem, GetStockObject(NULL_BRUSH));
                    Ellipse(mem, cx - fovR, cy - fovR, cx + fovR, cy + fovR);
                    DeleteObject(fovPen);
                }
            }

            // Aim FOV circle + target line (GDI+ antialiased)
            if (g_silentAim.load()) {
                if (g_gfx) {
                    Gdiplus::Pen ap(GdipCol(RGB(255, 40, 40), 180), 1.0f);
                    g_gfx->DrawEllipse(&ap, cx - aimFovI, cy - aimFovI, aimFovI * 2, aimFovI * 2);
                }
                else {
                    HPEN aimPen = CreatePen(PS_SOLID, 1, RGB(255, 40, 40));
                    SelectObject(mem, aimPen); SelectObject(mem, GetStockObject(NULL_BRUSH));
                    Ellipse(mem, cx - aimFovI, cy - aimFovI, cx + aimFovI, cy + aimFovI);
                    DeleteObject(aimPen);
                }
                if (hasAimTarget) {
                    int atx, aty;
                    if (W2S(aimTarget, atx, aty, frame, SW, SH)) {
                        HPEN tline = CreatePen(PS_SOLID, 1, RGB(255, 60, 60));
                        SelectObject(mem, tline); MoveToEx(mem, cx, cy, NULL); LineTo(mem, atx, aty); DeleteObject(tline);
                        HBRUSH tdot = CreateSolidBrush(RGB(255, 40, 40));
                        SelectObject(mem, tdot); Ellipse(mem, atx - 4, aty - 4, atx + 4, aty + 4); DeleteObject(tdot);
                    }
                }
            }
            if (g_mouseAim.load() || g_railgunAim.load()) {
                HPEN maimPen = CreatePen(PS_SOLID, 1, RGB(40, 255, 40));
                SelectObject(mem, maimPen); SelectObject(mem, GetStockObject(NULL_BRUSH));
                Ellipse(mem, cx - aimFovI, cy - aimFovI, cx + aimFovI, cy + aimFovI);
                DeleteObject(maimPen);
                if (hasAimTarget) {
                    int atx, aty;
                    if (W2S(aimTarget, atx, aty, frame, SW, SH)) {
                        HPEN tline = CreatePen(PS_SOLID, 1, RGB(40, 255, 40));
                        SelectObject(mem, tline); MoveToEx(mem, cx, cy, NULL); LineTo(mem, atx, aty); DeleteObject(tline);
                        HBRUSH tdot = CreateSolidBrush(RGB(40, 255, 40));
                        SelectObject(mem, tdot); Ellipse(mem, atx - 4, aty - 4, atx + 4, aty + 4); DeleteObject(tdot);
                    }
                }
            }

            // ── RAID POINT MARKER ──
            if (g_raidMode.load() && g_raidPointSet) {
                int rpx, rpy;
                if (W2S(g_raidPoint, rpx, rpy, frame, SW, SH)) {
                    // Diamond crosshair — orange
                    HPEN rp = CreatePen(PS_SOLID, 2, RGB(255, 140, 40));
                    SelectObject(mem, rp);
                    // Diamond
                    MoveToEx(mem, rpx, rpy - 10, NULL); LineTo(mem, rpx + 10, rpy);
                    LineTo(mem, rpx, rpy + 10); LineTo(mem, rpx - 10, rpy); LineTo(mem, rpx, rpy - 10);
                    // Crosshair lines extending from diamond
                    MoveToEx(mem, rpx, rpy - 16, NULL); LineTo(mem, rpx, rpy - 10);
                    MoveToEx(mem, rpx, rpy + 10, NULL); LineTo(mem, rpx, rpy + 16);
                    MoveToEx(mem, rpx - 16, rpy, NULL); LineTo(mem, rpx - 10, rpy);
                    MoveToEx(mem, rpx + 10, rpy, NULL); LineTo(mem, rpx + 16, rpy);
                    DeleteObject(rp);
                    // Distance + active loops text
                    float rdx = g_raidPoint.x - frame.camPos.x;
                    float rdy = g_raidPoint.y - frame.camPos.y;
                    float rdz = g_raidPoint.z - frame.camPos.z;
                    float rdist = sqrtf(rdx * rdx + rdy * rdy + rdz * rdz);
                    char raidTxt[64];
                    if (g_raidBulletCount > 0) {
                        int totalLoops = 0;
                        for (int ri = 0; ri < g_raidBulletCount; ri++) totalLoops += g_raidBullets[ri].loops;
                        snprintf(raidTxt, sizeof(raidTxt), "RAID %.0fm [%d bullets, %d hits]", rdist, g_raidBulletCount, totalLoops);
                    }
                    else {
                        snprintf(raidTxt, sizeof(raidTxt), "RAID %.0fm", rdist);
                    }
                    SelectObject(mem, espFItemDist);
                    SetTextColor(mem, RGB(255, 160, 60));
                    SIZE rts; GetTextExtentPoint32A(mem, raidTxt, (int)strlen(raidTxt), &rts);
                    TextOutA(mem, rpx - rts.cx / 2, rpy + 18, raidTxt, (int)strlen(raidTxt));
                }
            }

            // ── MORTAR TARGET MARKER ──
            if (g_mortarMode.load() && g_mortarSet) {
                int mtx, mty;
                if (W2S(g_mortarTarget, mtx, mty, frame, SW, SH)) {
                    // Crosshair reticle — cyan/teal
                    HPEN mp = CreatePen(PS_SOLID, 2, RGB(0, 220, 220));
                    SelectObject(mem, mp);
                    SelectObject(mem, GetStockObject(NULL_BRUSH));
                    Ellipse(mem, mtx - 12, mty - 12, mtx + 12, mty + 12);
                    MoveToEx(mem, mtx, mty - 18, NULL); LineTo(mem, mtx, mty - 6);
                    MoveToEx(mem, mtx, mty + 6, NULL); LineTo(mem, mtx, mty + 18);
                    MoveToEx(mem, mtx - 18, mty, NULL); LineTo(mem, mtx - 6, mty);
                    MoveToEx(mem, mtx + 6, mty, NULL); LineTo(mem, mtx + 18, mty);
                    HBRUSH md = CreateSolidBrush(RGB(0, 255, 255));
                    SelectObject(mem, md);
                    Ellipse(mem, mtx - 2, mty - 2, mtx + 3, mty + 3);
                    DeleteObject(md);
                    DeleteObject(mp);
                    // Distance + label
                    float mdx = g_mortarTarget.x - frame.camPos.x;
                    float mdy = g_mortarTarget.y - frame.camPos.y;
                    float mdz = g_mortarTarget.z - frame.camPos.z;
                    float mdist = sqrtf(mdx * mdx + mdy * mdy + mdz * mdz);
                    char mortarTxt[96];
                    snprintf(mortarTxt, sizeof(mortarTxt), "(TARGET) %.0fm  [%.0f, %.0f, %.0f]",
                        mdist, g_mortarTarget.x, g_mortarTarget.y, g_mortarTarget.z);
                    SelectObject(mem, espFItemDist);
                    SIZE mts; GetTextExtentPoint32A(mem, mortarTxt, (int)strlen(mortarTxt), &mts);
                    int mLblX = mtx - mts.cx / 2 - 6, mLblY = mty + 20;
                    HBRUSH mBg = CreateSolidBrush(RGB(8, 20, 20));
                    HPEN mBgP = CreatePen(PS_SOLID, 1, RGB(0, 80, 80));
                    SelectObject(mem, mBg); SelectObject(mem, mBgP);
                    RoundRect(mem, mLblX, mLblY, mLblX + mts.cx + 12, mLblY + mts.cy + 4, 6, 6);
                    DeleteObject(mBg); DeleteObject(mBgP);
                    SetTextColor(mem, RGB(0, 220, 220));
                    TextOutA(mem, mtx - mts.cx / 2, mLblY + 2, mortarTxt, (int)strlen(mortarTxt));
                }
            }

            // Collect player screen positions during ESP for arrow reuse
            struct ArrowData { int sx, sy; float dist; bool onScreen; float dirX, dirZ; };
            std::vector<ArrowData> arrowPlayers;
            int zombiesRendered = 0;

            for (auto& ent : frame.entities) {
                if (ent.isPlayer && !espShowPlayers)continue;
                if (ent.isZombie && !espShowZombies)continue;
                if (ent.isZombie && zombiesRendered >= (int)g_maxZombieRender) continue;
                int sxF, syF, sxH, syH;
                bool w2sOk = W2S(ent.pos, sxF, syF, frame, SW, SH);

                // Stash player data for arrows (even if off-screen)
                if (ent.isPlayer && ent.dist >= 2.f && ent.dist <= 800.f) {
                    ArrowData ad;
                    ad.sx = sxF; ad.sy = syF; ad.dist = ent.dist; ad.onScreen = w2sOk;
                    ad.dirX = ent.pos.x - frame.camPos.x; ad.dirZ = ent.pos.z - frame.camPos.z;
                    arrowPlayers.push_back(ad);
                }

                if (!w2sOk)continue;
                if (!W2S(ent.headPos, sxH, syH, frame, SW, SH))continue;
                if (fovFilter && !InFovCircle((sxF + sxH) / 2, (syF + syH) / 2, SW, SH))continue;

                if (ent.isPlayer || ent.isZombie) {
                    if (ent.isZombie) zombiesRendered++;
                    COLORREF boxCol = ent.isZombie ? g_colZombieBox : g_colPlayerBox;
                    COLORREF snapCol = ent.isZombie ? RGB(200, 60, 20) : g_colSnapLine;
                    float a = 0.85f; auto& p = espPrev[ent.ptr];
                    float sx2 = p.first * (1 - a) + sxF * a, sy2 = p.second * (1 - a) + syF * a;
                    p.first = sx2; p.second = sy2;
                    int bH2 = abs(syF - syH), dW = (int)(1000.f / ent.dist);
                    int bW2 = (std::min)(bH2, (std::max)(dW, 6));
                    int ecx = (int)sx2;

                    // Corner box (GDI+ antialiased)
                    int left = ecx - bW2 / 2, right = ecx + bW2 / 2, top = syH, bot = syF;
                    int cLen = (std::max)(4, (std::min)(bH2 / 4, bW2 / 3));
                    if (g_gfx) {
                        Gdiplus::Pen bp(GdipCol(boxCol), 2.0f);
                        bp.SetStartCap(Gdiplus::LineCapRound);
                        bp.SetEndCap(Gdiplus::LineCapRound);
                        // Top-left
                        g_gfx->DrawLine(&bp, left, top + cLen, left, top); g_gfx->DrawLine(&bp, left, top, left + cLen, top);
                        // Top-right
                        g_gfx->DrawLine(&bp, right - cLen, top, right, top); g_gfx->DrawLine(&bp, right, top, right, top + cLen);
                        // Bottom-left
                        g_gfx->DrawLine(&bp, left, bot - cLen, left, bot); g_gfx->DrawLine(&bp, left, bot, left + cLen, bot);
                        // Bottom-right
                        g_gfx->DrawLine(&bp, right - cLen, bot, right, bot); g_gfx->DrawLine(&bp, right, bot, right, bot - cLen);
                        // Snapline
                        bool wantSnap = ent.isPlayer ? g_snapPlayers.load() : g_snapZombies.load();
                        if (wantSnap) {
                            Gdiplus::Pen sp2(GdipCol(snapCol, 180), 1.0f);
                            g_gfx->DrawLine(&sp2, SW / 2, SH - 1, ecx, syF);
                        }
                    }
                    else {
                        HPEN pen = CreatePen(PS_SOLID, 2, boxCol);
                        SelectObject(mem, pen);
                        MoveToEx(mem, left, top + cLen, NULL); LineTo(mem, left, top); LineTo(mem, left + cLen, top);
                        MoveToEx(mem, right - cLen, top, NULL); LineTo(mem, right, top); LineTo(mem, right, top + cLen);
                        MoveToEx(mem, left, bot - cLen, NULL); LineTo(mem, left, bot); LineTo(mem, left + cLen, bot);
                        MoveToEx(mem, right - cLen, bot, NULL); LineTo(mem, right, bot); LineTo(mem, right, bot - cLen);
                        DeleteObject(pen);
                        bool wantSnap2 = ent.isPlayer ? g_snapPlayers.load() : g_snapZombies.load();
                        if (wantSnap2) {
                            HPEN sp = CreatePen(PS_SOLID, 1, snapCol);
                            SelectObject(mem, sp); MoveToEx(mem, SW / 2, SH - 1, NULL); LineTo(mem, ecx, syF); DeleteObject(sp);
                        }
                    }

                    // Distance tag with dark rounded background
                    SelectObject(mem, espFItemDist);
                    char dt[32]; sprintf_s(dt, "%.0fm", ent.dist);
                    SIZE ts; GetTextExtentPoint32A(mem, dt, (int)strlen(dt), &ts);
                    int tagX = ecx - ts.cx / 2 - 4, tagY = syF + 3;
                    if (g_gfx) {
                        GdipRoundRect(tagX, tagY, ts.cx + 8, ts.cy + 2, 3, RGB(8, 10, 14), RGB(30, 32, 42), 1);
                    }
                    else {
                        HBRUSH dtBg = CreateSolidBrush(RGB(12, 14, 18));
                        RECT dtR = { tagX, tagY, tagX + ts.cx + 8, tagY + ts.cy + 2 };
                        FillRect(mem, &dtR, dtBg); DeleteObject(dtBg);
                    }
                    SetTextColor(mem, RGB(200, 210, 220));
                    TextOutA(mem, ecx - ts.cx / 2, tagY + 1, dt, (int)strlen(dt));

                    // Weapon in hand (players only)
                    if (ent.isPlayer && !ent.weaponName.empty()) {
                        const char* wn = ent.weaponName.c_str();
                        SIZE ws; GetTextExtentPoint32A(mem, wn, (int)ent.weaponName.length(), &ws);
                        int wTagX = ecx - ws.cx / 2 - 4, wTagY = tagY + ts.cy + 4;
                        if (g_gfx) {
                            GdipRoundRect(wTagX, wTagY, ws.cx + 8, ws.cy + 2, 3, RGB(8, 10, 14), RGB(40, 30, 20), 1);
                        }
                        else {
                            HBRUSH wBg = CreateSolidBrush(RGB(12, 14, 18));
                            RECT wR = { wTagX, wTagY, wTagX + ws.cx + 8, wTagY + ws.cy + 2 };
                            FillRect(mem, &wR, wBg); DeleteObject(wBg);
                        }
                        SetTextColor(mem, RGB(255, 160, 80));
                        TextOutA(mem, ecx - ws.cx / 2, wTagY + 1, wn, (int)ent.weaponName.length());
                    }

                    // ── Player inventory list (toggled via g_showPlayerInv) ──
                    if (ent.isPlayer && g_showPlayerInv.load() && !ent.inventory.empty() && ent.dist < 200.f) {
                        SelectObject(mem, espFItemDist);
                        int invStartY = syF + ts.cy + 4;
                        if (!ent.weaponName.empty()) invStartY += ts.cy + 6;
                        int maxW = 0;
                        // Measure max width first
                        for (auto& invItem : ent.inventory) {
                            SIZE is; GetTextExtentPoint32A(mem, invItem.c_str(), (int)invItem.length(), &is);
                            if (is.cx > maxW) maxW = is.cx;
                        }
                        int invPad = 5, lineH = 13;
                        int invBoxW = maxW + invPad * 2 + 6;
                        int invBoxH = (int)ent.inventory.size() * lineH + invPad * 2;
                        int invBoxX = ecx - invBoxW / 2;
                        // Semi-transparent inventory panel
                        if (g_gfx) {
                            Gdiplus::SolidBrush ibg(GdipCol(RGB(8, 8, 14), 160));
                            Gdiplus::Pen ibp(GdipCol(RGB(60, 55, 80), 120), 1.0f);
                            Gdiplus::GraphicsPath ip;
                            ip.AddArc(invBoxX, invStartY, 6, 6, 180, 90);
                            ip.AddArc(invBoxX + invBoxW - 6, invStartY, 6, 6, 270, 90);
                            ip.AddArc(invBoxX + invBoxW - 6, invStartY + invBoxH - 6, 6, 6, 0, 90);
                            ip.AddArc(invBoxX, invStartY + invBoxH - 6, 6, 6, 90, 90);
                            ip.CloseFigure();
                            g_gfx->FillPath(&ibg, &ip);
                            g_gfx->DrawPath(&ibp, &ip);
                        }
                        else {
                            HBRUSH ibg2 = CreateSolidBrush(RGB(8, 8, 14));
                            RECT ir2 = { invBoxX, invStartY, invBoxX + invBoxW, invStartY + invBoxH };
                            FillRect(mem, &ir2, ibg2); DeleteObject(ibg2);
                        }
                        // Render each item
                        int iy = invStartY + invPad;
                        for (auto& invItem : ent.inventory) {
                            // Color by category
                            int ic = CategorizeItem(invItem);
                            COLORREF icc = g_catColors[ic >= 0 && ic < CAT_COUNT ? ic : CAT_OTHER];
                            int ir = (GetRValue(icc) * 2 + 180) / 3;
                            int ig = (GetGValue(icc) * 2 + 180) / 3;
                            int ib = (GetBValue(icc) * 2 + 180) / 3;
                            SetTextColor(mem, RGB(ir > 255 ? 255 : ir, ig > 255 ? 255 : ig, ib > 255 ? 255 : ib));
                            TextOutA(mem, invBoxX + invPad + 4, iy, invItem.c_str(), (int)invItem.length());
                            iy += lineH;
                        }
                    }

                    if (ent.isZombie) {
                        SelectObject(mem, espF);
                        SetTextColor(mem, g_colPlayerBox);
                        TextOutA(mem, ecx - 12, syH - 14, "Zmb", 3);
                    }

                    // Tagged target indicator
                    if ((ent.isPlayer || ent.isZombie) && ent.ptr == g_taggedTarget) {
                        // Pulsing red circle around box
                        int pulse = (tick % T_1S < T_HALF_S) ? 255 : 180;
                        HPEN tagPen = CreatePen(PS_SOLID, 2, RGB(pulse, 20, 20));
                        SelectObject(mem, tagPen); SelectObject(mem, GetStockObject(NULL_BRUSH));
                        int pad = 6;
                        Ellipse(mem, left - pad, top - pad, right + pad, bot + pad);
                        DeleteObject(tagPen);
                        // TAG label above head
                        SelectObject(mem, espFItem);
                        SetTextColor(mem, RGB(255, 40, 40));
                        SIZE tagSz; GetTextExtentPoint32A(mem, "TAGGED", 6, &tagSz);
                        // Dark background for label
                        HBRUSH tagBg = CreateSolidBrush(RGB(40, 8, 8));
                        RECT tagR = { ecx - tagSz.cx / 2 - 4, syH - 28, ecx + tagSz.cx / 2 + 4, syH - 28 + tagSz.cy + 2 };
                        FillRect(mem, &tagR, tagBg); DeleteObject(tagBg);
                        TextOutA(mem, ecx - tagSz.cx / 2, syH - 28, "TAGGED", 6);
                    }

                    SelectObject(mem, espF); SetTextColor(mem, g_colPlayerBox);

                    if (((ent.isPlayer && g_showBones.load()) || (ent.isZombie && g_showZombieBones.load())) && ent.hasBones && ent.dist <= g_skeleDist) {
                        HPEN bp2 = CreatePen(PS_SOLID, 1, g_colBone); SelectObject(mem, bp2);
                        for (int li = 0; li < NUM_BONE_LINKS; li++) {
                            auto& lk = g_boneLinks[li];
                            if (!ent.bones[lk.from].valid || !ent.bones[lk.to].valid)continue;
                            int x1, y1, x2, y2;
                            if (W2S(ent.bones[lk.from].pos, x1, y1, frame, SW, SH) &&
                                W2S(ent.bones[lk.to].pos, x2, y2, frame, SW, SH))
                            {
                                MoveToEx(mem, x1, y1, NULL); LineTo(mem, x2, y2);
                            }
                        }
                        DeleteObject(bp2);
                        if (ent.bones[PB_HEAD].valid) {
                            int hx, hy;
                            if (W2S(ent.bones[PB_HEAD].pos, hx, hy, frame, SW, SH)) {
                                HBRUSH hb = CreateSolidBrush(g_colHeadDot);
                                SelectObject(mem, hb); Ellipse(mem, hx - 3, hy - 3, hx + 3, hy + 3); DeleteObject(hb);
                            }
                        }
                    }
                }
                else if (ent.isItem) {
                    int ecx = sxF, ecy = syF;

                    // Get category color (custom per-type)
                    int ci = ent.catIdx;
                    if (ci < 0 || ci >= CAT_COUNT) ci = CAT_OTHER;
                    COLORREF catCol = g_catColors[ci];

                    // Measure item name and distance separately
                    SelectObject(mem, espFItem);
                    const char* iname = ent.name.c_str();
                    int nameLen = (int)ent.name.length();
                    SIZE ns; GetTextExtentPoint32A(mem, iname, nameLen, &ns);

                    SelectObject(mem, espFItemDist);
                    char distBuf[24]; snprintf(distBuf, sizeof(distBuf), "%.0fm", ent.dist);
                    int distLen = (int)strlen(distBuf);
                    SIZE ds; GetTextExtentPoint32A(mem, distBuf, distLen, &ds);

                    // Total tag dimensions: name + gap + distance
                    int gap = 6;
                    int totalW = ns.cx + gap + ds.cx;
                    int tagH = ns.cy + 2;
                    int padX = 7, padY = 3;
                    int tagX = ecx - totalW / 2 - padX;
                    int tagY = ecy - tagH / 2 - padY - 2;
                    int tagW = totalW + padX * 2;
                    int tagHFull = tagH + padY * 2;

                    // Semi-transparent background with category-tinted border (GDI+)
                    if (g_gfx) {
                        // Alpha-blended dark pill — 70% opacity so stacked items are readable
                        Gdiplus::SolidBrush abg(GdipCol(RGB(10, 12, 16), 180));
                        Gdiplus::GraphicsPath tp;
                        tp.AddArc(tagX, tagY, 6, 6, 180, 90);
                        tp.AddArc(tagX + tagW - 6, tagY, 6, 6, 270, 90);
                        tp.AddArc(tagX + tagW - 6, tagY + tagHFull - 6, 6, 6, 0, 90);
                        tp.AddArc(tagX, tagY + tagHFull - 6, 6, 6, 90, 90);
                        tp.CloseFigure();
                        g_gfx->FillPath(&abg, &tp);
                        // Category-colored left accent strip
                        Gdiplus::SolidBrush acb(GdipCol(catCol, 200));
                        g_gfx->FillRectangle(&acb, tagX + 1, tagY + 3, 2, tagHFull - 6);
                        // Subtle border
                        Gdiplus::Pen bp(GdipCol(catCol, 60), 1.0f);
                        g_gfx->DrawPath(&bp, &tp);
                    }
                    else {
                        HBRUSH bgBr = CreateSolidBrush(RGB(12, 14, 18));
                        HPEN bgPen = CreatePen(PS_SOLID, 1, RGB(40, 44, 52));
                        SelectObject(mem, bgBr); SelectObject(mem, bgPen);
                        RoundRect(mem, tagX, tagY, tagX + tagW, tagY + tagHFull, 6, 6);
                        DeleteObject(bgBr); DeleteObject(bgPen);
                    }

                    // Item name — tinted by category color
                    int textX = ecx - totalW / 2;
                    int textY = tagY + padY;
                    SelectObject(mem, espFItem);
                    // Blend category color toward white for readability
                    int nr = (GetRValue(catCol) + 255) / 2;
                    int ng = (GetGValue(catCol) + 255) / 2;
                    int nb = (GetBValue(catCol) + 255) / 2;
                    SetTextColor(mem, RGB(nr, ng, nb));
                    TextOutA(mem, textX, textY, iname, nameLen);

                    // Distance — dimmer
                    SelectObject(mem, espFItemDist);
                    SetTextColor(mem, RGB(130, 140, 155));
                    TextOutA(mem, textX + ns.cx + gap, textY + (ns.cy - ds.cy), distBuf, distLen);
                }
            }

            // Well ESP
            if (g_showWells.load()) {
                SelectObject(mem, espF);
                for (int wi = 0; wi < NUM_WELLS; wi++) {
                    vector3 wpos = { g_wells[wi].x,frame.camPos.y,g_wells[wi].z };
                    float wdx = wpos.x - frame.camPos.x, wdz = wpos.z - frame.camPos.z;
                    float wdist = sqrtf(wdx * wdx + wdz * wdz);
                    if (wdist < 5.f || wdist>1500.f)continue;
                    int wsx, wsy;
                    if (!W2S(wpos, wsx, wsy, frame, SW, SH))continue;
                    HPEN wp = CreatePen(PS_SOLID, 1, COL_WELL); SelectObject(mem, wp);
                    int ix = wsx, iy2 = wsy - 10;
                    MoveToEx(mem, ix - 5, iy2 + 4, NULL); LineTo(mem, ix - 4, iy2 + 12);
                    LineTo(mem, ix + 4, iy2 + 12); LineTo(mem, ix + 5, iy2 + 4);
                    MoveToEx(mem, ix - 5, iy2 + 4, NULL); LineTo(mem, ix + 5, iy2 + 4);
                    MoveToEx(mem, ix - 3, iy2 + 4, NULL); LineTo(mem, ix, iy2); LineTo(mem, ix + 3, iy2 + 4);
                    MoveToEx(mem, ix - 3, iy2 + 8, NULL); LineTo(mem, ix - 1, iy2 + 7); LineTo(mem, ix + 1, iy2 + 9); LineTo(mem, ix + 3, iy2 + 8);
                    DeleteObject(wp);
                    char wl[48]; snprintf(wl, sizeof(wl), "Well  %.0fm", wdist);
                    SIZE ws; GetTextExtentPoint32A(mem, wl, (int)strlen(wl), &ws);
                    SetTextColor(mem, RGB(0, 0, 0)); TextOutA(mem, ix - ws.cx / 2 + 1, iy2 + 14, wl, (int)strlen(wl));
                    SetTextColor(mem, COL_WELL); TextOutA(mem, ix - ws.cx / 2, iy2 + 13, wl, (int)strlen(wl));
                }
                SetTextColor(mem, g_colPlayerBox);
            }

            // Landmark ESP
            if (g_showLandmarks.load()) {
                bool wantCities = g_lmCities.load(), wantTowns = g_lmTowns.load(), wantMili = g_lmMilitary.load();
                SelectObject(mem, espFL);
                for (int li = 0; li < NUM_LANDMARKS; li++) {
                    auto& lm = g_landmarks[li];
                    if (lm.cat == LM_CITY && !wantCities)continue;
                    if (lm.cat == LM_TOWN && !wantTowns)continue;
                    if (lm.cat == LM_MILI && !wantMili)continue;
                    vector3 lpos = { lm.x,frame.camPos.y,lm.z };
                    float dx = lpos.x - frame.camPos.x, dz = lpos.z - frame.camPos.z;
                    float ldist = sqrtf(dx * dx + dz * dz);
                    if (ldist < 50.f || ldist>8000.f)continue;
                    int lsx, lsy;
                    if (!W2S(lpos, lsx, lsy, frame, SW, SH))continue;
                    lsx = (std::max)(30, (std::min)(lsx, SW - 100));
                    lsy = (std::max)(20, (std::min)(lsy, SH - 20));
                    COLORREF lcol;
                    if (lm.cat == LM_MILI)lcol = RGB(255, 100, 100);
                    else if (lm.cat == LM_CITY)lcol = RGB(200, 180, 255);
                    else lcol = RGB(160, 200, 160);
                    char lb[64]; snprintf(lb, sizeof(lb), "%s [%.0fm]", lm.name, ldist);
                    SIZE ls; GetTextExtentPoint32A(mem, lb, (int)strlen(lb), &ls);
                    SetTextColor(mem, RGB(0, 0, 0)); TextOutA(mem, lsx - ls.cx / 2 + 1, lsy + 1, lb, (int)strlen(lb));
                    SetTextColor(mem, lcol); TextOutA(mem, lsx - ls.cx / 2, lsy, lb, (int)strlen(lb));
                }
                SelectObject(mem, espF); SetTextColor(mem, g_colPlayerBox);
            }

            // ── Waypoint ESP ──
            if (!g_waypoints.empty()) {
                SelectObject(mem, espFL);
                for (size_t wi = 0; wi < g_waypoints.size(); wi++) {
                    auto& wp = g_waypoints[wi];
                    vector3 wpPos = { wp.x, wp.y, wp.z };
                    int wsx, wsy;
                    if (!W2S(wpPos, wsx, wsy, frame, SW, SH)) continue;
                    float wdx = wp.x - frame.camPos.x, wdz = wp.z - frame.camPos.z;
                    float wdist = sqrtf(wdx * wdx + wdz * wdz);
                    HPEN wpPen = CreatePen(PS_SOLID, 2, RGB(0, 170, 255));
                    SelectObject(mem, wpPen); SelectObject(mem, GetStockObject(NULL_BRUSH));
                    POINT diamond[4] = { {wsx, wsy - 6},{wsx + 5, wsy},{wsx, wsy + 6},{wsx - 5, wsy} };
                    Polygon(mem, diamond, 4);
                    DeleteObject(wpPen);
                    char wpLbl[48]; snprintf(wpLbl, sizeof(wpLbl), "%s [%.0fm]", wp.name, wdist);
                    SetTextColor(mem, RGB(0, 170, 255));
                    SIZE ws; GetTextExtentPoint32A(mem, wpLbl, (int)strlen(wpLbl), &ws);
                    TextOutA(mem, wsx - ws.cx / 2, wsy + 8, wpLbl, (int)strlen(wpLbl));
                }
                SelectObject(mem, espF); SetTextColor(mem, g_colPlayerBox);
            }

            // ── Out-of-view player arrows (uses ESP-collected screen data) ──
            {
                int arrowRad = aimFovI;
                bool aimActive = g_silentAim.load() || g_mouseAim.load() || g_railgunAim.load();
                if (!aimActive) arrowRad = 120;

                if (!aimActive) {
                    HPEN indPen = CreatePen(PS_SOLID, 1, RGB(50, 55, 70));
                    SelectObject(mem, indPen); SelectObject(mem, GetStockObject(NULL_BRUSH));
                    Ellipse(mem, cx - arrowRad, cy - arrowRad, cx + arrowRad, cy + arrowRad);
                    DeleteObject(indPen);
                }

                for (size_t ai = 0; ai < arrowPlayers.size(); ai++) {
                    ArrowData& ad2 = arrowPlayers[ai];
                    float scrDx, scrDy;
                    if (ad2.onScreen) {
                        scrDx = (float)(ad2.sx - cx); scrDy = (float)(ad2.sy - cy);
                        if (scrDx * scrDx + scrDy * scrDy <= (float)(arrowRad * arrowRad)) continue;
                    }
                    else {
                        scrDx = ad2.dirX * frame.camRight.x + ad2.dirZ * frame.camRight.z;
                        scrDy = -(ad2.dirX * frame.camUp.x + ad2.dirZ * frame.camUp.z);
                        float fwdDot = ad2.dirX * frame.camForward.x + ad2.dirZ * frame.camForward.z;
                        if (fwdDot < 0) { scrDx = -scrDx; scrDy = -scrDy; }
                    }

                    float ang = atan2f(scrDy, scrDx);
                    float r = (float)arrowRad;
                    float tipR = r + 8.f, baseR = r - 3.f, perpOff = 5.f;
                    float ca = cosf(ang), sa = sinf(ang);
                    POINT tri[3];
                    tri[0] = { cx + (int)(tipR * ca),       cy + (int)(tipR * sa) };
                    tri[1] = { cx + (int)(baseR * ca - perpOff * sa), cy + (int)(baseR * sa + perpOff * ca) };
                    tri[2] = { cx + (int)(baseR * ca + perpOff * sa), cy + (int)(baseR * sa - perpOff * ca) };

                    int bright = 255 - (int)(ad2.dist * 0.3f);
                    if (bright < 80) bright = 80;
                    if (bright > 255) bright = 255;
                    HBRUSH arBr = CreateSolidBrush(RGB(bright, (int)(bright * 0.35f), (int)(bright * 0.35f)));
                    HPEN arPen = CreatePen(PS_SOLID, 1, RGB(bright, (int)(bright * 0.35f), (int)(bright * 0.35f)));
                    SelectObject(mem, arBr); SelectObject(mem, arPen);
                    Polygon(mem, tri, 3);
                    DeleteObject(arBr); DeleteObject(arPen);
                }
            }

            // ── Cleanup ESP GDI+ ──
            g_gfx = nullptr;

            BitBlt(hdc, 0, 0, SW, SH, mem, 0, 0, SRCCOPY);
            DeleteObject(bmp); DeleteDC(mem); ReleaseDC(espH, hdc);
        }

        // ══════════════════════════════
        //  RENDER RADAR (circular)
        // ══════════════════════════════
        if (radOn && (tick % 30 == 0)) {
            HDC hdc = GetDC(radH);
            if (hdc) {
                HDC mem = CreateCompatibleDC(hdc);
                HBITMAP mb = CreateCompatibleBitmap(hdc, RW, RH);
                HBITMAP ob = (HBITMAP)SelectObject(mem, mb);

                // Dark background
                RECT cl = { 0,0,RW,RH }; HBRUSH bg = CreateSolidBrush(RGB(10, 12, 16));
                FillRect(mem, &cl, bg); DeleteObject(bg);
                SetBkMode(mem, TRANSPARENT);

                const int RAD = 150; // circle radius
                int cx = RW / 2, cy = RAD + 16; // center with small top margin
                bool showTrails = g_radTrails.load();

                // Circular clip region
                HRGN clipRgn = CreateEllipticRgn(cx - RAD, cy - RAD, cx + RAD + 1, cy + RAD + 1);
                SelectClipRgn(mem, clipRgn);

                // Fill circle background
                HBRUSH circBg = CreateSolidBrush(RGB(6, 8, 12));
                HPEN circBgPen = CreatePen(PS_SOLID, 1, RGB(6, 8, 12));
                SelectObject(mem, circBg); SelectObject(mem, circBgPen);
                Ellipse(mem, cx - RAD, cy - RAD, cx + RAD, cy + RAD);
                DeleteObject(circBg); DeleteObject(circBgPen);

                // Concentric range rings
                SelectObject(mem, GetStockObject(NULL_BRUSH));
                for (int ring = 1; ring <= 3; ring++) {
                    int rr = RAD * ring / 3;
                    HPEN ringPen = CreatePen(PS_SOLID, 1, RGB(20, 25, 35));
                    SelectObject(mem, ringPen);
                    Ellipse(mem, cx - rr, cy - rr, cx + rr, cy + rr);
                    DeleteObject(ringPen);
                }

                // Cross-hair grid lines
                HPEN gridPen = CreatePen(PS_SOLID, 1, RGB(20, 25, 35));
                SelectObject(mem, gridPen);
                MoveToEx(mem, cx, cy - RAD, NULL); LineTo(mem, cx, cy + RAD);
                MoveToEx(mem, cx - RAD, cy, NULL); LineTo(mem, cx + RAD, cy);
                DeleteObject(gridPen);

                // Self dot
                HBRUSH selfBr = CreateSolidBrush(colors.self);
                HPEN selfPen = CreatePen(PS_SOLID, 1, RGB(0, 0, 0));
                SelectObject(mem, selfBr); SelectObject(mem, selfPen);
                Ellipse(mem, cx - 4, cy - 4, cx + 4, cy + 4);
                DeleteObject(selfBr); DeleteObject(selfPen);

                // Self direction arrow
                if (hasDir) {
                    int ax = cx + int(moveDir.x * 16), ay = cy + int(moveDir.z * 16);
                    HPEN sa = CreatePen(PS_SOLID, 2, colors.self);
                    SelectObject(mem, sa); MoveToEx(mem, cx, cy, NULL); LineTo(mem, ax, ay); DeleteObject(sa);
                }

                // Entities
                int playerCount = 0, zombieCount = 0;
                for (size_t i = 0; i < frame.entities.size(); i++) {
                    auto& ent = frame.entities[i];
                    if (!ent.isPlayer && !ent.isZombie)continue;
                    if (ent.isPlayer && !radShowPlayers)continue;
                    if (ent.isZombie && !radShowZombies)continue;
                    float dx = ent.pos.x - frame.localPos.x, dz = ent.pos.z - frame.localPos.z;
                    float d2 = sqrtf(dx * dx + dz * dz);
                    if (d2 > 1400 || d2 < 0.5f)continue;
                    int px = cx + int(dx * radScale), py = cy + int(dz * radScale);
                    // Clip to circle
                    float fromCenter = sqrtf((float)((px - cx) * (px - cx) + (py - cy) * (py - cy)));
                    if (fromCenter > RAD - 4) continue;

                    COLORREF dotCol = ent.isZombie ? colors.zombie : colors.player;
                    if (ent.isPlayer)playerCount++; else zombieCount++;

                    // Draw trails first (behind dots)
                    if (showTrails && ent.isPlayer) {
                        auto& trail = radTrails[ent.ptr];
                        // Add current position to trail (throttle by movement)
                        if (trail.empty() || sqrtf(powf(ent.pos.x - trail.back().x, 2) + powf(ent.pos.z - trail.back().z, 2)) > 1.5f) {
                            trail.push_back(ent.pos);
                            if ((int)trail.size() > MAX_TRAIL_POINTS)
                                trail.erase(trail.begin());
                        }
                        // Draw trail dots (fade out older ones)
                        for (int ti = 0; ti < (int)trail.size() - 1; ti++) {
                            float tdx = trail[ti].x - frame.localPos.x, tdz = trail[ti].z - frame.localPos.z;
                            int tx = cx + int(tdx * radScale), ty = cy + int(tdz * radScale);
                            float tf = sqrtf((float)((tx - cx) * (tx - cx) + (ty - cy) * (ty - cy)));
                            if (tf > RAD - 2) continue;
                            float fade = (float)ti / (float)trail.size();
                            int alpha = (int)(fade * 80) + 15;
                            COLORREF tc = RGB(
                                GetRValue(dotCol) * alpha / 255,
                                GetGValue(dotCol) * alpha / 255,
                                GetBValue(dotCol) * alpha / 255);
                            HBRUSH tb = CreateSolidBrush(tc);
                            SelectObject(mem, tb); SelectObject(mem, GetStockObject(NULL_PEN));
                            Ellipse(mem, tx - 1, ty - 1, tx + 2, ty + 2);
                            DeleteObject(tb);
                        }
                    }

                    // Dot with dark outline
                    int dotR = ent.isZombie ? 2 : 4;
                    HPEN outPen = CreatePen(PS_SOLID, 1, RGB(0, 0, 0));
                    HBRUSH db = CreateSolidBrush(dotCol);
                    SelectObject(mem, db); SelectObject(mem, outPen);
                    Ellipse(mem, px - dotR, py - dotR, px + dotR, py + dotR);
                    DeleteObject(db); DeleteObject(outPen);

                    // Direction arrow for moving players
                    if (ent.isPlayer) {
                        auto it = radPrev.find(ent.ptr);
                        if (it != radPrev.end()) {
                            vector3 dd = { ent.pos.x - it->second.x,0,ent.pos.z - it->second.z };
                            float len = sqrtf(dd.x * dd.x + dd.z * dd.z); if (len > 0.4f) {
                                int ex = px + int((dd.x / len) * 14), ey = py + int((dd.z / len) * 14);
                                HPEN ap2 = CreatePen(PS_SOLID, 2, colors.arrow);
                                SelectObject(mem, ap2); MoveToEx(mem, px, py, NULL); LineTo(mem, ex, ey); DeleteObject(ap2);
                            }
                        }
                        radPrev[ent.ptr] = ent.pos;
                    }
                }

                // Remove clip for outer ring and stats
                SelectClipRgn(mem, NULL);
                DeleteObject(clipRgn);

                // Outer ring border (double ring for clean look)
                SelectObject(mem, GetStockObject(NULL_BRUSH));
                HPEN outerPen = CreatePen(PS_SOLID, 2, RGB(40, 50, 65));
                SelectObject(mem, outerPen);
                Ellipse(mem, cx - RAD, cy - RAD, cx + RAD, cy + RAD);
                DeleteObject(outerPen);
                HPEN outerGlow = CreatePen(PS_SOLID, 1, RGB(25, 30, 40));
                SelectObject(mem, outerGlow);
                Ellipse(mem, cx - RAD - 2, cy - RAD - 2, cx + RAD + 2, cy + RAD + 2);
                DeleteObject(outerGlow);

                // Cardinal direction labels
                HFONT radLabel = CreateFontA(11, 0, 0, 0, FW_SEMIBOLD, 0, 0, 0, ANSI_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, "Segoe UI");
                SelectObject(mem, radLabel);
                SetTextColor(mem, RGB(60, 75, 95));
                TextOutA(mem, cx - 3, cy - RAD - 14, "N", 1);
                TextOutA(mem, cx - 3, cy + RAD + 3, "S", 1);
                TextOutA(mem, cx + RAD + 4, cy - 5, "E", 1);
                TextOutA(mem, cx - RAD - 12, cy - 5, "W", 1);
                DeleteObject(radLabel);

                // Stats bar below radar
                int ly = cy + RAD + 20;
                HFONT statFont = CreateFontA(12, 0, 0, 0, FW_NORMAL, 0, 0, 0, ANSI_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, "Segoe UI");
                SelectObject(mem, statFont);

                // Player/zombie count
                char cb2[80]; snprintf(cb2, sizeof(cb2), " %d Players   %d Zombies", playerCount, zombieCount);
                SetTextColor(mem, RGB(180, 190, 200));
                TextOutA(mem, 14, ly, cb2, (int)strlen(cb2));
                ly += 18;

                // Color pickers (compact)
                SetTextColor(mem, RGB(80, 90, 105));
                TextOutA(mem, 14, ly, "Colors (click to cycle):", 24); ly += 16;
                char c[80];
                HBRUSH sw;
                // Player color swatch + label
                sw = CreateSolidBrush(colors.player);
                RECT sr1 = { 14,ly + 2,24,ly + 12 }; FillRect(mem, &sr1, sw); DeleteObject(sw);
                snprintf(c, sizeof(c), "Player"); SetTextColor(mem, RGB(160, 170, 180)); TextOutA(mem, 28, ly, c, (int)strlen(c)); ly += 16;
                // Zombie
                sw = CreateSolidBrush(colors.zombie);
                RECT sr2 = { 14,ly + 2,24,ly + 12 }; FillRect(mem, &sr2, sw); DeleteObject(sw);
                snprintf(c, sizeof(c), "Zombie"); TextOutA(mem, 28, ly, c, (int)strlen(c)); ly += 16;
                // Other
                sw = CreateSolidBrush(colors.other);
                RECT sr3 = { 14,ly + 2,24,ly + 12 }; FillRect(mem, &sr3, sw); DeleteObject(sw);
                snprintf(c, sizeof(c), "Other"); TextOutA(mem, 28, ly, c, (int)strlen(c)); ly += 16;
                // Self
                sw = CreateSolidBrush(colors.self);
                RECT sr4 = { 14,ly + 2,24,ly + 12 }; FillRect(mem, &sr4, sw); DeleteObject(sw);
                snprintf(c, sizeof(c), "Self"); TextOutA(mem, 28, ly, c, (int)strlen(c)); ly += 16;
                // Arrow
                sw = CreateSolidBrush(colors.arrow);
                RECT sr5 = { 14,ly + 2,24,ly + 12 }; FillRect(mem, &sr5, sw); DeleteObject(sw);
                snprintf(c, sizeof(c), "Arrow"); TextOutA(mem, 28, ly, c, (int)strlen(c)); ly += 16;

                DeleteObject(statFont);

                BitBlt(hdc, 0, 0, RW, RH, mem, 0, 0, SRCCOPY);
                SelectObject(mem, ob); DeleteObject(mb); DeleteDC(mem); ReleaseDC(radH, hdc);
            }

            // Bearing bar
            if (hasDir) {
                HDC bh = GetDC(barH); HDC bm = CreateCompatibleDC(bh);
                HBITMAP bb = CreateCompatibleBitmap(bh, BW, BH); SelectObject(bm, bb);
                RECT br = { 0,0,BW,BH }; HBRUSH bbg = CreateSolidBrush(RGB(10, 14, 20));
                FillRect(bm, &br, bbg); DeleteObject(bbg);
                vector3 fwd = moveDir, right = { -fwd.z,0,fwd.x };
                int bcx = BW / 2, bcy = BH / 2;
                HPEN cp = CreatePen(PS_SOLID, 2, RGB(0, 200, 255));
                SelectObject(bm, cp); MoveToEx(bm, bcx, 4, NULL); LineTo(bm, bcx, BH - 4); DeleteObject(cp);
                for (auto& ent : frame.entities) {
                    if (!ent.isPlayer)continue;
                    float dx = ent.pos.x - frame.localPos.x, dz = ent.pos.z - frame.localPos.z;
                    float len = sqrtf(dx * dx + dz * dz); if (len < 1)continue;
                    float ex = dx / len, ez = dz / len; float side = ex * right.x + ez * right.z, forward = ex * fwd.x + ez * fwd.z;
                    if (forward > -0.25f) {
                        int bx = bcx + int(side * (BW / 2 - 6));
                        HBRUSH db = CreateSolidBrush(RGB(255, 80, 80));
                        SelectObject(bm, db); Ellipse(bm, bx - 3, bcy - 3, bx + 3, bcy + 3); DeleteObject(db);
                    }
                }
                BitBlt(bh, 0, 0, BW, BH, bm, 0, 0, SRCCOPY);
                DeleteObject(bb); DeleteDC(bm); ReleaseDC(barH, bh);
            }
        }

        drainWrites(1); // process at most 1 queued speed write per frame
        tick++; Sleep(2); // ~500fps with batch reads
    }

    DeleteObject(espF); DeleteObject(espFL); DeleteObject(espFItem); DeleteObject(espFItemDist); DeleteObject(radF);
    DestroyWindow(espH); DestroyWindow(radH); DestroyWindow(barH);
    UnregisterClassA("ESP_OVL", GetModuleHandleA(nullptr));
    UnregisterClassA("RADAR_OVL", GetModuleHandleA(nullptr));
    RestoreBrightness();
    g_overlayRunning = false;
    printf("[overlay] Stopped.\n");
}

// ── Remote loot dedicated write thread — spoof player to item location ──
static void RemoteLootWriteThread() {
    while (!g_shutdownAll.load()) {
        if (g_remoteLootActive.load()) {
            uintptr_t pvs = g_remoteLootPlayerVS.load();
            uintptr_t cam = g_remoteLootCamPtr.load();
            vector3 pos = g_remoteLootTarget;
            // Write BOTH camera and playerVS — same as freecam
            if (cam && cam > 0x10000000ULL)
                write_dedicated<vector3>(cam + Off::CamPos, pos, processId1);
            if (pvs && pvs > 0x10000000ULL)
                write_dedicated<vector3>(pvs + Off::GetCoord, pos, processId1);
            // Check timeout
            DWORD elapsed = GetTickCount() - g_remoteLootStartMs;
            if (elapsed >= (DWORD)(g_remoteLootTime * 1000.f)) {
                // Restore original position to both
                if (pvs && pvs > 0x10000000ULL)
                    write_dedicated<vector3>(pvs + Off::GetCoord, g_remoteLootSavedPos, processId1);
                if (cam && cam > 0x10000000ULL)
                    write_dedicated<vector3>(cam + Off::CamPos, g_remoteLootSavedPos, processId1);
                g_remoteLootActive = false;
                g_remoteLootPlayerVS = 0;
                g_remoteLootCamPtr = 0;
                printf("[REMOTE-LOOT] Done — restored position\n"); fflush(stdout);
            }
            // NO sleep — spin as fast as driver allows
        }
        else {
            Sleep(10);
        }
    }
}

// ── Freecam dedicated write thread — tight spin loop like reference DriverThread ──
static void FreecamWriteThread() {
    while (!g_shutdownAll.load()) {
        if (g_freecamActive.load()) {
            uintptr_t cam = g_freecamCamPtr.load();
            uintptr_t pvs = g_freecamPlayerVS.load();
            vector3 pos = g_freecamPos; // snapshot
            if (cam && cam > 0x10000000ULL)
                write_dedicated<vector3>(cam + Off::CamPos, pos, processId1);
            if (pvs && pvs > 0x10000000ULL)
                write_dedicated<vector3>(pvs + Off::GetCoord, pos, processId1);
            // NO sleep — spin as fast as the driver allows, just like reference
        }
        else {
            Sleep(10);
        }
    }
}

void StartOverlayThread(int d) {
    if (g_overlayRunning.exchange(true))return;
    g_shutdownAll = false;
    g_overlayThread = std::thread([=] {RunOverlays(d); }); g_overlayThread.detach();
    std::thread(FreecamWriteThread).detach();
    std::thread(RemoteLootWriteThread).detach();
}

void StopAll() {
    g_shutdownAll = true; Sleep(300);
    RestoreBrightness(); g_brightness = 1.0f;
    g_espEnabled = false; g_radarEnabled = false; g_showBones = false; g_showZombieBones = false; g_showItems = false;
    g_snapPlayers = true; g_snapZombies = false; g_skeleDist = 200.f; g_maxZombieRender = 30.f;
    g_fovOnly = false; g_silentAim = false; g_showLandmarks = false; g_showWells = false; g_mouseAim = false; g_railgunAim = false;
    g_laserFire = false; g_raidMode = false; g_raidPointSet = false; g_raidBulletCount = 0;
    g_mortarMode = false; g_mortarSet = false;
    g_showPlayerInv = false;
    g_bulletTracers = false; g_tracerCount = 0;
    g_lootTP = false;
    g_remoteLoot = false; g_remoteLootActive = false; g_remoteLootPlayerVS = 0; g_remoteLootCamPtr = 0;
    g_freecam = false; g_freecamActive = false; g_freecamCamPtr = 0; g_freecamPlayerVS = 0;
    g_noGrass = false; g_noGrassApplied = false;
    g_radTrails = false;
    g_writeHead = g_writeTail = 0; // discard pending writes
    g_entityCache.clear(); g_camCache.valid = false; // clear dead reckoning cache
    g_taggedTarget = 0;
}
// ═══════════════════════════════════════════════════════════════
//  CARD-BASED MENU UI — 5 Pages: Visuals | Aimbot | Radar | Filter | Exploits
// ═══════════════════════════════════════════════════════════════

// ── Layout constants ──
static constexpr int MW = 720;
static constexpr int MH = 540;
static constexpr int HDR_H = 48;
static constexpr int FTR_H = 30;
static constexpr int SB_W = 58;
static constexpr int CONT_L = SB_W;
static constexpr int CONT_W = MW - SB_W;
static constexpr int CONT_T = HDR_H;
static constexpr int CONT_VH = MH - HDR_H - FTR_H; // visible content height
static constexpr int CPAD = 10;   // content edge padding (tighter)
static constexpr int CGAP = 8;    // gap between cards (tighter)
static constexpr int COLW = (CONT_W - CPAD * 2 - CGAP) / 2;
static constexpr int CHDR = 40;   // card header height (bigger)
static constexpr int CPAD_I = 12;   // card internal side padding
static constexpr int ROW_H = 28;   // toggle row height (slightly taller for readability)
static constexpr int SLD_H = 42;   // slider total height
static constexpr int BONE_H = 34;   // bone selector
static constexpr int SEP_H = 10;   // separator
static constexpr int LABEL_H = 22;   // label row
static constexpr int TGL_W = 32;   // small toggle width
static constexpr int TGL_H = 17;   // small toggle height
static constexpr int HTGL_W = 38;   // header toggle width
static constexpr int HTGL_H = 21;   // header toggle height
static constexpr int CARD_R = 12;   // card corner radius

// ── Colors ──
static const COLORREF C_BG = RGB(18, 18, 22);
static const COLORREF C_SIDEBAR = RGB(13, 13, 16);
static const COLORREF C_HDR = RGB(20, 20, 24);
static const COLORREF C_CARD = RGB(22, 22, 28);
static const COLORREF C_CARD_HDR = RGB(19, 19, 24);
static const COLORREF C_BORDER = RGB(32, 34, 38);
static const COLORREF C_BORDER_A = RGB(42, 46, 52);  // active card border (will be tinted)
static const COLORREF C_TEXT = RGB(220, 225, 222);
static const COLORREF C_TEXT_DIM = RGB(108, 118, 112);
static const COLORREF C_TEXT_VDIM = RGB(55, 62, 58);
static const COLORREF C_ACCENT = RGB(0, 230, 118);
static const COLORREF C_GREEN = RGB(0, 230, 118);
static const COLORREF C_SB_LINE = RGB(28, 30, 32);
static const COLORREF C_TOGGLE_OFF = RGB(30, 30, 36);
static const COLORREF C_KNOB_OFF = RGB(68, 72, 70);
static const COLORREF C_SLD_BG = RGB(24, 24, 28);
static const COLORREF C_INFO_BG = RGB(16, 16, 20);
static const COLORREF C_BLUE = RGB(0, 170, 255);   // electric blue secondary accent

// ── Menu state ──
static HWND g_menuHwnd = nullptr;
static bool g_menuVisible = true;
static HFONT g_mF = nullptr, g_mFB = nullptr, g_mFS = nullptr, g_mFT = nullptr;
static bool g_menuDrag = false;
static POINT g_dragStart = {};
static int g_dayzid_menu = 0;
static int g_scrollY = 0;          // page scroll offset
static int g_contentH = 600;       // total content height (computed per page)
static int g_sidebarHover = -1;

// ── Hit zones (rebuilt each paint) ──
struct HitZone {
    RECT r;
    int type; // 0=toggle, 1=slider, 2=bone, 3=sidebar, 4=color_swatch, 5=filter_clear, 6=filter_row, 7=dropdown
    std::atomic<bool>* toggle;
    int idx;        // slider index, bone index, sidebar page, color index, filter row
    float* sval;    // slider value ptr
    float slo, shi, sstep;
};
static std::vector<HitZone> g_hz;

// Flat slider info for drag
struct LiveSlider {
    float* val; float lo, hi, step;
    int trackL, trackR, trackY;
};
static std::vector<LiveSlider> g_liveSliders;
static int g_sliderDragIdx = -1;

// ── Sidebar nav (icon-only) ──
struct SBItem { const char* label; int page; };
static const SBItem g_sbItems[] = {
    {"Visuals",  0},
    {"Aimbot",   1},
    {"Radar",    2},
    {"Filter",   3},
    {"Exploits", 4},
    {"Config",   5},
};
static constexpr int SB_COUNT = 6;
static constexpr int SB_ICON_SZ = 38;
static constexpr int SB_ICON_GAP = 2;

// ── GDI icon drawing ──
static void DrawIconEye2(HDC dc, int cx, int cy, COLORREF c) {
    HPEN p = CreatePen(PS_SOLID, 1, c); SelectObject(dc, p); SelectObject(dc, GetStockObject(NULL_BRUSH));
    Ellipse(dc, cx - 7, cy - 4, cx + 7, cy + 4);
    HBRUSH b = CreateSolidBrush(c); SelectObject(dc, b);
    Ellipse(dc, cx - 2, cy - 2, cx + 3, cy + 3); DeleteObject(b); DeleteObject(p);
}
static void DrawIconCross2(HDC dc, int cx, int cy, COLORREF c) {
    HPEN p = CreatePen(PS_SOLID, 1, c); SelectObject(dc, p); SelectObject(dc, GetStockObject(NULL_BRUSH));
    Ellipse(dc, cx - 6, cy - 6, cx + 6, cy + 6);
    Ellipse(dc, cx - 3, cy - 3, cx + 3, cy + 3);
    MoveToEx(dc, cx, cy - 7, NULL); LineTo(dc, cx, cy - 3);
    MoveToEx(dc, cx, cy + 3, NULL); LineTo(dc, cx, cy + 7);
    MoveToEx(dc, cx - 7, cy, NULL); LineTo(dc, cx - 3, cy);
    MoveToEx(dc, cx + 3, cy, NULL); LineTo(dc, cx + 7, cy);
    DeleteObject(p);
}
static void DrawIconRadar2(HDC dc, int cx, int cy, COLORREF c) {
    HPEN p = CreatePen(PS_SOLID, 1, c); SelectObject(dc, p); SelectObject(dc, GetStockObject(NULL_BRUSH));
    Ellipse(dc, cx - 6, cy - 6, cx + 6, cy + 6);
    Arc(dc, cx - 6, cy - 6, cx + 6, cy + 6, cx, cy - 6, cx + 6, cy);
    MoveToEx(dc, cx, cy, NULL); LineTo(dc, cx + 5, cy - 5);
    DeleteObject(p);
}
static void DrawIconFilter2(HDC dc, int cx, int cy, COLORREF c) {
    HPEN p = CreatePen(PS_SOLID, 2, c); SelectObject(dc, p);
    MoveToEx(dc, cx - 5, cy - 4, NULL); LineTo(dc, cx + 5, cy - 4);
    MoveToEx(dc, cx - 3, cy, NULL); LineTo(dc, cx + 3, cy);
    MoveToEx(dc, cx - 1, cy + 4, NULL); LineTo(dc, cx + 2, cy + 4);
    DeleteObject(p);
}
static void DrawIconGear2(HDC dc, int cx, int cy, COLORREF c) {
    HPEN p = CreatePen(PS_SOLID, 1, c); SelectObject(dc, p); SelectObject(dc, GetStockObject(NULL_BRUSH));
    Ellipse(dc, cx - 3, cy - 3, cx + 3, cy + 3);
    for (int i = 0; i < 8; i++) { float a = i * 0.785398f; int ex = cx + (int)(7 * cosf(a)); int ey = cy + (int)(7 * sinf(a)); MoveToEx(dc, cx + (int)(4 * cosf(a)), cy + (int)(4 * sinf(a)), NULL); LineTo(dc, ex, ey); }
    DeleteObject(p);
}
static void DrawIconBolt2(HDC dc, int cx, int cy, COLORREF c) {
    HPEN p = CreatePen(PS_SOLID, 2, c); SelectObject(dc, p);
    MoveToEx(dc, cx + 1, cy - 7, NULL); LineTo(dc, cx - 3, cy + 1); LineTo(dc, cx + 1, cy + 1); LineTo(dc, cx - 1, cy + 7);
    DeleteObject(p);
}
typedef void(*IconFn2)(HDC, int, int, COLORREF);
static IconFn2 g_sbIcons[] = { DrawIconEye2, DrawIconCross2, DrawIconRadar2, DrawIconFilter2, DrawIconBolt2, DrawIconGear2 };

// ── Color utility ──
static COLORREF TintBorder(COLORREF accent) {
    int r = GetRValue(accent) / 6 + GetRValue(C_BORDER);
    int g = GetGValue(accent) / 6 + GetGValue(C_BORDER);
    int b = GetBValue(accent) / 6 + GetBValue(C_BORDER);
    return RGB(r > 255 ? 255 : r, g > 255 ? 255 : g, b > 255 ? 255 : b);
}
static COLORREF DimColor(COLORREF c, int factor) {
    return RGB(GetRValue(c) * factor / 100, GetGValue(c) * factor / 100, GetBValue(c) * factor / 100);
}

// ── Color pointer lookup: 0-4=ESP colors, 5-13=item category colors ──
static COLORREF* GetColorPtr(int idx) {
    static COLORREF* espPtrs[] = { &g_colPlayerBox, &g_colZombieBox, &g_colSnapLine, &g_colBone, &g_colHeadDot };
    if (idx >= 0 && idx < 5) return espPtrs[idx];
    if (idx >= 5 && idx < 14) return &g_catColors[idx - 5];
    if (idx == 14) return &g_fovCircleColor;
    if (idx == 15) return &g_tracerColor;
    return nullptr;
}


// ═══════════════════════════════════════════════════════════════
//  CARD DRAWING HELPERS
// ═══════════════════════════════════════════════════════════════

// Draw card background + border. Returns content-start Y (below header).
static int CardBegin(HDC dc, int x, int y, int w, int h, const char* title, COLORREF accent,
    std::atomic<bool>* toggle, const char* badge, bool dimIfOff = true)
{
    bool on = toggle ? toggle->load() : true;
    COLORREF border = on ? TintBorder(accent) : C_BORDER;

    // ── GDI+ path: antialiased card with optional glow ──
    if (g_gfx) {
        if (on && toggle) GdipGlow(x, y, w, h, CARD_R, accent, 8, 20);
        GdipRoundRect(x, y, w, h, CARD_R, C_CARD, border, 1);
        // Gradient header fill when active
        if (on && toggle) {
            COLORREF hdrTop = RGB(
                (std::min)(255, GetRValue(accent) / 10 + GetRValue(C_CARD) + 10),
                (std::min)(255, GetGValue(accent) / 10 + GetGValue(C_CARD) + 8),
                (std::min)(255, GetBValue(accent) / 10 + GetBValue(C_CARD) + 14));
            COLORREF hdrBot = C_CARD;
            // Clip to card area for gradient header
            Gdiplus::GraphicsPath clipPath;
            int cd = CARD_R * 2;
            clipPath.AddArc(x + 1, y + 1, cd, cd, 180, 90);
            clipPath.AddArc(x + w - cd - 1, y + 1, cd, cd, 270, 90);
            clipPath.AddLine(x + w - 1, y + CHDR, x + 1, y + CHDR);
            clipPath.CloseFigure();
            Gdiplus::Region clipRgn(&clipPath);
            g_gfx->SetClip(&clipRgn);
            GdipGradientV(x + 1, y + 1, w - 2, CHDR, hdrTop, hdrBot);
            g_gfx->ResetClip();
        }
        // Header separator - subtle gradient line
        GdipLine(x + 1, y + CHDR, x + w - 1, y + CHDR, RGB(36, 38, 40), 1.0f);
    }
    else {
        // Fallback: original GDI
        HBRUSH bg = CreateSolidBrush(C_CARD);
        HPEN pen = CreatePen(PS_SOLID, 1, border);
        SelectObject(dc, bg); SelectObject(dc, pen);
        RoundRect(dc, x, y, x + w, y + h, CARD_R, CARD_R);
        DeleteObject(bg); DeleteObject(pen);
        if (on && toggle) {
            HBRUSH hbg = CreateSolidBrush(RGB(
                (std::min)(255, GetRValue(accent) / 20 + GetRValue(C_CARD) + 4),
                (std::min)(255, GetGValue(accent) / 20 + GetGValue(C_CARD) + 3),
                (std::min)(255, GetBValue(accent) / 20 + GetBValue(C_CARD) + 6)));
            HRGN hdrClip = CreateRoundRectRgn(x + 1, y + 1, x + w, y + CHDR + 1, CARD_R, CARD_R);
            SelectClipRgn(dc, hdrClip);
            RECT hr = { x + 1, y + 1, x + w - 1, y + CHDR };
            FillRect(dc, &hr, hbg); DeleteObject(hbg);
            SelectClipRgn(dc, NULL); DeleteObject(hdrClip);
        }
        HPEN hsep = CreatePen(PS_SOLID, 1, RGB(30, 32, 34));
        SelectObject(dc, hsep);
        MoveToEx(dc, x + 1, y + CHDR, NULL); LineTo(dc, x + w - 1, y + CHDR);
        DeleteObject(hsep);
    }

    // Title text
    SelectObject(dc, g_mFT); SetTextColor(dc, C_TEXT);
    TextOutA(dc, x + CPAD_I, y + 10, title, (int)strlen(title));
    // Badge
    if (badge) {
        SIZE ts; GetTextExtentPoint32A(dc, title, (int)strlen(title), &ts);
        int bx = x + CPAD_I + ts.cx + 8;
        SelectObject(dc, g_mFS);
        SIZE bs; GetTextExtentPoint32A(dc, badge, (int)strlen(badge), &bs);
        if (g_gfx) {
            COLORREF badgeBg = RGB(GetRValue(accent) / 10 + 8, GetGValue(accent) / 10 + 6, GetBValue(accent) / 10 + 12);
            COLORREF badgeBrd = RGB(GetRValue(accent) / 3, GetGValue(accent) / 3, GetBValue(accent) / 3);
            GdipRoundRect(bx, y + 13, bs.cx + 10, bs.cy + 3, 3, badgeBg, badgeBrd, 1);
        }
        else {
            HBRUSH bbg = CreateSolidBrush(RGB(GetRValue(accent) / 12, GetGValue(accent) / 12, GetBValue(accent) / 12));
            RECT br = { bx, y + 13, bx + bs.cx + 10, y + 13 + bs.cy + 3 };
            FillRect(dc, &br, bbg); DeleteObject(bbg);
            HPEN bpen = CreatePen(PS_SOLID, 1, RGB(GetRValue(accent) / 4, GetGValue(accent) / 4, GetBValue(accent) / 4));
            SelectObject(dc, bpen); SelectObject(dc, GetStockObject(NULL_BRUSH));
            RoundRect(dc, bx, y + 13, bx + bs.cx + 10, y + 13 + bs.cy + 3, 4, 4);
            DeleteObject(bpen);
        }
        SetTextColor(dc, accent);
        TextOutA(dc, bx + 5, y + 14, badge, (int)strlen(badge));
    }
    // Header toggle (antialiased pill + knob)
    if (toggle) {
        int tx = x + w - CPAD_I - HTGL_W;
        int ty = y + (CHDR - HTGL_H) / 2;
        COLORREF pillCol = on ? accent : C_TOGGLE_OFF;
        if (g_gfx) {
            GdipPill(tx, ty, HTGL_W, HTGL_H, pillCol);
            int kd = HTGL_H - 6;
            int kx = on ? (tx + HTGL_W - kd - 3) : (tx + 3);
            GdipCircle(kx + kd / 2, ty + HTGL_H / 2, kd / 2, on ? RGB(255, 255, 255) : C_KNOB_OFF);
            if (on) GdipRadialGlow(kx + kd / 2, ty + HTGL_H / 2, kd + 4, accent, 25);
        }
        else {
            HBRUSH pill = CreateSolidBrush(pillCol);
            SelectObject(dc, pill); SelectObject(dc, GetStockObject(NULL_PEN));
            RoundRect(dc, tx, ty, tx + HTGL_W, ty + HTGL_H, HTGL_H, HTGL_H);
            DeleteObject(pill);
            int kd = HTGL_H - 6;
            int kx = on ? (tx + HTGL_W - kd - 3) : (tx + 3);
            HBRUSH kb = CreateSolidBrush(on ? RGB(255, 255, 255) : C_KNOB_OFF);
            SelectObject(dc, kb);
            Ellipse(dc, kx, ty + 3, kx + kd, ty + 3 + kd);
            DeleteObject(kb);
        }
        HitZone hz; hz.r = { tx - 4, ty - 4, tx + HTGL_W + 4, ty + HTGL_H + 4 };
        hz.type = 0; hz.toggle = toggle; hz.idx = 0; hz.sval = nullptr;
        g_hz.push_back(hz);
    }
    return y + CHDR;
}

// Draw a toggle row inside a card. Returns new Y.
static int DrawTglRow(HDC dc, int x, int y, int w, const char* label, std::atomic<bool>* val, COLORREF accent) {
    bool on = val->load();
    SelectObject(dc, g_mF); SetTextColor(dc, on ? C_TEXT : C_TEXT_DIM);
    TextOutA(dc, x + CPAD_I, y + 5, label, (int)strlen(label));
    int tx = x + w - CPAD_I - TGL_W;
    int ty = y + (ROW_H - TGL_H) / 2;
    COLORREF pillCol = on ? accent : C_TOGGLE_OFF;
    if (g_gfx) {
        GdipPill(tx, ty, TGL_W, TGL_H, pillCol);
        int kd = TGL_H - 6;
        int kx = on ? (tx + TGL_W - kd - 3) : (tx + 3);
        GdipCircle(kx + kd / 2, ty + TGL_H / 2, kd / 2, on ? RGB(255, 255, 255) : C_KNOB_OFF);
    }
    else {
        HBRUSH pill = CreateSolidBrush(pillCol);
        SelectObject(dc, pill); SelectObject(dc, GetStockObject(NULL_PEN));
        RoundRect(dc, tx, ty, tx + TGL_W, ty + TGL_H, TGL_H, TGL_H);
        DeleteObject(pill);
        int kd = TGL_H - 6;
        int kx = on ? (tx + TGL_W - kd - 3) : (tx + 3);
        HBRUSH kb = CreateSolidBrush(on ? RGB(255, 255, 255) : C_KNOB_OFF);
        SelectObject(dc, kb);
        Ellipse(dc, kx, ty + 3, kx + kd, ty + 3 + kd);
        DeleteObject(kb);
    }
    HitZone hz; hz.r = { x, y, x + w, y + ROW_H }; hz.type = 0; hz.toggle = val; hz.idx = 0; hz.sval = nullptr;
    g_hz.push_back(hz);
    return y + ROW_H;
}


// Draw a slider row. Returns new Y.
static int DrawSldRow(HDC dc, int x, int y, int w, const char* label, float* val,
    float lo, float hi, float step, const char* unit, int dec, COLORREF accent)
{
    float v = *val;
    float pct = (v - lo) / (hi - lo);
    if (pct < 0) pct = 0; if (pct > 1) pct = 1;
    SelectObject(dc, g_mF); SetTextColor(dc, C_TEXT_DIM);
    TextOutA(dc, x + CPAD_I, y + 2, label, (int)strlen(label));
    char vbuf[32];
    if (dec == 0) snprintf(vbuf, sizeof(vbuf), "%.0f%s", v, unit);
    else snprintf(vbuf, sizeof(vbuf), "%.1f%s", v, unit);
    SetTextColor(dc, accent);
    SIZE vs; GetTextExtentPoint32A(dc, vbuf, (int)strlen(vbuf), &vs);
    TextOutA(dc, x + w - CPAD_I - vs.cx, y + 2, vbuf, (int)strlen(vbuf));
    int tL = x + CPAD_I, tR = x + w - CPAD_I, tY = y + 22;
    int knobX = tL + (int)(pct * (tR - tL));

    if (g_gfx) {
        // Track background (rounded)
        GdipRoundRect(tL, tY - 3, tR - tL, 6, 3, C_SLD_BG, C_SLD_BG, 0);
        // Filled portion with gradient
        if (knobX > tL + 2) {
            COLORREF fillDark = RGB(GetRValue(accent) / 2, GetGValue(accent) / 2, GetBValue(accent) / 2);
            Gdiplus::GraphicsPath fp;
            fp.AddArc(tL, tY - 3, 6, 6, 90, 180);
            int fEnd = knobX;
            fp.AddArc(fEnd - 6, tY - 3, 6, 6, 270, 180);
            fp.CloseFigure();
            Gdiplus::LinearGradientBrush gb(Gdiplus::Point(tL, tY - 3), Gdiplus::Point(tL, tY + 3), GdipCol(accent), GdipCol(fillDark));
            g_gfx->FillPath(&gb, &fp);
        }
        // Knob with glow
        GdipRadialGlow(knobX, tY, 10, accent, 20);
        GdipCircle(knobX, tY, 6, accent, C_CARD, 2);
        // White dot center
        GdipCircle(knobX, tY, 2, RGB(255, 255, 255));
    }
    else {
        int tH = 4;
        HBRUSH tbg = CreateSolidBrush(C_SLD_BG);
        RECT tr = { tL, tY - tH / 2, tR, tY + tH / 2 }; FillRect(dc, &tr, tbg); DeleteObject(tbg);
        HBRUSH tfill = CreateSolidBrush(accent);
        RECT fr = { tL, tY - tH / 2, knobX, tY + tH / 2 }; FillRect(dc, &fr, tfill); DeleteObject(tfill);
        int kr = 6;
        HBRUSH knob = CreateSolidBrush(accent);
        HPEN kpen = CreatePen(PS_SOLID, 2, C_CARD);
        SelectObject(dc, knob); SelectObject(dc, kpen);
        Ellipse(dc, knobX - kr, tY - kr, knobX + kr, tY + kr);
        DeleteObject(knob); DeleteObject(kpen);
    }

    LiveSlider ls; ls.val = val; ls.lo = lo; ls.hi = hi; ls.step = step;
    ls.trackL = tL; ls.trackR = tR; ls.trackY = tY;
    g_liveSliders.push_back(ls);
    int kr = 6;
    HitZone hz; hz.r = { tL - kr, tY - kr - 4, tR + kr, tY + kr + 4 };
    hz.type = 1; hz.toggle = nullptr; hz.idx = (int)g_liveSliders.size() - 1;
    hz.sval = val; hz.slo = lo; hz.shi = hi; hz.sstep = step;
    g_hz.push_back(hz);
    return y + SLD_H;
}


// Draw bone selector buttons. Returns new Y.
static int DrawBoneSel(HDC dc, int x, int y, int w, int* selected, COLORREF accent) {
    SelectObject(dc, g_mF); SetTextColor(dc, C_TEXT_DIM);
    TextOutA(dc, x + CPAD_I, y, "Target Bone", 11);
    y += 16;
    int bw = (w - CPAD_I * 2 - 9) / 4;
    for (int i = 0; i < 4; i++) {
        int bx = x + CPAD_I + i * (bw + 3);
        bool sel = (*selected == i);
        COLORREF bg = sel ? RGB(GetRValue(accent) / 6, GetGValue(accent) / 6, GetBValue(accent) / 6) : RGB(16, 16, 20);
        COLORREF brd = sel ? accent : RGB(30, 32, 34);
        if (g_gfx) {
            if (sel) GdipGlow(bx, y, bw, 22, 4, accent, 4, 15);
            GdipRoundRect(bx, y, bw, 22, 4, bg, brd, 1);
        }
        else {
            HBRUSH bb = CreateSolidBrush(bg);
            HPEN bp = CreatePen(PS_SOLID, 1, brd);
            SelectObject(dc, bb); SelectObject(dc, bp);
            RoundRect(dc, bx, y, bx + bw, y + 22, 6, 6);
            DeleteObject(bb); DeleteObject(bp);
        }
        SelectObject(dc, g_mFS);
        SetTextColor(dc, sel ? accent : C_TEXT_DIM);
        SIZE ts; GetTextExtentPoint32A(dc, g_boneChoiceNames[i], (int)strlen(g_boneChoiceNames[i]), &ts);
        TextOutA(dc, bx + (bw - ts.cx) / 2, y + 4, g_boneChoiceNames[i], (int)strlen(g_boneChoiceNames[i]));
        HitZone hz; hz.r = { bx, y, bx + bw, y + 22 }; hz.type = 2; hz.toggle = nullptr; hz.idx = i; hz.sval = (float*)selected;
        g_hz.push_back(hz);
    }
    return y + 26;
}

// Draw separator line. Returns new Y.
static int DrawCSep(HDC dc, int x, int y, int w) {
    if (g_gfx) {
        // Gradient separator: accent edge to dark center to accent edge
        int lx = x + CPAD_I, rx = x + w - CPAD_I, sy2 = y + SEP_H / 2;
        int mid = (lx + rx) / 2;
        Gdiplus::LinearGradientBrush gb(Gdiplus::Point(lx, sy2), Gdiplus::Point(rx, sy2),
            GdipCol(RGB(22, 22, 26), 60), GdipCol(RGB(40, 42, 44), 180));
        Gdiplus::Pen gp(&gb, 1.0f);
        g_gfx->DrawLine(&gp, lx, sy2, rx, sy2);
    }
    else {
        HPEN p = CreatePen(PS_SOLID, 1, RGB(28, 30, 32));
        SelectObject(dc, p);
        MoveToEx(dc, x + CPAD_I, y + SEP_H / 2, NULL); LineTo(dc, x + w - CPAD_I, y + SEP_H / 2);
        DeleteObject(p);
    }
    return y + SEP_H;
}

// Draw label + colored value row. Returns new Y.
static int DrawLabelVal(HDC dc, int x, int y, int w, const char* label, const char* val, COLORREF vc) {
    SelectObject(dc, g_mF);
    SetTextColor(dc, C_TEXT_DIM);
    TextOutA(dc, x + CPAD_I, y + 3, label, (int)strlen(label));
    SetTextColor(dc, vc);
    SIZE vs; GetTextExtentPoint32A(dc, val, (int)strlen(val), &vs);
    TextOutA(dc, x + w - CPAD_I - vs.cx, y + 3, val, (int)strlen(val));
    return y + LABEL_H;
}

// Draw label + key badge. Returns new Y.
static int DrawKeyRow(HDC dc, int x, int y, int w, const char* label, const char* key) {
    SelectObject(dc, g_mF);
    SetTextColor(dc, C_TEXT_DIM);
    TextOutA(dc, x + CPAD_I, y + 3, label, (int)strlen(label));
    SIZE ks; GetTextExtentPoint32A(dc, key, (int)strlen(key), &ks);
    int kx = x + w - CPAD_I - ks.cx - 12;
    HBRUSH kb = CreateSolidBrush(RGB(24, 24, 28));
    RECT kr = { kx, y + 1, kx + ks.cx + 12, y + LABEL_H - 1 };
    FillRect(dc, &kr, kb); DeleteObject(kb);
    HPEN kp = CreatePen(PS_SOLID, 1, RGB(36, 38, 40));
    SelectObject(dc, kp); SelectObject(dc, GetStockObject(NULL_BRUSH));
    RoundRect(dc, kr.left, kr.top, kr.right, kr.bottom, 4, 4);
    DeleteObject(kp);
    SetTextColor(dc, RGB(140, 142, 148));
    TextOutA(dc, kx + 6, y + 3, key, (int)strlen(key));
    return y + LABEL_H;
}

// Draw color swatch row. Returns new Y.
static int DrawSwatchRow(HDC dc, int x, int y, int w, const char* label, COLORREF col, int colorIdx) {
    SelectObject(dc, g_mF); SetTextColor(dc, C_TEXT_DIM);
    TextOutA(dc, x + CPAD_I, y + 3, label, (int)strlen(label));
    int sx = x + w - CPAD_I - 22, sy2 = y + 2;
    HBRUSH sb = CreateSolidBrush(col);
    RECT sr = { sx, sy2, sx + 22, sy2 + 14 }; FillRect(dc, &sr, sb); DeleteObject(sb);
    HPEN sp2 = CreatePen(PS_SOLID, 1, RGB(50, 52, 54));
    SelectObject(dc, sp2); SelectObject(dc, GetStockObject(NULL_BRUSH));
    RoundRect(dc, sx, sy2, sx + 22, sy2 + 14, 3, 3);
    DeleteObject(sp2);
    // Hit zone for color picker
    HitZone hz; hz.r = { sx - 4, sy2 - 4, sx + 26, sy2 + 18 }; hz.type = 4; hz.toggle = nullptr; hz.idx = colorIdx; hz.sval = nullptr;
    g_hz.push_back(hz);
    return y + LABEL_H;
}

// Draw an info box with text lines. Returns new Y.
static int DrawInfoBox(HDC dc, int x, int y, int w, COLORREF accent, const char** lines, int nlines) {
    int h = 8 + nlines * 16 + 8;
    COLORREF bg = RGB(GetRValue(accent) / 16 + 14, GetGValue(accent) / 16 + 12, GetBValue(accent) / 16 + 22);
    COLORREF brd = RGB(GetRValue(accent) / 8 + 20, GetGValue(accent) / 8 + 18, GetBValue(accent) / 8 + 30);
    HBRUSH bb = CreateSolidBrush(bg);
    HPEN bp = CreatePen(PS_SOLID, 1, brd);
    SelectObject(dc, bb); SelectObject(dc, bp);
    RoundRect(dc, x + CPAD_I, y, x + w - CPAD_I, y + h, 6, 6);
    DeleteObject(bb); DeleteObject(bp);
    SelectObject(dc, g_mFS); SetTextColor(dc, RGB(150, 152, 156));
    for (int i = 0; i < nlines; i++) {
        TextOutA(dc, x + CPAD_I + 8, y + 8 + i * 16, lines[i], (int)strlen(lines[i]));
    }
    return y + h + 4;
}

// Draw description text. Returns new Y.
static int DrawDesc(HDC dc, int x, int y, int w, const char* text) {
    SelectObject(dc, g_mFS); SetTextColor(dc, RGB(90, 94, 98));
    // Simple single-line (or use DrawTextA for wrapping)
    RECT r = { x + CPAD_I, y, x + w - CPAD_I, y + 40 };
    DrawTextA(dc, text, -1, &r, DT_WORDBREAK | DT_LEFT | DT_TOP);
    int h = DrawTextA(dc, text, -1, &r, DT_WORDBREAK | DT_LEFT | DT_TOP | DT_CALCRECT);
    return y + (r.bottom - r.top) + 4;
}

// Draw a dropdown (display only, click cycles). Returns new Y.
static int DrawDropRow(HDC dc, int x, int y, int w, const char* label, const char* val, COLORREF accent, int* choice, int nChoices) {
    SelectObject(dc, g_mFS); SetTextColor(dc, C_TEXT_DIM);
    TextOutA(dc, x + CPAD_I, y + 3, label, (int)strlen(label));
    SIZE vs; GetTextExtentPoint32A(dc, val, (int)strlen(val), &vs);
    int dx = x + w - CPAD_I - vs.cx - 12;
    HBRUSH db = CreateSolidBrush(RGB(18, 18, 22));
    RECT dr = { dx, y + 1, x + w - CPAD_I, y + LABEL_H - 1 };
    FillRect(dc, &dr, db); DeleteObject(db);
    HPEN dp = CreatePen(PS_SOLID, 1, RGB(36, 38, 40));
    SelectObject(dc, dp); SelectObject(dc, GetStockObject(NULL_BRUSH));
    RoundRect(dc, dr.left, dr.top, dr.right, dr.bottom, 4, 4);
    DeleteObject(dp);
    SetTextColor(dc, RGB(165, 168, 172));
    TextOutA(dc, dx + 6, y + 3, val, (int)strlen(val));
    // Hit zone for cycling
    HitZone hz; hz.r = { dx, y, x + w - CPAD_I, y + LABEL_H }; hz.type = 7;
    hz.toggle = nullptr; hz.idx = 0; hz.sval = (float*)choice; hz.slo = 0; hz.shi = (float)nChoices; hz.sstep = 1;
    g_hz.push_back(hz);
    return y + LABEL_H;
}

// Dropdown options for box style
static int g_boxStyleChoice = 0;
static const char* g_boxStyles[] = { "Corner", "Full", "3D" };

// ═══════════════════════════════════════════════════════════════
//  PAGE DRAWING FUNCTIONS
//  Each returns total content height needed.
// ═══════════════════════════════════════════════════════════════

static int DrawVisualsPage(HDC dc, int scrollY) {
    int x0 = CONT_L + CPAD, x1 = x0 + COLW + CGAP;
    int c0 = 0, c1 = 0; // column offsets from content top

    // ── Card: Player ESP (col 0) ──
    {
        int ch = CHDR + 8 * ROW_H + SEP_H + LABEL_H + SLD_H + 8;
        int cy = CONT_T + c0 - scrollY;
        CardBegin(dc, x0, cy, COLW, ch, "Player ESP", RGB(0, 170, 255), &g_espPlayers, nullptr);
        int y = cy + CHDR + 4;
        static std::atomic<bool> s_healthBar{ true }, s_names{ true }, s_dist{ true }, s_weapon{ true };
        y = DrawDropRow(dc, x0, y, COLW, "Box Style", g_boxStyles[g_boxStyleChoice], RGB(0, 170, 255), &g_boxStyleChoice, 3);
        y = DrawTglRow(dc, x0, y, COLW, "Health Bar", &s_healthBar, RGB(0, 170, 255));
        y = DrawTglRow(dc, x0, y, COLW, "Name", &s_names, RGB(0, 170, 255));
        y = DrawTglRow(dc, x0, y, COLW, "Distance", &s_dist, RGB(0, 170, 255));
        y = DrawTglRow(dc, x0, y, COLW, "Weapon", &s_weapon, RGB(0, 170, 255));
        y = DrawTglRow(dc, x0, y, COLW, "Skeleton", &g_showBones, RGB(0, 170, 255));
        y = DrawTglRow(dc, x0, y, COLW, "Snap Lines", &g_snapPlayers, RGB(0, 170, 255));
        y = DrawTglRow(dc, x0, y, COLW, "Inventory", &g_showPlayerInv, RGB(0, 170, 255));
        y = DrawCSep(dc, x0, y, COLW);
        y = DrawSwatchRow(dc, x0, y, COLW, "Color", g_colPlayerBox, 0);
        y = DrawSldRow(dc, x0, y, COLW, "Skeleton Dist", &g_skeleDist, 25, 500, 25, "m", 0, RGB(0, 170, 255));
        c0 += ch + CGAP;
    }

    // ── Card: Zombie ESP (col 1) ──
    {
        int ch = CHDR + 4 * ROW_H + SEP_H + LABEL_H + 2 * SLD_H + 8;
        int cy = CONT_T + c1 - scrollY;
        CardBegin(dc, x1, cy, COLW, ch, "Zombie ESP", RGB(245, 85, 85), &g_espZombies, nullptr);
        int y = cy + CHDR + 4;
        static std::atomic<bool> s_zHealthBar{ true }, s_zDist{ true };
        y = DrawTglRow(dc, x1, y, COLW, "Health Bar", &s_zHealthBar, RGB(245, 85, 85));
        y = DrawTglRow(dc, x1, y, COLW, "Distance", &s_zDist, RGB(245, 85, 85));
        y = DrawTglRow(dc, x1, y, COLW, "Skeleton", &g_showZombieBones, RGB(245, 85, 85));
        y = DrawTglRow(dc, x1, y, COLW, "Snap Lines", &g_snapZombies, RGB(245, 85, 85));
        y = DrawCSep(dc, x1, y, COLW);
        y = DrawSwatchRow(dc, x1, y, COLW, "Color", g_colZombieBox, 1);
        static float s_zDist2 = 200.f;
        y = DrawSldRow(dc, x1, y, COLW, "Max Distance", &s_zDist2, 50, 500, 10, "m", 0, RGB(245, 85, 85));
        y = DrawSldRow(dc, x1, y, COLW, "Max Rendered", &g_maxZombieRender, 15, 120, 15, "", 0, RGB(245, 85, 85));
        c1 += ch + CGAP;
    }

    // ── Card: Item ESP (col 0) ──
    {
        int ch = CHDR + 6 * ROW_H + SEP_H + SLD_H + 8;
        int cy = CONT_T + c0 - scrollY;
        static std::atomic<bool> s_wpn{ true }, s_ammo{ true }, s_med{ true }, s_food{ false }, s_cloth{ false }, s_bp{ true };
        CardBegin(dc, x0, cy, COLW, ch, "Item ESP", RGB(245, 190, 50), &g_showItems, nullptr);
        int y = cy + CHDR + 4;
        y = DrawTglRow(dc, x0, y, COLW, "Weapons", &s_wpn, RGB(245, 190, 50));
        y = DrawTglRow(dc, x0, y, COLW, "Ammo", &s_ammo, RGB(245, 190, 50));
        y = DrawTglRow(dc, x0, y, COLW, "Medical", &s_med, RGB(245, 190, 50));
        y = DrawTglRow(dc, x0, y, COLW, "Food", &s_food, RGB(245, 190, 50));
        y = DrawTglRow(dc, x0, y, COLW, "Clothing", &s_cloth, RGB(245, 190, 50));
        y = DrawTglRow(dc, x0, y, COLW, "Backpacks", &s_bp, RGB(245, 190, 50));
        y = DrawCSep(dc, x0, y, COLW);
        static float s_itemDist = 80.f;
        y = DrawSldRow(dc, x0, y, COLW, "Max Distance", &s_itemDist, 10, 200, 5, "m", 0, RGB(245, 190, 50));
        c0 += ch + CGAP;
    }

    // ── Card: Overlay (col 1) ──
    {
        int ch = CHDR + 3 * ROW_H + 2 * LABEL_H + SEP_H + 2 * SLD_H + 8;
        int cy = CONT_T + c1 - scrollY;
        CardBegin(dc, x1, cy, COLW, ch, "Overlay", C_ACCENT, &g_espEnabled, nullptr);
        int y = cy + CHDR + 4;
        y = DrawTglRow(dc, x1, y, COLW, "FOV Circle", &g_fovOnly, C_ACCENT);
        y = DrawTglRow(dc, x1, y, COLW, "Well ESP", &g_showWells, C_ACCENT);
        y = DrawTglRow(dc, x1, y, COLW, "Bullet Tracers", &g_bulletTracers, C_ACCENT);
        y = DrawSwatchRow(dc, x1, y, COLW, "FOV Color", g_fovCircleColor, 14);
        y = DrawSwatchRow(dc, x1, y, COLW, "Tracer Color", g_tracerColor, 15);
        y = DrawCSep(dc, x1, y, COLW);
        y = DrawSldRow(dc, x1, y, COLW, "FOV Radius", &g_fovRadiusF, 50, 800, 10, "px", 0, C_ACCENT);
        y = DrawSldRow(dc, x1, y, COLW, "Brightness", &g_brightness, 0.5f, 3.0f, 0.1f, "x", 1, C_ACCENT);
        c1 += ch + CGAP;
    }

    // ── Card: Item Colors (col 1) — per-category color customization ──
    {
        int ch = CHDR + 9 * LABEL_H + 8;
        int cy = CONT_T + c1 - scrollY;
        CardBegin(dc, x1, cy, COLW, ch, "Item Colors", RGB(245, 190, 50), nullptr, nullptr);
        int y = cy + CHDR + 4;
        for (int ci = 0; ci < 9; ci++) {
            y = DrawSwatchRow(dc, x1, y, COLW, g_catNames[ci], g_catColors[ci], 5 + ci);
        }
        c1 += ch + CGAP;
    }

    return (c0 > c1 ? c0 : c1);
}

static int DrawAimbotPage(HDC dc, int scrollY) {
    int x0 = CONT_L + CPAD, x1 = x0 + COLW + CGAP;
    int c0 = 0, c1 = 0;

    // ── Card: Magic Bullet (col 0) ──
    {
        int ch = CHDR + 30 + BONE_H + 2 * SLD_H + SEP_H + ROW_H + 8;
        int cy = CONT_T + c0 - scrollY;
        CardBegin(dc, x0, cy, COLW, ch, "Magic Bullet", RGB(245, 85, 85), &g_silentAim, "TP");
        int y = cy + CHDR + 4;
        y = DrawDesc(dc, x0, y, COLW, "Teleports bullet to target bone each tick.");
        y = DrawBoneSel(dc, x0, y, COLW, &g_mbBoneChoice, RGB(245, 85, 85));
        y = DrawSldRow(dc, x0, y, COLW, "Range", &g_silentRange, 5, 1000, 5, "m", 0, RGB(245, 85, 85));
        y = DrawSldRow(dc, x0, y, COLW, "Bullet Speed", &g_bulletSpeed, 500, 50000, 500, "m/s", 0, RGB(245, 85, 85));
        y = DrawCSep(dc, x0, y, COLW);
        y = DrawTglRow(dc, x0, y, COLW, "Death Markers", &g_deathMarkers, RGB(255, 100, 100));
        c0 += ch + CGAP;
    }

    // ── Card: Mouse Aimbot (col 1) ──
    {
        int ch = CHDR + 30 + BONE_H + 3 * SLD_H + SEP_H + ROW_H + SLD_H + 8;
        int cy = CONT_T + c1 - scrollY;
        CardBegin(dc, x1, cy, COLW, ch, "Mouse Aimbot", RGB(0, 230, 118), &g_mouseAim, nullptr);
        int y = cy + CHDR + 4;
        y = DrawDesc(dc, x1, y, COLW, "Moves crosshair toward target with mouse_event.");
        y = DrawBoneSel(dc, x1, y, COLW, &g_maBoneChoice, RGB(0, 230, 118));
        y = DrawSldRow(dc, x1, y, COLW, "Aim FOV", &g_aimFovF, 30, 300, 5, "px", 0, RGB(0, 230, 118));
        y = DrawSldRow(dc, x1, y, COLW, "Range", &g_mouseRange, 5, 500, 5, "m", 0, RGB(0, 230, 118));
        y = DrawSldRow(dc, x1, y, COLW, "Smoothing", &g_mouseSmooth, 1, 10, 0.5f, "x", 1, RGB(0, 230, 118));
        y = DrawCSep(dc, x1, y, COLW);
        y = DrawTglRow(dc, x1, y, COLW, "Prediction", &g_aimPrediction, RGB(0, 230, 118));
        y = DrawSldRow(dc, x1, y, COLW, "Lead Factor", &g_leadFactor, 0, 2, 0.1f, "x", 1, RGB(0, 230, 118));
        c1 += ch + CGAP;
    }

    // ── Card: Railgun Aimbot (col 0) ──
    {
        int ch = CHDR + 30 + BONE_H + 3 * SLD_H + SEP_H + LABEL_H + 8;
        int cy = CONT_T + c0 - scrollY;
        CardBegin(dc, x0, cy, COLW, ch, "Railgun", RGB(0, 170, 255), &g_railgunAim, "50K M/S");
        int y = cy + CHDR + 4;
        y = DrawDesc(dc, x0, y, COLW, "Mouse aimbot + forced 50,000 m/s bullet speed. Instant hit.");
        y = DrawBoneSel(dc, x0, y, COLW, &g_rgBoneChoice, RGB(0, 170, 255));
        y = DrawSldRow(dc, x0, y, COLW, "Aim FOV", &g_aimFovF, 30, 300, 5, "px", 0, RGB(0, 170, 255));
        y = DrawSldRow(dc, x0, y, COLW, "Range", &g_mouseRange, 5, 500, 5, "m", 0, RGB(0, 170, 255));
        y = DrawSldRow(dc, x0, y, COLW, "Smoothing", &g_mouseSmooth, 1, 10, 0.5f, "x", 1, RGB(0, 170, 255));
        y = DrawCSep(dc, x0, y, COLW);
        y = DrawLabelVal(dc, x0, y, COLW, "Forced Speed", "50,000 m/s", RGB(0, 170, 255));
        c0 += ch + CGAP;
    }

    // ── Card: Speed Hack (col 1) ──
    {
        int ch = CHDR + 30 + SLD_H + 30 + 8;
        int cy = CONT_T + c1 - scrollY;
        CardBegin(dc, x1, cy, COLW, ch, "Speed Hack", RGB(255, 150, 50), &g_laserFire, "RISKY");
        int y = cy + CHDR + 4;
        y = DrawDesc(dc, x1, y, COLW, "Overwrites InitSpeed on active firearm.");
        y = DrawSldRow(dc, x1, y, COLW, "Bullet Speed", &g_bulletSpeed, 500, 50000, 500, "m/s", 0, RGB(255, 150, 50));
        // Warning box
        const char* warn[] = { "!! Holster weapon to disable writes" };
        y = DrawInfoBox(dc, x1, y, COLW, RGB(255, 150, 50), warn, 1);
        c1 += ch + CGAP;
    }

    // ── Card: No Recoil (col 1) ──
    {
        int ch = CHDR + 30 + SLD_H + 8;
        int cy = CONT_T + c1 - scrollY;
        CardBegin(dc, x1, cy, COLW, ch, "No Recoil", RGB(250, 210, 40), &g_noRecoil, nullptr);
        int y = cy + CHDR + 4;
        y = DrawDesc(dc, x1, y, COLW, "Counter-pulls mouse to neutralize weapon recoil.");
        y = DrawSldRow(dc, x1, y, COLW, "Pull Strength", &g_recoilPull, 0.5f, 5, 0.25f, "px", 1, RGB(250, 210, 40));
        c1 += ch + CGAP;
    }

    // ── Card: No Grass (col 1) ──
    {
        int ch = CHDR + 30 + 8;
        int cy = CONT_T + c1 - scrollY;
        CardBegin(dc, x1, cy, COLW, ch, "No Grass", RGB(120, 200, 80), &g_noGrass, "VISUAL");
        int y = cy + CHDR + 4;
        y = DrawDesc(dc, x1, y, COLW, "Removes grass. Required for freecam.");
        c1 += ch + CGAP;
    }

    return (c0 > c1 ? c0 : c1);
}

static int DrawRadarPage(HDC dc, int scrollY) {
    int x0 = CONT_L + CPAD, x1 = x0 + COLW + CGAP;
    int c0 = 0, c1 = 0;

    // ── Card: Radar (col 0) ──
    {
        int ch = CHDR + 3 * ROW_H + SEP_H + 2 * SLD_H + 8;
        int cy = CONT_T + c0 - scrollY;
        CardBegin(dc, x0, cy, COLW, ch, "Radar", RGB(0, 170, 255), &g_radarEnabled, nullptr);
        int y = cy + CHDR + 4;
        y = DrawTglRow(dc, x0, y, COLW, "Players", &g_radPlayers, RGB(0, 170, 255));
        y = DrawTglRow(dc, x0, y, COLW, "Zombies", &g_radZombies, RGB(0, 170, 255));
        y = DrawTglRow(dc, x0, y, COLW, "Movement Trails", &g_radTrails, RGB(0, 170, 255));
        y = DrawCSep(dc, x0, y, COLW);
        static float s_radSize = 180.f, s_radZoom = 1.5f;
        y = DrawSldRow(dc, x0, y, COLW, "Radar Size", &s_radSize, 100, 350, 10, "px", 0, RGB(0, 170, 255));
        y = DrawSldRow(dc, x0, y, COLW, "Zoom", &s_radZoom, 0.5f, 5, 0.1f, "x", 1, RGB(0, 170, 255));
        c0 += ch + CGAP;
    }

    // ── Card: Landmarks (col 1) ──
    {
        int ch = CHDR + 3 * ROW_H + SEP_H + LABEL_H + 8;
        int cy = CONT_T + c1 - scrollY;
        CardBegin(dc, x1, cy, COLW, ch, "Landmarks", RGB(0, 170, 255), &g_showLandmarks, nullptr);
        int y = cy + CHDR + 4;
        y = DrawTglRow(dc, x1, y, COLW, "Cities", &g_lmCities, RGB(0, 170, 255));
        y = DrawTglRow(dc, x1, y, COLW, "Towns", &g_lmTowns, RGB(0, 170, 255));
        y = DrawTglRow(dc, x1, y, COLW, "Military", &g_lmMilitary, RGB(0, 170, 255));
        y = DrawCSep(dc, x1, y, COLW);
        y = DrawLabelVal(dc, x1, y, COLW, "42 landmarks  79 wells", "", C_TEXT_VDIM);
        c1 += ch + CGAP;
    }

    return (c0 > c1 ? c0 : c1);
}

static int DrawExploitsPage(HDC dc, int scrollY) {
    int x0 = CONT_L + CPAD, x1 = x0 + COLW + CGAP;
    int c0 = 0, c1 = 0;

    // ── Card: Raid Mode (col 0) ──
    {
        int ch = CHDR + 30 + 3 * SLD_H + SEP_H + LABEL_H + 8;
        int cy = CONT_T + c0 - scrollY;
        CardBegin(dc, x0, cy, COLW, ch, "Raid Mode", RGB(255, 150, 50), &g_raidMode, "WALLS");
        int y = cy + CHDR + 4;
        y = DrawDesc(dc, x0, y, COLW, "Bullet TP oscillation through walls. Aim at sky, fire.");
        y = DrawSldRow(dc, x0, y, COLW, "Marker Dist", &g_raidDist, 1, 50, 0.5f, "m", 1, RGB(255, 150, 50));
        y = DrawSldRow(dc, x0, y, COLW, "Penetration", &g_raidOscDist, 0.5f, 100, 0.5f, "m", 1, RGB(255, 150, 50));
        y = DrawSldRow(dc, x0, y, COLW, "Bullet Speed", &g_raidSpeed, 500, 10000, 500, "m/s", 0, RGB(255, 150, 50));
        y = DrawCSep(dc, x0, y, COLW);
        const char* status = g_raidMode.load() ? (g_raidPointSet ? "SET" : "ARMED") : "Idle";
        COLORREF sc = g_raidMode.load() ? RGB(255, 150, 50) : C_TEXT_DIM;
        y = DrawLabelVal(dc, x0, y, COLW, "Status", status, sc);
        c0 += ch + CGAP;
    }

    // ── Card: Mortar Mode (col 1) ──
    {
        int ch = CHDR + 30 + SLD_H + SEP_H + LABEL_H + 2 * ROW_H + 8;
        int cy = CONT_T + c1 - scrollY;
        CardBegin(dc, x1, cy, COLW, ch, "Mortar", RGB(0, 230, 118), &g_mortarMode, "XYZ TP");
        int y = cy + CHDR + 4;
        y = DrawDesc(dc, x1, y, COLW, "Teleport bullets to a fixed world coordinate.");
        y = DrawSldRow(dc, x1, y, COLW, "Bullet Speed", &g_mortarSpeed, 500, 50000, 500, "m/s", 0, RGB(0, 230, 118));
        y = DrawCSep(dc, x1, y, COLW);
        if (g_mortarSet) {
            char tgtBuf[64];
            snprintf(tgtBuf, sizeof(tgtBuf), "%.0f, %.0f, %.0f", g_mortarTarget.x, g_mortarTarget.y, g_mortarTarget.z);
            y = DrawLabelVal(dc, x1, y, COLW, "Target", tgtBuf, RGB(0, 220, 220));
        }
        else {
            y = DrawLabelVal(dc, x1, y, COLW, "Target", "NOT SET", C_TEXT_VDIM);
        }
        // "Set Target XYZ" button
        {
            int bx = x1 + CPAD_I, by = y + 2, bw2 = COLW - CPAD_I * 2, bh = ROW_H - 4;
            HBRUSH bb = CreateSolidBrush(RGB(16, 28, 28));
            HPEN bp = CreatePen(PS_SOLID, 1, RGB(0, 160, 160));
            SelectObject(dc, bb); SelectObject(dc, bp);
            RoundRect(dc, bx, by, bx + bw2, by + bh, 4, 4);
            DeleteObject(bb); DeleteObject(bp);
            SelectObject(dc, g_mFS); SetTextColor(dc, RGB(0, 230, 118));
            const char* setTxt = "Set Target X, Y, Z";
            SIZE ss; GetTextExtentPoint32A(dc, setTxt, (int)strlen(setTxt), &ss);
            TextOutA(dc, bx + (bw2 - ss.cx) / 2, by + 4, setTxt, (int)strlen(setTxt));
            HitZone hz; hz.r = { bx, by, bx + bw2, by + bh }; hz.type = 12; hz.toggle = nullptr; hz.idx = 0; hz.sval = nullptr;
            g_hz.push_back(hz);
            y += ROW_H;
        }
        // "Clear Target" button
        if (g_mortarSet) {
            int bx = x1 + CPAD_I, by = y + 2, bw2 = COLW - CPAD_I * 2, bh = ROW_H - 4;
            HBRUSH bb = CreateSolidBrush(RGB(24, 16, 16));
            HPEN bp = CreatePen(PS_SOLID, 1, RGB(160, 60, 60));
            SelectObject(dc, bb); SelectObject(dc, bp);
            RoundRect(dc, bx, by, bx + bw2, by + bh, 4, 4);
            DeleteObject(bb); DeleteObject(bp);
            SelectObject(dc, g_mFS); SetTextColor(dc, RGB(245, 85, 85));
            const char* clrTxt = "Clear Target";
            SIZE cs; GetTextExtentPoint32A(dc, clrTxt, (int)strlen(clrTxt), &cs);
            TextOutA(dc, bx + (bw2 - cs.cx) / 2, by + 4, clrTxt, (int)strlen(clrTxt));
            HitZone hz; hz.r = { bx, by, bx + bw2, by + bh }; hz.type = 13; hz.toggle = nullptr; hz.idx = 0; hz.sval = nullptr;
            g_hz.push_back(hz);
            y += ROW_H;
        }
        c1 += ch + CGAP;
    }

    // ── Card: Loot Teleport (col 1) ──
    {
        int ch = CHDR + 30 + ROW_H + SLD_H + SEP_H + LABEL_H + 8;
        int cy = CONT_T + c1 - scrollY;
        CardBegin(dc, x1, cy, COLW, ch, "Loot Teleport", RGB(245, 190, 50), &g_lootTP, "ITEMS");
        int y = cy + CHDR + 4;
        y = DrawDesc(dc, x1, y, COLW, "Press T to TP nearest item to your feet. Requires Item ESP on.");
        y = DrawTglRow(dc, x1, y, COLW, "Enabled", &g_lootTP, RGB(245, 190, 50));
        y = DrawSldRow(dc, x1, y, COLW, "Max Range", &g_lootTPRange, 10, 150, 5, "m", 0, RGB(245, 190, 50));
        y = DrawCSep(dc, x1, y, COLW);
        y = DrawKeyRow(dc, x1, y, COLW, "Grab Nearest", "T");
        c1 += ch + CGAP;
    }

    // ── Card: Remote Loot (col 1) ──
    {
        int ch = CHDR + 30 + ROW_H + 2 * SLD_H + SEP_H + LABEL_H + 8;
        int cy = CONT_T + c1 - scrollY;
        CardBegin(dc, x1, cy, COLW, ch, "Remote Loot", RGB(255, 140, 60), &g_remoteLoot, "ITEMS");
        int y = cy + CHDR + 4;
        y = DrawDesc(dc, x1, y, COLW, "Press Y to TP yourself to nearest item. Snaps back after timer.");
        y = DrawTglRow(dc, x1, y, COLW, "Enabled", &g_remoteLoot, RGB(255, 140, 60));
        y = DrawSldRow(dc, x1, y, COLW, "Max Range", &g_remoteLootRange, 10, 500, 10, "m", 0, RGB(255, 140, 60));
        y = DrawSldRow(dc, x1, y, COLW, "Hold Time", &g_remoteLootTime, 2.0f, 15.0f, 0.5f, "s", 1, RGB(255, 140, 60));
        y = DrawCSep(dc, x1, y, COLW);
        y = DrawKeyRow(dc, x1, y, COLW, "Loot / Cancel", "Y");
        c1 += ch + CGAP;
    }

    // ── Card: Freecam (col 0) ──
    {
        int ch = CHDR + 30 + ROW_H + LABEL_H + 3 * SLD_H + 8;
        int cy = CONT_T + c0 - scrollY;
        CardBegin(dc, x0, cy, COLW, ch, "Freecam", RGB(100, 180, 255), &g_freecam, "CAMERA");
        int y = cy + CHDR + 4;
        y = DrawDesc(dc, x0, y, COLW, "WASD+Space/C to fly. Shift=fast, Ctrl=slow.");
        y = DrawTglRow(dc, x0, y, COLW, "Enabled", &g_freecam, RGB(100, 180, 255));
        y = DrawDropRow(dc, x0, y, COLW, "Hotkey", g_freecamHotkeys[g_freecamHotkeyIdx].name, RGB(100, 180, 255), &g_freecamHotkeyIdx, g_freecamHotkeyCount);
        y = DrawSldRow(dc, x0, y, COLW, "Slow Speed", &g_freecamSpeedSlow, 0.05f, 1.0f, 0.05f, "", 2, RGB(100, 180, 255));
        y = DrawSldRow(dc, x0, y, COLW, "Normal Speed", &g_freecamSpeedNormal, 0.1f, 5.0f, 0.1f, "", 1, RGB(100, 180, 255));
        y = DrawSldRow(dc, x0, y, COLW, "Fast Speed", &g_freecamSpeedFast, 1.0f, 30.0f, 1.0f, "", 0, RGB(100, 180, 255));
        c0 += ch + CGAP;
    }

    // ── Card: Waypoints (col 0) ──
    {
        int wpCount = (int)g_waypoints.size();
        int ch = CHDR + 30 + wpCount * LABEL_H + 2 * ROW_H + 4 + 8; // 2 buttons now
        if (ch < CHDR + 110) ch = CHDR + 110;
        int cy = CONT_T + c0 - scrollY;
        static std::atomic<bool> s_wpEnabled{ false };
        CardBegin(dc, x0, cy, COLW, ch, "Waypoints", RGB(0, 170, 255), &s_wpEnabled, nullptr);
        int y = cy + CHDR + 4;
        y = DrawDesc(dc, x0, y, COLW, "ESP markers at saved world positions.");
        if (wpCount == 0) {
            SelectObject(dc, g_mFS); SetTextColor(dc, C_TEXT_VDIM);
            TextOutA(dc, x0 + CPAD_I, y + 4, "No waypoints saved", 18);
            y += LABEL_H;
        }
        else {
            for (int wi = 0; wi < wpCount; wi++) {
                char wpBuf[64];
                snprintf(wpBuf, sizeof(wpBuf), "%s (%.0f, %.0f, %.0f)", g_waypoints[wi].name, g_waypoints[wi].x, g_waypoints[wi].y, g_waypoints[wi].z);
                y = DrawLabelVal(dc, x0, y, COLW, wpBuf, "X", RGB(245, 85, 85));
                // Hit zone on the "X" to delete
                HitZone hz; hz.r = { x0 + COLW - CPAD_I - 20, y - LABEL_H, x0 + COLW - CPAD_I, y };
                hz.type = 10; hz.toggle = nullptr; hz.idx = wi; hz.sval = nullptr;
                g_hz.push_back(hz);
            }
        }
        // "Add Current Position" button
        {
            int bx = x0 + CPAD_I, by = y + 2, bw2 = COLW - CPAD_I * 2, bh = ROW_H - 4;
            HBRUSH bb = CreateSolidBrush(RGB(24, 24, 28));
            HPEN bp = CreatePen(PS_SOLID, 1, RGB(0, 170, 255));
            SelectObject(dc, bb); SelectObject(dc, bp);
            RoundRect(dc, bx, by, bx + bw2, by + bh, 4, 4);
            DeleteObject(bb); DeleteObject(bp);
            SelectObject(dc, g_mFS); SetTextColor(dc, RGB(0, 170, 255));
            const char* addTxt = "+ Where I'm Standing";
            SIZE as; GetTextExtentPoint32A(dc, addTxt, (int)strlen(addTxt), &as);
            TextOutA(dc, bx + (bw2 - as.cx) / 2, by + 4, addTxt, (int)strlen(addTxt));
            HitZone hz; hz.r = { bx, by, bx + bw2, by + bh }; hz.type = 8; hz.toggle = nullptr; hz.idx = 0; hz.sval = nullptr;
            g_hz.push_back(hz);
            y += ROW_H;
        }
        // "Add Custom XYZ" button
        {
            int bx = x0 + CPAD_I, by = y + 2, bw2 = COLW - CPAD_I * 2, bh = ROW_H - 4;
            HBRUSH bb = CreateSolidBrush(RGB(20, 18, 30));
            HPEN bp = CreatePen(PS_SOLID, 1, RGB(100, 95, 130));
            SelectObject(dc, bb); SelectObject(dc, bp);
            RoundRect(dc, bx, by, bx + bw2, by + bh, 4, 4);
            DeleteObject(bb); DeleteObject(bp);
            SelectObject(dc, g_mFS); SetTextColor(dc, RGB(140, 135, 165));
            const char* custTxt = "+ Custom X, Y, Z";
            SIZE cs; GetTextExtentPoint32A(dc, custTxt, (int)strlen(custTxt), &cs);
            TextOutA(dc, bx + (bw2 - cs.cx) / 2, by + 4, custTxt, (int)strlen(custTxt));
            HitZone hz; hz.r = { bx, by, bx + bw2, by + bh }; hz.type = 11; hz.toggle = nullptr; hz.idx = 0; hz.sval = nullptr;
            g_hz.push_back(hz);
        }
        c0 += ch + CGAP;
    }

    // ── Card: Hit Log (full width) ──
    {
        int fullW = CONT_W - CPAD * 2;
        int maxCol = c0 > c1 ? c0 : c1;
        int logCount = (int)g_hitLog.size();
        int visLog = logCount > 8 ? 8 : logCount; // show max 8 entries
        int logH = visLog > 0 ? visLog * 18 + 8 : 24;
        int ch = CHDR + logH + 16;
        int cy = CONT_T + maxCol - scrollY;
        CardBegin(dc, x0, cy, fullW, ch, "Hit Log", RGB(245, 85, 85), nullptr, nullptr);
        int y = cy + CHDR + 4;
        // Log area background
        HBRUSH lbg = CreateSolidBrush(RGB(10, 10, 12));
        RECT lr = { x0 + CPAD_I, y, x0 + fullW - CPAD_I, y + logH + 4 };
        FillRect(dc, &lr, lbg); DeleteObject(lbg);
        HPEN lp = CreatePen(PS_SOLID, 1, RGB(22, 20, 34));
        SelectObject(dc, lp); SelectObject(dc, GetStockObject(NULL_BRUSH));
        RoundRect(dc, lr.left, lr.top, lr.right, lr.bottom, 6, 6);
        DeleteObject(lp);
        SelectObject(dc, g_mFS);
        if (logCount == 0) {
            SetTextColor(dc, C_TEXT_VDIM);
            TextOutA(dc, x0 + CPAD_I + 8, y + 6, "No hits recorded yet", 20);
        }
        else {
            // Show hits newest first (max 8)
            DWORD now = GetTickCount();
            int showStart = logCount - visLog; // start index
            for (int hi = logCount - 1; hi >= showStart; hi--) {
                HitEntry& he = g_hitLog[hi];
                int row = (logCount - 1 - hi);
                if (row >= visLog) break; // safety
                int ry = y + 4 + row * 18;
                // Timestamp (seconds ago)
                int secsAgo = (int)((now - he.timestamp) / 1000);
                char timeBuf[16];
                if (secsAgo < 60) snprintf(timeBuf, sizeof(timeBuf), "%ds ago", secsAgo);
                else snprintf(timeBuf, sizeof(timeBuf), "%dm ago", secsAgo / 60);
                SetTextColor(dc, C_TEXT_VDIM);
                TextOutA(dc, x0 + CPAD_I + 8, ry, timeBuf, (int)strlen(timeBuf));
                // HIT badge
                SetTextColor(dc, RGB(245, 85, 85));
                TextOutA(dc, x0 + CPAD_I + 70, ry, "HIT", 3);
                // Details
                char detBuf[64];
                snprintf(detBuf, sizeof(detBuf), "%s  %.0fm  %s", he.bone.c_str(), he.dist, he.method.c_str());
                SetTextColor(dc, C_TEXT_DIM);
                TextOutA(dc, x0 + CPAD_I + 100, ry, detBuf, (int)strlen(detBuf));
            }
        }
        // Total hits count on right side
        char totBuf[32]; snprintf(totBuf, sizeof(totBuf), "Total: %d", g_totalHits);
        SIZE ts; GetTextExtentPoint32A(dc, totBuf, (int)strlen(totBuf), &ts);
        SetTextColor(dc, RGB(245, 85, 85));
        TextOutA(dc, x0 + fullW - CPAD_I - ts.cx - 4, cy + CHDR - ts.cy - 4, totBuf, (int)strlen(totBuf));

        maxCol += ch + CGAP;
        return maxCol;
    }
}

static int DrawConfigPage(HDC dc, int scrollY) {
    int x0 = CONT_L + CPAD, x1 = x0 + COLW + CGAP;
    int c0 = 0, c1 = 0;

    // ── Card: Colors (col 0) ──
    {
        int ch = CHDR + 5 * LABEL_H + 8;
        int cy = CONT_T + c0 - scrollY;
        CardBegin(dc, x0, cy, COLW, ch, "Colors", C_ACCENT, nullptr, nullptr);
        int y = cy + CHDR + 4;
        y = DrawSwatchRow(dc, x0, y, COLW, "Player Box", g_colPlayerBox, 0);
        y = DrawSwatchRow(dc, x0, y, COLW, "Zombie Box", g_colZombieBox, 1);
        y = DrawSwatchRow(dc, x0, y, COLW, "Snap Lines", g_colSnapLine, 2);
        y = DrawSwatchRow(dc, x0, y, COLW, "Bones", g_colBone, 3);
        y = DrawSwatchRow(dc, x0, y, COLW, "Head Dot", g_colHeadDot, 4);
        c0 += ch + CGAP;
    }

    // ── Card: Keybinds (col 1) ──
    {
        int ch = CHDR + 5 * LABEL_H + 8;
        int cy = CONT_T + c1 - scrollY;
        CardBegin(dc, x1, cy, COLW, ch, "Keybinds", RGB(140, 150, 160), nullptr, nullptr);
        int y = cy + CHDR + 4;
        y = DrawKeyRow(dc, x1, y, COLW, "Menu Toggle", "INS");
        y = DrawKeyRow(dc, x1, y, COLW, "Tag Target", "MMB");
        y = DrawKeyRow(dc, x1, y, COLW, "Raid Place", "MMB");
        y = DrawKeyRow(dc, x1, y, COLW, "Loot TP", "T");
        y = DrawKeyRow(dc, x1, y, COLW, "Quick ESP", "F1");
        c1 += ch + CGAP;
    }

    // ── Card: System (full width) ──
    {
        int fullW = CONT_W - CPAD * 2;
        int maxCol = c0 > c1 ? c0 : c1;
        int ch = CHDR + 60 + 8;
        int cy = CONT_T + maxCol - scrollY;
        CardBegin(dc, x0, cy, fullW, ch, "System", RGB(110, 120, 130), nullptr, nullptr);
        int y = cy + CHDR + 8;
        // 4x2 stat grid
        struct Stat { const char* l; char v[32]; COLORREF c; };
        Stat stats[8];
        snprintf(stats[0].v, 32, "v1.28.8"); stats[0].l = "VERSION"; stats[0].c = C_ACCENT;
        snprintf(stats[1].v, 32, "Active"); stats[1].l = "DRIVER"; stats[1].c = C_GREEN;
        snprintf(stats[2].v, 32, "GDI"); stats[2].l = "OVERLAY"; stats[2].c = RGB(0, 170, 255);
        snprintf(stats[3].v, 32, "~500"); stats[3].l = "FPS"; stats[3].c = RGB(245, 190, 50);
        snprintf(stats[4].v, 32, "--"); stats[4].l = "ENTITIES"; stats[4].c = RGB(0, 170, 255);
        snprintf(stats[5].v, 32, "--"); stats[5].l = "ITEMS"; stats[5].c = RGB(245, 190, 50);
        snprintf(stats[6].v, 32, "--"); stats[6].l = "NEAR"; stats[6].c = RGB(245, 85, 85);
        snprintf(stats[7].v, 32, "%d", processId1); stats[7].l = "PID"; stats[7].c = RGB(110, 120, 130);

        int cellW = (fullW - CPAD_I * 2 - 3 * 8) / 4;
        int cellH = 44;
        for (int i = 0; i < 8; i++) {
            int col = i % 4, row = i / 4;
            int cx = x0 + CPAD_I + col * (cellW + 8);
            int cy2 = y + row * (cellH + 6);
            HBRUSH cb = CreateSolidBrush(RGB(16, 16, 20));
            RECT cr = { cx, cy2, cx + cellW, cy2 + cellH }; FillRect(dc, &cr, cb); DeleteObject(cb);
            HPEN cp = CreatePen(PS_SOLID, 1, RGB(28, 30, 32));
            SelectObject(dc, cp); SelectObject(dc, GetStockObject(NULL_BRUSH));
            RoundRect(dc, cx, cy2, cx + cellW, cy2 + cellH, 6, 6);
            DeleteObject(cp);
            SelectObject(dc, g_mFS); SetTextColor(dc, C_TEXT_VDIM);
            TextOutA(dc, cx + 8, cy2 + 4, stats[i].l, (int)strlen(stats[i].l));
            SelectObject(dc, g_mFT); SetTextColor(dc, stats[i].c);
            TextOutA(dc, cx + 8, cy2 + 20, stats[i].v, (int)strlen(stats[i].v));
        }
        maxCol += ch + CGAP;
        return maxCol;
    }
}


// Filter page (adapted from original, now in a card)
struct FilterRow { bool isHeader; int catIdx; int itemIdx; };
static std::vector<FilterRow> BuildFilterRows() {
    std::vector<FilterRow> rows;
    int lastCat = -1;
    for (int i = 0; i < (int)g_nearbyItems.size(); i++) {
        if (g_nearbyItems[i].cat != lastCat) {
            lastCat = g_nearbyItems[i].cat;
            FilterRow hr; hr.isHeader = true; hr.catIdx = lastCat; hr.itemIdx = -1;
            rows.push_back(hr);
        }
        FilterRow ir; ir.isHeader = false; ir.catIdx = g_nearbyItems[i].cat; ir.itemIdx = i;
        rows.push_back(ir);
    }
    return rows;
}

static int DrawFilterPage(HDC dc, int scrollY) {
    int x0 = CONT_L + CPAD;
    int fullW = CONT_W - CPAD * 2;
    int cy = CONT_T - scrollY;

    // Search card
    {
        int ch = CHDR + 60;
        CardBegin(dc, x0, cy, fullW, ch, "Item Filter", RGB(245, 190, 50), nullptr, nullptr);
        int y = cy + CHDR + 8;
        // Search box
        RECT sr = { x0 + CPAD_I, y, x0 + fullW - CPAD_I, y + 28 };
        HBRUSH sbx = CreateSolidBrush(RGB(16, 16, 20)); FillRect(dc, &sr, sbx); DeleteObject(sbx);
        std::string filterDisplay;
        { std::lock_guard<std::mutex> lk(g_itemMtx); filterDisplay = g_espFilterText; }
        HPEN sp3 = CreatePen(PS_SOLID, 1, filterDisplay.empty() ? RGB(36, 38, 40) : RGB(245, 190, 50));
        SelectObject(dc, sp3); SelectObject(dc, GetStockObject(NULL_BRUSH));
        RoundRect(dc, sr.left, sr.top, sr.right, sr.bottom, 4, 4);
        DeleteObject(sp3);
        SelectObject(dc, g_mF);
        if (filterDisplay.empty()) {
            SetTextColor(dc, C_TEXT_VDIM);
            TextOutA(dc, x0 + CPAD_I + 8, y + 6, "Search items...", 15);
        }
        else {
            SetTextColor(dc, C_TEXT);
            TextOutA(dc, x0 + CPAD_I + 8, y + 6, filterDisplay.c_str(), (int)filterDisplay.length());
            if ((GetTickCount() / 500) % 2 == 0) {
                SIZE cs2; GetTextExtentPoint32A(dc, filterDisplay.c_str(), (int)filterDisplay.length(), &cs2);
                HPEN cp2 = CreatePen(PS_SOLID, 1, RGB(245, 190, 50)); SelectObject(dc, cp2);
                MoveToEx(dc, x0 + CPAD_I + 8 + cs2.cx + 2, y + 4, NULL); LineTo(dc, x0 + CPAD_I + 8 + cs2.cx + 2, y + 24); DeleteObject(cp2);
            }
        }
        cy += ch + CGAP;
    }

    // Items card
    {
        std::lock_guard<std::mutex> lk(g_itemMtx);
        int numItems = (int)g_nearbyItems.size();
        int numSelected = (int)g_selectedItems.size();
        std::vector<FilterRow> rows = BuildFilterRows();
        int totalRows = (int)rows.size();
        int maxScroll2 = totalRows > FILTER_MAX_VISIBLE ? totalRows - FILTER_MAX_VISIBLE : 0;
        if (g_filterScroll > maxScroll2) g_filterScroll = maxScroll2;
        int visCount = totalRows < FILTER_MAX_VISIBLE ? totalRows : FILTER_MAX_VISIBLE;
        int ch = CHDR + 24 + visCount * FILTER_ROW_H + 24 + 8;
        CardBegin(dc, x0, cy, fullW, ch, "Nearby Items", RGB(245, 190, 50), nullptr, nullptr);
        int y = cy + CHDR + 4;
        // Header row
        char hdr2[80];
        if (numSelected > 0) snprintf(hdr2, sizeof(hdr2), "%d items - %d selected  [CLEAR]", numItems, numSelected);
        else snprintf(hdr2, sizeof(hdr2), "%d items nearby", numItems);
        SelectObject(dc, g_mFS); SetTextColor(dc, numSelected > 0 ? RGB(245, 190, 50) : C_TEXT_DIM);
        TextOutA(dc, x0 + CPAD_I, y, hdr2, (int)strlen(hdr2));
        if (numSelected > 0) {
            // Clear button hit zone
            HitZone hz; hz.r = { x0 + CPAD_I, y, x0 + fullW - CPAD_I, y + 16 };
            hz.type = 5; hz.toggle = nullptr; hz.idx = 0; hz.sval = nullptr;
            g_hz.push_back(hz);
        }
        y += 20;
        HPEN sep4 = CreatePen(PS_SOLID, 1, RGB(30, 28, 42));
        SelectObject(dc, sep4); MoveToEx(dc, x0 + CPAD_I, y, NULL); LineTo(dc, x0 + fullW - CPAD_I, y); DeleteObject(sep4);
        y += 4;
        for (int vi = 0; vi < visCount; vi++) {
            int ri = vi + g_filterScroll; if (ri >= totalRows) break;
            FilterRow& fr = rows[ri];
            if (fr.isHeader) {
                COLORREF catCol = g_catColors[fr.catIdx];
                HBRUSH accBr = CreateSolidBrush(catCol);
                RECT accR = { x0 + CPAD_I, y + 3, x0 + CPAD_I + 3, y + FILTER_ROW_H - 3 };
                FillRect(dc, &accR, accBr); DeleteObject(accBr);
                SelectObject(dc, g_mFS); SetTextColor(dc, catCol);
                TextOutA(dc, x0 + CPAD_I + 8, y + 4, g_catNames[fr.catIdx], (int)strlen(g_catNames[fr.catIdx]));
            }
            else {
                int ii = fr.itemIdx;
                const std::string& name = g_nearbyItems[ii].name;
                COLORREF catCol = g_catColors[fr.catIdx];
                bool selected = g_selectedItems.count(name) > 0;
                if (selected) {
                    HBRUSH rb = CreateSolidBrush(RGB(30, 14, 22));
                    RECT rr2 = { x0 + CPAD_I, y, x0 + fullW - CPAD_I, y + FILTER_ROW_H };
                    FillRect(dc, &rr2, rb); DeleteObject(rb);
                }
                HBRUSH dotBr = CreateSolidBrush(catCol);
                SelectObject(dc, dotBr); SelectObject(dc, GetStockObject(NULL_PEN));
                Ellipse(dc, x0 + CPAD_I + 4, y + 8, x0 + CPAD_I + 10, y + 14); DeleteObject(dotBr);
                // Checkbox
                int cbx = x0 + CPAD_I + 14, cby = y + 5, cbs = 12;
                HPEN cbp = CreatePen(PS_SOLID, 1, selected ? RGB(245, 85, 85) : RGB(40, 38, 55));
                SelectObject(dc, cbp);
                if (selected) {
                    HBRUSH cbf = CreateSolidBrush(RGB(245, 85, 85)); SelectObject(dc, cbf);
                    Rectangle(dc, cbx, cby, cbx + cbs, cby + cbs); DeleteObject(cbf);
                    HPEN ckp = CreatePen(PS_SOLID, 2, RGB(240, 240, 240)); SelectObject(dc, ckp);
                    MoveToEx(dc, cbx + 2, cby + 6, NULL); LineTo(dc, cbx + 5, cby + 10); LineTo(dc, cbx + 10, cby + 2); DeleteObject(ckp);
                }
                else {
                    SelectObject(dc, GetStockObject(NULL_BRUSH));
                    Rectangle(dc, cbx, cby, cbx + cbs, cby + cbs);
                }
                DeleteObject(cbp);
                SelectObject(dc, g_mF); SetTextColor(dc, selected ? C_TEXT : C_TEXT_DIM);
                TextOutA(dc, x0 + CPAD_I + 32, y + 3, name.c_str(), (int)name.length());
                // Hit zone for row
                HitZone hz; hz.r = { x0 + CPAD_I, y, x0 + fullW - CPAD_I, y + FILTER_ROW_H };
                hz.type = 6; hz.toggle = nullptr; hz.idx = ri; hz.sval = nullptr;
                g_hz.push_back(hz);
            }
            HPEN rl = CreatePen(PS_SOLID, 1, RGB(22, 20, 34));
            SelectObject(dc, rl); MoveToEx(dc, x0 + CPAD_I, y + FILTER_ROW_H - 1, NULL);
            LineTo(dc, x0 + fullW - CPAD_I, y + FILTER_ROW_H - 1); DeleteObject(rl);
            y += FILTER_ROW_H;
        }
        if (totalRows > FILTER_MAX_VISIBLE) {
            SelectObject(dc, g_mFS); SetTextColor(dc, C_TEXT_VDIM);
            int endRow = g_filterScroll + FILTER_MAX_VISIBLE;
            if (endRow > totalRows) endRow = totalRows;
            char si2[48]; snprintf(si2, sizeof(si2), "scroll %d-%d of %d", g_filterScroll + 1, endRow, totalRows);
            TextOutA(dc, x0 + CPAD_I, y + 2, si2, (int)strlen(si2));
        }
        if (numItems == 0) {
            SelectObject(dc, g_mFS); SetTextColor(dc, C_TEXT_VDIM);
            TextOutA(dc, x0 + CPAD_I, y + 4, g_showItems.load() ? "No items in range" : "Enable Item ESP first", 21);
        }
        cy += ch + CGAP;
    }
    return cy - CONT_T + scrollY;
}

// ── Color wheel helpers (unchanged from original) ──
static void EnsureWheel(HDC refDC) {
    if (g_wheelBmp) return;
    int d = WHEEL_R * 2;
    BITMAPINFO bmi = {}; bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth = d; bmi.bmiHeader.biHeight = -d;
    bmi.bmiHeader.biPlanes = 1; bmi.bmiHeader.biBitCount = 32;
    unsigned char* bits = nullptr;
    g_wheelBmp = CreateDIBSection(refDC, &bmi, DIB_RGB_COLORS, (void**)&bits, NULL, 0);
    if (!bits) return;
    for (int py = 0; py < d; py++) for (int px = 0; px < d; px++) {
        unsigned char* q = bits + (py * d + px) * 4;
        float dx = (float)(px - WHEEL_R), dy = (float)(py - WHEEL_R);
        float dist = sqrtf(dx * dx + dy * dy);
        if (dist >= WHEEL_R - 1) { q[0] = 18; q[1] = 16; q[2] = 14; q[3] = 255; continue; }
        float hue = atan2f(dy, dx) * 57.2958f + 180.0f;
        float sat = dist / (float)(WHEEL_R - 1);
        float c2 = sat, x2 = c2 * (1.f - fabsf(fmodf(hue / 60.f, 2.f) - 1.f)), m2 = 1.f - c2;
        float rf, gf, bf;
        if (hue < 60) { rf = c2; gf = x2; bf = 0; }
        else if (hue < 120) { rf = x2; gf = c2; bf = 0; }
        else if (hue < 180) { rf = 0; gf = c2; bf = x2; }
        else if (hue < 240) { rf = 0; gf = x2; bf = c2; }
        else if (hue < 300) { rf = x2; gf = 0; bf = c2; }
        else { rf = c2; gf = 0; bf = x2; }
        q[2] = (unsigned char)((rf + m2) * 255); q[1] = (unsigned char)((gf + m2) * 255);
        q[0] = (unsigned char)((bf + m2) * 255); q[3] = 255;
    }
}
static COLORREF WheelPick(int cx, int cy, int mx, int my) {
    float dx = (float)(mx - cx), dy = (float)(my - cy);
    float dist = sqrtf(dx * dx + dy * dy);
    if (dist >= WHEEL_R) return RGB(255, 255, 255);
    float hue = atan2f(dy, dx) * 57.2958f + 180.0f;
    float sat = dist / (float)(WHEEL_R - 1);
    float c2 = sat, x2 = c2 * (1.f - fabsf(fmodf(hue / 60.f, 2.f) - 1.f)), m2 = 1.f - c2;
    float rf, gf, bf;
    if (hue < 60) { rf = c2; gf = x2; bf = 0; }
    else if (hue < 120) { rf = x2; gf = c2; bf = 0; }
    else if (hue < 180) { rf = 0; gf = c2; bf = x2; }
    else if (hue < 240) { rf = 0; gf = x2; bf = c2; }
    else if (hue < 300) { rf = x2; gf = 0; bf = c2; }
    else { rf = c2; gf = 0; bf = x2; }
    return RGB((int)((rf + m2) * 255), (int)((gf + m2) * 255), (int)((bf + m2) * 255));
}
static void RGBtoHS(COLORREF col, float& hue, float& sat) {
    float r = GetRValue(col) / 255.f, g = GetGValue(col) / 255.f, b = GetBValue(col) / 255.f;
    float mx = r > g ? (r > b ? r : b) : (g > b ? g : b);
    float mn = r < g ? (r < b ? r : b) : (g < b ? g : b);
    float c = mx - mn;
    if (c < 0.001f) { hue = 0; sat = 0; return; }
    if (mx == r) hue = 60.f * fmodf((g - b) / c + 6.f, 6.f);
    else if (mx == g) hue = 60.f * ((b - r) / c + 2.f);
    else hue = 60.f * ((r - g) / c + 4.f);
    sat = (mx > 0.001f) ? c / mx : 0;
}


// ═══════════════════════════════════════════════════════════════
//  WINDOW PROCEDURE
// ═══════════════════════════════════════════════════════════════

static LRESULT CALLBACK MenuWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
    case WM_CREATE: RegisterHotKey(hwnd, 100, 0, VK_F1); return 0;
    case WM_HOTKEY:
        if (wParam == 100) { g_menuVisible = !g_menuVisible; ShowWindow(hwnd, g_menuVisible ? SW_SHOW : SW_HIDE); }
        return 0;

    case WM_LBUTTONDOWN: {
        int mx = GET_X_LPARAM(lParam), my = GET_Y_LPARAM(lParam);
        // Header drag
        if (my < HDR_H) { g_menuDrag = true; g_dragStart = { mx, my }; SetCapture(hwnd); return 0; }
        // Sidebar clicks
        if (mx < SB_W && my >= HDR_H) {
            int sy = HDR_H + 16;
            for (int i = 0; i < SB_COUNT; i++) {
                int iy = sy + i * (SB_ICON_SZ + SB_ICON_GAP);
                if (my >= iy && my < iy + SB_ICON_SZ) {
                    if (g_sbItems[i].page != g_menuPage) {
                        g_menuPage = g_sbItems[i].page; g_scrollY = 0; g_colorPickerOpen = -1;
                        if (g_menuPage == 3) SetTimer(hwnd, 200, 1000, NULL);
                        else KillTimer(hwnd, 200);
                        InvalidateRect(hwnd, nullptr, FALSE);
                    }
                    return 0;
                }
            }
            return 0;
        }
        // Test hit zones (overlay gets priority when color picker is open)
        if (g_colorPickerOpen >= 0) {
            // Check overlay hit zones first (type 9 = wheel, type 4 = close/swatch)
            for (int i = (int)g_hz.size() - 1; i >= 0; i--) {
                auto& hz = g_hz[i];
                if (mx >= hz.r.left && mx < hz.r.right && my >= hz.r.top && my < hz.r.bottom) {
                    if (hz.type == 9) { // Color wheel click
                        int wcx = (int)hz.slo, wcy = (int)hz.shi;
                        float ddx = (float)(mx - wcx), ddy = (float)(my - wcy);
                        if (sqrtf(ddx * ddx + ddy * ddy) < (float)WHEEL_R) {

                            COLORREF* cp = GetColorPtr(hz.idx); if (cp)
                                *cp = WheelPick(wcx, wcy, mx, my);
                            InvalidateRect(hwnd, nullptr, FALSE); return 0;
                        }
                    }
                    if (hz.type == 4) { // Close X or swatch toggle
                        g_colorPickerOpen = (g_colorPickerOpen == hz.idx) ? -1 : hz.idx;
                        InvalidateRect(hwnd, nullptr, FALSE); return 0;
                    }
                }
            }
            // Click was in overlay area but not on wheel/button — close overlay
            if (mx >= CONT_L && my >= CONT_T && my < MH - FTR_H) {
                g_colorPickerOpen = -1;
                InvalidateRect(hwnd, nullptr, FALSE); return 0;
            }
        }
        for (auto& hz : g_hz) {
            if (mx >= hz.r.left && mx < hz.r.right && my >= hz.r.top && my < hz.r.bottom) {
                if (hz.type == 0 && hz.toggle) { // Toggle
                    bool nv = !hz.toggle->load();
                    hz.toggle->store(nv);
                    if ((g_espEnabled.load() || g_radarEnabled.load() || g_silentAim.load() || g_mouseAim.load() || g_railgunAim.load() || g_laserFire.load() || g_raidMode.load() || g_noRecoil.load() || g_mortarMode.load() || g_lootTP.load() || g_remoteLoot.load() || g_freecam.load() || g_noGrass.load()) && !g_overlayRunning.load())
                        StartOverlayThread(g_dayzid_menu);
                    InvalidateRect(hwnd, nullptr, FALSE); return 0;
                }
                if (hz.type == 1) { // Slider
                    g_sliderDragIdx = hz.idx;
                    if (g_sliderDragIdx >= 0 && g_sliderDragIdx < (int)g_liveSliders.size()) {
                        auto& ls = g_liveSliders[g_sliderDragIdx];
                        float t = (float)(mx - ls.trackL) / (float)(ls.trackR - ls.trackL);
                        t = (std::max)(0.f, (std::min)(1.f, t));
                        float raw = ls.lo + t * (ls.hi - ls.lo);
                        raw = roundf(raw / ls.step) * ls.step;
                        *ls.val = (std::max)(ls.lo, (std::min)(ls.hi, raw));
                    }
                    SetCapture(hwnd); InvalidateRect(hwnd, nullptr, FALSE); return 0;
                }
                if (hz.type == 2) { // Bone selector
                    if (hz.sval) *((int*)hz.sval) = hz.idx;
                    else g_aimBoneChoice = hz.idx; // fallback
                    InvalidateRect(hwnd, nullptr, FALSE); return 0;
                }
                if (hz.type == 4) { // Color swatch
                    g_colorPickerOpen = (g_colorPickerOpen == hz.idx) ? -1 : hz.idx;
                    InvalidateRect(hwnd, nullptr, FALSE); return 0;
                }
                if (hz.type == 5) { // Filter clear
                    std::lock_guard<std::mutex> lk(g_itemMtx);
                    g_selectedItems.clear();
                    InvalidateRect(hwnd, nullptr, FALSE); return 0;
                }
                if (hz.type == 6) { // Filter row click
                    std::lock_guard<std::mutex> lk(g_itemMtx);
                    std::vector<FilterRow> rows = BuildFilterRows();
                    int ri = hz.idx;
                    if (ri >= 0 && ri < (int)rows.size() && !rows[ri].isHeader) {
                        int ii = rows[ri].itemIdx;
                        if (ii >= 0 && ii < (int)g_nearbyItems.size()) {
                            const std::string& name = g_nearbyItems[ii].name;
                            if (g_selectedItems.count(name)) g_selectedItems.erase(name);
                            else g_selectedItems.insert(name);
                        }
                    }
                    InvalidateRect(hwnd, nullptr, FALSE); return 0;
                }
                if (hz.type == 7) { // Dropdown cycle
                    int* choice = (int*)hz.sval;
                    int n = (int)hz.shi;
                    *choice = (*choice + 1) % n;
                    InvalidateRect(hwnd, nullptr, FALSE); return 0;
                }
                if (hz.type == 8) { // Waypoint add
                    if ((int)g_waypoints.size() < MAX_WAYPOINTS) {
                        Waypoint wp;
                        snprintf(wp.name, sizeof(wp.name), "WP%d", (int)g_waypoints.size() + 1);
                        wp.x = g_lastCamPos.x; wp.y = g_lastCamPos.y; wp.z = g_lastCamPos.z;
                        g_waypoints.push_back(wp);
                    }
                    InvalidateRect(hwnd, nullptr, FALSE); return 0;
                }
                if (hz.type == 9) { // Color wheel click
                    int wcx = (int)hz.slo, wcy = (int)hz.shi;
                    float ddx = (float)(mx - wcx), ddy = (float)(my - wcy);
                    if (sqrtf(ddx * ddx + ddy * ddy) < (float)WHEEL_R) {

                        COLORREF* cp = GetColorPtr(hz.idx); if (cp)
                            *cp = WheelPick(wcx, wcy, mx, my);
                        InvalidateRect(hwnd, nullptr, FALSE); return 0;
                    }
                }
                if (hz.type == 10) { // Waypoint delete
                    if (hz.idx >= 0 && hz.idx < (int)g_waypoints.size()) {
                        g_waypoints.erase(g_waypoints.begin() + hz.idx);
                    }
                    InvalidateRect(hwnd, nullptr, FALSE); return 0;
                }
                if (hz.type == 11) { // Custom waypoint XYZ dialog
                    ShowWaypointDialog();
                    return 0;
                }
                if (hz.type == 12) { // Mortar set target dialog
                    ShowMortarDialog();
                    return 0;
                }
                if (hz.type == 13) { // Mortar clear target
                    g_mortarSet = false;
                    g_mortarTarget = { 0,0,0 };
                    printf("[MORTAR] Target cleared\n");
                    InvalidateRect(hwnd, nullptr, FALSE); return 0;
                }
            }
        }
        return 0;
    }
    case WM_LBUTTONUP:
        if (g_menuDrag) { g_menuDrag = false; ReleaseCapture(); }
        if (g_sliderDragIdx >= 0) { g_sliderDragIdx = -1; ReleaseCapture(); }
        return 0;

    case WM_MOUSEMOVE: {
        int mx = GET_X_LPARAM(lParam), my = GET_Y_LPARAM(lParam);
        if (g_menuDrag) {
            RECT wr; GetWindowRect(hwnd, &wr);
            SetWindowPos(hwnd, NULL, wr.left + mx - g_dragStart.x, wr.top + my - g_dragStart.y, 0, 0, SWP_NOSIZE | SWP_NOZORDER);
            return 0;
        }
        if (g_sliderDragIdx >= 0 && g_sliderDragIdx < (int)g_liveSliders.size()) {
            auto& ls = g_liveSliders[g_sliderDragIdx];
            float t = (float)(mx - ls.trackL) / (float)(ls.trackR - ls.trackL);
            t = (std::max)(0.f, (std::min)(1.f, t));
            float raw = ls.lo + t * (ls.hi - ls.lo);
            raw = roundf(raw / ls.step) * ls.step;
            *ls.val = (std::max)(ls.lo, (std::min)(ls.hi, raw));
            InvalidateRect(hwnd, nullptr, FALSE); return 0;
        }
        // Sidebar hover
        int oldHov = g_sidebarHover; g_sidebarHover = -1;
        if (mx < SB_W && my >= HDR_H) {
            int sy = HDR_H + 16;
            for (int i = 0; i < SB_COUNT; i++) {
                int iy = sy + i * (SB_ICON_SZ + SB_ICON_GAP);
                if (my >= iy && my < iy + SB_ICON_SZ) { g_sidebarHover = i; break; }
            }
        }
        if (g_sidebarHover != oldHov) InvalidateRect(hwnd, nullptr, FALSE);
        TRACKMOUSEEVENT tme = { sizeof(tme), TME_LEAVE, hwnd, 0 }; TrackMouseEvent(&tme);
        return 0;
    }
    case WM_MOUSELEAVE:
        if (g_sidebarHover >= 0) { g_sidebarHover = -1; InvalidateRect(hwnd, nullptr, FALSE); }
        return 0;

    case WM_MOUSEWHEEL: {
        int delta = GET_WHEEL_DELTA_WPARAM(wParam);
        if (g_menuPage == 3) {
            // Filter page: scroll items
            std::lock_guard<std::mutex> lk(g_itemMtx);
            std::vector<FilterRow> rows = BuildFilterRows();
            int totalRows = (int)rows.size();
            int maxScroll2 = totalRows > FILTER_MAX_VISIBLE ? totalRows - FILTER_MAX_VISIBLE : 0;
            if (delta > 0) g_filterScroll = g_filterScroll > 0 ? g_filterScroll - 1 : 0;
            else g_filterScroll = g_filterScroll < maxScroll2 ? g_filterScroll + 1 : maxScroll2;
        }
        else {
            // Page scroll
            int scrollStep = 30;
            if (delta > 0) g_scrollY -= scrollStep;
            else g_scrollY += scrollStep;
            int maxScroll = g_contentH - CONT_VH;
            if (maxScroll < 0) maxScroll = 0;
            if (g_scrollY < 0) g_scrollY = 0;
            if (g_scrollY > maxScroll) g_scrollY = maxScroll;
        }
        InvalidateRect(hwnd, nullptr, FALSE);
        return 0;
    }
    case WM_TIMER: {
        if (wParam == 200 && g_menuPage == 3)
            InvalidateRect(hwnd, nullptr, FALSE);
        return 0;
    }
    case WM_CHAR: {
        if (g_menuPage == 3) {
            char ch = (char)wParam;
            {
                std::lock_guard<std::mutex> lk(g_itemMtx);
                if (ch == '\b') { if (!g_espFilterText.empty()) g_espFilterText.pop_back(); }
                else if (ch >= 0x20 && ch <= 0x7E && g_espFilterText.length() < 32) g_espFilterText += ch;
            }
            InvalidateRect(hwnd, nullptr, FALSE);
        }
        return 0;
    }
    case WM_KEYDOWN: {
        if (g_menuPage == 3 && wParam == VK_ESCAPE) {
            {
                std::lock_guard<std::mutex> lk(g_itemMtx);
                if (!g_espFilterText.empty()) g_espFilterText.clear();
                else { g_selectedItems.clear(); g_filterScroll = 0; }
            }
            InvalidateRect(hwnd, nullptr, FALSE); return 0;
        }
        return 0;
    }

    case WM_PAINT: {
        PAINTSTRUCT ps; HDC hdc = BeginPaint(hwnd, &ps);
        RECT rc; GetClientRect(hwnd, &rc); int w = rc.right, h = rc.bottom;
        HDC mem = CreateCompatibleDC(hdc);
        HBITMAP mbitmap = CreateCompatibleBitmap(hdc, w, h);
        HBITMAP ob = (HBITMAP)SelectObject(mem, mbitmap);
        SetBkMode(mem, TRANSPARENT);

        // ── GDI+ setup for this frame ──
        Gdiplus::Graphics gfx(mem);
        gfx.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
        gfx.SetTextRenderingHint(Gdiplus::TextRenderingHintClearTypeGridFit);
        g_gfx = &gfx;

        // Clear hit zones and live sliders for this frame
        g_hz.clear();
        g_liveSliders.clear();

        // Full background
        HBRUSH bgBr = CreateSolidBrush(C_BG); FillRect(mem, &rc, bgBr); DeleteObject(bgBr);

        // ── Sidebar with vertical gradient ──
        GdipGradientV(0, HDR_H, SB_W, h - HDR_H, RGB(15, 15, 18), RGB(10, 10, 12));
        GdipLine(SB_W - 1, HDR_H, SB_W - 1, h, RGB(34, 38, 36), 1.0f);

        // ── Sidebar icons (GDI+ enhanced) ──
        {
            int sy = HDR_H + 16;
            for (int i = 0; i < SB_COUNT; i++) {
                int iy = sy + i * (SB_ICON_SZ + SB_ICON_GAP);
                bool active = (g_sbItems[i].page == g_menuPage);
                bool hov = (i == g_sidebarHover);
                int ix = (SB_W - SB_ICON_SZ) / 2;
                if (active) {
                    // Glowing active indicator
                    GdipGlow(ix, iy, SB_ICON_SZ, SB_ICON_SZ, 7, C_ACCENT, 6, 22);
                    COLORREF abg = RGB(GetRValue(C_ACCENT) / 12 + 14, GetGValue(C_ACCENT) / 12 + 14, GetBValue(C_ACCENT) / 12 + 14);
                    COLORREF abrd = RGB(GetRValue(C_ACCENT) / 5 + 22, GetGValue(C_ACCENT) / 5 + 22, GetBValue(C_ACCENT) / 5 + 22);
                    GdipRoundRect(ix, iy, SB_ICON_SZ, SB_ICON_SZ, 7, abg, abrd, 1);
                    // Accent pip on left edge
                    GdipRoundRect(0, iy + 8, 3, SB_ICON_SZ - 16, 2, C_ACCENT, C_ACCENT, 0);
                }
                else if (hov) {
                    GdipRoundRect(ix, iy, SB_ICON_SZ, SB_ICON_SZ, 7, RGB(20, 20, 24), RGB(32, 34, 36), 1);
                }
                COLORREF ic = active ? C_ACCENT : RGB(65, 68, 72);
                int icx = SB_W / 2, icy = iy + SB_ICON_SZ / 2;
                if (g_sbIcons[i]) g_sbIcons[i](mem, icx, icy, ic);
            }
            // Status LED with glow
            int lx = SB_W / 2, ly = h - FTR_H - 14;
            GdipRadialGlow(lx, ly, 12, C_GREEN, 30);
            GdipCircle(lx, ly, 4, C_GREEN);
        }

        // ── Header bar (gradient + glow logo) ──
        {
            GdipGradientV(0, 0, w, HDR_H, RGB(24, 24, 28), RGB(16, 16, 20));
            GdipLine(0, HDR_H - 1, w, HDR_H - 1, RGB(34, 38, 36), 1.0f);
            // Logo "PL" with glow
            int lx = (SB_W - 28) / 2, ly = (HDR_H - 28) / 2;
            GdipRadialGlow(lx + 14, ly + 14, 22, C_ACCENT, 25);
            COLORREF logoBg = RGB(GetRValue(C_ACCENT) / 10 + 14, GetGValue(C_ACCENT) / 10 + 14, GetBValue(C_ACCENT) / 10 + 14);
            COLORREF logoBrd = RGB(GetRValue(C_ACCENT) / 4 + 22, GetGValue(C_ACCENT) / 4 + 22, GetBValue(C_ACCENT) / 4 + 22);
            GdipRoundRect(lx, ly, 28, 28, 7, logoBg, logoBrd, 1);
            SelectObject(mem, g_mFS); SetTextColor(mem, C_ACCENT);
            TextOutA(mem, lx + 5, ly + 7, "PL", 2);
            // Page title
            const char* pageTitle = g_sbItems[g_menuPage < SB_COUNT ? g_menuPage : 0].label;
            SelectObject(mem, g_mFT); SetTextColor(mem, C_TEXT);
            TextOutA(mem, SB_W + 14, 13, pageTitle, (int)strlen(pageTitle));
            // "PACKETLOSS" dim
            SelectObject(mem, g_mFS); SetTextColor(mem, C_TEXT_VDIM);
            SIZE ts; GetTextExtentPoint32A(mem, pageTitle, (int)strlen(pageTitle), &ts);
            TextOutA(mem, SB_W + 14 + ts.cx + 10, 16, "PACKETLOSS", 10);
            // Version
            SetTextColor(mem, C_TEXT_VDIM);
            TextOutA(mem, w - 140, 16, "v1.28.8", 7);
            // Connected badge (GDI+ enhanced)
            const char* conn = "CONNECTED";
            SIZE cs; GetTextExtentPoint32A(mem, conn, 9, &cs);
            int bx = w - cs.cx - 22;
            COLORREF cbg2 = RGB(GetRValue(C_GREEN) / 14 + 14, GetGValue(C_GREEN) / 14 + 14, GetBValue(C_GREEN) / 14 + 14);
            COLORREF cbrd = RGB(GetRValue(C_GREEN) / 6 + 22, GetGValue(C_GREEN) / 6 + 22, GetBValue(C_GREEN) / 6 + 22);
            GdipRoundRect(bx, 12, cs.cx + 14, cs.cy + 4, 4, cbg2, cbrd, 1);
            GdipRadialGlow(bx + (cs.cx + 14) / 2, 12 + (cs.cy + 4) / 2, 20, C_GREEN, 10);
            SetTextColor(mem, C_GREEN);
            TextOutA(mem, bx + 7, 14, conn, 9);
        }

        // ── Content area (with clip) ──
        {
            HRGN clipRgn = CreateRectRgn(CONT_L, CONT_T, w, h - FTR_H);
            SelectClipRgn(mem, clipRgn);

            int totalH = 0;
            if (g_menuPage == 0) totalH = DrawVisualsPage(mem, g_scrollY);
            else if (g_menuPage == 1) totalH = DrawAimbotPage(mem, g_scrollY);
            else if (g_menuPage == 2) totalH = DrawRadarPage(mem, g_scrollY);
            else if (g_menuPage == 3) totalH = DrawFilterPage(mem, g_scrollY);
            else if (g_menuPage == 4) totalH = DrawExploitsPage(mem, g_scrollY);
            else if (g_menuPage == 5) totalH = DrawConfigPage(mem, g_scrollY);
            // Note: Config is accessed via page 4 in old code, but we have 5 pages now
            // Let me map: 0=Visuals, 1=Aimbot, 2=Radar, 3=Filter, 4=Exploits
            // Config needs to be page 5? Or replace Exploits... Actually let me handle this.
            // For now, let's keep 5 sidebar items as defined.

            g_contentH = totalH;

            // Scrollbar indicator (thin bar on right edge)
            if (totalH > CONT_VH) {
                int maxScrl = totalH - CONT_VH;
                int sbHeight = CONT_VH * CONT_VH / totalH;
                if (sbHeight < 20) sbHeight = 20;
                int sbY = CONT_T + (g_scrollY * (CONT_VH - sbHeight)) / maxScrl;
                HBRUSH sbBr2 = CreateSolidBrush(RGB(40, 38, 55));
                RECT sbRect = { w - 4, sbY, w - 1, sbY + sbHeight };
                FillRect(mem, &sbRect, sbBr2); DeleteObject(sbBr2);
            }

            SelectClipRgn(mem, NULL);
            DeleteObject(clipRgn);
        }

        // ── Floating Color Wheel Overlay (renders on ANY page) ──
        if (g_colorPickerOpen >= 0 && g_colorPickerOpen < 16) {
            EnsureWheel(mem);
            if (g_wheelBmp) {

                const char* colorLabels[] = { "Player Box", "Zombie Box", "Snap Lines", "Bones", "Head Dot",
                    "Weapons","Ammo/Mags","Medical","Food/Drink","Clothing","Backpacks","Attachments","Tools","Other","FOV Circle","Tracers" };
                // Overlay panel position: centered in content area
                int panW = WHEEL_R * 2 + 60, panH = WHEEL_R * 2 + 70;
                int panX = CONT_L + (CONT_W - panW) / 2;
                int panY = CONT_T + (CONT_VH - panH) / 2;
                // Dark overlay backdrop
                HBRUSH dimBr = CreateSolidBrush(RGB(5, 4, 10));
                RECT dimR = { CONT_L, CONT_T, w, h - FTR_H };
                FillRect(mem, &dimR, dimBr); DeleteObject(dimBr);
                // Panel card
                HBRUSH panBg = CreateSolidBrush(C_CARD);
                HPEN panPn = CreatePen(PS_SOLID, 1, C_ACCENT);
                SelectObject(mem, panBg); SelectObject(mem, panPn);
                RoundRect(mem, panX, panY, panX + panW, panY + panH, CARD_R, CARD_R);
                DeleteObject(panBg); DeleteObject(panPn);
                // Header
                SelectObject(mem, g_mFB); SetTextColor(mem, C_TEXT);
                char pickHdr[48]; snprintf(pickHdr, sizeof(pickHdr), "Pick: %s", colorLabels[g_colorPickerOpen]);
                TextOutA(mem, panX + 16, panY + 10, pickHdr, (int)strlen(pickHdr));
                // Close X button
                SelectObject(mem, g_mF); SetTextColor(mem, RGB(245, 85, 85));
                TextOutA(mem, panX + panW - 24, panY + 10, "X", 1);
                HitZone hzClose; hzClose.r = { panX + panW - 30, panY + 4, panX + panW - 4, panY + 28 };
                hzClose.type = 4; hzClose.toggle = nullptr; hzClose.idx = g_colorPickerOpen; hzClose.sval = nullptr;
                g_hz.push_back(hzClose); // clicking X toggles picker closed (same idx = close)
                // Wheel
                int wcx = panX + panW / 2, wcy = panY + 38 + WHEEL_R;
                HDC wdc = CreateCompatibleDC(mem);
                SelectObject(wdc, g_wheelBmp);
                BitBlt(mem, wcx - WHEEL_R, wcy - WHEEL_R, WHEEL_R * 2, WHEEL_R * 2, wdc, 0, 0, SRCCOPY);
                DeleteDC(wdc);
                // Wheel border
                HPEN rp = CreatePen(PS_SOLID, 1, RGB(50, 52, 54));
                SelectObject(mem, rp); SelectObject(mem, GetStockObject(NULL_BRUSH));
                Ellipse(mem, wcx - WHEEL_R, wcy - WHEEL_R, wcx + WHEEL_R, wcy + WHEEL_R);
                DeleteObject(rp);
                // Current color indicator
                COLORREF* curColor = GetColorPtr(g_colorPickerOpen);
                if (curColor) {
                    float ch2, cs2;
                    RGBtoHS(*curColor, ch2, cs2);
                    float ang = (ch2 - 180.f) * 0.0174533f;
                    float rad = cs2 * (WHEEL_R - 2.f);
                    int ix2 = wcx + (int)(cosf(ang) * rad), iy2 = wcy + (int)(sinf(ang) * rad);
                    HPEN ip = CreatePen(PS_SOLID, 2, RGB(255, 255, 255));
                    SelectObject(mem, ip); Ellipse(mem, ix2 - 5, iy2 - 5, ix2 + 5, iy2 + 5); DeleteObject(ip);
                    HPEN ip2 = CreatePen(PS_SOLID, 1, RGB(0, 0, 0));
                    SelectObject(mem, ip2); Ellipse(mem, ix2 - 4, iy2 - 4, ix2 + 4, iy2 + 4); DeleteObject(ip2);
                    // Current swatch preview at bottom
                    int swX = panX + 16, swY2 = panY + panH - 22;
                    HBRUSH swBr = CreateSolidBrush(*curColor);
                    RECT swR = { swX, swY2, swX + panW - 32, swY2 + 14 }; FillRect(mem, &swR, swBr); DeleteObject(swBr);
                    HPEN swP = CreatePen(PS_SOLID, 1, RGB(50, 52, 54));
                    SelectObject(mem, swP); SelectObject(mem, GetStockObject(NULL_BRUSH));
                    RoundRect(mem, swX, swY2, swX + panW - 32, swY2 + 14, 3, 3); DeleteObject(swP);
                }
                // Hit zone for wheel click
                HitZone hzWheel; hzWheel.r = { wcx - WHEEL_R, wcy - WHEEL_R, wcx + WHEEL_R, wcy + WHEEL_R };
                hzWheel.type = 9; hzWheel.toggle = nullptr; hzWheel.idx = g_colorPickerOpen;
                hzWheel.sval = nullptr; hzWheel.slo = (float)wcx; hzWheel.shi = (float)wcy; hzWheel.sstep = 0;
                g_hz.push_back(hzWheel);
            }
        }

        // ── Footer (gradient) ──
        {
            int fy = h - FTR_H;
            GdipGradientV(0, fy, w, FTR_H, RGB(14, 14, 17), RGB(10, 10, 12));
            GdipLine(0, fy, w, fy, RGB(26, 24, 40), 1.0f);
            SelectObject(mem, g_mFS); SetTextColor(mem, C_TEXT_VDIM);
            TextOutA(mem, SB_W + 10, fy + 8, "INS toggle  |  DayZ 1.28", 24);
            char sb[80]; snprintf(sb, sizeof(sb), "%d hits", g_totalHits);
            SIZE ss; GetTextExtentPoint32A(mem, sb, (int)strlen(sb), &ss);
            TextOutA(mem, w - ss.cx - 10, fy + 8, sb, (int)strlen(sb));
        }

        // ── Cleanup GDI+ for this frame ──
        g_gfx = nullptr;

        BitBlt(hdc, 0, 0, w, h, mem, 0, 0, SRCCOPY);
        SelectObject(mem, ob); DeleteObject(mbitmap); DeleteDC(mem);
        EndPaint(hwnd, &ps); return 0;
    }
    case WM_DESTROY:
        UnregisterHotKey(hwnd, 100);
        if (g_mF) { DeleteObject(g_mF); g_mF = nullptr; }
        if (g_mFB) { DeleteObject(g_mFB); g_mFB = nullptr; }
        if (g_mFS) { DeleteObject(g_mFS); g_mFS = nullptr; }
        if (g_mFT) { DeleteObject(g_mFT); g_mFT = nullptr; }
        return 0;
    }
    return DefWindowProc(hwnd, msg, wParam, lParam);
}

void ShowMenu(int dayzid) {
    g_dayzid_menu = dayzid;
    g_menuPage = 0;
    g_scrollY = 0;

    static ATOM mc2 = 0; HINSTANCE hi = GetModuleHandle(nullptr);
    if (!mc2) {
        WNDCLASSEXA wc = {}; wc.cbSize = sizeof(wc); wc.lpfnWndProc = MenuWndProc; wc.hInstance = hi;
        wc.hCursor = LoadCursor(nullptr, IDC_ARROW); wc.lpszClassName = "CompassionV5"; mc2 = RegisterClassExA(&wc); if (!mc2) return;
    }
    g_mF = CreateFontA(15, 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET, OUT_TT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_NATURAL_QUALITY, DEFAULT_PITCH | FF_DONTCARE, "Segoe UI");
    g_mFB = CreateFontA(15, 0, 0, 0, FW_SEMIBOLD, 0, 0, 0, DEFAULT_CHARSET, OUT_TT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_NATURAL_QUALITY, DEFAULT_PITCH | FF_DONTCARE, "Segoe UI Semibold");
    g_mFS = CreateFontA(12, 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET, OUT_TT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_NATURAL_QUALITY, DEFAULT_PITCH | FF_DONTCARE, "Segoe UI");
    g_mFT = CreateFontA(17, 0, 0, 0, FW_BOLD, 0, 0, 0, DEFAULT_CHARSET, OUT_TT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_NATURAL_QUALITY, DEFAULT_PITCH | FF_DONTCARE, "Segoe UI");

    g_menuHwnd = CreateWindowExA(WS_EX_TOPMOST | WS_EX_LAYERED, "CompassionV5", "", WS_POPUP,
        40, 40, MW, MH, nullptr, nullptr, hi, nullptr);
    if (!g_menuHwnd) return;
    // Rounded window corners
    HRGN rgn = CreateRoundRectRgn(0, 0, MW + 1, MH + 1, 16, 16);
    SetWindowRgn(g_menuHwnd, rgn, TRUE);
    SetLayeredWindowAttributes(g_menuHwnd, 0, 242, LWA_ALPHA);
    ShowWindow(g_menuHwnd, SW_SHOW); UpdateWindow(g_menuHwnd);

    MSG msg{};
    while (true) {
        while (PeekMessageA(&msg, nullptr, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_QUIT) goto ex;
            TranslateMessage(&msg); DispatchMessageA(&msg);
        }
        CheckPendingWaypoint();
        CheckPendingMortar();
        if (g_menuHwnd && g_menuVisible) InvalidateRect(g_menuHwnd, nullptr, FALSE);
        Sleep(33);
    }
ex: StopAll();
    if (g_menuHwnd) { DestroyWindow(g_menuHwnd); g_menuHwnd = nullptr; }
}

// ═══════════════════════════════════════════════════════════════
//  MAIN
// ═══════════════════════════════════════════════════════════════
int main()
{
    SetUnhandledExceptionFilter(CrashHandler);
    Gdiplus::GdiplusStartupInput gdipInput;
    Gdiplus::GdiplusStartup(&g_gdipToken, &gdipInput, nullptr);
    printf("[!] Starting... (crash handler installed)\n");
    g_isAdmin = CheckAdminRights();
    printf("[!] Admin rights: %s\n", g_isAdmin ? "YES" : "NO");
    system("color 5");

    if (OpenSharedMemory())printf("[!] SharedMemory OK\n");
    else { printf("SharedMemory FAIL\n"); clean(); ExitSystemThread(); return 1; }
    if (OpenNamedEvents())printf("[!] NamedEvents OK\n");
    else { printf("NamedEvents FAIL\n"); clean(); ExitSystemThread(); return 1; }
    OpenWriteChannel(); // non-fatal: falls back to single-channel writes if driver doesn't support it
    if (base = GetBaseAddr(process_name))printf("[!] Base: %p\n", (void*)base);
    else { printf("Base FAIL\n"); clean(); ExitSystemThread(); return 1; }

    int dayzid = 0;
    printf("Enter DayZ Process ID: ");
    std::cin >> dayzid; processId1 = dayzid;

    printf("\n");
    printf("  +==========================================+\n");
    printf("  |             PACKETLOSS v1.28.8           |\n");
    printf("  +==========================================+\n");
    printf("  |  [1]  ESP only                           |\n");
    printf("  |  [2]  Radar only                         |\n");
    printf("  |  [3]  Full Menu (recommended)            |\n");
    printf("  |  [4]  ESP + Radar (no menu)              |\n");
    printf("  +==========================================+\n");
    printf("\n  Choice: ");

    int choice; std::cin >> choice;
    switch (choice) {
    case 1:g_espEnabled = true; StartOverlayThread(dayzid);
        printf("[!] ESP running. END to stop.\n");
        while (!g_shutdownAll.load())Sleep(100); StopAll(); break;
    case 2:g_radarEnabled = true; StartOverlayThread(dayzid);
        printf("[!] Radar running. END to stop.\n");
        while (!g_shutdownAll.load())Sleep(100); StopAll(); break;
    case 3:ShowMenu(dayzid); break;
    case 4:g_espEnabled = true; g_radarEnabled = true; StartOverlayThread(dayzid);
        printf("[!] Both running. END to stop.\n");
        while (!g_shutdownAll.load()) { PumpUIOnce(); Sleep(33); }StopAll(); break;
    default:printf("Invalid.\n"); return 1;
    }
    printf("[!] Exiting.\n");
    if (g_gdipToken) Gdiplus::GdiplusShutdown(g_gdipToken);
    return 0;
}