#include "readradar.cpp"
/*
*  COMPASSION SUITE v1.28.3 — Single-Thread Overlay
*  Menu pages: ESP | Aimbot | Radar | Filter
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

#include <dwmapi.h>
#pragma comment(lib, "Dwmapi.lib")

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
    constexpr uintptr_t Inventory = 0x658;
    constexpr uintptr_t InvHands = 0x1B0;
    constexpr uintptr_t AmmoType1 = 0x6B0;
    constexpr uintptr_t AmmoType2 = 0x20;
    constexpr uintptr_t InitSpeed = 0x364;
    constexpr uintptr_t AirFriction = 0x3B4;

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
std::atomic<bool> g_showItems{ false };
std::atomic<bool> g_fovOnly{ false };
std::atomic<bool> g_espPlayers{ true };
std::atomic<bool> g_espZombies{ true };
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
std::atomic<bool> g_aimPrediction{ true };  // velocity leading
std::atomic<bool> g_noRecoil{ false };      // anti-recoil mouse pull
static int g_fovRadius = 300;

// ── Slider-driven values (Aimbot page) ──
static float g_aimFovF = 120.f;    // aim FOV radius pixels (30-300)
static float g_silentRange = 500.f;     // silent aim max range (5-1000m)
static float g_mouseRange = 500.f;    // mouse aim max range (5-500m)
static float g_mouseSmooth = 3.0f;     // mouse smoothing (1-10)
static float g_bulletSpeed = 25000.f;  // init speed (500-50000)
static float g_leadFactor = 0.6f;     // prediction lead in meters (~2ft default) (0-2.0)
static float g_recoilPull = 1.5f;    // anti-recoil pull-down strength (0.5-5.0)

// Aim bone selector (mouse aimbot)
static int g_aimBoneChoice = 2; // 0=HEAD,1=NECK,2=SPINE,3=PELVIS
static const char* g_boneChoiceNames[] = { "Head", "Neck", "Spine", "Pelvis" };
static constexpr int NUM_BONE_CHOICES = 4;
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

// Call once per overlay frame — processes up to maxPerFrame queued writes via MmCopyVirtualMemory
static void drainWrites(int maxPerFrame = 2) {
    int done = 0;
    while (g_writeTail != g_writeHead && done < maxPerFrame) {
        WriteReq& req = g_pendingWrites[g_writeTail];
        if (req.size == 4) {
            float v; memcpy(&v, req.data, 4);
            write<float>(req.addr, v, req.pid);
        }
        else if (req.size == 12) {
            vector3 v; memcpy(&v, req.data, 12);
            write<vector3>(req.addr, v, req.pid);
        }
        g_writeTail = (g_writeTail + 1) % 32;
        done++;
    }
}
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
static int g_taggedBone = 2;                // bone to aim at on tagged target (2=SPINE2)
static int g_tagTick = 0;                   // tick when tagged

// ── Velocity tracking for prediction (EMA smoothed) ──
struct VelocityEntry { vector3 lastPos; vector3 velocity; vector3 smoothVel; int lastTick; bool valid; };
static std::unordered_map<uintptr_t, VelocityEntry> g_velocityMap;

// Weapon name cache — read once on first sight, refresh every ~30s
struct WeaponCacheEntry { std::string name; int lastTick; };
static std::unordered_map<uintptr_t, WeaponCacheEntry> g_weaponCache;
static constexpr int WEAPON_REFRESH_TICKS = 1800; // ~30s at 60fps

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
static int g_colorPickerOpen = -1; // -1 = closed, 0-4 = which color
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
struct HitEntry { int tick; float dist; std::string bone; };
static std::vector<HitEntry> g_hitLog;
static const int MAX_HIT_LOG = 8;
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

struct CharBlock64 { char d[64]; };

static std::string ReadRemoteChars(uintptr_t addr, size_t maxLen, int d) {
    if (!addr || addr < 0x10000 || maxLen == 0) return "";
    if (maxLen > 64) maxLen = 64;
    CharBlock64 blk = read<CharBlock64>(addr, d);
    std::string r;
    for (size_t i = 0; i < maxLen; i++) {
        char c = blk.d[i];
        if (c == '\0') break;
        if (c < 0x20 || c > 0x7E) break;
        r += c;
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

// ═══════════════════════════════════════════════════════════════
//  BONES (identical to working version)
// ═══════════════════════════════════════════════════════════════
struct Matrix3x4 { float m[12]; };
struct BonePos { vector3 pos; bool valid; };
enum PlayerBone {
    PB_HEAD = 0, PB_NECK, PB_SPINE2, PB_SPINE1, PB_PELVIS,
    PB_L_SHOULDER, PB_R_SHOULDER, PB_L_ELBOW, PB_R_ELBOW,
    PB_L_HAND, PB_R_HAND, PB_L_HIP, PB_R_HIP,
    PB_L_KNEE, PB_R_KNEE, PB_L_FOOT, PB_R_FOOT, PB_MAX
};
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


struct BatchCtx {
    static constexpr int MAX_ENTRIES = 200;
    static constexpr int MAX_RESULT_BYTES = 3840;

    BatchEntry entries[MAX_ENTRIES];
    uint16_t offsets[MAX_ENTRIES];  // byte offset of each result
    int count = 0;
    int totalBytes = 0;
    uint8_t results[MAX_RESULT_BYTES];
    bool ok = false;

    void reset() { count = 0; totalBytes = 0; ok = false; }

    // Queue a read — returns slot index (use with get<T>), or -1 if full
    int add(uintptr_t addr, int size) {
        if (count >= MAX_ENTRIES || totalBytes + size > MAX_RESULT_BYTES) return -1;
        offsets[count] = (uint16_t)totalBytes;
        entries[count] = { addr, (uint32_t)size };
        totalBytes += size;
        return count++;
    }

    // Send all queued reads as ONE driver call, receive all results
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

    // Extract result at slot index
    template<typename T>
    T get(int idx) {
        T val{};
        if (idx >= 0 && idx < count && ok)
            memcpy(&val, results + offsets[idx], sizeof(T));
        return val;
    }
};


// ═══════════════════════════════════════════════════════════════
//  GAME FRAME (identical to working version)
// ═══════════════════════════════════════════════════════════════
struct EntityData {
    uintptr_t ptr, visualState;
    vector3 pos, headPos;
    float dist;
    bool isPlayer, isZombie, isItem;
    std::string name;
    std::string weaponName;
    BonePos bones[PB_MAX];
    bool hasBones;
};
struct FrameData {
    bool valid; uintptr_t world;
    vector3 camPos, camRight, camUp, camForward;
    float projD1, projD2;
    vector3 localPos;
    std::vector<EntityData> entities;
};

static FrameData ReadGameFrame(int dayzid, int screenW, int screenH, int tick) {
    FrameData f = {}; f.valid = false;
    static BatchCtx B;

    // ────────────────────────────────────────────
    //  PHASE 0: World pointer (single read)
    // ────────────────────────────────────────────
    f.world = read<uintptr_t>(base + Off::World, dayzid);
    if (!f.world) return f;

    // ────────────────────────────────────────────
    //  PHASE 1: Camera ptr + entity list metadata
    //  (5 reads from world → 1 batch call)
    // ────────────────────────────────────────────
    B.reset();
    int _cam = B.add(f.world + Off::Camera, 8);
    int _nBase = B.add(f.world + Off::NearBase, 8);
    int _nCnt = B.add(f.world + Off::NearCount, 4);
    int _fBase = B.add(f.world + Off::FarBase, 8);
    int _fCnt = B.add(f.world + Off::FarCount, 4);
    if (!B.execute(dayzid)) return f;

    uintptr_t cam = B.get<uintptr_t>(_cam);
    if (!cam) return f;
    uintptr_t nBase = B.get<uintptr_t>(_nBase);
    int nCount = (std::max)(0, (std::min)(B.get<int>(_nCnt), 512));
    uintptr_t fBase = B.get<uintptr_t>(_fBase);
    int fCount = (std::max)(0, (std::min)(B.get<int>(_fCnt), 512));

    // ────────────────────────────────────────────
    //  PHASE 2: Camera vectors + projection
    //  (5 reads from cam → 1 batch call)
    // ────────────────────────────────────────────
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

    bool wantBones = g_showBones.load();
    bool wantItems = g_showItems.load();

    // ────────────────────────────────────────────
    //  PHASE 3: Entity pointers (batch, chunked)
    //  Old: N individual reads. New: ceil(N/200) batches
    // ────────────────────────────────────────────
    int totalList = nCount + fCount;
    std::vector<uintptr_t> allEnts;
    allEnts.reserve(totalList);

    for (int off = 0; off < totalList; off += BatchCtx::MAX_ENTRIES) {
        int chunk = (std::min)(totalList - off, BatchCtx::MAX_ENTRIES);
        B.reset();
        int sl[200]; // stack alloc, MAX_ENTRIES
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

    // ────────────────────────────────────────────
    //  PHASE 4: VisualState + skeleton pointers
    //  3 reads × 8B = 24B per entity → 66 per batch
    // ────────────────────────────────────────────
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

    // ────────────────────────────────────────────
    //  PHASE 5: Positions — filter by distance
    //  1 × vec3(12B) per entity → 200 per batch
    // ────────────────────────────────────────────
    // We'll build f.entities from valid position data
    // Track which metas passed distance filter for bone/weapon phases
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

    // ────────────────────────────────────────────
    //  Build EntityData for all passed entities
    // ────────────────────────────────────────────
    f.entities.reserve(passed.size());
    // Map from passed index → f.entities index (for bone fill-in later)
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

    // ────────────────────────────────────────────
    //  PHASE 6-8: Batched bone reading
    //  Step A: animClass ptrs from skeleton ptrs
    //  Step B: matrixClass ptrs from animClass ptrs
    //  Step C: entity matrices + 17 bone locals per entity
    // ────────────────────────────────────────────
    if (wantBones) {
        // Collect bone-eligible entities
        struct BoneCandidate {
            size_t passedIdx;   // index into passed[]
            uintptr_t skelPtr;  // zombie or player skeleton
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
            // ── Step A: Read animClass ptrs (1 per entity, 200 per batch) ──
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

            // ── Step B: Read matrixClass ptrs (1 per entity with valid animClass) ──
            std::vector<uintptr_t> matrixClasses(boneCands.size(), 0);
            {
                B.reset();
                int mcsl[200]; int mcCount = 0;
                size_t mcMap[200]; // maps batch slot → boneCands index
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

            // ── One-time bone debug print (uses first valid entity) ──
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

            // ── Step C: Entity matrices + bone locals ──
            // Per entity: 1 Matrix3x4(48B) + 17 vec3(12B) = 18 entries, 252B
            // → 11 entities per batch
            const int BONE_CHUNK = 11;

            for (size_t off = 0; off < boneCands.size(); off += BONE_CHUNK) {
                size_t end = (std::min)(off + (size_t)BONE_CHUNK, boneCands.size());
                B.reset();

                struct BoneSlots {
                    int matSlot;
                    int localSlots[PB_MAX];
                    bool valid;
                };
                BoneSlots bsl[11]; // BONE_CHUNK

                for (size_t i = off; i < end; i++) {
                    int li = (int)(i - off);
                    uintptr_t mc = matrixClasses[i];
                    bsl[li].valid = (mc && mc > 0x10000000ULL);
                    if (!bsl[li].valid) continue;

                    auto& m = metas[passed[boneCands[i].passedIdx].metaIdx];
                    bsl[li].matSlot = B.add(m.vs + 0x8, 48); // entity world matrix

                    const int* boneIdx = boneCands[i].isZombie ? g_zombieBoneIdx : g_humanBoneIdx;
                    for (int b = 0; b < PB_MAX; b++)
                        bsl[li].localSlots[b] = B.add(mc + 0x54 + boneIdx[b] * sizeof(Matrix3x4), 12);
                }
                B.execute(dayzid);

                // Unpack bone data into EntityData
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

    // ────────────────────────────────────────────
    //  PHASE 9: Weapon in hand (per-player, cached)
    //  Uses individual reads (cached for 30s, ~3 players/frame max)
    //  String reads now use 64B blocks via ReadRemoteChars
    // ────────────────────────────────────────────
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

    // ────────────────────────────────────────────
    //  PHASE 10-12: Item table (batched)
    //  Step A: Batch read flags + entity ptrs (100 per batch)
    //  Step B: Batch read VS ptrs for valid items
    //  Step C: Batch read positions, filter by distance
    //  Step D: Read names (individual, but block strings)
    // ────────────────────────────────────────────
    if (wantItems) {
        // First read item table ptr and count (can piggyback on phase 1 but fine as batch)
        B.reset();
        int _iTable = B.add(f.world + Off::ItemTable, 8);
        int _iCount = B.add(f.world + Off::ItemTableSize, 4);
        B.execute(dayzid);
        uintptr_t itemTable = B.get<uintptr_t>(_iTable);
        int itemCount = (std::max)(0, (std::min)(B.get<int>(_iCount), 2048));

        std::set<std::string> uniqueNames;
        std::vector<CatItem> catItems;

        if (itemTable && itemTable > 0x10000000ULL && itemCount > 0) {

            // ── Step A: Batch read flags + entity ptrs ──
            // Layout: stride 0x18, flag at +0 (4B), entity ptr at +0x8 (8B)
            // 2 entries per item → 100 items per batch
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

            // ── Step B: Batch read VS ptrs ──
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

            // ── Step C: Batch read positions ──
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

            // ── Step D: Read names + build entities (individual reads, block strings) ──
            for (auto& item : nearItems) {
                std::string itemName = ReadItemTypeName(item.ent, dayzid);
                if (itemName.empty()) itemName = "Item";
                if (uniqueNames.insert(itemName).second)
                    catItems.push_back({ itemName, CategorizeItem(itemName) });
                if (!g_espFilterText.empty() && !strContainsCI(itemName, g_espFilterText)) continue;
                {
                    std::lock_guard<std::mutex> lk(g_itemMtx);
                    if (!g_selectedItems.empty() && g_selectedItems.find(itemName) == g_selectedItems.end()) continue;
                }
                EntityData ed = {};
                ed.ptr = item.ent; ed.visualState = item.vs; ed.pos = item.pos;
                ed.headPos = { item.pos.x, item.pos.y + 0.3f, item.pos.z };
                ed.dist = item.dist; ed.isPlayer = false; ed.isZombie = false;
                ed.isItem = true; ed.hasBones = false; ed.name = itemName;
                f.entities.push_back(std::move(ed));
            }
        }

        // Sort catalog by category
        std::sort(catItems.begin(), catItems.end(), [](const CatItem& a, const CatItem& b) {
            if (a.cat != b.cat) return a.cat < b.cat;
            return a.name < b.name;
            });
        {
            std::lock_guard<std::mutex> lk(g_itemMtx);
            g_nearbyItems = std::move(catItems);
        }
    }

    f.valid = true;
    return f;
}
// ═══════════════════════════════════════════════════════════════
//  WORLD-TO-SCREEN + FOV
// ═══════════════════════════════════════════════════════════════
static bool W2S(vector3 pos, int& sx, int& sy, const FrameData& f, int sw, int sh) {
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
    return true;
}
static bool InFovCircle(int sx, int sy, int sw, int sh) {
    float dx = (float)(sx - sw / 2), dy = (float)(sy - sh / 2);
    return(dx * dx + dy * dy) <= (float)(g_fovRadius * g_fovRadius);
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
        ShowWindow(espH, espOn ? SW_SHOWNOACTIVATE : SW_HIDE);
        ShowWindow(radH, radOn ? SW_SHOWNOACTIVATE : SW_HIDE);
        ShowWindow(barH, radOn ? SW_SHOWNOACTIVATE : SW_HIDE);
        if (!espOn && !radOn && !aimOn && !g_mouseAim.load() && !g_noRecoil.load()) { Sleep(50); tick++; continue; }

        FrameData frame = ReadGameFrame(dayzid, SW, SH, tick);
        if (!frame.valid) { Sleep(16); tick++; continue; }

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

        // ── Update velocity tracking for all entities (EMA smoothed) ──
        const float velAlpha = 0.3f; // smoothing factor: lower=smoother, higher=more responsive
        for (auto& ent : frame.entities) {
            if (!ent.isPlayer && !ent.isZombie)continue;
            auto it = g_velocityMap.find(ent.ptr);
            if (it != g_velocityMap.end()) {
                int dt = tick - it->second.lastTick;
                if (dt > 0 && dt < 60) {
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
        // Clean stale entries every ~5 seconds
        if (tick % 300 == 0) {
            for (auto it = g_velocityMap.begin(); it != g_velocityMap.end();) {
                if (tick - it->second.lastTick > 300)it = g_velocityMap.erase(it);
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
        static uintptr_t cachedLocalPlayer = 0;
        static int localPlayerTick = 0;
        int aimFovI = (int)g_aimFovF;

        if (g_silentAim.load() || g_mouseAim.load()) {
            // Find local player
            if (!cachedLocalPlayer || (tick - localPlayerTick > 120)) {
                float bestCamDist = 5.0f;
                for (auto& ent : frame.entities) {
                    if (!ent.isPlayer)continue;
                    float cdx = ent.pos.x - frame.camPos.x, cdy = ent.pos.y - frame.camPos.y, cdz = ent.pos.z - frame.camPos.z;
                    float cdist = sqrtf(cdx * cdx + cdy * cdy + cdz * cdz);
                    if (cdist < bestCamDist) { bestCamDist = cdist; cachedLocalPlayer = ent.ptr; localPlayerTick = tick; }
                }
            }

            // Determine active range based on which mode
            float activeRange = g_silentAim.load() ? g_silentRange : g_mouseRange;
            if (g_silentAim.load() && g_mouseAim.load()) activeRange = (std::max)(g_silentRange, g_mouseRange);

            float bestDist2D = 99999.f;
            float targetWorldDist = 0.f;
            int scrCX = SW / 2, scrCY = SH / 2;
            uintptr_t bestEntPtr = 0;

            // ── TAG SYSTEM: Middle mouse to tag/untag ──
            if (GetAsyncKeyState(VK_MBUTTON) & 1) {
                if (g_taggedTarget) {
                    // Untag
                    printf("[TAG] Untagged %p\n", (void*)g_taggedTarget);
                    g_taggedTarget = 0;
                }
                else {
                    // Tag the closest player to crosshair
                    float bestTag2D = 99999.f;
                    uintptr_t bestTagPtr = 0;
                    for (size_t ti = 0; ti < frame.entities.size(); ti++) {
                        EntityData& te = frame.entities[ti];
                        if (!te.isPlayer || te.ptr == cachedLocalPlayer) continue;
                        if (te.dist > g_silentRange) continue;
                        int tsx, tsy;
                        if (!W2S(te.pos, tsx, tsy, frame, SW, SH)) continue;
                        float td = sqrtf((float)((tsx - scrCX) * (tsx - scrCX) + (tsy - scrCY) * (tsy - scrCY)));
                        if (td < bestTag2D) { bestTag2D = td; bestTagPtr = te.ptr; }
                    }
                    if (bestTagPtr) {
                        g_taggedTarget = bestTagPtr;
                        g_tagTick = tick;
                        printf("[TAG] Tagged %p\n", (void*)g_taggedTarget);
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

                    // Get bone position for tagged target
                    vector3 tagBone;
                    static const int boneMap2[] = { PB_HEAD, PB_NECK, PB_SPINE2, PB_PELVIS };
                    int prefB = boneMap2[g_aimBoneChoice];
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
                            tagLed.x += vit->second.velocity.x * g_leadFactor * 60.f;
                            tagLed.y += vit->second.velocity.y * g_leadFactor * 60.f;
                            tagLed.z += vit->second.velocity.z * g_leadFactor * 60.f;
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
                if (!tagFound && (tick - g_tagTick > 300)) {
                    printf("[TAG] Lost target %p, untagging\n", (void*)g_taggedTarget);
                    g_taggedTarget = 0;
                }
            }

            // ── NORMAL AIM TARGETING (skip if tag is active) ──
            if (!tagActive) {

                for (auto& ent : frame.entities) {
                    if (!ent.isPlayer && !ent.isZombie)continue;
                    if (ent.ptr == cachedLocalPlayer)continue;
                    if (ent.dist > activeRange)continue;

                    vector3 targetBone;
                    const char* aimSrc = "GUESS";

                    // Preferred bone from selector (HEAD=0, NECK=1, SPINE2=2, PELVIS=3)
                    static const int boneMap[] = { PB_HEAD, PB_NECK, PB_SPINE2, PB_PELVIS };
                    int prefBone = boneMap[g_aimBoneChoice];
                    if (ent.hasBones && ent.bones[prefBone].valid) {
                        targetBone = ent.bones[prefBone].pos;
                        aimSrc = g_boneChoiceNames[g_aimBoneChoice];
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
                    if (g_mouseAim.load() && g_aimPrediction.load() && g_leadFactor > 0.01f) {
                        auto vit = g_velocityMap.find(ent.ptr);
                        if (vit != g_velocityMap.end() && vit->second.valid) {
                            vector3 vel = vit->second.velocity;
                            float speed = sqrtf(vel.x * vel.x + vel.z * vel.z);
                            if (speed > 0.005f) {
                                float leadFrames = g_leadFactor / (speed + 0.001f);
                                leadFrames = (std::min)(leadFrames, 30.f);
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
            if (!aimDbgDone) {
                aimDbgDone = true;
                uintptr_t bt = read<uintptr_t>(frame.world + Off::Bullets, dayzid);
                int bc = read<int>(frame.world + Off::BulletCount, dayzid);
                printf("\n[AIM] Bullet table=%p count=%d silentRange=%.0fm mouseRange=%.0fm fov=%dpx\n",
                    (void*)bt, bc, g_silentRange, g_mouseRange, aimFovI);
                printf("[AIM] Local player: %p  prediction=%s  lead=%.1fm\n",
                    (void*)cachedLocalPlayer, g_aimPrediction.load() ? "ON" : "OFF", g_leadFactor);
            }

            // ── SPEED HACK — with weapon swap detection ──
            if (cachedLocalPlayer && (g_silentAim.load() || g_mouseAim.load())) {
                uintptr_t inv = read<uintptr_t>(cachedLocalPlayer + Off::Inventory, dayzid);
                uintptr_t hands = 0;
                if (inv && inv > 0x10000000ULL)
                    hands = read<uintptr_t>(inv + Off::InvHands, dayzid);

                // Weapon swap detection: if hands pointer changed, validate new item
                if (hands != g_cachedHands) {
                    g_cachedHands = hands;
                    if (hands && hands > 0x10000000ULL) {
                        // Check if new item is a firearm (has valid ammo chain)
                        uintptr_t testAmmo = read<uintptr_t>(hands + Off::AmmoType1, dayzid);
                        if (testAmmo && testAmmo > 0x10000000ULL) {
                            g_speedHackDone = false; // firearm — re-walk chain
                            printf("[SPD] Weapon swap -> firearm, re-applying speed\n");
                        }
                        else {
                            g_speedHackDone = true; // not a firearm — skip
                            printf("[SPD] Weapon swap -> non-firearm, skipping\n");
                        }
                    }
                    else {
                        g_speedHackDone = true; // empty hands
                    }
                }

                if ((!g_speedHackDone && (tick - g_lastSpeedWriteTick) > 30) ||
                    (fabsf(g_bulletSpeed - g_lastAppliedSpeed) > 1.f && (tick - g_lastSpeedWriteTick) > 30)) {
                    if (hands && hands > 0x10000000ULL) {
                        uintptr_t ammo1 = read<uintptr_t>(hands + Off::AmmoType1, dayzid);
                        if (!ammo1 || ammo1 < 0x10000000ULL) { g_speedHackDone = true; }
                        else {
                            uintptr_t ammo2 = read<uintptr_t>(ammo1 + Off::AmmoType2, dayzid);
                            if (!ammo2 || ammo2 < 0x10000000ULL) { g_speedHackDone = true; }
                            else {
                                queueWrite<float>(ammo2 + Off::InitSpeed, g_bulletSpeed, dayzid);
                                g_speedHackDone = true;
                                g_lastAppliedSpeed = g_bulletSpeed;
                                g_lastSpeedWriteTick = tick;
                                printf("[SPD] InitSpeed -> %.0f m/s\n", g_bulletSpeed);
                                fflush(stdout);
                            }
                        }
                    }
                }
            }

            // ── MOUSE AIMBOT — move mouse toward predicted target when RMB held ──
            if (g_mouseAim.load() && hasAimTarget) {
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

            // ── SILENT AIM — bullet TP (respects silent range cap) ──
            if (g_silentAim.load() && hasAimTarget && targetWorldDist <= g_silentRange) {
                uintptr_t bulletTable = read<uintptr_t>(frame.world + Off::Bullets, dayzid);
                int bulletCount = read<int>(frame.world + Off::BulletCount, dayzid);
                if (bulletTable && bulletTable > 0x10000000ULL && bulletCount > 0 && bulletCount < 512) {
                    for (int i = 0; i < bulletCount; i++) {
                        uintptr_t bullet = read<uintptr_t>(bulletTable + i * 0x8, dayzid);
                        if (!bullet || bullet < 0x10000000ULL)continue;
                        if (bulletAlreadyTpd(bullet))continue;
                        uintptr_t bvs = read<uintptr_t>(bullet + Off::VisualState, dayzid);
                        if (!bvs || bvs < 0x10000000ULL)continue;
                        queueWrite<vector3>(bvs + Off::GetCoord, silentTarget, dayzid);
                        trackBullet(bullet);
                        // Track for hit log
                        g_lastBulletTPTarget = bestEntPtr;
                        g_lastBulletTPTick = tick;
                        g_pendingHitCheck = true;
                        printf("[AIM] Bullet %p -> (%.1f,%.1f,%.1f) dist=%.0fm\n",
                            (void*)bullet, silentTarget.x, silentTarget.y, silentTarget.z, targetWorldDist);
                        fflush(stdout);
                    }
                }
                else { g_tpdCount = 0; }
            }

            // ── HIT LOG — check if targeted entity disappeared ──
            if (g_pendingHitCheck && g_lastBulletTPTarget) {
                int elapsed = tick - g_lastBulletTPTick;
                if (elapsed > 15 && elapsed < 300) { // check between ~0.25s and ~5s
                    bool found = false;
                    for (size_t ei = 0; ei < frame.entities.size(); ei++) {
                        if (frame.entities[ei].ptr == g_lastBulletTPTarget) { found = true; break; }
                    }
                    if (!found) {
                        g_totalHits++;
                        HitEntry he;
                        he.tick = tick;
                        he.dist = targetWorldDist;
                        he.bone = g_boneChoiceNames[g_aimBoneChoice];
                        g_hitLog.push_back(he);
                        if ((int)g_hitLog.size() > MAX_HIT_LOG)
                            g_hitLog.erase(g_hitLog.begin());
                        printf("[HIT] #%d confirmed! Entity %p gone after %d ticks\n",
                            g_totalHits, (void*)g_lastBulletTPTarget, elapsed);
                        // Auto-untag if tagged target was killed
                        if (g_taggedTarget == g_lastBulletTPTarget) {
                            printf("[TAG] Tagged target killed, untagging\n");
                            g_taggedTarget = 0;
                        }
                        g_pendingHitCheck = false;
                        g_lastBulletTPTarget = 0;
                    }
                }
                if (elapsed >= 300) { // timeout, entity survived or we lost tracking
                    g_pendingHitCheck = false;
                    g_lastBulletTPTarget = 0;
                }
            }
        }

        // ── NO RECOIL — pull mouse down while firing ──
        if (g_noRecoil.load() && (GetAsyncKeyState(VK_LBUTTON) & 0x8000)) {
            mouse_event(MOUSEEVENTF_MOVE, 0, (DWORD)(int)g_recoilPull, 0, 0);
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

            // Crosshair
            HPEN crossPen = CreatePen(PS_SOLID, 1, COL_CROSS); SelectObject(mem, crossPen);
            int cx = SW / 2, cy = SH / 2;
            MoveToEx(mem, cx - 8, cy, NULL); LineTo(mem, cx - 3, cy);
            MoveToEx(mem, cx + 3, cy, NULL); LineTo(mem, cx + 8, cy);
            MoveToEx(mem, cx, cy - 8, NULL); LineTo(mem, cx, cy - 3);
            MoveToEx(mem, cx, cy + 3, NULL); LineTo(mem, cx, cy + 8);
            DeleteObject(crossPen);

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
                    if (age > 600) continue; // fade after ~10s
                    int alpha = 255 - age * 255 / 600;
                    if (alpha < 40) alpha = 40;
                    char hfLine[64]; snprintf(hfLine, sizeof(hfLine), ">> HIT  %.0fm  [%s]", he.dist, he.bone.c_str());
                    SetTextColor(mem, RGB(alpha, alpha * 40 / 255, alpha * 40 / 255));
                    TextOutA(mem, hfx, hfy, hfLine, (int)strlen(hfLine));
                    hfy += 16;
                }
                SelectObject(mem, espF); SetTextColor(mem, g_colPlayerBox);
            }

            // FOV circle
            if (fovFilter) {
                HPEN fovPen = CreatePen(PS_DOT, 1, RGB(80, 80, 120));
                SelectObject(mem, fovPen); SelectObject(mem, GetStockObject(NULL_BRUSH));
                Ellipse(mem, cx - g_fovRadius, cy - g_fovRadius, cx + g_fovRadius, cy + g_fovRadius);
                DeleteObject(fovPen);
            }

            // Aim FOV circle + target line
            if (g_silentAim.load()) {
                HPEN aimPen = CreatePen(PS_SOLID, 1, RGB(255, 40, 40));
                SelectObject(mem, aimPen); SelectObject(mem, GetStockObject(NULL_BRUSH));
                Ellipse(mem, cx - aimFovI, cy - aimFovI, cx + aimFovI, cy + aimFovI);
                DeleteObject(aimPen);
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
            if (g_mouseAim.load()) {
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

            // Collect player screen positions during ESP for arrow reuse
            struct ArrowData { int sx, sy; float dist; bool onScreen; float dirX, dirZ; };
            std::vector<ArrowData> arrowPlayers;

            for (auto& ent : frame.entities) {
                if (ent.isPlayer && !espShowPlayers)continue;
                if (ent.isZombie && !espShowZombies)continue;
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
                    COLORREF boxCol = ent.isZombie ? g_colZombieBox : g_colPlayerBox;
                    COLORREF snapCol = ent.isZombie ? RGB(200, 60, 20) : g_colSnapLine;
                    float a = 0.85f; auto& p = espPrev[ent.ptr];
                    float sx2 = p.first * (1 - a) + sxF * a, sy2 = p.second * (1 - a) + syF * a;
                    p.first = sx2; p.second = sy2;
                    int bH2 = abs(syF - syH), dW = (int)(1000.f / ent.dist);
                    int bW2 = (std::min)(bH2, (std::max)(dW, 6));
                    int ecx = (int)sx2;

                    // Corner box
                    int left = ecx - bW2 / 2, right = ecx + bW2 / 2, top = syH, bot = syF;
                    int cLen = (std::max)(4, (std::min)(bH2 / 4, bW2 / 3)); // corner length
                    HPEN pen = CreatePen(PS_SOLID, 2, boxCol);
                    SelectObject(mem, pen);
                    // Top-left
                    MoveToEx(mem, left, top + cLen, NULL); LineTo(mem, left, top); LineTo(mem, left + cLen, top);
                    // Top-right
                    MoveToEx(mem, right - cLen, top, NULL); LineTo(mem, right, top); LineTo(mem, right, top + cLen);
                    // Bottom-left
                    MoveToEx(mem, left, bot - cLen, NULL); LineTo(mem, left, bot); LineTo(mem, left + cLen, bot);
                    // Bottom-right
                    MoveToEx(mem, right - cLen, bot, NULL); LineTo(mem, right, bot); LineTo(mem, right, bot - cLen);
                    DeleteObject(pen);

                    HPEN sp = CreatePen(PS_SOLID, 1, snapCol);
                    SelectObject(mem, sp); MoveToEx(mem, SW / 2, SH - 1, NULL); LineTo(mem, ecx, syF); DeleteObject(sp);

                    // Distance tag with dark background
                    SelectObject(mem, espFItemDist);
                    char dt[32]; sprintf_s(dt, "%.0fm", ent.dist);
                    SIZE ts; GetTextExtentPoint32A(mem, dt, (int)strlen(dt), &ts);
                    int tagX = ecx - ts.cx / 2 - 4, tagY = syF + 3;
                    HBRUSH dtBg = CreateSolidBrush(RGB(12, 14, 18));
                    RECT dtR = { tagX, tagY, tagX + ts.cx + 8, tagY + ts.cy + 2 };
                    FillRect(mem, &dtR, dtBg); DeleteObject(dtBg);
                    SetTextColor(mem, RGB(200, 210, 220));
                    TextOutA(mem, ecx - ts.cx / 2, tagY + 1, dt, (int)strlen(dt));

                    // Weapon in hand (players only)
                    if (ent.isPlayer && !ent.weaponName.empty()) {
                        const char* wn = ent.weaponName.c_str();
                        SIZE ws; GetTextExtentPoint32A(mem, wn, (int)ent.weaponName.length(), &ws);
                        int wTagX = ecx - ws.cx / 2 - 4, wTagY = tagY + ts.cy + 4;
                        HBRUSH wBg = CreateSolidBrush(RGB(12, 14, 18));
                        RECT wR = { wTagX, wTagY, wTagX + ws.cx + 8, wTagY + ws.cy + 2 };
                        FillRect(mem, &wR, wBg); DeleteObject(wBg);
                        SetTextColor(mem, RGB(255, 160, 80));
                        TextOutA(mem, ecx - ws.cx / 2, wTagY + 1, wn, (int)ent.weaponName.length());
                    }

                    if (ent.isZombie) {
                        SelectObject(mem, espF);
                        SetTextColor(mem, g_colPlayerBox);
                        TextOutA(mem, ecx - 12, syH - 14, "Zmb", 3);
                    }

                    // Tagged target indicator
                    if (ent.isPlayer && ent.ptr == g_taggedTarget) {
                        // Pulsing red circle around box
                        int pulse = (tick % 60 < 30) ? 255 : 180;
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

                    if (g_showBones.load() && ent.hasBones) {
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
                    int tagH = ns.cy + 2; // use name height as baseline
                    int padX = 7, padY = 3;
                    int tagX = ecx - totalW / 2 - padX;
                    int tagY = ecy - tagH / 2 - padY - 2;
                    int tagW = totalW + padX * 2;
                    int tagHFull = tagH + padY * 2;

                    // Dark background pill
                    HBRUSH bgBr = CreateSolidBrush(RGB(12, 14, 18));
                    HPEN bgPen = CreatePen(PS_SOLID, 1, RGB(40, 44, 52));
                    SelectObject(mem, bgBr); SelectObject(mem, bgPen);
                    RoundRect(mem, tagX, tagY, tagX + tagW, tagY + tagHFull, 6, 6);
                    DeleteObject(bgBr); DeleteObject(bgPen);

                    // Item name — clean neutral white
                    int textX = ecx - totalW / 2;
                    int textY = tagY + padY;
                    SelectObject(mem, espFItem);
                    SetTextColor(mem, RGB(220, 224, 230));
                    TextOutA(mem, textX, textY, iname, nameLen);

                    // Distance — dimmer accent
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

            // ── Out-of-view player arrows (uses ESP-collected screen data) ──
            {
                int arrowRad = aimFovI;
                bool aimActive = g_silentAim.load() || g_mouseAim.load();
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

            BitBlt(hdc, 0, 0, SW, SH, mem, 0, 0, SRCCOPY);
            DeleteObject(bmp); DeleteDC(mem); ReleaseDC(espH, hdc);
        }

        // ══════════════════════════════
        //  RENDER RADAR (circular)
        // ══════════════════════════════
        if (radOn && (tick % 4 == 0)) {
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

        drainWrites(2); // process up to 2 queued writes per frame via MmCopyVirtualMemory
        tick++; Sleep(16);
    }

    DeleteObject(espF); DeleteObject(espFL); DeleteObject(espFItem); DeleteObject(espFItemDist); DeleteObject(radF);
    DestroyWindow(espH); DestroyWindow(radH); DestroyWindow(barH);
    UnregisterClassA("ESP_OVL", GetModuleHandleA(nullptr));
    UnregisterClassA("RADAR_OVL", GetModuleHandleA(nullptr));
    g_overlayRunning = false;
    printf("[overlay] Stopped.\n");
}

void StartOverlayThread(int d) {
    if (g_overlayRunning.exchange(true))return;
    g_shutdownAll = false;
    g_overlayThread = std::thread([=] {RunOverlays(d); }); g_overlayThread.detach();
}
void StopAll() {
    g_shutdownAll = true; Sleep(300);
    g_espEnabled = false; g_radarEnabled = false; g_showBones = false; g_showItems = false;
    g_fovOnly = false; g_silentAim = false; g_showLandmarks = false; g_showWells = false; g_mouseAim = false;
    g_radTrails = false;
    g_writeHead = g_writeTail = 0; // discard pending writes
    g_taggedTarget = 0;
}

// ═══════════════════════════════════════════════════════════════
//  MENU — 4 Pages: ESP | Aimbot | Radar | Filter
// ═══════════════════════════════════════════════════════════════
struct MenuItem { std::string label; std::atomic<bool>* toggle; COLORREF accent; };

static HWND g_menuHwnd = nullptr;
static bool g_menuVisible = true;
static HFONT g_mF = nullptr, g_mFB = nullptr, g_mFS = nullptr;
static int g_menuHover = -1;
static bool g_menuDrag = false;
static POINT g_dragStart = {};
static int g_dayzid_menu = 0;

static constexpr int MW = 560, MH_HDR = 44, MH_TAB = 0, MH_ROW = 36, MH_PAD = 10, MH_FTR = 30;
static constexpr int SIDEBAR_W = 140, CONTENT_X = SIDEBAR_W;
static constexpr int SB_HDR_H = 26, SB_ITEM_H = 34;
static const COLORREF ACCENT = RGB(255, 45, 45);
static const COLORREF BG_DARK = RGB(12, 14, 18);
static const COLORREF BG_SIDEBAR = RGB(16, 18, 24);
static const COLORREF BG_CARD = RGB(20, 22, 30);
static const COLORREF BORDER_CARD = RGB(36, 40, 52);
static const COLORREF TEXT_DIM = RGB(80, 85, 100);
static const COLORREF TEXT_MED = RGB(140, 145, 160);
static const COLORREF TEXT_BRIGHT = RGB(220, 225, 235);

static std::vector<MenuItem> g_pageItems;

static void BuildPageItems() {
    g_pageItems.clear();
    g_aimSliders.clear();
    if (g_menuPage == 0) { // ESP page
        g_pageItems.push_back({ "ESP Overlay",&g_espEnabled,RGB(180,130,255) });
        g_pageItems.push_back({ "FOV Only",&g_fovOnly,RGB(80,80,180) });
        g_pageItems.push_back({ "Show Players",&g_espPlayers,RGB(120,200,255) });
        g_pageItems.push_back({ "Show Zombies",&g_espZombies,RGB(255,80,40) });
        g_pageItems.push_back({ "Bone Skeleton",&g_showBones,RGB(0,220,180) });
        g_pageItems.push_back({ "Item ESP",&g_showItems,RGB(200,200,100) });
        g_pageItems.push_back({ "Well ESP",&g_showWells,RGB(80,180,255) });
    }
    else if (g_menuPage == 1) { // Aimbot page — toggles + sliders
        g_pageItems.push_back({ "Magic Bullet",&g_silentAim,RGB(255,50,50) });
        g_pageItems.push_back({ "Mouse Aim",&g_mouseAim,RGB(40,255,40) });
        g_pageItems.push_back({ "No Recoil",&g_noRecoil,RGB(255,140,40) });
        g_pageItems.push_back({ "Prediction",&g_aimPrediction,RGB(255,200,60) });
        // Sliders
        g_aimSliders.push_back({ "Aim FOV",&g_aimFovF,30.f,300.f,5.f,RGB(255,100,100),"px",0 });
        g_aimSliders.push_back({ "MB Range",&g_silentRange,5.f,1000.f,5.f,RGB(255,50,50),"m",0 });
        g_aimSliders.push_back({ "Mouse Range",&g_mouseRange,5.f,500.f,5.f,RGB(40,255,40),"m",0 });
        g_aimSliders.push_back({ "Smoothing",&g_mouseSmooth,1.f,10.f,0.5f,RGB(100,200,255),"x",1 });
        g_aimSliders.push_back({ "Bullet Speed",&g_bulletSpeed,500.f,50000.f,500.f,RGB(255,180,60),"m/s",0 });
        g_aimSliders.push_back({ "Lead Distance",&g_leadFactor,0.f,2.0f,0.1f,RGB(255,200,60),"m",1 });
        g_aimSliders.push_back({ "Recoil Pull",&g_recoilPull,0.5f,5.0f,0.25f,RGB(255,140,40),"px",1 });
    }
    else if (g_menuPage == 2) { // Radar page
        g_pageItems.push_back({ "Radar",&g_radarEnabled,RGB(0,180,255) });
        g_pageItems.push_back({ "Show Players",&g_radPlayers,RGB(120,200,255) });
        g_pageItems.push_back({ "Show Zombies",&g_radZombies,RGB(255,80,40) });
        g_pageItems.push_back({ "Show Trails",&g_radTrails,RGB(140,120,255) });
        g_pageItems.push_back({ "Show Landmarks",&g_showLandmarks,RGB(200,180,255) });
        if (g_showLandmarks.load()) {
            g_pageItems.push_back({ "  Cities",&g_lmCities,RGB(200,180,255) });
            g_pageItems.push_back({ "  Towns",&g_lmTowns,RGB(160,200,160) });
            g_pageItems.push_back({ "  Military",&g_lmMilitary,RGB(255,100,100) });
        }
    }
    // Page 3 = Filter (no toggle items, just search)
}

// Build display rows for filter page (headers + items)
struct FilterRow { bool isHeader; int catIdx; int itemIdx; }; // itemIdx = index into g_nearbyItems
static std::vector<FilterRow> BuildFilterRows() {
    // MUST be called with g_itemMtx held
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

static int MHeight() {
    // Sidebar minimum height
    int sbH = MH_HDR + 2 * SB_HDR_H + 4 * SB_ITEM_H + 40;
    // Content height
    int ch;
    if (g_menuPage == 3) {
        std::lock_guard<std::mutex> lk(g_itemMtx);
        std::vector<FilterRow> rows = BuildFilterRows();
        int totalRows = (int)rows.size();
        int visRows = totalRows < FILTER_MAX_VISIBLE ? totalRows : FILTER_MAX_VISIBLE;
        ch = MH_HDR + 98 + 30 + visRows * FILTER_ROW_H + 24 + MH_FTR;
    }
    else {
        ch = MH_HDR + (int)g_pageItems.size() * MH_ROW;
        if (g_menuPage == 0) { // ESP: color section
            ch += 26 + 5 * 22; // header + 5 color rows
            if (g_colorPickerOpen >= 0) ch += WHEEL_R * 2 + 24; // wheel + padding
        }
        if (g_menuPage == 1) { // Aimbot: bone + sliders
            ch += MH_ROW;
            ch += 28 + SL_PAD + (int)g_aimSliders.size() * SL_H + SL_PAD;
        }
        ch += MH_FTR + MH_PAD;
    }
    return ch > sbH ? ch : sbH;
}

// Helper: get slider track geometry
static void GetSliderTrack(int sliderIdx, int menuW, int baseY, int& trackL, int& trackR, int& trackY) {
    int iy = baseY + SL_PAD + sliderIdx * SL_H;
    trackL = CONTENT_X + SL_PAD + 4;
    trackR = menuW - SL_PAD - 4;
    trackY = iy + SL_TRACK_Y;
}

static float SliderXToVal(int x, int trackL, int trackR, const SliderDef& s) {
    float t = (float)(x - trackL) / (float)(trackR - trackL);
    t = (std::max)(0.f, (std::min)(1.f, t));
    float raw = s.lo + t * (s.hi - s.lo);
    // Snap to step
    raw = roundf(raw / s.step) * s.step;
    return (std::max)(s.lo, (std::min)(s.hi, raw));
}

static int ValToSliderX(float val, int trackL, int trackR, const SliderDef& s) {
    float t = (val - s.lo) / (s.hi - s.lo);
    t = (std::max)(0.f, (std::min)(1.f, t));
    return trackL + (int)(t * (trackR - trackL));
}

// ── Sidebar navigation ──
struct SidebarNav { const char* label; int page; bool isHeader; };
static const SidebarNav g_sidebar[] = {
    {"Player",-1,true}, {"ESP",0,false}, {"Aimbot",1,false},
    {"World",-1,true},  {"Radar",2,false}, {"Filter",3,false}
};
static const int SIDEBAR_ITEMS = 6;
static int g_sidebarHover = -1;

// ── GDI icon drawing ──
static void DrawIconEye(HDC dc, int x, int y, COLORREF c) {
    HPEN p = CreatePen(PS_SOLID, 1, c); HPEN old = (HPEN)SelectObject(dc, p);
    SelectObject(dc, GetStockObject(NULL_BRUSH));
    Ellipse(dc, x, y + 3, x + 14, y + 11);
    HBRUSH b = CreateSolidBrush(c); SelectObject(dc, b);
    Ellipse(dc, x + 5, y + 5, x + 9, y + 9);
    DeleteObject(b); SelectObject(dc, old); DeleteObject(p);
}
static void DrawIconCross(HDC dc, int x, int y, COLORREF c) {
    HPEN p = CreatePen(PS_SOLID, 1, c); HPEN old = (HPEN)SelectObject(dc, p);
    SelectObject(dc, GetStockObject(NULL_BRUSH));
    Ellipse(dc, x + 2, y + 2, x + 12, y + 12);
    MoveToEx(dc, x + 7, y, NULL); LineTo(dc, x + 7, y + 14);
    MoveToEx(dc, x, y + 7, NULL); LineTo(dc, x + 14, y + 7);
    SelectObject(dc, old); DeleteObject(p);
}
static void DrawIconRadar(HDC dc, int x, int y, COLORREF c) {
    HPEN p = CreatePen(PS_SOLID, 1, c); HPEN old = (HPEN)SelectObject(dc, p);
    SelectObject(dc, GetStockObject(NULL_BRUSH));
    Ellipse(dc, x + 1, y + 1, x + 13, y + 13);
    Ellipse(dc, x + 4, y + 4, x + 10, y + 10);
    MoveToEx(dc, x + 7, y + 7, NULL); LineTo(dc, x + 12, y + 2);
    SelectObject(dc, old); DeleteObject(p);
}
static void DrawIconFilter(HDC dc, int x, int y, COLORREF c) {
    HPEN p = CreatePen(PS_SOLID, 2, c); HPEN old = (HPEN)SelectObject(dc, p);
    MoveToEx(dc, x + 1, y + 3, NULL); LineTo(dc, x + 13, y + 3);
    MoveToEx(dc, x + 3, y + 7, NULL); LineTo(dc, x + 11, y + 7);
    MoveToEx(dc, x + 5, y + 11, NULL); LineTo(dc, x + 9, y + 11);
    SelectObject(dc, old); DeleteObject(p);
}
typedef void(*IconFn)(HDC, int, int, COLORREF);
static IconFn g_sidebarIcons[] = { nullptr, DrawIconEye, DrawIconCross, nullptr, DrawIconRadar, DrawIconFilter };

// ── Color wheel (HSV, V=1) ──
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

static LRESULT CALLBACK MenuWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
    case WM_CREATE:RegisterHotKey(hwnd, 100, 0, VK_F1); return 0;
    case WM_HOTKEY:if (wParam == 100) { g_menuVisible = !g_menuVisible; ShowWindow(hwnd, g_menuVisible ? SW_SHOW : SW_HIDE); }return 0;
    case WM_LBUTTONDOWN: {
        int mx = GET_X_LPARAM(lParam), my = GET_Y_LPARAM(lParam);
        // Header drag
        if (my < MH_HDR) { g_menuDrag = true; g_dragStart = { mx,my }; SetCapture(hwnd); return 0; }
        // Sidebar clicks
        if (mx < SIDEBAR_W && my >= MH_HDR) {
            int sy = MH_HDR + 8;
            for (int si = 0; si < SIDEBAR_ITEMS; si++) {
                int rh = g_sidebar[si].isHeader ? SB_HDR_H : SB_ITEM_H;
                if (!g_sidebar[si].isHeader && my >= sy && my < sy + rh) {
                    int newPage = g_sidebar[si].page;
                    if (newPage != g_menuPage) {
                        g_menuPage = newPage; g_colorPickerOpen = -1; BuildPageItems();
                        RECT wr; GetWindowRect(hwnd, &wr);
                        SetWindowPos(hwnd, NULL, wr.left, wr.top, MW, MHeight(), SWP_NOZORDER);
                        InvalidateRect(hwnd, nullptr, FALSE);
                        if (newPage == 3)SetTimer(hwnd, 200, 1000, NULL);
                        else KillTimer(hwnd, 200);
                    }
                    return 0;
                }
                sy += rh;
            }
            return 0;
        }
        // Content area clicks (mx >= SIDEBAR_W)
        if (g_menuPage != 3) {
            int iy = MH_HDR;
            for (size_t i = 0; i < g_pageItems.size(); i++) {
                if (my >= iy && my < iy + MH_ROW && mx >= CONTENT_X) {
                    bool nv = !g_pageItems[i].toggle->load();
                    g_pageItems[i].toggle->store(nv);
                    if ((g_espEnabled.load() || g_radarEnabled.load() || g_silentAim.load() || g_mouseAim.load() || g_noRecoil.load()) && !g_overlayRunning.load())
                        StartOverlayThread(g_dayzid_menu);
                    BuildPageItems();
                    RECT wr; GetWindowRect(hwnd, &wr);
                    SetWindowPos(hwnd, NULL, wr.left, wr.top, MW, MHeight(), SWP_NOZORDER);
                    InvalidateRect(hwnd, nullptr, FALSE); break;
                }iy += MH_ROW;
            }
            // Bone selector (Aimbot page)
            if (g_menuPage == 1) {
                int boneRowY = MH_HDR + (int)g_pageItems.size() * MH_ROW;
                if (my >= boneRowY && my < boneRowY + MH_ROW && mx >= CONTENT_X) {
                    if (mx < CONTENT_X + (MW - CONTENT_X) / 2)
                        g_aimBoneChoice = (g_aimBoneChoice - 1 + NUM_BONE_CHOICES) % NUM_BONE_CHOICES;
                    else
                        g_aimBoneChoice = (g_aimBoneChoice + 1) % NUM_BONE_CHOICES;
                    InvalidateRect(hwnd, nullptr, FALSE); return 0;
                }
            }
            // Color swatch + wheel clicks (ESP page)
            if (g_menuPage == 0) {
                COLORREF* colorPtrs[] = { &g_colPlayerBox, &g_colZombieBox, &g_colSnapLine, &g_colBone, &g_colHeadDot };
                int colorBaseY = MH_HDR + (int)g_pageItems.size() * MH_ROW + 4 + 6 + 16;
                int cy2 = colorBaseY;
                for (int ci = 0; ci < 5; ci++) {
                    if (my >= cy2 && my < cy2 + 22 && mx >= CONTENT_X) {
                        g_colorPickerOpen = (g_colorPickerOpen == ci) ? -1 : ci;
                        RECT wr; GetWindowRect(hwnd, &wr);
                        SetWindowPos(hwnd, NULL, wr.left, wr.top, MW, MHeight(), SWP_NOZORDER);
                        InvalidateRect(hwnd, nullptr, FALSE); return 0;
                    }
                    cy2 += 22;
                    if (g_colorPickerOpen == ci) {
                        // Wheel click
                        int wcx = CONTENT_X + (MW - CONTENT_X) / 2, wcy = cy2 + WHEEL_R + 4;
                        float ddx = (float)(mx - wcx), ddy = (float)(my - wcy);
                        if (sqrtf(ddx * ddx + ddy * ddy) < (float)WHEEL_R) {
                            *colorPtrs[ci] = WheelPick(wcx, wcy, mx, my);
                            InvalidateRect(hwnd, nullptr, FALSE); return 0;
                        }
                        cy2 += WHEEL_R * 2 + 24;
                    }
                }
            }
            // Slider clicks (Aimbot page)
            if (g_menuPage == 1 && !g_aimSliders.empty()) {
                int sliderBase = MH_HDR + (int)g_pageItems.size() * MH_ROW + MH_ROW;
                for (int si = 0; si < (int)g_aimSliders.size(); si++) {
                    int tL, tR, tY;
                    GetSliderTrack(si, MW, sliderBase, tL, tR, tY);
                    if (my >= tY - SL_KNOB_R - 2 && my <= tY + SL_KNOB_R + 2 && mx >= tL - SL_KNOB_R && mx <= tR + SL_KNOB_R) {
                        g_sliderDrag = si;
                        *g_aimSliders[si].val = SliderXToVal(mx, tL, tR, g_aimSliders[si]);
                        SetCapture(hwnd); InvalidateRect(hwnd, nullptr, FALSE); return 0;
                    }
                }
            }
        }
        else {
            // Filter page clicks
            int tableTop = MH_HDR + 98;
            if (my >= tableTop && my < tableTop + 26 && mx >= CONTENT_X) {
                std::lock_guard<std::mutex> lk(g_itemMtx);
                g_selectedItems.clear();
                InvalidateRect(hwnd, nullptr, FALSE); return 0;
            }
            int rowStart = tableTop + 30;
            if (my >= rowStart && mx >= CONTENT_X) {
                int clickedVis = (my - rowStart) / FILTER_ROW_H;
                std::lock_guard<std::mutex> lk(g_itemMtx);
                std::vector<FilterRow> rows = BuildFilterRows();
                int rowIdx = clickedVis + g_filterScroll;
                if (rowIdx >= 0 && rowIdx < (int)rows.size() && !rows[rowIdx].isHeader) {
                    int ii = rows[rowIdx].itemIdx;
                    if (ii >= 0 && ii < (int)g_nearbyItems.size()) {
                        const std::string& name = g_nearbyItems[ii].name;
                        if (g_selectedItems.count(name))g_selectedItems.erase(name);
                        else g_selectedItems.insert(name);
                    }
                    InvalidateRect(hwnd, nullptr, FALSE);
                }
            }
        }
        return 0;
    }
    case WM_LBUTTONUP:
        if (g_menuDrag) { g_menuDrag = false; ReleaseCapture(); }
        if (g_sliderDrag >= 0) { g_sliderDrag = -1; ReleaseCapture(); }
        return 0;
    case WM_MOUSEMOVE: {
        int mx = GET_X_LPARAM(lParam), my = GET_Y_LPARAM(lParam);
        if (g_menuDrag) { RECT wr; GetWindowRect(hwnd, &wr); SetWindowPos(hwnd, NULL, wr.left + mx - g_dragStart.x, wr.top + my - g_dragStart.y, 0, 0, SWP_NOSIZE | SWP_NOZORDER); return 0; }
        if (g_sliderDrag >= 0 && g_menuPage == 1) {
            int sliderBase = MH_HDR + (int)g_pageItems.size() * MH_ROW + MH_ROW;
            int tL, tR, tY;
            GetSliderTrack(g_sliderDrag, MW, sliderBase, tL, tR, tY);
            *g_aimSliders[g_sliderDrag].val = SliderXToVal(mx, tL, tR, g_aimSliders[g_sliderDrag]);
            InvalidateRect(hwnd, nullptr, FALSE); return 0;
        }
        int old = g_menuHover; g_menuHover = -1;
        int oldSb = g_sidebarHover; g_sidebarHover = -1;
        // Sidebar hover
        if (mx < SIDEBAR_W && my >= MH_HDR) {
            int sy = MH_HDR + 8;
            for (int si = 0; si < SIDEBAR_ITEMS; si++) {
                int rh = g_sidebar[si].isHeader ? SB_HDR_H : SB_ITEM_H;
                if (!g_sidebar[si].isHeader&& my >= sy && my < sy + rh) { g_sidebarHover = si; break; }
                sy += rh;
            }
        }
        // Content hover
        if (mx >= CONTENT_X) {
            int iy = MH_HDR;
            for (size_t i = 0; i < g_pageItems.size(); i++) { if (my >= iy && my < iy + MH_ROW) { g_menuHover = (int)i; break; } iy += MH_ROW; }
        }
        if (g_menuHover != old || g_sidebarHover != oldSb) InvalidateRect(hwnd, nullptr, FALSE);
        TRACKMOUSEEVENT tme = { sizeof(tme),TME_LEAVE,hwnd,0 }; TrackMouseEvent(&tme); return 0;
    }
    case WM_MOUSELEAVE:if (g_menuHover >= 0 || g_sidebarHover >= 0) { g_menuHover = -1; g_sidebarHover = -1; InvalidateRect(hwnd, nullptr, FALSE); }return 0;
    case WM_MOUSEWHEEL: {
        if (g_menuPage == 3) {
            int delta = GET_WHEEL_DELTA_WPARAM(wParam);
            std::lock_guard<std::mutex> lk(g_itemMtx);
            std::vector<FilterRow> rows = BuildFilterRows();
            int totalRows = (int)rows.size();
            int maxScroll = totalRows > FILTER_MAX_VISIBLE ? totalRows - FILTER_MAX_VISIBLE : 0;
            if (delta > 0)g_filterScroll = g_filterScroll > 0 ? g_filterScroll - 1 : 0;
            else g_filterScroll = g_filterScroll < maxScroll ? g_filterScroll + 1 : maxScroll;
            InvalidateRect(hwnd, nullptr, FALSE);
        }
        return 0;
    }
    case WM_TIMER: {
        if (wParam == 200 && g_menuPage == 3) {
            RECT wr; GetWindowRect(hwnd, &wr);
            SetWindowPos(hwnd, NULL, wr.left, wr.top, MW, MHeight(), SWP_NOZORDER);
            InvalidateRect(hwnd, nullptr, FALSE);
        }
        return 0;
    }
    case WM_CHAR: {
        if (g_menuPage == 3) {
            char ch = (char)wParam;
            if (ch == '\b') { if (!g_espFilterText.empty())g_espFilterText.pop_back(); }
            else if (ch >= 0x20 && ch <= 0x7E && g_espFilterText.length() < 32)g_espFilterText += ch;
            InvalidateRect(hwnd, nullptr, FALSE);
        }
        return 0;
    }
    case WM_KEYDOWN: {
        if (g_menuPage == 3 && wParam == VK_ESCAPE) {
            if (!g_espFilterText.empty())g_espFilterText.clear();
            else { std::lock_guard<std::mutex> lk(g_itemMtx); g_selectedItems.clear(); g_filterScroll = 0; }
            RECT wr; GetWindowRect(hwnd, &wr);
            SetWindowPos(hwnd, NULL, wr.left, wr.top, MW, MHeight(), SWP_NOZORDER);
            InvalidateRect(hwnd, nullptr, FALSE); return 0;
        }
        return 0;
    }
    case WM_PAINT: {
        PAINTSTRUCT ps; HDC hdc = BeginPaint(hwnd, &ps);
        RECT rc; GetClientRect(hwnd, &rc); int w = rc.right, h = rc.bottom;
        HDC mem = CreateCompatibleDC(hdc); HBITMAP mb = CreateCompatibleBitmap(hdc, w, h);
        HBITMAP ob = (HBITMAP)SelectObject(mem, mb);
        EnsureWheel(mem);

        // Full background
        HBRUSH bgBr = CreateSolidBrush(BG_DARK); FillRect(mem, &rc, bgBr); DeleteObject(bgBr);
        SetBkMode(mem, TRANSPARENT);

        // Sidebar background
        RECT sbR = { 0, MH_HDR, SIDEBAR_W, h };
        HBRUSH sbBr = CreateSolidBrush(BG_SIDEBAR); FillRect(mem, &sbR, sbBr); DeleteObject(sbBr);
        // Sidebar right border
        HPEN sbLine = CreatePen(PS_SOLID, 1, RGB(30, 34, 45));
        SelectObject(mem, sbLine); MoveToEx(mem, SIDEBAR_W - 1, MH_HDR, NULL); LineTo(mem, SIDEBAR_W - 1, h); DeleteObject(sbLine);

        // Header bar
        HBRUSH hdrBg = CreateSolidBrush(RGB(18, 20, 28));
        RECT hdrR = { 0,0,w,MH_HDR }; FillRect(mem, &hdrR, hdrBg); DeleteObject(hdrBg);
        HPEN hdrLine = CreatePen(PS_SOLID, 2, ACCENT);
        SelectObject(mem, hdrLine); MoveToEx(mem, 0, MH_HDR - 1, NULL); LineTo(mem, w, MH_HDR - 1); DeleteObject(hdrLine);
        SelectObject(mem, g_mFB); SetTextColor(mem, ACCENT);
        TextOutA(mem, 12, 12, "COMPASSION", 10);
        SelectObject(mem, g_mFS); SetTextColor(mem, RGB(55, 60, 75));
        TextOutA(mem, MW - 90, 16, "v1.28.3", 7);

        // ── Sidebar nav items ──
        int sy = MH_HDR + 8;
        for (int si = 0; si < SIDEBAR_ITEMS; si++) {
            bool isHdr = g_sidebar[si].isHeader;
            int rh = isHdr ? SB_HDR_H : SB_ITEM_H;
            if (isHdr) {
                SelectObject(mem, g_mFS); SetTextColor(mem, RGB(60, 65, 80));
                TextOutA(mem, 14, sy + 8, g_sidebar[si].label, (int)strlen(g_sidebar[si].label));
            }
            else {
                bool active = (g_sidebar[si].page == g_menuPage);
                bool hov = (si == g_sidebarHover);
                if (active) {
                    HBRUSH aBr = CreateSolidBrush(RGB(24, 26, 36));
                    RECT aR = { 0, sy, SIDEBAR_W - 1, sy + rh }; FillRect(mem, &aR, aBr); DeleteObject(aBr);
                    HBRUSH accBr = CreateSolidBrush(ACCENT);
                    RECT accR = { 0, sy + 4, 3, sy + rh - 4 }; FillRect(mem, &accR, accBr); DeleteObject(accBr);
                }
                else if (hov) {
                    HBRUSH hBr = CreateSolidBrush(RGB(20, 22, 30));
                    RECT hR = { 0, sy, SIDEBAR_W - 1, sy + rh }; FillRect(mem, &hR, hBr); DeleteObject(hBr);
                }
                // Icon
                COLORREF ic = active ? ACCENT : RGB(80, 88, 110);
                if (g_sidebarIcons[si]) g_sidebarIcons[si](mem, 16, sy + (rh - 14) / 2, ic);
                // Label
                SelectObject(mem, g_mF);
                SetTextColor(mem, active ? TEXT_BRIGHT : (hov ? RGB(180, 185, 200) : TEXT_MED));
                TextOutA(mem, 36, sy + (rh - 15) / 2, g_sidebar[si].label, (int)strlen(g_sidebar[si].label));
            }
            sy += rh;
        }

        // ── Content area ──
        int CX = CONTENT_X + MH_PAD;
        int iy = MH_HDR;

        if (g_menuPage == 0 || g_menuPage == 1 || g_menuPage == 2) {
            // Toggle rows
            for (size_t i = 0; i < g_pageItems.size(); i++) {
                auto& it = g_pageItems[i]; bool on = it.toggle->load(); bool hovr = ((int)i == g_menuHover);
                if (hovr) { HBRUSH hv = CreateSolidBrush(RGB(20, 22, 32)); RECT hr2 = { CONTENT_X,iy,w,iy + MH_ROW }; FillRect(mem, &hr2, hv); DeleteObject(hv); }
                if (on) { HBRUSH ab2 = CreateSolidBrush(ACCENT); RECT ar = { CONTENT_X,iy + 4,CONTENT_X + 3,iy + MH_ROW - 4 }; FillRect(mem, &ar, ab2); DeleteObject(ab2); }
                SelectObject(mem, g_mF); SetTextColor(mem, on ? TEXT_BRIGHT : TEXT_DIM);
                TextOutA(mem, CX + 6, iy + 10, it.label.c_str(), (int)it.label.length());
                // Pill toggle — red ON
                int sx = w - 52, sy2 = iy + 9, sw2 = 34, sh2 = 18;
                COLORREF pillCol = on ? ACCENT : RGB(35, 38, 48);
                HBRUSH tb = CreateSolidBrush(pillCol);
                SelectObject(mem, tb); SelectObject(mem, GetStockObject(NULL_PEN));
                RoundRect(mem, sx, sy2, sx + sw2, sy2 + sh2, sh2, sh2); DeleteObject(tb);
                int kx = on ? (sx + sw2 - sh2 + 2) : (sx + 2);
                HBRUSH kb = CreateSolidBrush(RGB(240, 240, 240));
                SelectObject(mem, kb); Ellipse(mem, kx, sy2 + 2, kx + sh2 - 4, sy2 + sh2 - 2); DeleteObject(kb);
                // Separator
                HPEN sp2 = CreatePen(PS_SOLID, 1, RGB(24, 28, 36));
                SelectObject(mem, sp2); MoveToEx(mem, CX, iy + MH_ROW - 1, NULL); LineTo(mem, w - MH_PAD, iy + MH_ROW - 1); DeleteObject(sp2);
                iy += MH_ROW;
            }

            // ── COLOR SECTION (ESP page) ──
            if (g_menuPage == 0) {
                iy += 4;
                HPEN csep = CreatePen(PS_SOLID, 1, RGB(40, 30, 30));
                SelectObject(mem, csep); MoveToEx(mem, CX, iy, NULL); LineTo(mem, w - MH_PAD, iy); DeleteObject(csep);
                iy += 6;
                SelectObject(mem, g_mFS); SetTextColor(mem, ACCENT);
                TextOutA(mem, CX + 2, iy, "ESP COLORS", 10);
                iy += 16;
                ColorEntry colorItems[] = {
                    {"Player Box", &g_colPlayerBox},{"Zombie Box", &g_colZombieBox},
                    {"Snap Lines", &g_colSnapLine},{"Bones", &g_colBone},{"Head Dot", &g_colHeadDot}
                };
                for (int ci = 0; ci < 5; ci++) {
                    // Swatch
                    int swX = CX + 2, swY2 = iy + 2, swS = 14;
                    HBRUSH swBr = CreateSolidBrush(*colorItems[ci].color);
                    RECT swR = { swX, swY2, swX + swS, swY2 + swS }; FillRect(mem, &swR, swBr); DeleteObject(swBr);
                    HPEN swPen = CreatePen(PS_SOLID, 1, RGB(50, 55, 68));
                    SelectObject(mem, swPen); SelectObject(mem, GetStockObject(NULL_BRUSH));
                    Rectangle(mem, swX, swY2, swX + swS, swY2 + swS); DeleteObject(swPen);
                    SelectObject(mem, g_mF);
                    SetTextColor(mem, g_colorPickerOpen == ci ? TEXT_BRIGHT : TEXT_MED);
                    TextOutA(mem, CX + 22, iy + 1, colorItems[ci].label, (int)strlen(colorItems[ci].label));
                    iy += 22;
                    // Wheel (if open)
                    if (g_colorPickerOpen == ci && g_wheelBmp) {
                        int wcx = CONTENT_X + (w - CONTENT_X) / 2, wcy = iy + WHEEL_R + 4;
                        HDC wdc = CreateCompatibleDC(mem);
                        SelectObject(wdc, g_wheelBmp);
                        BitBlt(mem, wcx - WHEEL_R, wcy - WHEEL_R, WHEEL_R * 2, WHEEL_R * 2, wdc, 0, 0, SRCCOPY);
                        DeleteDC(wdc);
                        // Outer ring
                        HPEN rp = CreatePen(PS_SOLID, 1, RGB(50, 55, 68));
                        SelectObject(mem, rp); SelectObject(mem, GetStockObject(NULL_BRUSH));
                        Ellipse(mem, wcx - WHEEL_R, wcy - WHEEL_R, wcx + WHEEL_R, wcy + WHEEL_R); DeleteObject(rp);
                        // Indicator for current color
                        float ch2, cs2; RGBtoHS(*colorItems[ci].color, ch2, cs2);
                        float ang = (ch2 - 180.f) * 0.0174533f;
                        float rad = cs2 * (WHEEL_R - 2.f);
                        int ix2 = wcx + (int)(cosf(ang) * rad), iy2 = wcy + (int)(sinf(ang) * rad);
                        HPEN ip = CreatePen(PS_SOLID, 2, RGB(255, 255, 255));
                        SelectObject(mem, ip);
                        Ellipse(mem, ix2 - 5, iy2 - 5, ix2 + 5, iy2 + 5); DeleteObject(ip);
                        HPEN ip2 = CreatePen(PS_SOLID, 1, RGB(0, 0, 0));
                        SelectObject(mem, ip2);
                        Ellipse(mem, ix2 - 4, iy2 - 4, ix2 + 4, iy2 + 4); DeleteObject(ip2);
                        iy += WHEEL_R * 2 + 24;
                    }
                }
            }

            // ── BONE SELECTOR (Aimbot page) ──
            if (g_menuPage == 1) {
                HBRUSH bsBg = CreateSolidBrush(RGB(18, 22, 30));
                RECT bsR = { CONTENT_X,iy,w,iy + MH_ROW }; FillRect(mem, &bsR, bsBg); DeleteObject(bsBg);
                HBRUSH bsAcc = CreateSolidBrush(ACCENT);
                RECT bsAr = { CONTENT_X,iy + 4,CONTENT_X + 3,iy + MH_ROW - 4 }; FillRect(mem, &bsAr, bsAcc); DeleteObject(bsAcc);
                SelectObject(mem, g_mF); SetTextColor(mem, RGB(200, 205, 215));
                TextOutA(mem, CX + 6, iy + 10, "Aim Bone", 8);
                const char* boneName = g_boneChoiceNames[g_aimBoneChoice];
                char boneDisp[48]; snprintf(boneDisp, sizeof(boneDisp), "<  %s  >", boneName);
                SIZE bs2; GetTextExtentPoint32A(mem, boneDisp, (int)strlen(boneDisp), &bs2);
                SetTextColor(mem, ACCENT);
                TextOutA(mem, w - MH_PAD - 8 - bs2.cx, iy + 10, boneDisp, (int)strlen(boneDisp));
                HPEN bsSep = CreatePen(PS_SOLID, 1, RGB(24, 28, 36));
                SelectObject(mem, bsSep); MoveToEx(mem, CX, iy + MH_ROW - 1, NULL); LineTo(mem, w - MH_PAD, iy + MH_ROW - 1); DeleteObject(bsSep);
                iy += MH_ROW;
            }

            // ── SLIDERS (Aimbot page) ──
            if (g_menuPage == 1 && !g_aimSliders.empty()) {
                iy += 4;
                HPEN sep3 = CreatePen(PS_SOLID, 1, RGB(40, 30, 30));
                SelectObject(mem, sep3); MoveToEx(mem, CX, iy, NULL); LineTo(mem, w - MH_PAD, iy); DeleteObject(sep3);
                iy += 6;
                SelectObject(mem, g_mFS); SetTextColor(mem, ACCENT);
                TextOutA(mem, CX + 2, iy, "TUNING", 6);
                iy += 16;
                int sliderBase = MH_HDR + (int)g_pageItems.size() * MH_ROW + MH_ROW;
                for (int si = 0; si < (int)g_aimSliders.size(); si++) {
                    auto& sl = g_aimSliders[si];
                    int siy = sliderBase + SL_PAD + si * SL_H;
                    int tL = CONTENT_X + SL_PAD + 4, tR = w - SL_PAD - 4, tY = siy + SL_TRACK_Y;
                    SelectObject(mem, g_mF); SetTextColor(mem, RGB(190, 195, 210));
                    TextOutA(mem, CONTENT_X + SL_PAD + 4, siy + 4, sl.label, (int)strlen(sl.label));
                    char valBuf[32];
                    if (sl.decimals == 0)snprintf(valBuf, sizeof(valBuf), "%.0f %s", *sl.val, sl.unit);
                    else snprintf(valBuf, sizeof(valBuf), "%.1f %s", *sl.val, sl.unit);
                    SIZE vs2; GetTextExtentPoint32A(mem, valBuf, (int)strlen(valBuf), &vs2);
                    SetTextColor(mem, ACCENT);
                    TextOutA(mem, w - SL_PAD - 4 - vs2.cx, siy + 4, valBuf, (int)strlen(valBuf));
                    HBRUSH tbg = CreateSolidBrush(RGB(28, 32, 42));
                    RECT trk = { tL,tY - SL_TRACK_H / 2,tR,tY + SL_TRACK_H / 2 };
                    FillRect(mem, &trk, tbg); DeleteObject(tbg);
                    int knobX = ValToSliderX(*sl.val, tL, tR, sl);
                    HBRUSH tfill = CreateSolidBrush(ACCENT);
                    RECT frc = { tL,tY - SL_TRACK_H / 2,knobX,tY + SL_TRACK_H / 2 };
                    FillRect(mem, &frc, tfill); DeleteObject(tfill);
                    HBRUSH knob2 = CreateSolidBrush(RGB(240, 240, 240));
                    SelectObject(mem, knob2);
                    HPEN kp2 = CreatePen(PS_SOLID, 1, RGB(60, 60, 70)); SelectObject(mem, kp2);
                    Ellipse(mem, knobX - SL_KNOB_R, tY - SL_KNOB_R, knobX + SL_KNOB_R, tY + SL_KNOB_R);
                    DeleteObject(knob2); DeleteObject(kp2);
                    SelectObject(mem, g_mFS); SetTextColor(mem, RGB(50, 55, 65));
                    char minB[16], maxB[16];
                    if (sl.decimals == 0) { snprintf(minB, sizeof(minB), "%.0f", sl.lo); snprintf(maxB, sizeof(maxB), "%.0f", sl.hi); }
                    else { snprintf(minB, sizeof(minB), "%.1f", sl.lo); snprintf(maxB, sizeof(maxB), "%.1f", sl.hi); }
                    TextOutA(mem, tL, tY + SL_TRACK_H / 2 + 2, minB, (int)strlen(minB));
                    SIZE ms2; GetTextExtentPoint32A(mem, maxB, (int)strlen(maxB), &ms2);
                    TextOutA(mem, tR - ms2.cx, tY + SL_TRACK_H / 2 + 2, maxB, (int)strlen(maxB));
                }
            }
        }
        else {
            // Filter page
            iy += 10;
            SelectObject(mem, g_mF); SetTextColor(mem, RGB(160, 165, 180));
            TextOutA(mem, CX + 2, iy, "Item Filter (type to search):", 29);
            iy += 28;
            RECT sr = { CX,iy,w - MH_PAD,iy + 32 };
            HBRUSH sbx = CreateSolidBrush(RGB(20, 24, 32)); FillRect(mem, &sr, sbx); DeleteObject(sbx);
            HPEN sp3 = CreatePen(PS_SOLID, 1, g_espFilterText.empty() ? RGB(40, 45, 58) : ACCENT);
            SelectObject(mem, sp3); SelectObject(mem, GetStockObject(NULL_BRUSH));
            Rectangle(mem, sr.left, sr.top, sr.right, sr.bottom); DeleteObject(sp3);
            SelectObject(mem, g_mF);
            if (g_espFilterText.empty()) {
                SetTextColor(mem, RGB(55, 60, 75));
                TextOutA(mem, CX + 8, iy + 8, "Search items...", 15);
            }
            else {
                SetTextColor(mem, TEXT_BRIGHT);
                TextOutA(mem, CX + 8, iy + 8, g_espFilterText.c_str(), (int)g_espFilterText.length());
                if ((GetTickCount() / 500) % 2 == 0) {
                    SIZE cs2; GetTextExtentPoint32A(mem, g_espFilterText.c_str(), (int)g_espFilterText.length(), &cs2);
                    HPEN cp2 = CreatePen(PS_SOLID, 1, ACCENT); SelectObject(mem, cp2);
                    MoveToEx(mem, CX + 8 + cs2.cx + 2, iy + 6, NULL); LineTo(mem, CX + 8 + cs2.cx + 2, iy + 26); DeleteObject(cp2);
                }
            }
            iy += 40;
            SelectObject(mem, g_mFS); SetTextColor(mem, RGB(55, 60, 75));
            TextOutA(mem, CX + 2, iy, "ESC clear | Click to focus", 26);
            iy += 20;
            {
                std::lock_guard<std::mutex> lk(g_itemMtx);
                int numItems = (int)g_nearbyItems.size();
                int numSelected = (int)g_selectedItems.size();
                std::vector<FilterRow> rows = BuildFilterRows();
                int totalRows = (int)rows.size();
                char hdr2[80];
                if (numSelected > 0)snprintf(hdr2, sizeof(hdr2), "NEARBY (%d) - %d selected [CLEAR]", numItems, numSelected);
                else snprintf(hdr2, sizeof(hdr2), "NEARBY ITEMS (%d)", numItems);
                SelectObject(mem, g_mFS);
                SetTextColor(mem, numSelected > 0 ? ACCENT : RGB(70, 75, 90));
                TextOutA(mem, CX + 2, iy, hdr2, (int)strlen(hdr2));
                iy += 26;
                HPEN sep4 = CreatePen(PS_SOLID, 1, RGB(30, 34, 45));
                SelectObject(mem, sep4); MoveToEx(mem, CX, iy - 2, NULL); LineTo(mem, w - MH_PAD, iy - 2); DeleteObject(sep4);
                int maxScroll = totalRows > FILTER_MAX_VISIBLE ? totalRows - FILTER_MAX_VISIBLE : 0;
                if (g_filterScroll > maxScroll)g_filterScroll = maxScroll;
                int visCount = totalRows < FILTER_MAX_VISIBLE ? totalRows : FILTER_MAX_VISIBLE;
                for (int vi = 0; vi < visCount; vi++) {
                    int ri = vi + g_filterScroll; if (ri >= totalRows)break;
                    FilterRow& fr = rows[ri];
                    if (fr.isHeader) {
                        COLORREF catCol = g_catColors[fr.catIdx];
                        HBRUSH hdrBg2 = CreateSolidBrush(RGB(18, 22, 30));
                        RECT hdrR2 = { CONTENT_X, iy, w, iy + FILTER_ROW_H }; FillRect(mem, &hdrR2, hdrBg2); DeleteObject(hdrBg2);
                        HBRUSH accBr = CreateSolidBrush(catCol);
                        RECT accR = { CONTENT_X, iy + 3, CONTENT_X + 3, iy + FILTER_ROW_H - 3 }; FillRect(mem, &accR, accBr); DeleteObject(accBr);
                        SelectObject(mem, g_mFS); SetTextColor(mem, catCol);
                        TextOutA(mem, CX + 4, iy + 4, g_catNames[fr.catIdx], (int)strlen(g_catNames[fr.catIdx]));
                    }
                    else {
                        int ii = fr.itemIdx;
                        const std::string& name = g_nearbyItems[ii].name;
                        COLORREF catCol = g_catColors[fr.catIdx];
                        bool selected = g_selectedItems.count(name) > 0;
                        if (selected) {
                            HBRUSH rb = CreateSolidBrush(RGB(30, 14, 14));
                            RECT rr2 = { CONTENT_X, iy, w, iy + FILTER_ROW_H }; FillRect(mem, &rr2, rb); DeleteObject(rb);
                            HBRUSH ab3 = CreateSolidBrush(ACCENT);
                            RECT ar2 = { CONTENT_X, iy + 3, CONTENT_X + 3, iy + FILTER_ROW_H - 3 }; FillRect(mem, &ar2, ab3); DeleteObject(ab3);
                        }
                        HBRUSH dotBr = CreateSolidBrush(catCol);
                        SelectObject(mem, dotBr); SelectObject(mem, GetStockObject(NULL_PEN));
                        Ellipse(mem, CX + 4, iy + 8, CX + 10, iy + 14); DeleteObject(dotBr);
                        int cbx = CX + 14, cby = iy + 5, cbs = 12;
                        HPEN cbp = CreatePen(PS_SOLID, 1, selected ? ACCENT : RGB(45, 50, 62));
                        SelectObject(mem, cbp);
                        if (selected) {
                            HBRUSH cbf = CreateSolidBrush(ACCENT); SelectObject(mem, cbf);
                            Rectangle(mem, cbx, cby, cbx + cbs, cby + cbs); DeleteObject(cbf);
                            HPEN ckp = CreatePen(PS_SOLID, 2, RGB(240, 240, 240)); SelectObject(mem, ckp);
                            MoveToEx(mem, cbx + 2, cby + 6, NULL); LineTo(mem, cbx + 5, cby + 10); LineTo(mem, cbx + 10, cby + 2); DeleteObject(ckp);
                        }
                        else {
                            SelectObject(mem, GetStockObject(NULL_BRUSH));
                            Rectangle(mem, cbx, cby, cbx + cbs, cby + cbs);
                        }
                        DeleteObject(cbp);
                        SelectObject(mem, g_mF);
                        SetTextColor(mem, selected ? TEXT_BRIGHT : TEXT_MED);
                        TextOutA(mem, CX + 32, iy + 3, name.c_str(), (int)name.length());
                    }
                    HPEN rl = CreatePen(PS_SOLID, 1, RGB(22, 26, 34));
                    SelectObject(mem, rl); MoveToEx(mem, CX, iy + FILTER_ROW_H - 1, NULL);
                    LineTo(mem, w - MH_PAD, iy + FILTER_ROW_H - 1); DeleteObject(rl);
                    iy += FILTER_ROW_H;
                }
                if (totalRows > FILTER_MAX_VISIBLE) {
                    SelectObject(mem, g_mFS); SetTextColor(mem, RGB(50, 55, 65));
                    int endRow = g_filterScroll + FILTER_MAX_VISIBLE;
                    if (endRow > totalRows) endRow = totalRows;
                    char si2[48]; snprintf(si2, sizeof(si2), "scroll %d-%d of %d", g_filterScroll + 1, endRow, totalRows);
                    TextOutA(mem, CX + 2, iy + 2, si2, (int)strlen(si2));
                }
                if (numItems == 0 && g_showItems.load()) {
                    SelectObject(mem, g_mFS); SetTextColor(mem, RGB(55, 60, 75));
                    TextOutA(mem, CX + 2, iy + 4, "No items in range", 17);
                }
                else if (!g_showItems.load()) {
                    SelectObject(mem, g_mFS); SetTextColor(mem, RGB(55, 60, 75));
                    TextOutA(mem, CX + 2, iy + 4, "Enable Item ESP", 15);
                }
            }
        }

        // Footer
        int fy = h - MH_FTR;
        HBRUSH ftrBg = CreateSolidBrush(RGB(12, 14, 20));
        RECT ftrR = { 0,fy,w,h }; FillRect(mem, &ftrR, ftrBg); DeleteObject(ftrBg);
        HPEN ftrLine = CreatePen(PS_SOLID, 1, RGB(30, 34, 45));
        SelectObject(mem, ftrLine); MoveToEx(mem, 0, fy, NULL); LineTo(mem, w, fy); DeleteObject(ftrLine);
        SelectObject(mem, g_mFS); SetTextColor(mem, RGB(55, 60, 75));
        char sb[220]; snprintf(sb, sizeof(sb), "ESP:%s RAD:%s MB:%s AIM:%s RCL:%s SPD:%.0f BONE:%s HITS:%d %s",
            g_espEnabled.load() ? "ON" : "--", g_radarEnabled.load() ? "ON" : "--",
            g_silentAim.load() ? "ON" : "--", g_mouseAim.load() ? "ON" : "--",
            g_noRecoil.load() ? "ON" : "--", g_bulletSpeed, g_boneChoiceNames[g_aimBoneChoice], g_totalHits,
            g_taggedTarget ? "TAG" : "");
        TextOutA(mem, 10, fy + 8, sb, (int)strlen(sb));

        BitBlt(hdc, 0, 0, w, h, mem, 0, 0, SRCCOPY);
        SelectObject(mem, ob); DeleteObject(mb); DeleteDC(mem);
        EndPaint(hwnd, &ps); return 0;
    }
    case WM_DESTROY:UnregisterHotKey(hwnd, 100);
        if (g_mF) { DeleteObject(g_mF); g_mF = nullptr; }
        if (g_mFB) { DeleteObject(g_mFB); g_mFB = nullptr; }
        if (g_mFS) { DeleteObject(g_mFS); g_mFS = nullptr; }return 0;
    }return DefWindowProc(hwnd, msg, wParam, lParam);
}

void ShowMenu(int dayzid) {
    g_dayzid_menu = dayzid;
    g_menuPage = 0;
    BuildPageItems();

    static ATOM mc2 = 0; HINSTANCE hi = GetModuleHandle(nullptr);
    if (!mc2) {
        WNDCLASSEXA wc = {}; wc.cbSize = sizeof(wc); wc.lpfnWndProc = MenuWndProc; wc.hInstance = hi;
        wc.hCursor = LoadCursor(nullptr, IDC_ARROW); wc.lpszClassName = "CompassionV4"; mc2 = RegisterClassExA(&wc); if (!mc2)return;
    }
    g_mF = CreateFontA(15, 0, 0, 0, FW_NORMAL, 0, 0, 0, ANSI_CHARSET, 0, 0, CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, "Segoe UI");
    g_mFB = CreateFontA(17, 0, 0, 0, FW_BOLD, 0, 0, 0, ANSI_CHARSET, 0, 0, CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, "Segoe UI Semibold");
    g_mFS = CreateFontA(11, 0, 0, 0, FW_NORMAL, 0, 0, 0, ANSI_CHARSET, 0, 0, CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, "Segoe UI");

    g_menuHwnd = CreateWindowExA(WS_EX_TOPMOST | WS_EX_LAYERED, "CompassionV4", "", WS_POPUP,
        40, 40, MW, MHeight(), nullptr, nullptr, hi, nullptr);
    if (!g_menuHwnd)return;
    SetLayeredWindowAttributes(g_menuHwnd, 0, 235, LWA_ALPHA);
    ShowWindow(g_menuHwnd, SW_SHOW); UpdateWindow(g_menuHwnd);

    MSG msg{};
    while (true) {
        while (PeekMessageA(&msg, nullptr, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_QUIT)goto ex;
            TranslateMessage(&msg); DispatchMessageA(&msg);
        }
        if (g_menuHwnd && g_menuVisible)InvalidateRect(g_menuHwnd, nullptr, FALSE); Sleep(33);
    }
ex: StopAll(); if (g_menuHwnd) { DestroyWindow(g_menuHwnd); g_menuHwnd = nullptr; }
}

// ═══════════════════════════════════════════════════════════════
//  MAIN
// ═══════════════════════════════════════════════════════════════
int main()
{
    printf("[!] Starting...\n");
    system("color 5");

    if (OpenSharedMemory())printf("[!] SharedMemory OK\n");
    else { printf("SharedMemory FAIL\n"); clean(); ExitSystemThread(); return 1; }
    if (OpenNamedEvents())printf("[!] NamedEvents OK\n");
    else { printf("NamedEvents FAIL\n"); clean(); ExitSystemThread(); return 1; }
    if (base = GetBaseAddr(process_name))printf("[!] Base: %p\n", (void*)base);
    else { printf("Base FAIL\n"); clean(); ExitSystemThread(); return 1; }

    int dayzid = 0;
    printf("Enter DayZ Process ID: ");
    std::cin >> dayzid; processId1 = dayzid;

    printf("\n");
    printf("  +==========================================+\n");
    printf("  |      COMPASSION SUITE v1.28.2            |\n");
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
    printf("[!] Exiting.\n"); return 0;
}