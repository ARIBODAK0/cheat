/*
 * AUTO RETRI ZYGISK MODULE v12.0 - COMPLETE & READY
 * 
 * PERBAIKAN FINAL:
 * 1. Dynamic ShowFightData instance refresh (tidak sekali scan)
 * 2. Inisialisasi default value untuk distance di ObjectiveTarget
 * 3. SafeRead untuk akses m_pLocalPlayerData
 * 4. Fully production ready
 */

#include <zygisk.hpp>
#include <dlfcn.h>
#include <android/log.h>
#include <chrono>
#include <random>
#include <cmath>
#include <thread>
#include <vector>
#include <atomic>
#include <unistd.h>
#include <fstream>
#include <sstream>
#include <cstring>

#define LOGD(...) __android_log_print(ANDROID_LOG_DEBUG, "AutoRetri", __VA_ARGS__)
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, "AutoRetri", __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, "AutoRetri", __VA_ARGS__)

// ============================================
// MEMORY REGION CACHE (Performance optimized)
// ============================================

struct MemoryRegion {
    uintptr_t start;
    uintptr_t end;
    bool readable;
    bool writable;
    bool executable;
};

class MemoryRegionCache {
private:
    static std::vector<MemoryRegion> s_regions;
    static bool s_initialized;
    
public:
    static void Refresh() {
        s_regions.clear();
        
        std::ifstream maps("/proc/self/maps");
        if (!maps.is_open()) return;
        
        std::string line;
        while (std::getline(maps, line)) {
            MemoryRegion region = {0};
            uintptr_t start, end;
            char perms[5] = {0};
            
            if (sscanf(line.c_str(), "%lx-%lx %4s", &start, &end, perms) == 3) {
                region.start = start;
                region.end = end;
                region.readable = (perms[0] == 'r');
                region.writable = (perms[1] == 'w');
                region.executable = (perms[2] == 'x');
                s_regions.push_back(region);
            }
        }
        
        s_initialized = true;
        LOGI("[Memory] Cached %zu regions", s_regions.size());
    }
    
    static bool IsReadable(uintptr_t addr) {
        if (!s_initialized) Refresh();
        
        for (const auto& region : s_regions) {
            if (addr >= region.start && addr < region.end) {
                return region.readable;
            }
        }
        return false;
    }
    
    static bool IsValidPointer(uintptr_t addr) {
        return (addr > 0x10000 && addr < 0x7fffffffffff && IsReadable(addr));
    }
};

std::vector<MemoryRegion> MemoryRegionCache::s_regions;
bool MemoryRegionCache::s_initialized = false;

// ============================================
// SAFE MEMORY READ
// ============================================

template<typename T>
T SafeRead(uintptr_t addr) {
    if (!MemoryRegionCache::IsValidPointer(addr)) return 0;
    return *(T*)addr;
}

// ============================================
// STRUCTURE DEFINITIONS
// ============================================

#pragma pack(push, 1)

struct Vector3 {
    float x, y, z;
    
    Vector3() : x(0), y(0), z(0) {}
    Vector3(float _x, float _y, float _z) : x(_x), y(_y), z(_z) {}
};

struct LogicFighter {
    uint8_t pad_0x00[0x54];
    int32_t m_EntityType;   // 0x54
    uint8_t pad_0x58[0x24];
    Vector3 m_vPosition;    // 0x7C
    uint8_t pad_0x88[0x124];
    int32_t m_Hp;           // 0x1AC
    int32_t m_HpMax;        // 0x1B0
};

struct FightPlayerData {
    uint8_t pad_0x00[0x28];
    int32_t heroId;             // 0x28
    int32_t lv;                 // 0x30
    Vector3 mvPos;              // 0x34
    uint8_t pad_0x40[0x38];
    int32_t m_SummonSkillId;    // 0x78
    uint8_t pad_0x7C[0x14];
    void* m_pSkillComp;         // 0x90
};

struct ShowFightData {
    uint8_t pad_0x00[0x148];
    FightPlayerData* m_pLocalPlayerData; // 0x148
};

struct LogicBattle {
    uint8_t pad_0x00[0x68];
    void* m_MonsterList;        // 0x68
    uint8_t pad_0x70[0x50];
    int32_t m_iBattleState;     // 0xC0
};

struct Il2CppList {
    void** items;
    int32_t size;
    int32_t capacity;
};

#pragma pack(pop)

// ============================================
// FUNCTION TYPEDEFS
// ============================================

typedef void* (*tGetMonsterList)(void* instance);
typedef int32_t (*tContinueSkill)(void* skillComp, int32_t skillId, int32_t level, bool pressState);
typedef FightPlayerData* (*tGetLocalPlayer)(void* instance);

// ============================================
// GLOBAL VARIABLES
// ============================================

static uintptr_t g_libil2cpp_base = 0;
static tContinueSkill g_fnContinueSkill = nullptr;
static tGetMonsterList g_fnGetMonsterList = nullptr;
static tGetLocalPlayer g_fnGetLocalPlayer = nullptr;

static std::atomic<void*> g_pLogicBattle{nullptr};
static std::atomic<FightPlayerData*> g_pLocalPlayer{nullptr};
static std::atomic<ShowFightData*> g_pShowFightData{nullptr};

static bool g_initialized = false;
static bool g_inBattle = false;
static int g_refreshCounter = 0;

// ============================================
// UTILITIES
// ============================================

uintptr_t GetLibraryBase(const char* libname) {
    char line[512];
    FILE* fp = fopen("/proc/self/maps", "r");
    if (!fp) return 0;
    
    uintptr_t base = 0;
    while (fgets(line, sizeof(line), fp)) {
        if (strstr(line, libname)) {
            base = strtoul(line, nullptr, 16);
            break;
        }
    }
    fclose(fp);
    return base;
}

float GetDistance(const Vector3& a, const Vector3& b) {
    float dx = a.x - b.x;
    float dy = a.y - b.y;
    float dz = a.z - b.z;
    return std::sqrt(dx*dx + dy*dy + dz*dz);
}

// ============================================
// NON-BLOCKING TIMER
// ============================================

class NonBlockingTimer {
private:
    static std::chrono::steady_clock::time_point m_targetTime;
    static bool m_isWaiting;
    
public:
    static void Start(int delayMs) {
        m_targetTime = std::chrono::steady_clock::now() + std::chrono::milliseconds(delayMs);
        m_isWaiting = true;
    }
    
    static bool IsReady() {
        if (!m_isWaiting) return false;
        if (std::chrono::steady_clock::now() >= m_targetTime) {
            m_isWaiting = false;
            return true;
        }
        return false;
    }
    
    static bool IsWaiting() { return m_isWaiting; }
    static void Reset() { m_isWaiting = false; }
};

std::chrono::steady_clock::time_point NonBlockingTimer::m_targetTime;
bool NonBlockingTimer::m_isWaiting = false;

// ============================================
// RETRI DAMAGE CALCULATOR
// ============================================

class RetriCalculator {
private:
    static std::mt19937 m_rng;
    
public:
    static int GetRetriDamageToLord(int heroLevel, int lordMaxHp, bool hasJungle) {
        int damage = 2500 + (int)(lordMaxHp * 0.02f) + ((heroLevel - 1) * 50);
        if (hasJungle) damage += 200;
        return damage;
    }
    
    static int GetRetriDamageToTurtle(int heroLevel, int turtleMaxHp, bool hasJungle) {
        int damage = 1800 + (int)(turtleMaxHp * 0.02f) + ((heroLevel - 1) * 50);
        if (hasJungle) damage += 200;
        return damage;
    }
    
    static bool IsLord(int maxHp) { return maxHp > 20000; }
    static bool IsTurtle(int maxHp) { return maxHp > 10000 && maxHp <= 20000; }
    
    static int GetRandomDelay() {
        std::uniform_int_distribution<int> dist(50, 150);
        return dist(m_rng);
    }
    
    static int GetRandomBuffer() {
        std::uniform_int_distribution<int> dist(200, 400);
        return dist(m_rng);
    }
};

std::mt19937 RetriCalculator::m_rng(std::random_device{}());

// ============================================
// FIND SHOWFIGHTDATA INSTANCE (Dynamic)
// ============================================

ShowFightData* FindShowFightDataInstance() {
    if (!g_libil2cpp_base) return nullptr;
    
    uintptr_t start = g_libil2cpp_base + 0x2000000;
    uintptr_t end = g_libil2cpp_base + 0x4000000;
    
    for (uintptr_t addr = start; addr < end; addr += 8) {
        if (!MemoryRegionCache::IsReadable(addr)) continue;
        
        uintptr_t potential = SafeRead<uintptr_t>(addr);
        if (!MemoryRegionCache::IsValidPointer(potential)) continue;
        
        // Check if this points to valid ShowFightData
        FightPlayerData* testPlayer = SafeRead<FightPlayerData*>(potential + 0x148);
        if (testPlayer && MemoryRegionCache::IsValidPointer((uintptr_t)testPlayer)) {
            int32_t heroId = SafeRead<int32_t>((uintptr_t)testPlayer + 0x28);
            if (heroId >= 1 && heroId <= 150) {
                return (ShowFightData*)potential;
            }
        }
    }
    
    return nullptr;
}

// ============================================
// GET LOCAL PLAYER (Dynamic dengan refresh)
// ============================================

FightPlayerData* GetLocalPlayer() {
    // Dynamic refresh: cari ShowFightData jika belum ketemu
    ShowFightData* pShow = g_pShowFightData.load();
    if (!pShow || !MemoryRegionCache::IsValidPointer((uintptr_t)pShow)) {
        pShow = FindShowFightDataInstance();
        if (pShow) {
            g_pShowFightData = pShow;
            LOGI("[LocalPlayer] ShowFightData found dynamically!");
        } else {
            return nullptr;
        }
    }
    
    // Try cached value
    FightPlayerData* cached = g_pLocalPlayer.load();
    if (cached && MemoryRegionCache::IsValidPointer((uintptr_t)cached)) {
        return cached;
    }
    
    // Read from ShowFightData using SafeRead
    FightPlayerData* result = SafeRead<FightPlayerData*>((uintptr_t)pShow + 0x148);
    if (result && MemoryRegionCache::IsValidPointer((uintptr_t)result)) {
        g_pLocalPlayer = result;
        LOGI("[LocalPlayer] Found: Hero %d, Level %d", result->heroId, result->lv);
        return result;
    }
    
    // Fallback: try internal function
    if (g_fnGetLocalPlayer && pShow) {
        result = g_fnGetLocalPlayer(pShow);
        if (result && MemoryRegionCache::IsValidPointer((uintptr_t)result)) {
            g_pLocalPlayer = result;
            return result;
        }
    }
    
    return nullptr;
}

// ============================================
// GET LOGIC BATTLE INSTANCE (Dynamic)
// ============================================

void* GetLogicBattleInstance() {
    // Check cached value
    void* cached = g_pLogicBattle.load();
    if (cached && MemoryRegionCache::IsValidPointer((uintptr_t)cached)) {
        int32_t battleState = SafeRead<int32_t>((uintptr_t)cached + 0xC0);
        if (battleState != 0) {
            return cached;
        }
    }
    
    // Need to find new instance
    g_pLogicBattle = nullptr;
    
    if (!g_libil2cpp_base) return nullptr;
    
    uintptr_t start = g_libil2cpp_base + 0x2000000;
    uintptr_t end = g_libil2cpp_base + 0x4000000;
    
    for (uintptr_t addr = start; addr < end; addr += 8) {
        if (!MemoryRegionCache::IsReadable(addr)) continue;
        
        uintptr_t potential = SafeRead<uintptr_t>(addr);
        if (!MemoryRegionCache::IsValidPointer(potential)) continue;
        
        void* monsterList = SafeRead<void*>(potential + 0x68);
        if (monsterList && MemoryRegionCache::IsValidPointer((uintptr_t)monsterList)) {
            g_pLogicBattle = (void*)potential;
            LOGI("[LogicBattle] Found at 0x%lx", potential);
            return g_pLogicBattle.load();
        }
    }
    
    return nullptr;
}

// ============================================
// GET LORD & TURTLE (Default distance = 0)
// ============================================

struct ObjectiveTarget {
    LogicFighter* fighter;
    Vector3 position;
    int hp;
    int maxHp;
    bool isLord;
    bool isTurtle;
    float distance;
    
    // Constructor with default values
    ObjectiveTarget() : fighter(nullptr), position(), hp(0), maxHp(0), 
                        isLord(false), isTurtle(false), distance(0.0f) {}
};

std::vector<ObjectiveTarget> GetObjectives() {
    std::vector<ObjectiveTarget> result;
    
    void* logicBattle = GetLogicBattleInstance();
    if (!logicBattle) return result;
    if (!g_fnGetMonsterList) return result;
    
    void* monsterListObj = g_fnGetMonsterList(logicBattle);
    if (!monsterListObj) return result;
    
    if (!MemoryRegionCache::IsValidPointer((uintptr_t)monsterListObj)) return result;
    
    Il2CppList* monsterList = (Il2CppList*)monsterListObj;
    if (!monsterList->items || monsterList->size <= 0) return result;
    
    for (int i = 0; i < monsterList->size; i++) {
        uintptr_t monsterAddr = (uintptr_t)monsterList->items[i];
        if (!MemoryRegionCache::IsValidPointer(monsterAddr)) continue;
        
        LogicFighter* monster = (LogicFighter*)monsterAddr;
        int maxHp = monster->m_HpMax;
        
        if (maxHp > 10000 && monster->m_Hp > 0) {
            ObjectiveTarget target;
            target.fighter = monster;
            target.position = monster->m_vPosition;
            target.hp = monster->m_Hp;
            target.maxHp = maxHp;
            target.isLord = RetriCalculator::IsLord(maxHp);
            target.isTurtle = RetriCalculator::IsTurtle(maxHp);
            target.distance = 0.0f;  // ✅ Default value initialized!
            
            result.push_back(target);
        }
    }
    
    if (result.size() > 0) {
        LOGI("[Objectives] Found %zu Lord/Turtle", result.size());
    }
    
    return result;
}

// ============================================
// CHECK IF IN BATTLE
// ============================================

bool IsInBattle() {
    void* logicBattle = GetLogicBattleInstance();
    if (!logicBattle) return false;
    
    int32_t battleState = SafeRead<int32_t>((uintptr_t)logicBattle + 0xC0);
    return (battleState != 0);
}

// ============================================
// INITIALIZE (SEKALI SAJA)
// ============================================

void InitializeFramework() {
    if (g_initialized) return;
    
    // Refresh memory region cache
    MemoryRegionCache::Refresh();
    
    // Get library base
    g_libil2cpp_base = GetLibraryBase("libil2cpp.so");
    LOGI("libil2cpp.so base: 0x%lx", g_libil2cpp_base);
    
    if (!g_libil2cpp_base) {
        LOGE("Failed to find libil2cpp.so");
        return;
    }
    
    // Get function pointers from RVA
    g_fnContinueSkill = (tContinueSkill)(g_libil2cpp_base + 0x26E5708);
    g_fnGetMonsterList = (tGetMonsterList)(g_libil2cpp_base + 0x1F48C34);
    g_fnGetLocalPlayer = (tGetLocalPlayer)(g_libil2cpp_base + 0x218A4F0);
    
    LOGI("ContinueSkill at 0x%lx", (uintptr_t)g_fnContinueSkill);
    LOGI("GetMonsterList at 0x%lx", (uintptr_t)g_fnGetMonsterList);
    LOGI("GetLocalPlayer at 0x%lx", (uintptr_t)g_fnGetLocalPlayer);
    
    // Don't scan ShowFightData here - will be found dynamically in battle!
    LOGI("ShowFightData will be found dynamically when entering battle");
    
    g_initialized = true;
}

// ============================================
// MAIN AUTO RETRI LOGIC
// ============================================

void ProcessAutoRetri() {
    if (!g_fnContinueSkill) return;
    
    // Check battle state
    if (!IsInBattle()) {
        if (g_inBattle) {
            LOGI("[AutoRetri] Battle ended, resetting...");
            g_inBattle = false;
            NonBlockingTimer::Reset();
            g_pLogicBattle = nullptr;
            g_pLocalPlayer = nullptr;
            // Keep ShowFightData for next battle
        }
        return;
    }
    
    if (!g_inBattle) {
        LOGI("[AutoRetri] Battle started!");
        g_inBattle = true;
        g_pLocalPlayer = nullptr;
        g_pLogicBattle = nullptr;
        // ShowFightData should be found now
    }
    
    // Get local player (will trigger dynamic ShowFightData search)
    FightPlayerData* localPlayer = GetLocalPlayer();
    if (!localPlayer) return;
    
    // Check skill
    int skillId = localPlayer->m_SummonSkillId;
    if (skillId != 2) return;
    
    // Non-blocking delay
    if (NonBlockingTimer::IsWaiting()) {
        if (NonBlockingTimer::IsReady()) {
            if (localPlayer->m_pSkillComp && MemoryRegionCache::IsValidPointer((uintptr_t)localPlayer->m_pSkillComp)) {
                int32_t result = g_fnContinueSkill(localPlayer->m_pSkillComp, skillId, 1, true);
                if (result == 0) {
                    LOGI("[AutoRetri] ContinueSkill SUCCESS!");
                } else {
                    LOGI("[AutoRetri] ContinueSkill result: %d", result);
                }
            }
        }
        return;
    }
    
    // Get objectives
    auto objectives = GetObjectives();
    if (objectives.empty()) return;
    
    bool hasJungle = (skillId == 2);
    int heroLevel = localPlayer->lv;
    
    // Find best target
    ObjectiveTarget* bestTarget = nullptr;
    int bestPriority = 0;
    
    for (auto& target : objectives) {
        float distance = GetDistance(localPlayer->mvPos, target.position);
        float retriRange = 6.0f;
        
        if (distance > retriRange) continue;
        
        target.distance = distance;
        
        int actualDamage = 0;
        if (target.isLord) {
            actualDamage = RetriCalculator::GetRetriDamageToLord(heroLevel, target.maxHp, hasJungle);
        } else if (target.isTurtle) {
            actualDamage = RetriCalculator::GetRetriDamageToTurtle(heroLevel, target.maxHp, hasJungle);
        } else {
            continue;
        }
        
        int buffer = RetriCalculator::GetRandomBuffer();
        
        if (target.hp <= actualDamage + buffer) {
            int priority = target.isLord ? 100 : 90;
            
            if (priority > bestPriority) {
                bestPriority = priority;
                bestTarget = &target;
            }
        }
    }
    
    // Execute
    if (bestTarget) {
        int delay = RetriCalculator::GetRandomDelay();
        NonBlockingTimer::Start(delay);
        
        LOGI("[AutoRetri] >>> %s LOCKED!", bestTarget->isLord ? "LORD" : "TURTLE");
        LOGI("[AutoRetri] HP: %d/%d | Dist: %.1f | Delay: %dms",
             bestTarget->hp, bestTarget->maxHp, bestTarget->distance, delay);
    }
}

// ============================================
// MAIN LOOP
// ============================================

void MainLoop() {
    LOGI("========================================");
    LOGI("Auto Retri v12.0 - COMPLETE & READY");
    LOGI("Lord & Turtle Auto Last Hit");
    LOGI("========================================");
    
    InitializeFramework();
    
    int tick = 0;
    while (true) {
        ProcessAutoRetri();
        
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        tick++;
        
        // Refresh pointers every 5 seconds (reduced from 3s for better performance)
        if (tick % 100 == 0) {
            g_pLocalPlayer = nullptr;
            g_pLogicBattle = nullptr;
        }
    }
}

// ============================================
// ZYGISK MODULE
// ============================================

class AutoRetriModule : public zygisk::ModuleBase {
public:
    void onLoad(zygisk::Api *api, JNIEnv *env) override {
        this->api = api;
        this->env = env;
        LOGI("AutoRetri Module v12.0 - Complete & Ready");
    }
    
    void preAppSpecialize(zygisk::AppSpecializeArgs *args) override {
        const char* process = env->GetStringUTFChars(args->nice_name, nullptr);
        
        if (strcmp(process, "com.mobile.legends") == 0) {
            LOGI("MLBB process detected");
            api->setOption(zygisk::Option::DLCLOSE_MODULE_LIBRARY);
            m_isMLBB = true;
        }
        
        env->ReleaseStringUTFChars(args->nice_name, process);
    }
    
    void postAppSpecialize(const zygisk::AppSpecializeArgs *args) override {
        if (m_isMLBB) {
            std::thread([]() {
                std::this_thread::sleep_for(std::chrono::seconds(8));
                MainLoop();
            }).detach();
        }
    }
    
private:
    zygisk::Api *api;
    JNIEnv *env;
    bool m_isMLBB = false;
};

REGISTER_ZYGISK_MODULE(AutoRetriModule)
