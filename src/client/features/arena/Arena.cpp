// Ground battles: /arena <enemies> <place> <waves> (the host; or alone, offline).
//
//   /arena yakuza random 5      /arena баллас притон 3      /arena list      /arena stop      /skip
//
// Every player is taken into the place: an interior of the story's missions (Big Smoke's crack palace, Madd Dogg's
// mansion, the casinos, the meat factory, the stadiums...) or a random one of them. Waves of the chosen gang or force
// come at them there (more each wave, with better guns, aim and armour); a wave is beaten when every one of it is
// down, and the next comes 6 s later.
// They come in squads of 3-4 from all over the place, away from the players and from where the last wave came (the
// host's game works out the place's floor first), some going round to the players' backs. Each squad has a grenadier
// who throws when the arc to the player is clear; knifemen come alone and steal up to a back out of the players' sight
// (StealthKnife kills from there); the last two waves bring snipers who shoot from far off. They know no fear and
// always know where the players are: out of sight they come along the floor, in sight they fight, take cover, wait
// and push on all together.
// The last wave brings a boss: Tenpenny, Pulaski, Hernandez, Big Smoke, Ryder, OG Loc, T-Bone, Jizzy or Toreno (one
// that fits the enemies, when there is one) with an AK-47 (a gang's) or an M4 (the state's), and with him, once and
// unannounced, his men: they say nothing and work round the players. All waves beaten: everyone gets $1000 a wave
// and is taken back where it stood. Everyone dead or out of the place: lost. /arena stop ends it. The place's doors
// out are shut meanwhile, and the armour of the battle is gone after it.
// No wanted stars meanwhile, and the weapons the players took up there they do not keep (game::challenge); the enemies
// drop none.
//
// The host's game makes the enemies (they come to the others as its peds do); it counts them by its own handles and
// tells the others how many are left. The boss is one of the game's special characters (slot 10, model 299): every
// game loads the same one into that slot first, so that its copy has the right look there too (G_ARENA).
#include <windows.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <map>
#include <queue>
#include <random>
#include <set>
#include <string>
#include <vector>

#include "common/api/protocol/Protocol.h"
#include "common/lib/log/Log.h"
#include "common/lib/util/Util.h"
#include "entities/ped/Peds.h"
#include "entities/player/Players.h"
#include "entities/radar-icons/RadarIcons.h"
#include "entities/stealth-knife/StealthKnife.h"
#include "shared/api/net-session/Session.h"
#include "shared/config/Config.h"
#include "shared/game/Challenge.h"
#include "shared/game/Game.h"
#include "shared/game/Script.h"
#include "shared/lib/core/Core.h"
#include "shared/ui/chat/Chat.h"
#include "shared/ui/marks/Marks.h"
#include "shared/ui/renderer/Renderer.h"
#include "shared/ui/widgets/Widgets.h"

namespace coop {

namespace {
using namespace game;
using game::script::Cmd;

constexpr int PED_TYPE_MISSION1 = 24;
constexpr int kBossSlot = 10, kBossModel = 290 + kBossSlot - 1;  // the special characters' models: 290-299
constexpr int kKnifeSlot = 9, kKnifeSpecial = 290 + kKnifeSlot - 1;  // the police's and SWAT's knifeman (Cerberus)
int g_knifeModel = -1;  // that special character, when the game has it
std::mt19937 g_rng{(uint32_t)GetTickCount() ^ 0xA2E7u};
int RandInt(int a, int b) { return a + (int)(g_rng() % (uint32_t)(b - a + 1)); }
float Rand(float a, float b) { return a + (b - a) * (float)(g_rng() % 10001) / 10000.0f; }

// ---------------------------------------------------------------- what can be chosen
struct Faction {
    const char* keys[2][2];  // what may be typed (lower case): in Russian, in English
    const char* name[2];
    int models[4];
    int weapons[4];  // by wave: the first ones get the first, later ones better
    int boss[3];     // the bosses that fit (index into kBosses, -1: none)
};
// weapons: 22 pistol, 24 desert eagle, 25 shotgun, 27 combat shotgun, 28 uzi, 29 mp5, 30 ak-47, 31 m4, 32 tec-9
const Faction kFactions[] = {
    {{TRP("якудза", "yakuza"), TRP("дананг", "danang")}, TRP("Якудза", "Yakuza"), {121, 122, 123, 186}, {22, 28, 29, 30}, {7, -1, -1}},
    {{TRP("баллас", "ballas"), TRP("балласы", "ballas")}, TRP("Баллас", "Ballas"), {102, 103, 104, 102}, {22, 32, 25, 30}, {3, 4, 5}},
    {{TRP("вагос", "vagos"), TRP("вагос", "vagos")}, TRP("Вагос", "Vagos"), {108, 109, 110, 108}, {22, 32, 25, 30}, {3, 5, -1}},
    {{TRP("ацтекас", "aztecas"), TRP("ацтеки", "aztecas")}, TRP("Ацтекас", "Aztecas"), {114, 115, 116, 114}, {22, 28, 25, 30}, {6, -1, -1}},
    {{TRP("триады", "triads"), TRP("триада", "triads")}, TRP("Триады", "Triads"), {117, 118, 120, 117}, {22, 28, 29, 30}, {7, -1, -1}},
    {{TRP("мафия", "mafia"), TRP("мафия", "mafia")}, TRP("Мафия", "Mafia"), {111, 112, 113, 111}, {22, 29, 27, 30}, {8, 0, -1}},
    {{TRP("рифа", "rifa"), TRP("рифа", "rifa")}, TRP("Сан-Фиеро Рифа", "San Fierro Rifa"), {173, 174, 175, 173}, {22, 32, 25, 30}, {6, -1, -1}},
    {{TRP("байкеры", "bikers"), TRP("байкеры", "bikers")}, TRP("Байкеры", "Bikers"), {247, 248, 254, 100}, {22, 25, 30, 27}, {-1, -1, -1}},
    {{TRP("полиция", "police"), TRP("копы", "cops")}, TRP("Полиция", "Police"), {280, 281, 282, 284}, {22, 25, 29, 31}, {0, 1, 2}},
    {{TRP("спецназ", "swat"), TRP("фбр", "fbi")}, TRP("Спецназ", "SWAT"), {285, 286, 285, 286}, {29, 29, 31, 31}, {0, 1, 8}},
    {{TRP("армия", "army"), TRP("военные", "army")}, TRP("Армия", "Army"), {287, 287, 287, 287}, {29, 31, 31, 31}, {8, -1, -1}},
};

struct Boss {
    const char* special;  // the special character's name
    const char* name[2];
    int weapon;  // a gang's boss: the AK-47 (30), the state's: the M4 (31)
};
const Boss kBosses[] = {
    {"TENPEN", TRP("Тенпенни", "Tenpenny"), 31},           // 0
    {"PULASKI", TRP("Пуласки", "Pulaski"), 31},            // 1
    {"HERN", TRP("Эрнандес", "Hernandez"), 31},            // 2
    {"SMOKEV", TRP("Биг Смоук", "Big Smoke"), 30},         // 3
    {"RYDER", TRP("Райдер", "Ryder"), 30},                 // 4
    {"OGLOC", TRP("Оу Джи Лок", "OG Loc"), 30},            // 5
    {"TBONE", TRP("Ти-Бон Мендез", "T-Bone Mendez"), 30},  // 6
    {"JIZZY", TRP("Джиззи Би", "Jizzy B"), 30},            // 7
    {"TORENO", TRP("Майк Торено", "Mike Toreno"), 31},     // 8
};
constexpr int kBossCount = (int)(sizeof(kBosses) / sizeof(kBosses[0]));

struct Place {
    const char* keys[2][2];  // in Russian, in English (the second Russian word with the same English one)
    const char* name[2];
    int area;
    CVector pos;
    float reach;  // how far from the middle the enemies may come from
    bool big;     // a big place: 30 s between the waves (20 s before the first), more to pick up
    bool soda;    // with drinks machines (health there): less armour lying about
};
const Place kPlaces[] = {
    {{TRP("притон", "crack"), TRP("смоук", "crack")}, TRP("Крэк-дворец Биг Смоука", "Big Smoke's crack palace"), 2, {2543.46f, -1308.38f, 1026.73f}, 14.0f, false, false},
    {{TRP("особняк", "mansion"), TRP("мэддогг", "mansion")}, TRP("Особняк Мэдд Догга", "Madd Dogg's mansion"), 5, {1267.66f, -781.32f, 1091.91f}, 18.0f, true, false},
    {{TRP("мясокомбинат", "meat"), TRP("бойня", "meat")}, TRP("Мясокомбинат", "The meat factory"), 1, {963.42f, 2108.29f, 1011.03f}, 16.0f, true, false},
    {{TRP("мотель", "motel"), TRP("джефферсон", "motel")}, TRP("Мотель «Джефферсон»", "The Jefferson motel"), 15, {2215.45f, -1147.48f, 1025.80f}, 20.0f, true, true},
    {{TRP("атриум", "atrium"), TRP("атриум", "atrium")}, TRP("Атриум", "The atrium"), 18, {1710.43f, -1669.38f, 20.23f}, 14.0f, false, true},
    {{TRP("либерти", "liberty"), TRP("бистро", "liberty")}, TRP("Либерти-Сити (бистро Марко)", "Liberty City (Marco's bistro)"), 1, {-794.81f, 497.74f, 1376.20f}, 14.0f, false, false},
    {{TRP("полиция", "police"), TRP("участок", "police")}, TRP("Полицейский участок", "The police station"), 6, {246.78f, 63.90f, 1003.64f}, 14.0f, false, true},
    {{TRP("дамба", "dam"), TRP("шерман", "dam")}, TRP("Дамба Шерман", "The Sherman dam"), 17, {-959.56f, 1848.58f, 9.00f}, 30.0f, true, false},
    {{TRP("кикстарт", "kickstart"), TRP("кикстарт", "kickstart")}, TRP("Стадион «Кикстарт»", "The Kickstart stadium"), 14, {-1465.27f, 1557.87f, 1052.53f}, 35.0f, true, true},
};
constexpr int kPlaceCount = (int)(sizeof(kPlaces) / sizeof(kPlaces[0]));
constexpr int kFactionCount = (int)(sizeof(kFactions) / sizeof(kFactions[0]));

// lower case, the Cyrillic letters too (А-Я, Ё in UTF-8)
std::string LowerRu(const std::string& in) {
    std::string out;
    for (size_t i = 0; i < in.size(); ++i) {
        uint8_t c = (uint8_t)in[i];
        if (c == 0xD0 && i + 1 < in.size()) {
            uint8_t d = (uint8_t)in[i + 1];
            if (d >= 0x90 && d <= 0x9F) out += (char)0xD0, out += (char)(d + 0x20);        // А-П
            else if (d >= 0xA0 && d <= 0xAF) out += (char)0xD1, out += (char)(d - 0x20);   // Р-Я
            else if (d == 0x81) out += (char)0xD1, out += (char)0x91;                      // Ё
            else out += (char)c, out += (char)d;
            ++i;
        } else {
            out += (char)std::tolower(c);
        }
    }
    return out;
}
bool Matches(const std::string& word, const char* const (&keys)[2][2]) {
    for (auto& pair : keys)
        for (const char* k : pair)
            if (k && word == k) return true;
    return false;
}
bool Matches(const std::string& word, const char* const (&pair)[2]) { return word == pair[0] || word == pair[1]; }
// the commands' words
const char* const kWordList[2] = TRP("список", "list");
const char* const kWordStop[2] = TRP("стоп", "stop");
const char* const kWordRandom[2] = TRP("рандом", "random");
const char* const kWordAny[2] = TRP("случайно", "any");
const char* const kWordArena[2] = TRP("арена", "arena");
const char* const kWordSkip[2] = TRP("скип", "skip");
int Lang() { return Cfg().english ? 1 : 0; }

// ---------------------------------------------------------------- the battle
enum ArenaOp : uint8_t { A_START = 1, A_WAVE, A_LEFT, A_BOSS, A_END, A_ITEMS, A_PREP, A_MARK, A_SKIP };
struct Arena {
    bool on = false, host = false;
    int faction = 0, place = 0, waves = 5, wave = 0, left = 0, boss = -1;
    uint64_t startMs = 0, nextWaveMs = 0, waveMs = 0, outSince = 0, endAt = 0;
    int itemsFor = 0;  // host: the wave whose weapons and armour are to be laid out (once the floor is known)
    bool won = false;
    // where this player stood, to go back
    int backArea = 0;
    CVector back;
    float backHeading = 0;
    bool taken = false;
} g_arena;
std::set<CPed*> g_enemies;  // the host's: this wave's (and the boss)
CPed* g_bossPed = nullptr;

bool Hosting() { return !Net().Online() || Net().IsHost(); }

void Send(ArenaOp op, const std::function<void(Writer&)>& body = nullptr) {
    if (!Net().Online()) return;
    Writer w(proto::G_ARENA);
    w.U8(op);
    if (body) body(w);
    Net().Send(proto::CH_RELIABLE, w);
}

std::string FactionName(int f) { return kFactions[f].name[Lang()]; }
std::string PlaceName(int p) { return kPlaces[p].name[Lang()]; }
std::string BossName(int b) { return b >= 0 && b < kBossCount ? std::string(kBosses[b].name[Lang()]) : std::string(); }

void Teleport(int area, const CVector& p, float heading) {
    CPlayerPed* me = FindPlayerPed(0);
    if (!me) return;
    int h = GetPedRef(me);
    if (CVehicle* in = me->VehicleIfIn()) Cmd(0x0362).I(h).F(in->GetPosition().x).F(in->GetPosition().y).F(in->GetPosition().z + 1.0f).Run();  // WARP_CHAR_FROM_CAR_TO_COORD
    Cmd(0x04BB).I(area).Run();                         // SET_AREA_VISIBLE
    Cmd(0x0860).I(h).I(area).Run();                    // SET_CHAR_AREA_VISIBLE
    Cmd(0x03CB).F(p.x).F(p.y).F(p.z).Run();            // LOAD_SCENE: its ground first
    Cmd(0x00A1).I(h).F(p.x).F(p.y).F(p.z - 1.0f).Run();  // SET_CHAR_COORDINATES
    Cmd(0x0173).I(h).F(heading).Run();                 // SET_CHAR_HEADING
    LightArea(area, p);
}

// The place's doors out (its entrances' markers) are shut while the battle is on: SET_INTERIOR_ACCESS off by the
// names of the entrances in it (a name is both sides of a door), and every entrance's flags as they were afterwards.
std::vector<std::pair<int, uint16_t>> g_enexFlags;  // pool index, CEntryExit::m_nFlags
void ShutDoors(bool shut) {
    auto* pool = Ref<CPoolHeader*>(0x96A7D8);  // CEntryExitManager::mp_poolEntryExits, CEntryExit 0x3C bytes
    if (!pool) return;
    if (!shut) {
        for (auto& [i, flags] : g_enexFlags)
            if (i < pool->size && pool->byteMap[i] >= 0) Ref<uint16_t>((uintptr_t)pool->objects + (uintptr_t)i * 0x3C + 0x30) = flags;
        if (!g_enexFlags.empty()) LOGI("Arena: the place's doors open again");
        g_enexFlags.clear();
        return;
    }
    g_enexFlags.clear();
    const Place& pl = kPlaces[g_arena.place];
    std::set<std::string> names;
    for (int i = 0; i < pool->size; ++i) {
        if (pool->byteMap[i] < 0) continue;
        uintptr_t e = (uintptr_t)pool->objects + (uintptr_t)i * 0x3C;
        g_enexFlags.push_back({i, Ref<uint16_t>(e + 0x30)});
        if (Ref<int8_t>(e + 0x32) != pl.area || (DoorSpot((void*)e) - pl.pos).Length2D() > 150.0f) continue;  // m_nArea
        char name[9] = {};
        std::memcpy(name, (const void*)e, 8);  // CEntryExit::m_szName
        if (name[0]) names.insert(name);
    }
    for (const std::string& n : names) Cmd(0x07FB).S8(n.c_str()).I(0).Run();  // SET_INTERIOR_ACCESS: off
    LOGI("Arena: %d doors of the place shut", (int)names.size());
}

// The police's and the SWAT's knifeman is Cerberus (Manhunt) when the game has his model: cerberus.dff and .txd in
// gta3.img (or another archive the game reads, or modloader), loaded as a special character; without it, a man of the
// force as before
void LoadKnifeModel() {
    g_knifeModel = -1;
    if (kFactions[g_arena.faction].models[0] != 280 && kFactions[g_arena.faction].models[0] != 285) return;  // the police, SWAT
    // Not LOAD_SPECIAL_CHARACTER: its name is 8 bytes with no room for the end of the string, and "cerberus" is 8
    // letters long; the game read on past it and crashed. CStreaming::RequestSpecialModel with the name as it is,
    // once the archive's list of extra models has it.
    void* dir = Ref<void*>(0x8E48D0);  // CStreaming::ms_pExtraObjectsDir
    if (!dir || !CallMethod<0x532450, void*>(dir, "cerberus")) {  // CDirectory::FindItem
        LOGW("Arena: cerberus.dff is not among the game's extra models");
        return;
    }
    Call<0x409D10>(kKnifeSpecial, "cerberus", 2);  // CStreaming::RequestSpecialModel (STREAMING_GAME_REQUIRED)
    LoadAllRequestedModels(false);
    if (!IsModelLoaded(kKnifeSpecial)) {
        LOGW("Arena: Cerberus did not load");
        return;
    }
    // a model that is no character (no skin's bones: a conversion gone wrong) crashed the game as the ped was made
    void* info = Ref<void*>(0xA9B0C8 + (uintptr_t)kKnifeSpecial * 4);  // CModelInfo::ms_modelInfoPtrs
    void* clump = info ? Field<void*>(info, 0x1C) : nullptr;     // CBaseModelInfo::m_pRwObject
    if (!clump || !Call<0x734A40, void*>(clump)) {                // GetAnimHierarchyFromSkinClump
        LOGW("Arena: cerberus.dff has no character's skeleton: not used");
        Cmd(0x0296).I(kKnifeSlot).Run();  // UNLOAD_SPECIAL_CHARACTER
        return;
    }
    g_knifeModel = kKnifeSpecial;
    LOGI("Arena: the knifemen are Cerberus");
}

// into the place, a little apart from the others
void TakeIn() {
    CPlayerPed* me = FindPlayerPed(0);
    if (!me || g_arena.taken) return;
    g_arena.backArea = me->AreaCode();
    g_arena.back = me->VehicleIfIn() ? me->VehicleIfIn()->GetPosition() + CVector{0, 0, 1.0f} : me->GetPosition();
    g_arena.backHeading = me->CurrentRotation() * 57.29578f;
    const Place& pl = kPlaces[g_arena.place];
    float a = (float)Net().MyId() * 1.25f;
    CVector at = pl.pos + CVector{std::cos(a) * 1.2f, std::sin(a) * 1.2f, 0.0f};
    if (!Net().Online()) at = pl.pos;
    Teleport(pl.area, at, Rand(0.0f, 360.0f));
    g_arena.taken = true;
    ShutDoors(true);
}

void TakeBack() {
    CPlayerPed* me = FindPlayerPed(0);
    if (!g_arena.taken || !me) return;
    g_arena.taken = false;
    // dead and taken to hospital, or gone out on its own: it is somewhere else already
    if (me->Health() <= 0.0f || me->AreaCode() != kPlaces[g_arena.place].area) return;
    Teleport(g_arena.backArea, g_arena.back, g_arena.backHeading);
}

// ---------------------------------------------------------------- the place's floor
// Where people can stand and walk in the place, worked out once by the host as the arena begins (a few hundred cells a
// frame): from the middle, cell by cell (1.5 m apart, 2 m in a big place), a step to a neighbour is taken when there
// is floor there within a metre up or down of this one (a ray down), no wall between them at the knees and at the
// chest, and room over it for a body. What it reaches is the walkable place, its other rooms and floors too (stairs
// are steps): the enemies come from cells of it far from the players and spread round them, the weapons and armour
// lie on cells spread over it, the ones who got stuck are set one step on along it, and cover is looked for on it.
struct ColPoint {
    CVector point;
    uint8_t rest[0x2C - 12];
};
bool Ray(const CVector& a, const CVector& b, CVector& at) {  // the first wall or floor on the way (buildings, objects)
    ColPoint cp{};
    CEntity* hit = nullptr;
    bool struck = Call<0x56BA00, bool>(&a, &b, &cp, &hit, true, false, false, true, false, false, false, false);  // ProcessLineOfSight
    if (struck) at = cp.point;
    return struck;
}
bool Clear(const CVector& a, const CVector& b) {
    return Call<0x56A490, bool>(&a, &b, true, false, false, true, false, false, false);  // GetIsLineOfSightClear
}

struct Cell {
    CVector p;              // the floor
    std::vector<int> next;  // the cells a step away
    bool room = false;      // half a metre free all round at the waist: one can stand there (not against a wall)
};
std::vector<Cell> g_cells;
std::map<int64_t, int> g_cellAt;  // (column, row, storey) -> cell
std::vector<int> g_open;          // the cells whose neighbours are still to be tried
std::vector<int> g_roomy;         // the cells with room round them
float g_step = 1.5f;
int g_reachCells = 0;
bool g_gridDone = false;

int64_t CellKey(int i, int j, float z) { return ((int64_t)(i + 512) << 40) | ((int64_t)(j + 512) << 20) | (int64_t)((z + 2000.0f) / 2.2f); }

bool FloorBelow(float x, float y, float fromZ, float& z) {
    CVector at;
    if (!Ray({x, y, fromZ}, {x, y, fromZ - 3.5f}, at)) return false;
    z = at.z;
    return true;
}

bool Roomy(const CVector& floor) {
    CVector waist = floor + CVector{0, 0, 1.0f}, knee = floor + CVector{0, 0, 0.45f};
    const float r = 0.45f;
    for (const CVector& d : {CVector{r, 0, 0}, CVector{-r, 0, 0}, CVector{0, r, 0}, CVector{0, -r, 0}})
        if (!Clear(waist, waist + d) || !Clear(knee, knee + d)) return false;
    return true;
}

void StartGrid() {
    g_cells.clear();
    g_cellAt.clear();
    g_open.clear();
    g_roomy.clear();
    g_gridDone = false;
    const Place& pl = kPlaces[g_arena.place];
    g_step = pl.reach > 30.0f ? 2.0f : 1.5f;
    g_reachCells = (int)(pl.reach * 1.2f / g_step) + 1;
    float z = 0;
    if (!FloorBelow(pl.pos.x, pl.pos.y, pl.pos.z + 1.0f, z)) z = pl.pos.z - 1.0f;
    CVector start{pl.pos.x, pl.pos.y, z};
    g_cells.push_back({start, {}, Roomy(start)});
    g_cellAt[CellKey(0, 0, z)] = 0;
    g_open.push_back(0);
}

// A step to a neighbour square: its middle first, and when that does not go (a doorway narrower than the grid, a
// pillar on the way) four spots off the middle of the square; the cell keeps the spot that went.
void GridStep() {  // a part of it each frame
    const Place& pl = kPlaces[g_arena.place];
    const float o = g_step * 0.3f;
    const float offs[5][2] = {{0, 0}, {o, 0}, {-o, 0}, {0, o}, {0, -o}};
    for (int budget = 25; budget > 0 && !g_open.empty() && g_cells.size() < 3000; --budget) {
        int ci = g_open.back();
        g_open.pop_back();
        CVector c = g_cells[(size_t)ci].p;
        int i0 = (int)std::lround((c.x - pl.pos.x) / g_step), j0 = (int)std::lround((c.y - pl.pos.y) / g_step);
        for (int di = -1; di <= 1; ++di)
            for (int dj = -1; dj <= 1; ++dj) {
                if (!di && !dj) continue;
                int i = i0 + di, j = j0 + dj;
                if (std::abs(i) > g_reachCells || std::abs(j) > g_reachCells) continue;
                for (const auto& of : offs) {
                    float x = pl.pos.x + (float)i * g_step + of[0], y = pl.pos.y + (float)j * g_step + of[1], z = 0;
                    if (!FloorBelow(x, y, c.z + 1.3f, z) || std::fabs(z - c.z) > 1.0f) continue;
                    auto known = g_cellAt.find(CellKey(i, j, z));
                    if (known != g_cellAt.end()) {  // a way to a cell found before
                        auto& a = g_cells[(size_t)ci].next;
                        CVector kp = g_cells[(size_t)known->second].p;
                        if (known->second != ci && std::find(a.begin(), a.end(), known->second) == a.end() &&
                            Clear(c + CVector{0, 0, 0.9f}, kp + CVector{0, 0, 0.9f}))
                            a.push_back(known->second), g_cells[(size_t)known->second].next.push_back(ci);
                        break;
                    }
                    CVector n{x, y, z};
                    if (!Clear(c + CVector{0, 0, 0.9f}, n + CVector{0, 0, 0.9f}) || !Clear(c + CVector{0, 0, 1.5f}, n + CVector{0, 0, 1.5f}) ||
                        !Clear(n + CVector{0, 0, 0.2f}, n + CVector{0, 0, 1.85f}))
                        continue;
                    int ni = (int)g_cells.size();
                    g_cells.push_back({n, {ci}, Roomy(n)});
                    g_cells[(size_t)ci].next.push_back(ni);
                    g_cellAt[CellKey(i, j, z)] = ni;
                    g_open.push_back(ni);
                    break;
                }
            }
    }
    if (g_open.empty() || g_cells.size() >= 3000) {
        g_gridDone = true;
        for (size_t k = 0; k < g_cells.size(); ++k)
            if (g_cells[k].room) g_roomy.push_back((int)k);
        LOGI("Arena: %d cells of floor found in %s (%d with room round them)", (int)g_cells.size(), kPlaces[g_arena.place].name[1],
             (int)g_roomy.size());
    }
}
bool GridUsable() { return g_gridDone && g_cells.size() >= 15; }
// the cells one may be put on: those with room round them (nobody comes out of a wall), all of them when too few
const std::vector<int>& StandCells() {
    static std::vector<int> all;
    if (g_roomy.size() >= 15) return g_roomy;
    if (all.size() != g_cells.size()) {
        all.resize(g_cells.size());
        for (size_t k = 0; k < all.size(); ++k) all[k] = (int)k;
    }
    return all;
}

int CellNear(const CVector& p, float within = 3.0f) {
    int best = -1;
    float bestD = within;
    for (size_t k = 0; k < g_cells.size(); ++k) {
        CVector d = g_cells[k].p - p;
        if (std::fabs(d.z + 1.0f) > 2.5f) continue;  // p is a body (a metre over its feet)
        float l = d.Length2D();
        if (l < bestD) bestD = l, best = (int)k;
    }
    return best;
}

// steps from every cell to cell `to` (BFS), -1 where it does not lead
std::vector<int> StepsTo(int to) {
    std::vector<int> dist(g_cells.size(), -1);
    if (to < 0) return dist;
    std::vector<int> q{to};
    dist[(size_t)to] = 0;
    for (size_t h = 0; h < q.size(); ++h)
        for (int n : g_cells[(size_t)q[h]].next)
            if (dist[(size_t)n] < 0) dist[(size_t)n] = dist[(size_t)q[h]] + 1, q.push_back(n);
    return dist;
}

std::vector<CVector> PlayerSpots() {  // the live players in the place
    std::vector<CVector> at;
    int area = kPlaces[g_arena.place].area;
    if (CPlayerPed* me = FindPlayerPed(0); me && me->Health() > 0.0f && me->AreaCode() == area) at.push_back(me->GetPosition());
    for (auto& [id, rp] : Players::Instance().All())
        if (rp.hasState && !(rp.state.flags & OnFootState::F_DEAD) && rp.state.area == area) at.push_back(rp.state.pos);
    if (at.empty()) at.push_back(kPlaces[g_arena.place].pos);
    return at;
}
float DistToPlayers(const CVector& p, const std::vector<CVector>& players) {
    float d = 1e9f;
    for (const CVector& q : players) d = std::min(d, (q - p).Length());
    return d;
}
float AngleDiff(float a, float b) {
    float d = std::fmod(std::fabs(a - b), 6.2831853f);
    return d > 3.14159265f ? 6.2831853f - d : d;
}

// ---------------------------------------------------------------- the enemies
constexpr int WEAPON_KNIFE = 4, WEAPON_GRENADE = 16, WEAPON_PISTOL = 22, WEAPON_SNIPER = 34;
constexpr int kEmptyDecisionMaker = 65542;  // SET_CHAR_DECISION_MAKER "Empty": reacts to nothing by itself
enum Role : uint8_t { R_GUN, R_GRENADIER, R_KNIFE, R_BOSS, R_SNIPER };
// what one does now: fights as the game has it, goes round to a back, runs to cover, holds it (down, up and shooting,
// down), pushes on at the players, (a knifeman) steals up to a back out of the player's sight, (a sniper) moves to
// another spot it sees the players from
enum Mode : uint8_t { M_FIGHT, M_FLANK, M_COVER_GO, M_COVER_HOLD, M_PUSH, M_SNEAK, M_RELOCATE };
struct Enemy {
    int squad = 0;
    Role role = R_GUN;
    int weapon = 0;  // its gun
    Mode mode = M_FIGHT;
    uint64_t modeUntil = 0, nextCoverMs = 0;
    bool popped = false;  // up and shooting in its cover
    uint64_t popUntil = 0;
    CVector cover;
    uint64_t grenadeUntil = 0;  // throwing till then
    uint64_t throwAt = 0;       // the grenade leaves the hand then
    float throwFlight = 1.0f;
    int grenades = 0;
    CPed* throwTarget = nullptr;
    float spawnHealth = 0;
    CPed* after = nullptr;  // the player it was set on
    uint64_t setOnMs = 0;   // when it was set on it
    CVector lastPos;        // where it was when it last moved on
    uint64_t lastMoveMs = 0;
    // going by the floor's cells (the player out of its sight): where to, since when
    bool routing = false;
    CVector routeTo;
    uint64_t routeMs = 0;
    uint64_t evalMs = 0, waitSince = 0, blindSince = 0;  // the knifeman's and the sniper's own reckoning
};
struct Squad {
    float angle = 0;  // where round the player it goes (degrees, 180 = behind)
    bool flank = false;
    uint64_t nextGrenadeMs = 0, nextPushMs = 0;
};
std::map<CPed*, Enemy> g_info;
std::vector<Squad> g_squads;
std::vector<CVector> g_lastSpawns, g_spawnsNow;  // where the last wave's and this wave's came from (the next ones elsewhere)

bool OnFloor(CVector& p, float baseZ) {  // the fallback without the floor's cells
    float z = -100.0f;
    Cmd(0x02CE).F(p.x).F(p.y).F(baseZ + 2.0f).Out(&z).Run();  // GET_GROUND_Z_FOR_3D_COORD
    if (z < baseZ - 4.0f || z > baseZ + 3.0f) return false;
    p.z = z + 1.0f;
    return true;
}

// any of the players sees a body at `at` (from their heads)
bool PlayersSee(const CVector& at, const std::vector<CVector>& players) {
    for (const CVector& q : players)
        if (Clear(q + CVector{0, 0, 0.6f}, at + CVector{0, 0, 0.2f})) return true;
    return false;
}

// A spot to come from (a body's, a metre over the floor): far from the players, as far as may be from the other
// spots of this wave (the squads spread over the whole place, not in one heap), not where the last wave came from,
// towards `angle` round the players (radians) if it can be, and out of the players' sight rather than in it. The
// distance kept from the players is what the place allows (a narrow hall has its far ends only). `minApart` from this
// wave's others; off the cells (too few): towards the angle from the middle.
CVector SpawnSpot(float angle, float halfSector, float minApart) {
    const Place& pl = kPlaces[g_arena.place];
    std::vector<CVector> players = PlayerSpots();
    CVector mid{};
    for (const CVector& q : players) mid = mid + q;
    mid = mid * (1.0f / (float)players.size());
    if (GridUsable()) {
        const std::vector<int>& cells = StandCells();
        float farthest = 0;
        for (int k : cells) farthest = std::max(farthest, DistToPlayers(g_cells[(size_t)k].p, players));
        float safe = std::min(pl.big ? 18.0f : 9.0f, farthest * 0.6f);
        for (int pass = 0; pass < 3; ++pass, safe *= 0.6f, minApart *= 0.5f) {
            struct Cand {
                int k;
                float score;
            };
            std::vector<Cand> cands;
            for (int k : cells) {
                const CVector& c = g_cells[(size_t)k].p;
                float dp = DistToPlayers(c, players);
                if (dp < safe) continue;
                float apart = 1e9f;
                for (const CVector& sp : g_spawnsNow) apart = std::min(apart, (sp - c - CVector{0, 0, 1.0f}).Length());
                if (apart < minApart) continue;
                float off = AngleDiff(std::atan2(c.y - mid.y, c.x - mid.x), angle);
                float score = std::min(dp, 30.0f) * 0.5f + std::min(apart, 30.0f) - std::max(0.0f, off - halfSector) * 6.0f + Rand(0.0f, 4.0f);
                for (const CVector& sp : g_lastSpawns) score -= std::max(0.0f, 10.0f - (sp - c).Length());  // not where they came from
                cands.push_back({k, score});
            }
            if (cands.empty()) continue;
            std::sort(cands.begin(), cands.end(), [](const Cand& a, const Cand& b) { return a.score > b.score; });
            int best = cands[0].k;
            float bestScore = -1e9f;
            for (size_t n = 0; n < cands.size() && n < 12; ++n) {  // the best few: one out of sight is better
                CVector body = g_cells[(size_t)cands[n].k].p + CVector{0, 0, 1.0f};
                float score = cands[n].score + (PlayersSee(body, players) ? 0.0f : 8.0f);
                if (score > bestScore) bestScore = score, best = cands[n].k;
            }
            CVector p = g_cells[(size_t)best].p + CVector{0, 0, 1.0f};
            g_spawnsNow.push_back(p);
            return p;
        }
    }
    CVector best = pl.pos;
    float bestScore = -1.0f;
    for (int t = 0; t < 24; ++t) {
        float a = angle + Rand(-0.35f, 0.35f), d = Rand(pl.reach * 0.45f, pl.reach);
        CVector p = pl.pos + CVector{std::cos(a) * d, std::sin(a) * d, 0.0f};
        if (!OnFloor(p, pl.pos.z)) continue;
        float dp = DistToPlayers(p, players), score = std::min(dp, 25.0f) + d * 0.3f;
        if (dp > 6.0f && score > bestScore) bestScore = score, best = p;
    }
    g_spawnsNow.push_back(best);
    return best;
}
// a member's place by its squad's (a cell with room round it a few metres off it, a way from it)
CVector MemberSpot(const CVector& anchor, int k) {
    if (GridUsable()) {
        std::vector<int> close;
        for (int i : StandCells()) {
            CVector d = g_cells[(size_t)i].p + CVector{0, 0, 1.0f} - anchor;
            if (d.Length2D() > 0.9f && d.Length2D() < 4.0f && std::fabs(d.z) < 1.5f && Clear(anchor, g_cells[(size_t)i].p + CVector{0, 0, 1.0f}))
                close.push_back(i);
        }
        if (!close.empty()) return g_cells[(size_t)close[(size_t)RandInt(0, (int)close.size() - 1)]].p + CVector{0, 0, 1.0f};
    }
    for (int t = 0; t < 8; ++t) {
        float a = Rand(0.0f, 6.2831853f), d = Rand(1.2f, 2.6f);
        CVector p = anchor + CVector{std::cos(a) * d, std::sin(a) * d, 0.0f};
        if (OnFloor(p, anchor.z - 1.0f) && Clear(anchor, p)) return p;
    }
    return anchor + CVector{0.6f * (float)(k % 2 ? 1 : -1), 0.6f * (float)(k / 2), 0.0f};
}

// the nearest live player ped (this game's own or a copy) in the place
CPed* NearestPlayer(const CVector& at) {
    CPed* best = nullptr;
    float bestD = 1e9f;
    int area = kPlaces[g_arena.place].area;
    auto consider = [&](CPed* ped) {
        if (!ped || ped->Health() <= 0.0f || ped->AreaCode() != area) return;
        float d = (ped->GetPosition() - at).Length();
        if (d < bestD) bestD = d, best = ped;
    };
    consider(FindPlayerPed(0));
    for (auto& [id, rp] : Players::Instance().All())
        if (rp.ped && rp.hasState && !(rp.state.flags & OnFootState::F_DEAD)) consider(rp.ped);
    return best;
}

int GunFor(int weapon) {  // the weapon if its model is in memory now, else a pistol (a gun without its model crashes)
    int wm = -1;
    Cmd(0x0781).I(weapon).Out(&wm).Run();  // GET_WEAPONTYPE_MODEL
    if (wm <= 0 || IsModelLoaded(wm) || LoadModelNow(wm)) return weapon;
    return IsModelLoaded(346) || LoadModelNow(346) ? WEAPON_PISTOL : 0;
}

void Give(int h, int weapon, int ammo) {
    weapon = GunFor(weapon);
    if (weapon) Cmd(0x01B2).I(h).I(weapon).I(ammo).Run();  // GIVE_WEAPON_TO_CHAR
}

void SetOn(CPed* p, Enemy& e, CPed* target) {
    e.after = target;
    e.routing = false;
    e.setOnMs = NowMs();
    if (!target) return;
    int h = GetPedRef(p), t = GetPedRef(target);
    const Squad& sq = g_squads[(size_t)e.squad];
    if (e.mode == M_FLANK) {
        // round to the player's back (the offset turns with the player), then the fight
        Cmd(0x06A8).I(h).I(t).I(14000).F(7.0f).F(sq.angle).Run();  // TASK_GOTO_CHAR_OFFSET
    } else {
        Cmd(0x05E2).I(h).I(t).Run();  // TASK_KILL_CHAR_ON_FOOT
    }
}

// on to a spot (the floor's) running (6) or sprinting (7); not given again while it is on its way to the same one
void GoTo(CPed* p, Enemy& e, const CVector& to, int move) {
    uint64_t now = NowMs();
    if (e.routing && (e.routeTo - to).Length() < 0.6f && now - e.routeMs < 2500) return;
    e.routing = true;
    e.routeTo = to;
    e.routeMs = now;
    Cmd(0x05D3).I(GetPedRef(p)).F(to.x).F(to.y).F(to.z).I(move).I(5000).Run();  // TASK_GO_STRAIGHT_TO_COORD
}

int MakeEnemy(int model, int weapon, int health, int armour, int accuracy, const CVector& at, int squad, Role role, bool silent) {
    if (!HasRoom(PedPool(), kKeepPeds + 2, "ped") || !LoadModelNow(model)) return -1;
    int h = -1;
    Cmd(0x009A).I(PED_TYPE_MISSION1).I(model).F(at.x).F(at.y).F(at.z - 1.0f).Out(&h).Run();  // CREATE_CHAR (on the floor)
    if (h < 0) return -1;
    if (CPed* made = GetPed(h)) Peds::Instance().Share(made);  // the others see it (no mission made it)
    Cmd(0x0860).I(h).I(kPlaces[g_arena.place].area).Run();  // SET_CHAR_AREA_VISIBLE
    Cmd(0x087E).I(h).I(0).Run();                             // SET_CHAR_DROPS_WEAPONS_WHEN_DEAD: none
    Cmd(0x0223).I(h).I(health).Run();                        // SET_CHAR_HEALTH
    Cmd(0x02E2).I(h).I(accuracy).Run();                      // SET_CHAR_ACCURACY
    Cmd(0x060F).I(h).F(150.0f).Run();                        // SET_SENSE_RANGE: it knows where the players are
    Cmd(0x060B).I(h).I(kEmptyDecisionMaker).Run();           // SET_CHAR_DECISION_MAKER: no fear (no running off, no cowering)
    if (silent) Cmd(0x094E).I(h).I(1).Run();                 // DISABLE_CHAR_SPEECH
    if (role == R_BOSS) Cmd(0x0446).I(h).I(0).Run();         // SET_CHAR_SUFFERS_CRITICAL_HITS: no one-shot headshot
    if (role == R_KNIFE) {
        Give(h, WEAPON_KNIFE, 1);
        weapon = WEAPON_KNIFE;
        Cmd(0x0245).I(h).S8("player").Run();  // SET_ANIM_GROUP_FOR_CHAR: it runs as CJ does
    } else {
        Give(h, weapon, role == R_SNIPER ? 80 : 500);
        weapon = GunFor(weapon);
    }
    if (role == R_SNIPER) Cmd(0x0350).I(h).I(1).Run();  // SET_CHAR_STAY_IN_SAME_PLACE: it shoots from where it is
    if (weapon) Cmd(0x01B9).I(h).I(weapon).Run();  // SET_CURRENT_CHAR_WEAPON
    CPed* ped = GetPed(h);
    if (!ped) return h;
    ped->Armour() = (float)armour;
    Enemy e;
    e.squad = squad;
    e.role = role;
    e.weapon = weapon;
    e.spawnHealth = (float)health;
    e.grenades = role == R_GRENADIER ? 6 : 0;
    e.mode = role == R_KNIFE ? M_SNEAK : g_squads[(size_t)squad].flank ? M_FLANK : M_FIGHT;
    e.modeUntil = NowMs() + 14000;
    e.nextCoverMs = NowMs() + (uint64_t)RandInt(3000, 8000);
    e.lastPos = at;
    e.lastMoveMs = NowMs();
    g_info[ped] = e;
    g_enemies.insert(ped);
    if (role == R_KNIFE) knife::MarkKnifeman(ped);
    if (role != R_KNIFE) SetOn(ped, g_info[ped], NearestPlayer(at));
    else g_info[ped].after = NearestPlayer(at);
    return h;
}

// the SWAT's armour is half the others' (they do not die of a headshot, Difficulty)
int ArmourFor(int wave, int extra) {
    int a = std::min(250, 25 * wave + extra);
    return kFactions[g_arena.faction].models[0] == 285 ? a / 2 : a;
}

// a squad of 3-4 towards angle `dir` (radians round the players), going round to `aroundDeg` (180: the back) or not
int MakeSquad(float dir, float halfSector, bool flank, float aroundDeg, int size, int wave, bool minions) {
    const Faction& f = kFactions[g_arena.faction];
    float progress = g_arena.waves > 1 ? (float)(wave - 1) / (float)(g_arena.waves - 1) : 1.0f;
    int gunTier = std::clamp((int)std::lround(progress * 3.0f) + (minions ? 1 : 0), 0, 3);
    Squad sq;
    sq.angle = aroundDeg;
    sq.flank = flank;
    sq.nextGrenadeMs = NowMs() + (uint64_t)RandInt(4000, 7000);
    sq.nextPushMs = NowMs() + (uint64_t)RandInt(18000, 26000);
    g_squads.push_back(sq);
    int squad = (int)g_squads.size() - 1;
    CVector anchor = SpawnSpot(dir, halfSector, 8.0f);
    bool scatter = kPlaces[g_arena.place].big && !minions;  // a big place: its members come from all over their side
    int made = 0;
    for (int k = 0; k < size; ++k) {
        Role role = k == 1 ? R_GRENADIER : R_GUN;
        CVector at = k == 0 ? anchor : scatter ? SpawnSpot(dir, halfSector * 1.3f, 6.0f) : MemberSpot(anchor, k);
        int weapon = f.weapons[std::clamp(gunTier - (k == 2 ? 1 : 0), 0, 3)];
        int model = f.models[(size_t)RandInt(0, 3)];
        int health = 100 + 15 * wave, armour = ArmourFor(wave, minions ? 50 : 0);
        int accuracy = std::clamp(40 + 6 * wave + (minions ? 10 : 0), 40, 90);
        made += MakeEnemy(model, weapon, health, armour, accuracy, at, squad, role, minions) >= 0;
    }
    return made;
}

// A knifeman comes alone, from a spot far from the squads (and from the players), silent, round to a player's back
void MakeKnifeman(int wave) {
    const Faction& f = kFactions[g_arena.faction];
    Squad sq;
    sq.angle = 180.0f;
    sq.flank = true;
    g_squads.push_back(sq);
    std::vector<CVector> players = PlayerSpots();
    CVector at;
    bool found = false;
    if (GridUsable()) {
        float best = -1;
        for (int k : StandCells()) {
            CVector p = g_cells[(size_t)k].p + CVector{0, 0, 1.0f};
            float fromSquads = 1e9f;
            for (const CVector& s : g_spawnsNow) fromSquads = std::min(fromSquads, (s - p).Length());
            float dp = DistToPlayers(p, players);
            if (fromSquads < 10.0f || dp < 10.0f) continue;
            float score = std::min(fromSquads, dp) + Rand(0.0f, 5.0f);
            if (score + 6.0f <= best) continue;
            if (!PlayersSee(p, players)) score += 6.0f;  // out of their sight is better
            if (score > best) best = score, at = p, found = true;
        }
    }
    if (!found) at = SpawnSpot(Rand(0.0f, 6.2831853f), 3.2f, 10.0f);
    else g_spawnsNow.push_back(at);
    int model = g_knifeModel >= 0 && IsModelLoaded(g_knifeModel) ? g_knifeModel : f.models[(size_t)RandInt(0, 3)];
    MakeEnemy(model, WEAPON_KNIFE, 100 + 15 * wave, ArmourFor(wave, 0), 60, at, (int)g_squads.size() - 1, R_KNIFE, true);
}

// Snipers (the last two waves): each alone on a spot far off that sees a player, staying there and shooting from
// there; one that loses sight of the players for 5 s moves to another such spot
bool SniperSpot(const CVector& from, CPed* target, float maxMove, CVector& out) {
    std::vector<CVector> players;
    if (target) players.push_back(target->GetPosition());
    else players = PlayerSpots();
    const Place& pl = kPlaces[g_arena.place];
    float want = pl.big ? 20.0f : 10.0f, best = -1e9f;
    for (int k : StandCells()) {
        CVector body = g_cells[(size_t)k].p + CVector{0, 0, 1.0f};
        if (maxMove > 0 && (body - from).Length() > maxMove) continue;
        float dp = DistToPlayers(body, players);
        if (dp < want * 0.5f) continue;
        bool apart = true;
        for (auto& [q, e] : g_info)
            if (e.role == R_SNIPER && IsPedValid(q) && (q->GetPosition() - body).Length() < 8.0f && (q->GetPosition() - from).Length() > 1.0f)
                apart = false;
        if (!apart) continue;
        float score = std::min(dp, 45.0f) + Rand(0.0f, 5.0f) - (dp < want ? 15.0f : 0.0f);
        if (score <= best) continue;
        CVector eye = body + CVector{0, 0, 0.6f};
        bool sees = false;
        for (const CVector& q : players) sees |= Clear(eye, q + CVector{0, 0, 0.4f});
        if (!sees) continue;
        best = score, out = body;
    }
    return best > -1e8f;
}
void MakeSniper(int wave) {
    const Faction& f = kFactions[g_arena.faction];
    g_squads.push_back(Squad{});
    CVector at;
    if (!GridUsable() || !SniperSpot({}, nullptr, 0.0f, at)) at = SpawnSpot(Rand(0.0f, 6.2831853f), 3.2f, 8.0f);
    else g_spawnsNow.push_back(at);
    MakeEnemy(f.models[(size_t)RandInt(0, 3)], WEAPON_SNIPER, 100 + 15 * wave, ArmourFor(wave, 0) / 2, 80, at,
              (int)g_squads.size() - 1, R_SNIPER, false);
}

void ReleaseDead() {  // the last wave's dead: the game's own again (they filled the ped pool)
    for (CPed* p : g_enemies)
        if (IsPedValid(p)) Cmd(p->Health() > 0.0f ? 0x009B : 0x01C2).I(GetPedRef(p)).Run();  // DELETE_CHAR / no longer needed
    g_enemies.clear();
    g_info.clear();
    g_squads.clear();
}

void LoadBoss(int b) {
    if (b < 0 || b >= kBossCount) return;
    Cmd(0x023C).I(kBossSlot).S8(kBosses[b].special).Run();  // LOAD_SPECIAL_CHARACTER
    Cmd(0x038B).Run();                                      // LOAD_ALL_MODELS_NOW
}
void UnloadBoss() { Cmd(0x0296).I(kBossSlot).Run(); }  // UNLOAD_SPECIAL_CHARACTER

int PickBoss(int faction) {
    std::vector<int> fit;
    for (int b : kFactions[faction].boss)
        if (b >= 0) fit.push_back(b);
    if (!fit.empty() && RandInt(0, 3) != 0) return fit[(size_t)RandInt(0, (int)fit.size() - 1)];
    return RandInt(0, kBossCount - 1);
}

int PlayersIn() {
    int n = 1;
    if (Net().Online())
        for (auto& [id, rp] : Players::Instance().All()) n += rp.ped != nullptr;
    return n;
}

// ---------------------------------------------------------------- things to pick up
// Before each wave (the first one too, once the floor is known) the host lays weapons and body armour about the place
// (A_ITEMS: every game makes the same pickups, and one taken is gone for all, Pickups): better ones as the waves go
// on, more in a big place, the sniper rifle and the rocket launcher only in a big one; where drinks machines stand
// (health from them) less armour. They lie spread over the whole place, each away from the others and from where the
// last wave's lay, so every wave they are somewhere else.
struct Loot {
    int weapon, model, ammo;
};
const Loot kLoot[3][6] = {
    {{22, 346, 68}, {24, 348, 35}, {25, 349, 30}, {28, 352, 180}, {32, 372, 180}, {26, 350, 24}},   // the first waves
    {{29, 353, 240}, {30, 355, 150}, {26, 350, 30}, {27, 351, 28}, {24, 348, 42}, {16, 342, 4}},    // the middle ones
    {{31, 356, 200}, {30, 355, 200}, {27, 351, 35}, {34, 358, 20}, {16, 342, 6}, {35, 359, 3}},     // the last ones
};
constexpr int MODEL_ARMOUR = 1242;
struct Item {
    uint16_t model = 0, ammo = 0;
    CVector pos;
};
std::vector<int> g_pickups;          // this game's pickups of the arena (handles)
std::vector<CVector> g_lastItemsAt;  // host: where the last ones lay

// drinks machines in the place (the game's objects near its middle)
int Machines() {
    static const int kModels[] = {955, 956, 1209, 1302, 1775, 1776, 1977};
    CPoolHeader* pool = ObjectPool();
    if (!pool) return 0;
    const Place& pl = kPlaces[g_arena.place];
    int n = 0;
    for (int i = 0; i < pool->size; ++i) {
        if (pool->byteMap[i] < 0) continue;
        auto* o = (CEntity*)(pool->objects + (size_t)i * kObjectPoolElemSize);
        CVector d = o->GetPosition() - pl.pos;
        if (std::fabs(d.z) > 12.0f || d.Length2D() > pl.reach + 15.0f) continue;
        for (int m : kModels) n += o->ModelIndex() == m;
    }
    return n;
}

void RemoveItems() {
    for (int h : g_pickups) Cmd(0x0215).I(h).Run();  // REMOVE_PICKUP
    g_pickups.clear();
}

void MakeItems(const std::vector<Item>& items) {
    RemoveItems();
    game::challenge::SetMakingPickups(true);  // every game makes these: not announced as this one's
    for (const Item& it : items) {
        if (!LoadModelNow(it.model)) continue;
        int h = -1;
        if (it.model == MODEL_ARMOUR) Cmd(0x0213).I(it.model).I(3).F(it.pos.x).F(it.pos.y).F(it.pos.z).Out(&h).Run();  // CREATE_PICKUP, once
        else Cmd(0x032B).I(it.model).I(3).I(it.ammo).F(it.pos.x).F(it.pos.y).F(it.pos.z).Out(&h).Run();  // CREATE_PICKUP_WITH_AMMO
        if (h >= 0) g_pickups.push_back(h);
    }
    game::challenge::SetMakingPickups(false);
}

void PlaceItems(int forWave) {
    const Place& pl = kPlaces[g_arena.place];
    float progress = g_arena.waves > 1 ? (float)(forWave - 1) / (float)(g_arena.waves - 1) : 1.0f;
    int tier = std::clamp((int)(progress * 2.99f), 0, 2);
    bool soda = pl.soda || Machines() > 0;
    int guns = 2 + forWave / 2 + (pl.big ? 2 : 0);
    int armour = soda ? (forWave % 2 == 0 ? 1 : 0) + (pl.big ? 1 : 0) : 1 + forWave / 3 + (pl.big ? 1 : 0);
    guns = std::min(guns, 7), armour = std::min(armour, 4);
    std::vector<Item> items;
    std::vector<CVector> players = PlayerSpots();
    // the next spot: of the place's cells the farthest from those taken and from the last wave's (a little luck too)
    auto spot = [&](CVector& out) {
        if (GridUsable()) {
            float best = -1;
            for (const Cell& c : g_cells) {
                if (DistToPlayers(c.p, players) < 3.0f) continue;
                float apart = 1e9f;
                for (const Item& it : items) apart = std::min(apart, (it.pos - c.p).Length());
                for (const CVector& q : g_lastItemsAt) apart = std::min(apart, (q - c.p).Length() + 2.0f);
                if (apart > 1e8f) apart = 30.0f;
                float score = std::min(apart, 40.0f) + Rand(0.0f, 6.0f);
                if (score > best) best = score, out = c.p + CVector{0, 0, 0.7f};
            }
            return best >= 0;
        }
        for (int t = 0; t < 20; ++t) {
            float a = Rand(0.0f, 6.2831853f), d = Rand(2.0f, pl.reach * 0.9f);
            CVector p = pl.pos + CVector{std::cos(a) * d, std::sin(a) * d, 0.0f};
            if (!OnFloor(p, pl.pos.z)) continue;
            bool apart = true;
            for (const Item& it : items) apart &= (it.pos - p).Length() > 3.5f;
            if (!apart) continue;
            out = p - CVector{0, 0, 0.3f};
            return true;
        }
        return false;
    };
    for (int k = 0; k < guns; ++k) {
        const Loot* l = nullptr;
        for (int t = 0; t < 6 && !l; ++t) {
            const Loot& c = kLoot[tier][(size_t)RandInt(0, 5)];
            if (!pl.big && (c.weapon == 34 || c.weapon == 35)) continue;  // no sniper rifle or rockets in a small room
            l = &c;
        }
        Item it;
        if (!l || !spot(it.pos)) continue;
        it.model = (uint16_t)l->model;
        it.ammo = (uint16_t)l->ammo;
        items.push_back(it);
    }
    for (int k = 0; k < armour; ++k) {
        Item it;
        if (!spot(it.pos)) continue;
        it.model = MODEL_ARMOUR;
        items.push_back(it);
    }
    g_lastItemsAt.clear();
    for (const Item& it : items) g_lastItemsAt.push_back(it.pos);
    Send(A_ITEMS, [&](Writer& w) {
        w.U8((uint8_t)items.size());
        for (const Item& it : items) {
            w.U16(it.model);
            w.U16(it.ammo);
            w.Vec({it.pos.x, it.pos.y, it.pos.z});
        }
    });
    MakeItems(items);
    LOGI("Arena: %d of %d weapons and %d armour laid out for wave %d (drinks machines: %d)", (int)items.size() - armour, guns,
         armour, forWave, soda ? 1 : 0);
}

uint64_t PrepMs(bool first) { return kPlaces[g_arena.place].big ? (first ? 20000 : 30000) : (first ? 8000 : 6000); }

void SpawnWave() {
    ReleaseDead();
    ++g_arena.wave;
    g_bossPed = nullptr;
    g_lastSpawns = g_spawnsNow;
    g_spawnsNow.clear();
    int w = g_arena.wave;
    int count = std::min(14, 2 + w * 2 + PlayersIn());
    // squads of 3-4, spread evenly round the players; the first comes straight on, the others go round; the knifemen
    // (1 from 6 enemies, 2 from 11) alone, from elsewhere
    int knives = count >= 11 ? 2 : count >= 6 ? 1 : 0;
    count -= knives;
    int squads = std::max(1, (count + 3) / 4);
    float base = Rand(0.0f, 6.2831853f), half = 3.14159265f / (float)squads;
    const float around[] = {180.0f, 135.0f, 225.0f, 90.0f, 270.0f};
    for (int i = 0; i < squads; ++i) {
        int size = count / squads + (i < count % squads ? 1 : 0);  // 3-4 each (fewer only in a small first wave)
        float dir = base + 6.2831853f * (float)i / (float)squads;
        MakeSquad(dir, half, i > 0, around[(size_t)(i - 1 + 5) % 5], size, w, false);
    }
    for (int k = 0; k < knives; ++k) MakeKnifeman(w);
    // the last two waves: snipers far off (1, 2 in a big place, one more with three players or more)
    if (w >= std::max(1, g_arena.waves - 1)) {
        int snipers = (kPlaces[g_arena.place].big ? 2 : 1) + (PlayersIn() >= 3 ? 1 : 0);
        for (int k = 0; k < snipers; ++k) MakeSniper(w);
    }
    std::string bossLine;
    if (w == g_arena.waves) {
        int b = PickBoss(g_arena.faction);
        Send(A_BOSS, [b](Writer& wr) { wr.U8((uint8_t)b); });
        LoadBoss(b);
        bool special = IsModelLoaded(kBossModel);
        const Faction& f = kFactions[g_arena.faction];
        int model = special ? kBossModel : f.models[0];
        g_squads.push_back(Squad{});
        int bossSquad = (int)g_squads.size() - 1;
        // as tough as the SWAT were in 0.4.1: 100 health, 100 armour, no one-shot headshot
        int h = MakeEnemy(model, kBosses[b].weapon, 100, 100, 85, SpawnSpot(base + 3.14159f, 1.0f, 8.0f), bossSquad, R_BOSS, false);
        if (CPed* p = h >= 0 ? GetPed(h) : nullptr) g_bossPed = p;
        // his men: once, unannounced, silent, round to the players' backs
        int men = std::min(6, 3 + PlayersIn() - 1);
        MakeSquad(base + 3.14159f + 1.2f, 0.8f, true, 150.0f, std::min(4, men), w, true);
        if (men > 4) MakeSquad(base + 3.14159f - 1.2f, 0.8f, true, 210.0f, men - 4, w, true);
        g_arena.boss = b;
        bossLine = BossName(b);
    }
    g_arena.waveMs = NowMs();
    int n = (int)g_enemies.size(), wave = g_arena.wave;
    Send(A_WAVE, [&](Writer& wr) {
        wr.U8((uint8_t)wave);
        wr.U8((uint8_t)n);
    });
    ui::TheChat().AddSystem(Format(TR("Арена: волна %d из %d — врагов %d.", "Arena: wave %d of %d, %d enemies."), wave, g_arena.waves, n));
    if (!bossLine.empty())
        ui::TheChat().AddSystem(Format(TR("Последняя волна! Босс: %s.", "The last wave! The boss: %s."), bossLine.c_str()));
    LOGI("Arena: wave %d, %d enemies in %d squads", wave, n, (int)g_squads.size());
}

// ---- grenades
// The game's people do not throw a grenade by themselves in a scripted fight, so the grenadier throws it here: he
// turns to the player, plays the throw (GRENADE.ifp, WEAPON_throw), and as the arm comes over the grenade leaves his
// hand (CProjectileInfo::AddProjectile in his name) on an arc that comes down where the player will be, leading a
// moving player, gravity as the game's (0.008 a frame squared: 20 m/s/s). He sees the arc first: of the flights
// 0.8-1.9 s (higher lobs for the longer ones) the first whose arc meets nothing on the way (a ceiling, a wall, a
// doorframe) till it comes down by the player is taken; none: no throw (it bounced back at his feet).
constexpr uintptr_t kProjectileInfos = 0xC891A8;  // gaProjectileInfo[32], 0x24 bytes, m_bActive at +0x10
constexpr uintptr_t kProjectiles = 0xC89110;      // CProjectileInfo::ms_apProjectile[32]
constexpr float kGravity = 20.0f;
uint32_t ActiveProjectiles() {
    uint32_t mask = 0;
    for (int i = 0; i < 32; ++i)
        if (Ref<uint8_t>(kProjectileInfos + (uintptr_t)i * 0x24 + 0x10)) mask |= 1u << i;
    return mask;
}

bool EnsureBlock(const char* block) {
    int idx = Call<0x4D3990, int>(block);  // CAnimManager::GetAnimationBlockIndex
    if (idx < 0) return false;
    uintptr_t b = 0xB5D4A0 + (uintptr_t)idx * 0x20;  // CAnimManager::ms_aAnimBlocks
    if (Ref<uint8_t>(b + 0x10)) return true;
    RequestModel(25575 + idx, 2);
    LoadAllRequestedModels(false);
    return Ref<uint8_t>(b + 0x10) != 0;
}

CVector ThrowFrom(CPed* p, const CVector& at) {  // the hand as the arm comes over, towards `at`
    CVector to = at - p->GetPosition();
    float len = std::max(0.1f, to.Length2D());
    return p->GetPosition() + CVector{to.x / len, to.y / len, 0.0f} * 0.5f + CVector{0, 0, 0.75f};
}
CVector ThrowVel(const CVector& from, const CVector& aim, float flight) {
    return {(aim.x - from.x) / flight, (aim.y - from.y) / flight, (aim.z - from.z + 0.5f * kGravity * flight * flight) / flight};
}
// the arc reaches the player: nothing in the way before it comes down within 3 m of it
bool ArcClear(const CVector& from, CVector vel, float flight, const CVector& aim) {
    CVector pos = from;
    constexpr float dt = 0.05f;
    for (float t = 0; t < flight + 0.2f; t += dt) {
        CVector prev = pos;
        vel.z -= kGravity * dt;
        pos = pos + vel * dt;
        CVector hit;
        if (Ray(prev, pos, hit)) return (hit - aim).Length() < 3.0f && t > flight * 0.55f;
    }
    return true;
}
// the flight whose arc is clear, 0 when none
float ClearFlight(const CVector& from, const CVector& aim) {
    float dist = (aim - from).Length2D();
    for (float f : {0.8f, 1.0f, 1.25f, 1.5f, 1.9f}) {
        if (dist / f > 22.0f) continue;  // a throw that hard is no throw
        if (ArcClear(from, ThrowVel(from, aim, f), f, aim)) return f;
    }
    return 0.0f;
}

bool StartThrow(CPed* p, Enemy& e, CPed* t) {
    CVector aim = t->GetPosition() - CVector{0, 0, 0.8f};
    if ((aim - p->GetPosition()).Length2D() < 8.0f) return false;  // not at his own feet
    float flight = ClearFlight(ThrowFrom(p, aim), aim);
    if (flight <= 0.0f) return false;
    int h = GetPedRef(p);
    CVector to = t->GetPosition() - p->GetPosition();
    Cmd(0x0173).I(h).F(std::atan2(-to.x, to.y) * 57.29578f).Run();  // SET_CHAR_HEADING: at the player
    if (EnsureBlock("GRENADE")) Cmd(0x0605).I(h).S("WEAPON_throw").S("GRENADE").F(8.0f).I(0).I(0).I(0).I(0).I(-1).Run();  // TASK_PLAY_ANIM
    e.throwAt = NowMs() + 380;
    e.throwTarget = t;
    e.throwFlight = flight;
    e.grenadeUntil = NowMs() + 1300;
    --e.grenades;
    return true;
}

void Release(CPed* p, Enemy& e) {
    e.throwAt = 0;
    CPed* t = e.throwTarget;
    if (!t || !IsPedValid(t) || !IsPedValid(p) || p->Health() <= 0.0f || !LoadModelNow(342)) return;
    CVector aim = t->GetPosition() - CVector{0, 0, 0.8f};  // at its feet, where it will be
    aim = aim + t->MoveSpeed() * 50.0f * e.throwFlight * 0.8f;
    CVector from = ThrowFrom(p, aim);
    float flight = ArcClear(from, ThrowVel(from, aim, e.throwFlight), e.throwFlight, aim) ? e.throwFlight : ClearFlight(from, aim);
    if (flight <= 0.0f) return;  // the player stepped behind something meanwhile: he keeps it
    CVector vel = ThrowVel(from, aim, flight), dir = vel.Normalized();
    uint32_t before = ActiveProjectiles();
    game::challenge::SetNextThrow(true, from, vel * (1.0f / 50.0f));  // metres a frame; Fire sets it (and sends it)
    bool ok = Call<0x737C80, bool>((CEntity*)p, WEAPON_GRENADE, from, 1.0f, &dir, (CEntity*)nullptr);  // AddProjectile
    CVector f, sp;
    if (game::challenge::NextThrow(f, sp)) {  // nobody took it (no Fire hook): set here
        uint32_t added = ActiveProjectiles() & ~before;
        for (int i = 0; i < 32; ++i)
            if (added & (1u << i))
                if (auto* obj = Ref<CPhysical*>(kProjectiles + (uintptr_t)i * 4)) obj->SetPositionRaw(f), obj->MoveSpeed() = sp;
    }
    if (ok) LOGD("Arena: a grenade thrown %.0f m, %.2f s", (aim - from).Length2D(), flight);
}

void Throws() {  // every frame: the grenades that leave the hand now
    uint64_t now = NowMs();
    for (auto& [p, e] : g_info)
        if (e.throwAt && now >= e.throwAt) Release(p, e);
}

// ---- how they fight
// Every 0.5 s each one. They always know where the players are (sense range over the place, the floor's cells):
// one that does not see its player goes to it along the cells (to the farthest of the next few cells it sees straight,
// running), not by the game's own way-finding, which in these interiors walked into walls; in sight it fights. The
// ones going round turn to the fight when they are there (or close, hurt, 14 s on). In the fight one with the player
// in sight within 28 m now and then runs to cover (a cell of the floor within 9 m where, crouched, nothing of it shows
// to the player and, standing, it sees the player) and holds it some 5-10 s: down, then up shooting for 2 s, down
// again; then it pushes on at the player for a while, and later looks for the next cover. A squad pushes all together
// every 18-26 s. The grenadiers throw when the arc is clear. One that has not moved for 4 s while the player is out
// of its sight is stuck: it is set one step on along the floor's cells towards the player. The knifemen steal up to a
// back out of the player's sight (Sneak), the snipers stay far off (Snipe).
struct Paths {
    std::vector<int> steps;  // steps from every cell to the player's (-1: no way)
    int cell = -1;
    uint64_t ms = 0;
};
std::map<CPed*, Paths> g_paths;

const std::vector<int>& StepsFor(CPed* t) {
    Paths& pa = g_paths[t];
    uint64_t now = NowMs();
    if (pa.steps.size() != g_cells.size() || now - pa.ms > 1500) {
        int cell = CellNear(t->GetPosition());
        if (cell >= 0 || pa.steps.size() != g_cells.size()) {
            pa.cell = cell;
            pa.steps = StepsTo(cell);
        }
        pa.ms = now;
    }
    return pa.steps;
}

// the cell it stands on, or the nearest within 6 m that leads somewhere
int CellOf(CPed* p, const std::vector<int>* steps = nullptr) {
    int here = CellNear(p->GetPosition());
    if (here >= 0 && (!steps || (*steps)[(size_t)here] >= 0)) return here;
    int best = -1;
    float bestD = 6.0f;
    for (size_t k = 0; k < g_cells.size(); ++k) {
        float d = (g_cells[k].p + CVector{0, 0, 1.0f} - p->GetPosition()).Length();
        if ((!steps || (*steps)[k] >= 0) && d < bestD) bestD = d, best = (int)k;
    }
    return best;
}

// straight from a body to a floor cell: nothing at the knees or the chest
bool Straight(const CVector& body, const CVector& floor) {
    return Clear(body - CVector{0, 0, 0.4f}, floor + CVector{0, 0, 0.6f}) && Clear(body + CVector{0, 0, 0.4f}, floor + CVector{0, 0, 1.4f});
}

// the next spot on the way to the player: of the next 6 cells down the steps the farthest it sees straight
bool Waypoint(CPed* p, CPed* t, CVector& out) {
    if (!GridUsable()) return false;
    const std::vector<int>& steps = StepsFor(t);
    int cur = CellOf(p, &steps);
    if (cur < 0) return false;
    CVector from = p->GetPosition();
    int best = -1;
    for (int s = 0; s < 6; ++s) {
        int next = cur;
        for (int n : g_cells[(size_t)cur].next)
            if (steps[(size_t)n] >= 0 && steps[(size_t)n] < steps[(size_t)next]) next = n;
        if (next == cur) break;
        cur = next;
        if (!Straight(from, g_cells[(size_t)cur].p)) break;
        best = cur;
    }
    if (best < 0) best = cur;  // round a corner: the next one at least
    out = g_cells[(size_t)best].p;
    return (out - (from - CVector{0, 0, 1.0f})).Length2D() > 0.4f;
}

// where the player looks: its camera (this game's own, or the one its state carries)
CVector ViewOf(CPed* t) {
    CVector f;
    if (t == FindPlayerPed(0)) f = cam::Front();
    else if (const RemotePlayer* rp = Players::Instance().FindByPed(t); rp && rp->hasState) f = rp->state.cam.front;
    else f = {-std::sin(t->GetHeading()), std::cos(t->GetHeading()), 0.0f};
    f.z = 0;
    float l = f.Length();
    return l > 0.01f ? f * (1.0f / l) : CVector{0, 1, 0};
}

// the player would see a body there: within 50 m, within some 70 degrees of where it looks, nothing between
bool InView(CPed* t, const CVector& view, const CVector& body) {
    CVector eye = t->GetPosition() + CVector{0, 0, 0.6f}, d = body - eye;
    float l = d.Length2D();
    if (l > 50.0f) return false;
    if (l > 0.5f && (d.x * view.x + d.y * view.y) < l * 0.34f) return false;
    return Clear(eye, body + CVector{0, 0, 0.3f});
}

// The knifeman: up to the player's back by the cells it would not be seen on (Dijkstra: a cell in the player's view
// costs as much as eight out of it), to a cell 1.2-4 m behind it; from there the last steps (TASK_GOTO_CHAR_OFFSET
// behind it) and the knife (StealthKnife). Out of sight with the next step in sight it waits for the player to look
// away (up to 6 s). Seen close by, it goes for the player at once.
void Sneak(CPed* p, Enemy& e, CPed* t, float d, uint64_t now) {
    int h = GetPedRef(p);
    CVector view = ViewOf(t), body = p->GetPosition(), rel = body - t->GetPosition();
    bool seen = InView(t, view, body);
    bool behind = rel.x * view.x + rel.y * view.y < -0.2f * std::max(0.1f, rel.Length2D());
    if (d < 4.5f && (!seen || d < 2.5f)) {  // the last steps
        if (e.routing || now - e.setOnMs > 3000) {
            e.routing = false;
            e.setOnMs = now;
            if (behind || !seen) Cmd(0x06A8).I(h).I(GetPedRef(t)).I(5000).F(0.8f).F(180.0f).Run();  // TASK_GOTO_CHAR_OFFSET: behind it
            else Cmd(0x05E2).I(h).I(GetPedRef(t)).Run();                                            // TASK_KILL_CHAR_ON_FOOT
        }
        return;
    }
    if (seen && d < 6.0f) {  // seen close by: at it
        if (e.routing || now - e.setOnMs > 3000) {
            e.routing = false;
            e.setOnMs = now;
            Cmd(0x05E2).I(h).I(GetPedRef(t)).Run();
        }
        return;
    }
    if (now < e.evalMs || !GridUsable()) return;
    e.evalMs = now + 600;
    int start = CellOf(p);
    if (start < 0) {
        if (!e.routing) Cmd(0x06A8).I(h).I(GetPedRef(t)).I(5000).F(1.0f).F(180.0f).Run(), e.routing = true, e.routeMs = now;
        return;
    }
    size_t n = g_cells.size();
    std::vector<int8_t> vis(n, -1);  // -1: not looked at yet
    auto visible = [&](int k) {
        if (vis[(size_t)k] < 0) vis[(size_t)k] = InView(t, view, g_cells[(size_t)k].p + CVector{0, 0, 1.0f}) ? 1 : 0;
        return vis[(size_t)k] == 1;
    };
    std::vector<float> cost(n, 1e9f);
    std::vector<int> from(n, -1);
    using Q = std::pair<float, int>;
    std::priority_queue<Q, std::vector<Q>, std::greater<Q>> q;
    cost[(size_t)start] = 0;
    q.push({0.0f, start});
    CVector tp = t->GetPosition();
    int goal = -1, expanded = 0;
    while (!q.empty() && expanded < 1500) {
        auto [c, k] = q.top();
        q.pop();
        if (c > cost[(size_t)k]) continue;
        ++expanded;
        CVector cp = g_cells[(size_t)k].p + CVector{0, 0, 1.0f}, r = cp - tp;
        float dist = r.Length2D();
        if (dist > 1.2f && dist < 4.0f && std::fabs(r.z) < 1.5f && r.x * view.x + r.y * view.y < -0.2f * dist && !visible(k)) {
            goal = k;
            break;
        }
        for (int m : g_cells[(size_t)k].next) {
            float nc = c + (visible(m) ? 8.0f : 1.0f);
            if (nc < cost[(size_t)m]) cost[(size_t)m] = nc, from[(size_t)m] = k, q.push({nc, m});
        }
    }
    if (goal < 0) {  // no way behind it now: round to its back the game's way
        if (!e.routing || now - e.routeMs > 4000) {
            Cmd(0x06A8).I(h).I(GetPedRef(t)).I(5000).F(1.0f).F(180.0f).Run();
            e.routing = true, e.routeMs = now, e.routeTo = tp;
        }
        return;
    }
    std::vector<int> path;  // start .. goal
    for (int k = goal; k >= 0; k = from[(size_t)k]) path.push_back(k);
    std::reverse(path.begin(), path.end());
    // the next spot: the farthest of the next 6 it sees straight
    int next = path.size() > 1 ? path[1] : path[0];
    for (size_t s = 1; s < path.size() && s <= 6; ++s) {
        if (!Straight(body, g_cells[(size_t)path[s]].p)) break;
        next = path[s];
    }
    if (!seen && visible(next) && (path.size() < 2 || visible(path[1]))) {  // one step on it would be seen: it waits
        if (!e.waitSince) e.waitSince = now;
        if (now - e.waitSince < 6000) {
            if (e.routing) Cmd(0x05BA).I(h).I(1000).Run(), e.routing = false;  // TASK_STAND_STILL
            return;
        }
    } else {
        e.waitSince = 0;
    }
    GoTo(p, e, g_cells[(size_t)next].p, 7);  // sprinting
}

// A sniper stays where it is and shoots at whom it sees; out of sight of the players for 5 s it moves on to a spot
// (within 25 m) far from its player that sees it
void Snipe(CPed* p, Enemy& e, CPed* t, bool sees, uint64_t now) {
    int h = GetPedRef(p);
    if (e.mode == M_RELOCATE) {
        if ((p->GetPosition() - e.routeTo - CVector{0, 0, 1.0f}).Length2D() < 1.3f || now > e.modeUntil) {
            e.mode = M_FIGHT;
            Cmd(0x0350).I(h).I(1).Run();  // SET_CHAR_STAY_IN_SAME_PLACE
            SetOn(p, e, t);
        }
        return;
    }
    if (sees) {
        e.blindSince = 0;
        if (t != e.after || now - e.setOnMs > 8000) SetOn(p, e, t);
        return;
    }
    if (!e.blindSince) e.blindSince = now;
    CVector to;
    if (now - e.blindSince > 5000 && GridUsable() && SniperSpot(p->GetPosition(), t, 25.0f, to)) {
        e.blindSince = 0;
        e.mode = M_RELOCATE;
        e.modeUntil = now + 9000;
        Cmd(0x0350).I(h).I(0).Run();
        GoTo(p, e, to - CVector{0, 0, 1.0f}, 6);
    }
}

bool FindCover(CPed* p, CPed* t, CVector& out) {
    if (!GridUsable()) return false;
    CVector eye = t->GetPosition() + CVector{0, 0, 0.6f}, here = p->GetPosition();
    float best = -1e9f;
    int tries = 0;
    for (int k : StandCells()) {
        const Cell& c = g_cells[(size_t)k];
        CVector d = c.p - here;
        if (d.Length2D() > 9.0f || std::fabs(d.z + 1.0f) > 2.0f) continue;
        float toT = (c.p - t->GetPosition()).Length();
        if (toT < 7.0f || toT > 26.0f) continue;
        bool taken = false;
        for (auto& [q, e] : g_info)
            if (q != p && (e.mode == M_COVER_GO || e.mode == M_COVER_HOLD) && (e.cover - c.p).Length() < 1.6f) taken = true;
        if (taken || ++tries > 60) continue;
        if (Clear(eye, c.p + CVector{0, 0, 0.6f}) || !Clear(eye, c.p + CVector{0, 0, 1.5f})) continue;  // hidden down, seeing up
        float score = -d.Length2D() - toT * 0.3f + Rand(0.0f, 2.0f);
        if (score > best) best = score, out = c.p;
    }
    return best > -1e8f;
}

void Unstick(CPed* p, CPed* t) {
    if (!GridUsable()) return;
    const std::vector<int>& steps = StepsFor(t);
    int here = CellOf(p, &steps);
    if (here < 0) return;
    if ((g_cells[(size_t)here].p + CVector{0, 0, 1.0f} - p->GetPosition()).Length() < 1.0f) {  // on it: two steps on
        for (int s = 0; s < 2; ++s) {
            int next = here;
            for (int n : g_cells[(size_t)here].next)
                if (steps[(size_t)n] >= 0 && steps[(size_t)n] < steps[(size_t)next] && (g_cells[(size_t)n].room || !g_cells[(size_t)next].room))
                    next = n;
            here = next;
        }
    }
    CVector to = g_cells[(size_t)here].p;
    Cmd(0x00A1).I(GetPedRef(p)).F(to.x).F(to.y).F(to.z).Run();  // SET_CHAR_COORDINATES (feet on the floor)
    LOGD("Arena: an enemy stuck for 4 s set on towards its player");
}

void Tactics() {
    uint64_t now = NowMs();
    // a squad's push: all of it at the players together
    for (auto& [p, e] : g_info) {
        Squad& sq = g_squads[(size_t)e.squad];
        if (e.role == R_KNIFE || e.role == R_SNIPER) continue;
        if (sq.nextPushMs && now >= sq.nextPushMs && (e.mode == M_FIGHT || e.mode == M_COVER_HOLD || e.mode == M_COVER_GO)) {
            e.mode = M_PUSH;
            e.modeUntil = now + (uint64_t)RandInt(7000, 11000);
            e.after = nullptr;  // set on again below
        }
    }
    for (auto& sq : g_squads)
        if (sq.nextPushMs && now >= sq.nextPushMs) sq.nextPushMs = now + (uint64_t)RandInt(18000, 26000);
    for (auto it = g_paths.begin(); it != g_paths.end();) it = now - it->second.ms > 10000 ? g_paths.erase(it) : std::next(it);

    for (CPed* p : g_enemies) {
        if (!IsPedValid(p) || p->Health() <= 0.0f) continue;
        auto it = g_info.find(p);
        if (it == g_info.end()) continue;
        Enemy& e = it->second;
        int h = GetPedRef(p);
        // its player, or a nearer one by 5 m (no going back and forth between two)
        CPed* t = e.after && IsPedValid(e.after) && e.after->Health() > 0.0f && e.after->AreaCode() == p->AreaCode() ? e.after : nullptr;
        CPed* n = NearestPlayer(p->GetPosition());
        if (!t || (n && (n->GetPosition() - p->GetPosition()).Length() + 5.0f < (t->GetPosition() - p->GetPosition()).Length())) t = n;
        if (!t) continue;
        float d = (t->GetPosition() - p->GetPosition()).Length();
        CVector eye = p->GetPosition() + CVector{0, 0, 0.6f}, at = t->GetPosition() + CVector{0, 0, 0.4f};
        bool sees = Clear(eye, at);
        // stuck: no step on in 4 s, the player out of its sight and not next to it
        bool still = e.mode == M_COVER_HOLD || (e.role == R_SNIPER && e.mode != M_RELOCATE) || (e.role == R_KNIFE && e.waitSince);
        if ((p->GetPosition() - e.lastPos).Length() > 0.7f || still || sees || d < 4.0f) {
            e.lastPos = p->GetPosition();
            e.lastMoveMs = now;
        } else if (now - e.lastMoveMs > 4000 && !e.grenadeUntil) {
            Unstick(p, t);
            e.lastPos = p->GetPosition();
            e.lastMoveMs = now;
            e.routing = false;
            if (e.mode == M_COVER_GO || e.mode == M_COVER_HOLD) e.mode = M_FIGHT;
            if (e.role != R_KNIFE) SetOn(p, e, t);
            continue;
        }
        if (e.role == R_KNIFE) {
            e.after = t;
            Sneak(p, e, t, d, now);
            continue;
        }
        if (e.role == R_SNIPER) {
            Snipe(p, e, t, sees, now);
            continue;
        }
        // the throw done: back to what it did
        if (e.grenadeUntil) {
            if (now > e.grenadeUntil && !e.throwAt) {
                e.grenadeUntil = 0;
                if (e.mode == M_COVER_HOLD) Cmd(0x05C5).I(h).I(1500).Run();  // TASK_DUCK
                else SetOn(p, e, t);
            }
            continue;
        }
        switch (e.mode) {
            case M_FLANK: {
                bool close = d < 9.0f;
                bool hurt = p->Health() < e.spawnHealth - 1.0f;
                if (close || hurt || now > e.modeUntil) {
                    e.mode = M_FIGHT;
                    SetOn(p, e, t);
                    continue;
                }
                break;
            }
            case M_FIGHT:
                if (sees && d < 28.0f && now >= e.nextCoverMs) {
                    e.nextCoverMs = now + (uint64_t)RandInt(6000, 12000);
                    if (RandInt(0, 99) < 60 && FindCover(p, t, e.cover)) {
                        e.mode = M_COVER_GO;
                        e.modeUntil = now + 6000;
                        e.routing = false;
                        Cmd(0x05D3).I(h).F(e.cover.x).F(e.cover.y).F(e.cover.z).I(6).I(6000).Run();  // TASK_GO_STRAIGHT_TO_COORD, running
                        continue;
                    }
                }
                break;
            case M_COVER_GO:
                if ((p->GetPosition() - e.cover).Length2D() < 1.3f || now > e.modeUntil) {
                    e.mode = M_COVER_HOLD;
                    e.modeUntil = now + (uint64_t)RandInt(5000, 10000);
                    e.popped = false;
                    e.popUntil = now + 1200;
                    Cmd(0x05C5).I(h).I(1500).Run();  // TASK_DUCK
                }
                continue;
            case M_COVER_HOLD:
                if (now > e.modeUntil || d < 6.0f) {  // waited enough, or the player is on it: at it
                    e.mode = M_PUSH;
                    e.modeUntil = now + (uint64_t)RandInt(6000, 10000);
                    SetOn(p, e, t);
                    continue;
                }
                if (now >= e.popUntil) {
                    e.popped = !e.popped;
                    if (e.popped) {
                        Cmd(0x074D).I(h).I(GetPedRef(t)).I(2000).Run();  // TASK_SHOOT_AT_CHAR: up and shooting
                        e.popUntil = now + 2000;
                    } else {
                        Cmd(0x05C5).I(h).I(1800).Run();  // TASK_DUCK: down again
                        e.popUntil = now + (uint64_t)RandInt(1200, 2200);
                    }
                }
                break;
            case M_PUSH:
                if (now > e.modeUntil) e.mode = M_FIGHT, e.nextCoverMs = now + (uint64_t)RandInt(3000, 6000);
                break;
            default:
                break;
        }
        // the player out of sight: to it along the floor's cells; in sight: the fight (given again now and then, the
        // game's fight ends by itself when its player goes down)
        if ((e.mode == M_FIGHT || e.mode == M_PUSH) && !sees && d > 7.0f) {
            CVector wp;
            if (Waypoint(p, t, wp)) {
                e.after = t;
                GoTo(p, e, wp, 6);
                continue;
            }
        }
        if (e.mode != M_COVER_HOLD && (t != e.after || e.routing || ((e.mode == M_FIGHT || e.mode == M_PUSH) && now - e.setOnMs > 8000)))
            SetOn(p, e, t);
        // a grenade: 8-26 m, the player in sight, none of its own near the player, the arc clear (StartThrow)
        Squad& sq = g_squads[(size_t)e.squad];
        if (e.role == R_GRENADIER && e.mode != M_FLANK && e.mode != M_COVER_GO && now >= sq.nextGrenadeMs && d > 8.0f && d < 26.0f &&
            e.grenades > 0 && !p->InVehicle()) {
            bool ownNear = false;
            for (CPed* q : g_enemies)
                if (q != p && IsPedValid(q) && q->Health() > 0.0f && (q->GetPosition() - t->GetPosition()).Length() < 6.0f) ownNear = true;
            if (!ownNear && StartThrow(p, e, t)) sq.nextGrenadeMs = now + (uint64_t)RandInt(6000, 9000);
        }
    }
}

int CountLeft() {
    int n = 0;
    for (CPed* p : g_enemies) n += IsPedValid(p) && p->Health() > 0.0f;
    return n;
}

// ---------------------------------------------------------------- the way to the last ones
// When nobody has fallen for 40 s the host marks where the enemies left are (A_MARK, once a second): every game shows
// them on the radar and on the screen - a red mark over each with its distance, seen through the walls, or at the edge
// of the screen an arrow towards it when it is behind or aside.
std::vector<CVector> g_marks;
uint64_t g_lastKillMs = 0;
void SetMarks(const std::vector<CVector>& at) {
    g_marks = at;
    for (int k = 0; k < 8; ++k) {
        if (k < (int)at.size()) radar::SetIcon(9700 + k, at[(size_t)k], 19 /* ENEMYATTACK */);
        else radar::RemoveIcon(9700 + k);
    }
}

void DrawMarks() { ui::DrawWorldMarks(g_marks, 0xE0FF3030); }

void Finish(bool won) {
    g_arena.won = won;
    g_arena.endAt = NowMs() + (won ? 6000 : 1500);
    if (won) {
        int prize = 1000 * g_arena.waves;  // $1000 a wave
        PlayerInfos()[0].money += prize;
        ui::TheChat().AddSystem(Format(TR("Арена пройдена! Награда $%d. Через 6 секунд — обратно.",
                                          "The arena is won! Prize $%d. Back in 6 seconds."), prize));
    } else {
        ui::TheChat().AddSystem(TR("Арена проиграна.", "The arena is lost."));
    }
}

void Clear() {
    if (g_arena.host) ReleaseDead();
    g_enemies.clear();
    g_info.clear();
    g_squads.clear();
    g_bossPed = nullptr;
    RemoveItems();
    SetMarks({});
    if (g_arena.boss >= 0) UnloadBoss();
    if (g_knifeModel >= 0) Cmd(0x0296).I(kKnifeSlot).Run();  // UNLOAD_SPECIAL_CHARACTER
    g_knifeModel = -1;
    g_paths.clear();
    for (int k = 0; k < 16; ++k) radar::RemoveIcon(9700 + k);
}

void End(bool tell) {
    if (!g_arena.on) return;
    if (tell && g_arena.host) Send(A_END, [](Writer& w) { w.U8(g_arena.won); });
    Clear();
    bool was = g_arena.taken;
    ShutDoors(false);
    TakeBack();
    if (CPlayerPed* me = FindPlayerPed(0); me && was) me->Armour() = 0.0f;  // the armour of the battle stays there
    game::challenge::End();
    LOGI("Arena over (%s)", g_arena.won ? "won" : "lost");
    g_arena = Arena{};
}

void Start(int faction, int place, int waves) {
    g_arena = Arena{};
    g_arena.on = true;
    g_arena.host = Hosting();
    g_arena.faction = faction;
    g_arena.place = place;
    g_arena.waves = waves;
    g_arena.startMs = NowMs();
    g_arena.nextWaveMs = NowMs() + PrepMs(true);
    game::challenge::Begin();
    TakeIn();
    g_lastSpawns.clear(), g_spawnsNow.clear(), g_lastItemsAt.clear();
    if (g_arena.host) {
        StartGrid();  // the floor's cells, over the next frames (the scene's collision is in by then)
        g_arena.itemsFor = 1;
        LoadKnifeModel();
    }
    ui::TheChat().AddSystem(Format(TR("Арена: %s, %s, волн: %d. Первая волна через %d секунд — соберите оружие и броню!",
                                      "Arena: %s at %s, %d waves. The first wave in %d seconds: pick up weapons and armour!"),
                                   FactionName(faction).c_str(), PlaceName(place).c_str(), waves, (int)(PrepMs(true) / 1000)));
}

void Skip(uint8_t by);

void Handle(uint8_t from, Reader& r) {
    uint8_t op = r.U8();
    if (r.Ok() && op == A_SKIP) {  // a player asks the host to start the next wave now
        if (g_arena.on && g_arena.host) Skip(from);
        return;
    }
    if (!r.Ok() || !core::IsPlayerReady() || from != Net().HostId()) return;
    if (op == A_START) {
        uint8_t f = r.U8(), p = r.U8(), w = r.U8();
        if (!r.Ok() || f >= kFactionCount || p >= kPlaceCount || w < 1 || w > 10) return;
        if (g_arena.on) End(false);
        Start(f, p, w);
    } else if (!g_arena.on) {
        return;
    } else if (op == A_WAVE) {
        uint8_t w = r.U8(), n = r.U8();
        if (!r.Ok()) return;
        g_arena.wave = w;
        g_arena.left = n;
        ui::TheChat().AddSystem(Format(TR("Арена: волна %d из %d — врагов %d.", "Arena: wave %d of %d, %d enemies."), w, g_arena.waves, n));
    } else if (op == A_LEFT) {
        uint8_t n = r.U8();
        if (r.Ok()) g_arena.left = n;
    } else if (op == A_BOSS) {
        uint8_t b = r.U8();
        if (!r.Ok() || b >= kBossCount) return;
        g_arena.boss = b;
        LoadBoss(b);  // its copy here has its look
        ui::TheChat().AddSystem(Format(TR("Последняя волна! Босс: %s.", "The last wave! The boss: %s."), BossName(b).c_str()));
    } else if (op == A_ITEMS) {
        uint8_t n = r.U8();
        std::vector<Item> items;
        for (int k = 0; k < n; ++k) {
            Item it;
            it.model = r.U16();
            it.ammo = r.U16();
            Vec3 v = r.Vec();
            it.pos = {v.x, v.y, v.z};
            if (it.model == MODEL_ARMOUR || (it.model >= 321 && it.model <= 372)) items.push_back(it);
        }
        if (r.Ok()) MakeItems(items);
    } else if (op == A_MARK) {
        uint8_t n = r.U8();
        std::vector<CVector> at;
        for (int k = 0; k < n && k < 8; ++k) {
            Vec3 v = r.Vec();
            at.push_back({v.x, v.y, v.z});
        }
        if (r.Ok()) SetMarks(at);
    } else if (op == A_PREP) {
        uint8_t secs = r.U8();
        if (r.Ok()) ui::TheChat().AddSystem(Format(TR("Волна отбита! Следующая через %d секунд.", "The wave is beaten! The next one in %d seconds."), (int)secs));
    } else if (op == A_END) {
        uint8_t won = r.U8();
        if (!r.Ok()) return;
        Finish(won != 0);
    }
}

void Update() {
    uint64_t now = NowMs();
    game::challenge::Tick();
    if (g_arena.endAt) {
        if (now >= g_arena.endAt) End(false);
        return;
    }
    if (!g_arena.host) return;
    if (!g_gridDone && now - g_arena.startMs > 1500) GridStep();
    if (g_arena.itemsFor && (g_gridDone || now - g_arena.startMs > 6000)) {
        PlaceItems(g_arena.itemsFor);
        g_arena.itemsFor = 0;
    }
    int left = CountLeft();
    if (left != g_arena.left) {
        if (left < g_arena.left) g_lastKillMs = now;
        g_arena.left = left;
        Send(A_LEFT, [left](Writer& w) { w.U8((uint8_t)std::min(left, 255)); });
    }
    Throws();
    static uint64_t tacticsMs = 0;
    if (now - tacticsMs > 500) {
        tacticsMs = now;
        Tactics();
    }
    // nobody down for 40 s (the ones left are hiding, or stuck behind a wall): everyone is shown where they are
    static uint64_t markMs = 0;
    bool hint = g_arena.wave && left > 0 && !g_arena.nextWaveMs && now - std::max(g_arena.waveMs, g_lastKillMs) > 40000;
    if (hint && now - markMs > 1000) {
        markMs = now;
        std::vector<CVector> at;
        for (CPed* p : g_enemies)
            if (at.size() < 8 && IsPedValid(p) && p->Health() > 0.0f) at.push_back(p->GetPosition());
        SetMarks(at);
        Send(A_MARK, [&](Writer& w) {
            w.U8((uint8_t)at.size());
            for (const CVector& v : at) w.Vec({v.x, v.y, v.z});
        });
    } else if (!hint && !g_marks.empty()) {
        SetMarks({});
        Send(A_MARK, [](Writer& w) { w.U8(0); });
    }
    // everyone dead or out of the place: lost
    int area = kPlaces[g_arena.place].area;
    bool anyIn = false;
    if (CPlayerPed* me = FindPlayerPed(0)) anyIn |= me->Health() > 0.0f && me->AreaCode() == area;
    for (auto& [id, rp] : Players::Instance().All())
        if (rp.hasState) anyIn |= !(rp.state.flags & OnFootState::F_DEAD) && rp.state.area == area && now - rp.stateRecvMs < 5000;
    if (!anyIn && now - g_arena.startMs > 8000) {
        if (!g_arena.outSince) g_arena.outSince = now;
        if (now - g_arena.outSince > 4000) {
            Finish(false);
            Send(A_END, [](Writer& w) { w.U8(0); });
            g_arena.endAt = now;  // ends next frame
            return;
        }
    } else {
        g_arena.outSince = 0;
    }
    if (g_arena.wave == 0 || left == 0) {
        if (g_arena.wave >= g_arena.waves) {
            Finish(true);
            Send(A_END, [](Writer& w) { w.U8(1); });
            return;
        }
        if (!g_arena.nextWaveMs) {
            int secs = (int)(PrepMs(false) / 1000);
            g_arena.nextWaveMs = now + PrepMs(false);
            g_arena.itemsFor = g_arena.wave + 1;
            Send(A_PREP, [secs](Writer& w) { w.U8((uint8_t)secs); });
            ui::TheChat().AddSystem(Format(TR("Волна отбита! Следующая через %d секунд.", "The wave is beaten! The next one in %d seconds."), secs));
        }
        if (now >= g_arena.nextWaveMs) {
            g_arena.nextWaveMs = 0;
            SpawnWave();
        }
    }
}

// /skip: the wait before the next wave ends now (the host's game decides; another player asks it)
void Skip(uint8_t by) {
    if (!g_arena.on || g_arena.endAt || !g_arena.nextWaveMs || NowMs() >= g_arena.nextWaveMs) return;
    g_arena.nextWaveMs = NowMs();
    std::string who = by == Net().MyId() || !Net().Online() ? std::string(TR("Вы", "You")) : Net().PeerName(by);
    std::string line = Format(TR("%s: ожидание пропущено, волна начинается.", "%s skipped the wait: the wave begins."), who.c_str());
    ui::TheChat().AddSystem(line);
    LOGI("Arena: the wait skipped by player %d", by);
}

void List() {
    std::string f, p;
    for (int i = 0; i < kFactionCount; ++i) f += (i ? ", " : "") + std::string(kFactions[i].keys[0][Lang()]);
    for (int i = 0; i < kPlaceCount; ++i) p += (i ? ", " : "") + std::string(kPlaces[i].keys[0][Lang()]);
    ui::TheChat().AddSystem(TR("/arena <враги> <место|рандом> <волны 1-10>, например: /arena якудза рандом 5",
                               "/arena <enemies> <place|random> <waves 1-10>, e.g. /arena yakuza random 5"));
    ui::TheChat().AddSystem(Format(TR("Враги: %s", "Enemies: %s"), f.c_str()));
    ui::TheChat().AddSystem(Format(TR("Места: %s", "Places: %s"), p.c_str()));
}

class ArenaModule : public Module {
public:
    const char* Name() const override { return "Arena"; }
    void OnInstallHooks() override { Net().SetHandler(proto::G_ARENA, &Handle); }
    void OnGameShutdown() override {
        if (g_arena.on) game::challenge::End();
        g_info.clear();
        g_squads.clear();
        g_enemies.clear();
        g_bossPed = nullptr;
        g_arena = Arena{};
    }
    void OnSessionEnd() override {
        if (g_arena.on) End(false);
    }
    void OnPostProcess() override {
        if (g_arena.on && core::IsPlayerReady()) Update();
    }
    void OnRender() override {
        if (!g_arena.on || MenuActive()) return;
        DrawMarks();
        auto& g = ui::Gfx();
        float s = g.Scale();
        std::string line;
        if (g_arena.endAt) line = g_arena.won ? TR("АРЕНА ПРОЙДЕНА", "ARENA WON") : TR("АРЕНА ПРОИГРАНА", "ARENA LOST");
        else if (!g_arena.wave) line = Format(TR("АРЕНА · %s · ПРОТИВНИК НА ПОДХОДЕ", "ARENA · %s · THE ENEMY IS COMING"), FactionName(g_arena.faction).c_str());
        else
            line = Format(TR("АРЕНА · %s · ВОЛНА %d/%d · ВРАГОВ: %d", "ARENA · %s · WAVE %d/%d · ENEMIES: %d"), FactionName(g_arena.faction).c_str(),
                          g_arena.wave, g_arena.waves, g_arena.left);
        g.Text(ui::FONT_NORMAL, g.Width() * 0.5f, 14 * s, line, 0xFFFFD060, ui::TEXT_CENTER | ui::TEXT_OUTLINE);
        if (g_arena.wave == g_arena.waves && g_arena.boss >= 0 && !g_arena.endAt) {
            std::string boss = Format(TR("БОСС: %s", "BOSS: %s"), BossName(g_arena.boss).c_str());
            if (g_bossPed && IsPedValid(g_bossPed) && g_bossPed->Health() > 0.0f)
                boss += Format(" · %.0f", g_bossPed->Health());
            g.Text(ui::FONT_SMALL, g.Width() * 0.5f, 14 * s + g.LineHeight(ui::FONT_NORMAL), boss, 0xFFFF7060, ui::TEXT_CENTER | ui::TEXT_OUTLINE);
        }
    }
    bool OnCommand(const std::string& cmd, const std::vector<std::string>& args) override {
        std::string c = LowerRu(cmd);
        bool skipCmd = Matches(c, kWordSkip);  // /skip alone too
        if (!Matches(c, kWordArena) && !skipCmd) return false;
        std::string a0 = skipCmd ? std::string("skip") : args.empty() ? "" : LowerRu(args[0]);
        if (a0.empty() || Matches(a0, kWordList) || a0 == "help") {
            List();
            return true;
        }
        if (Matches(a0, kWordSkip)) {
            if (!g_arena.on) ui::TheChat().AddError(TR("Арена не идёт.", "No arena is on."));
            else if (Hosting()) Skip(Net().MyId());
            else Send(A_SKIP);
            return true;
        }
        if (!Hosting()) {
            ui::TheChat().AddError(TR("Арену начинает хост.", "The host starts an arena."));
            return true;
        }
        if (Matches(a0, kWordStop)) {
            if (!g_arena.on) ui::TheChat().AddError(TR("Арена не идёт.", "No arena is on."));
            else {
                g_arena.won = false;
                End(true);
                ui::TheChat().AddSystem(TR("Арена остановлена.", "The arena is stopped."));
            }
            return true;
        }
        if (g_arena.on) {
            ui::TheChat().AddError(TR("Арена уже идёт (/arena stop — закончить).", "An arena is on already (/arena stop ends it)."));
            return true;
        }
        CPlayerPed* me = FindPlayerPed(0);
        if (!me || me->Health() <= 0.0f) return true;
        int faction = -1;
        for (int i = 0; i < kFactionCount; ++i)
            if (Matches(a0, kFactions[i].keys)) faction = i;
        if (faction < 0) {
            ui::TheChat().AddError(Format(TR("Нет таких врагов: %s (/arena list).", "No such enemies: %s (/arena list)."), args[0].c_str()));
            return true;
        }
        std::string a1 = args.size() > 1 ? LowerRu(args[1]) : "random";
        int place = -1;
        if (Matches(a1, kWordRandom) || Matches(a1, kWordAny)) place = RandInt(0, kPlaceCount - 1);
        else
            for (int i = 0; i < kPlaceCount; ++i)
                if (Matches(a1, kPlaces[i].keys)) place = i;
        if (place < 0) {
            ui::TheChat().AddError(Format(TR("Нет такого места: %s (/arena list).", "No such place: %s (/arena list)."), args[1].c_str()));
            return true;
        }
        int waves = args.size() > 2 ? std::atoi(args[2].c_str()) : 5;
        if (waves < 1 || waves > 10) {
            ui::TheChat().AddError(TR("Волн — от 1 до 10.", "Waves: 1 to 10."));
            return true;
        }
        Send(A_START, [&](Writer& w) {
            w.U8((uint8_t)faction);
            w.U8((uint8_t)place);
            w.U8((uint8_t)waves);
        });
        Start(faction, place, waves);
        LOGI("Arena started: faction %d, place %d, %d waves", faction, place, waves);
        return true;
    }
};

ArenaModule g_module;
struct Reg {
    Reg() { core::RegisterModule(&g_module); }
} g_reg;
}  // namespace

}  // namespace coop
