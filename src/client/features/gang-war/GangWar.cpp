// The gang war of Los Santos, the game mode the host picks when it creates the server (entities/gang-mode).
//
// Everyone plays a new game with its prologue left out (the "intro" mission ended as soon as it begins: no story, the
// city as a new game has it, its territories as well). Each player comes into the war at its gang's base, as one of
// its men (the gang's first player, its leader, as Sweet, Big Smoke, Ryder or Cesar), with its gang's weapons, and back
// there after a death. The streets have far fewer people and cars than usual. The gang's own people on the street
// respect its players, the other gangs hate them; players of different gangs hurt each other (Damage). The city is the
// war's: its territories in their gangs' colours on the map and the radar, the whole map known, no doors with markers,
// no weapons lying about, and a passer-by killed pays $50-150.
//
// The host's game keeps the war: every gang makes $200 of goods every 6 minutes (the Vagos sell faster; upgrades make
// more of it, and dearer; the Ballas take protection money from the shops, bars and clubs in their territories), and
// its players buy from the gang's money in its menu (F5):
//   an attack on a territory (the one the buyer stands in): four cars of the gang's men drive in from the side of its
//     base and fight the owners' two waves of defenders; the gang takes it when all of them are down and someone of
//     the gang still stands there (a free territory: when it holds it for 20 s);
//   body armour, bought once for good (its players at every start, all its men); the police bribed in an enemy
//   territory (cops there go for that gang for 5 minutes); faster goods; dearer goods.
// Its players come back at its base after every death (Players, CGameLogic::RestorePlayerStuffDuringResurrection).
// The gangs' own: Grove Street's first four attacks cost $150 with three cars, Emmet gives its players armour and once
// a weapons upgrade; the Ballas' men aim worse the more goods they sold; the Vagos have more men, with less health and
// aim; the Aztecas aim best and come on bikes, two on each, shooting from them.
// A military convoy turns up now and then: the gang whose player drives its truck to the gang's base gets better
// weapons (four levels, the Ballas three). A gang that nobody plays is played by the computer (it buys attacks and
// upgrades) or left out (its territories are free), as the host set it in the lobby. The war is won as the host set it
// there: all the territories, a number of them, or the most of them when the time is up. A gang with no territory is
// out.
#include "features/gang-war/GangWar.h"

#include <windows.h>

#include <algorithm>
#include <cmath>
#include <map>
#include <random>
#include <set>
#include <string>
#include <vector>

#include "common/api/protocol/Protocol.h"
#include "common/lib/log/Log.h"
#include "common/lib/util/Util.h"
#include "entities/gang-mode/GangMode.h"
#include "entities/ped/Peds.h"
#include "entities/player/Players.h"
#include "entities/radar-icons/RadarIcons.h"
#include "entities/session/Flow.h"
#include "entities/vehicle/Vehicles.h"
#include "shared/api/net-session/Session.h"
#include "shared/config/Config.h"
#include "shared/game/Game.h"
#include "shared/game/Script.h"
#include "shared/lib/core/Core.h"
#include "shared/lib/memory/Memory.h"
#include "shared/ui/chat/Chat.h"
#include "shared/ui/marks/Marks.h"
#include "shared/ui/renderer/Renderer.h"

namespace coop {

namespace {
using namespace game;
using namespace gangmode;
using game::script::Cmd;

std::mt19937 g_rng{(uint32_t)GetTickCount() ^ 0x6A9Bu};
int RandInt(int a, int b) { return a + (int)(g_rng() % (uint32_t)(b - a + 1)); }
float Rand(float a, float b) { return a + (b - a) * (float)(g_rng() % 10001) / 10000.0f; }
int Lang() { return Cfg().english ? 1 : 0; }

// the feature's ops of G_GANG
enum : uint8_t { OP_BUY = 10, OP_EVENT = 11, OP_GIFT = 12, OP_CAPT = 13, OP_CONVOY = 14 };
enum Item : uint8_t { IT_CAPT = 1, IT_ARMOUR, IT_BRIBE, IT_PRODUCTION, IT_QUALITY, IT_EMMET, kItems };
enum Gift : uint8_t { GIFT_ARMOUR = 1 };
// what happened, told to everyone (each game words it in its language)
enum Event : uint8_t {
    EV_BEGIN = 1, EV_INCOME, EV_BOUGHT, EV_CAPT_START, EV_CAPT_WON, EV_CAPT_LOST, EV_BRIBE, EV_LEVEL, EV_CONVOY,
    EV_CONVOY_TAKEN, EV_CONVOY_GONE, EV_OUT, EV_WIN, EV_DENIED, EV_EMMET
};
enum Denied : uint8_t { NO_MONEY = 1, NO_ZONE, OWN_ZONE, CAPT_ON, HAVE_IT, NOT_HERE, MAX_LEVEL };

constexpr uint64_t kMin = 60 * 1000;
constexpr int PED_TYPE_COP = 6, PED_TYPE_MISSION1 = 24;
constexpr int MODEL_COP = 280, MODEL_ARMY = 287, MODEL_BARRACKS = 433, MODEL_FREEWAY = 463;
constexpr float kBaseRadius = 25.0f;
const CVector kEmmet{2447.40f, -1975.70f, 13.55f};

// ---------------------------------------------------------------- prices and the gangs' own
int Price(int gang, Item item) {
    const GangState& st = State(gang);
    switch (item) {
        case IT_CAPT: return gang == GROVE && st.capts < 4 ? 150 : 500;
        case IT_ARMOUR: return 200;
        case IT_BRIBE: return gang == GROVE ? 700 : gang == BALLAS ? 350 : 550;
        case IT_PRODUCTION: return gang == VAGOS ? 600 : 1000;
        case IT_QUALITY: return gang == VAGOS ? 1000 : (st.flags & F_PRODUCTION) ? 2000 : 1000;
        default: return 0;
    }
}
const char* const kItemNames[kItems][2] = {
    {"", ""}, TRP("Капт территории", "Attack a territory"), TRP("Бронежилеты", "Body armour"), TRP("Подкуп полиции", "Bribe the police"),
    TRP("Улучшение производства", "Faster goods"), TRP("Улучшение качества", "Dearer goods"), TRP("Эммет", "Emmet")};
const char* const kItemHints[kItems][2] = {
    {"", ""},
    TRP("на территорию, где вы стоите, едут машины ваших бойцов", "your men's cars drive to the territory you stand in"),
    TRP("броня навсегда: игрокам банды при каждом появлении и всем её бойцам",
        "armour for good: the gang's players at every start and all its men"),
    TRP("полиция 5 минут охотится на банду этой территории", "the police hunt this territory's gang for 5 minutes"),
    TRP("товар производится быстрее", "the goods are made faster"), TRP("товар стоит дороже", "the goods sell for more"),
    {"", ""}};

uint64_t IncomeEvery(int gang) {
    uint64_t ms = gang == VAGOS ? 4 * kMin + 30000 : 6 * kMin;  // the Vagos sell faster
    if (State(gang).flags & F_PRODUCTION) ms = ms * 2 / 3;
    return ms;
}
int IncomeValue(int gang) { return (State(gang).flags & F_QUALITY) ? 400 : 200; }

struct Fighter {
    int health, accuracy, armour;
};
Fighter FighterOf(int gang) {
    const GangState& st = State(gang);
    Fighter f{120, 55, 25 * (st.level - 1)};
    if (gang == VAGOS) f.health = 70, f.accuracy = 30;
    if (gang == AZTECAS) f.accuracy = 75;
    if (gang == BALLAS) f.accuracy = std::max(25, 55 - 3 * (int)st.sold);  // more goods sold, worse aim
    return f;
}

// weapons by level: 22 pistol, 24 desert eagle, 28 uzi, 29 mp5, 30 ak-47, 31 m4, 32 tec-9, 16 grenades, 18 molotovs
struct Kit {
    int w[3];
};
const Kit kKits[kGangs][4] = {
    {{{24, 0, 0}}, {{22, 32, 0}}, {{22, 32, 18}}, {{22, 30, 18}}},  // Grove Street
    {{{22, 0, 0}}, {{22, 28, 0}}, {{22, 29, 16}}, {{22, 29, 16}}},  // the Ballas (the third is their last)
    {{{22, 0, 0}}, {{22, 32, 0}}, {{22, 32, 16}}, {{22, 31, 16}}},  // the Vagos
    {{{22, 0, 0}}, {{22, 28, 0}}, {{22, 28, 18}}, {{30, 28, 18}}},  // the Aztecas
};
const Kit& KitOf(int gang) { return kKits[std::clamp(gang, 0, 3)][std::clamp((int)State(gang).level, 1, 4) - 1]; }
int AmmoFor(int w) { return w == 16 || w == 18 ? 6 : w == 22 || w == 24 ? 150 : 400; }
int MainGun(int gang) {  // what its men carry: the best gun of the kit
    const Kit& k = KitOf(gang);
    int best = k.w[0];
    for (int w : k.w)
        if (w && w != 16 && w != 18) best = w;
    return best;
}
int CarOf(int gang) {
    switch (gang) {
        case GROVE: return 492;    // Greenwood
        case BALLAS: return 466;   // Glendale (four doors: four men)
        case VAGOS: return 405;    // Sentinel
        default: return MODEL_FREEWAY;  // the Aztecas ride bikes
    }
}

int GunModel(int weapon) {
    int wm = -1;
    Cmd(0x0781).I(weapon).Out(&wm).Run();  // GET_WEAPONTYPE_MODEL
    return wm;
}
bool GunReady(int weapon) {
    int wm = GunModel(weapon);
    return wm <= 0 || IsModelLoaded(wm) || LoadModelNow(wm);
}

CVector Ground(CVector p) {
    float z = -100.0f;
    Cmd(0x02CE).F(p.x).F(p.y).F(p.z + 50.0f).Out(&z).Run();  // GET_GROUND_Z_FOR_3D_COORD
    if (z > -50.0f) p.z = z;
    return p;
}
CVector RoadNear(const CVector& p) {
    float x = 0, y = 0, z = 0;
    Cmd(0x02C1).F(p.x).F(p.y).F(p.z).Out(&x).Out(&y).Out(&z).Run();  // GET_CLOSEST_CAR_NODE
    return (x || y) ? CVector{x, y, z} : p;
}
CVector WalkNear(const CVector& p) {
    float x = 0, y = 0, z = 0;
    Cmd(0x02C0).F(p.x).F(p.y).F(p.z).Out(&x).Out(&y).Out(&z).Run();  // GET_CLOSEST_CHAR_NODE
    return (x || y) ? CVector{x, y, z} : p;
}

// a player ped (this game's or a copy) and its gang
struct PlayerPed {
    CPed* ped;
    uint8_t id;
    uint8_t gang;
};
std::vector<PlayerPed> LivePlayers() {
    std::vector<PlayerPed> out;
    if (CPlayerPed* me = FindPlayerPed(0); me && me->Health() > 0.0f && me->AreaCode() == 0)
        out.push_back({me, Net().MyId(), MyGang()});
    for (auto& [id, rp] : Players::Instance().All())
        if (rp.ped && rp.hasState && !(rp.state.flags & OnFootState::F_DEAD) && rp.ped->Health() > 0.0f && rp.state.area == 0)
            out.push_back({rp.ped, id, GangOf(id)});
    return out;
}

// ---------------------------------------------------------------- what happened, in words
void SendEvent(uint8_t ev, uint8_t a = NONE, uint8_t b = NONE, int zone = -1, int32_t value = 0, int to = -1);
void ShowEvent(uint8_t ev, uint8_t a, uint8_t b, int zone, int32_t value) {
    auto& chat = ui::TheChat();
    std::string ga = Name(a), gb = Name(b), z = zone >= 0 ? TerritoryName(zone) : std::string();
    switch (ev) {
        case EV_BEGIN:
            chat.AddSystem(TR("Война банд началась! F5 — меню банды.", "The gang war is on! F5: the gang's menu."));
            break;
        case EV_INCOME:
            if (a == MyGang()) chat.AddSystem(Format(TR("Банда продала товар: +$%d.", "The gang sold its goods: +$%d."), (int)value));
            break;
        case EV_BOUGHT:
            if (a == MyGang() && value >= 1 && value < kItems)
                chat.AddSystem(Format(TR("Куплено: %s.", "Bought: %s."), kItemNames[value][Lang()]));
            break;
        case EV_CAPT_START:
            chat.AddSystem(Format(TR("%s едут на «%s» (%s).", "%s are going for %s (%s)."), ga.c_str(), z.c_str(),
                                  b == NONE ? TR("ничья", "nobody's") : gb.c_str()));
            if (b == MyGang()) chat.AddError(Format(TR("Ваш район «%s» атакуют!", "Your territory %s is under attack!"), z.c_str()));
            break;
        case EV_CAPT_WON:
            chat.AddSystem(Format(TR("%s захватили «%s».", "%s took %s."), ga.c_str(), z.c_str()));
            break;
        case EV_CAPT_LOST:
            chat.AddSystem(Format(TR("Капт «%s» провален: %s отбились.", "The attack on %s failed: %s held it."), z.c_str(),
                                  b == NONE ? TR("район", "the territory") : gb.c_str()));
            break;
        case EV_BRIBE:
            chat.AddSystem(Format(TR("Полиция зачищает «%s»: %s под ударом 5 минут.", "The police sweep %s: %s are hunted for 5 minutes."),
                                  z.c_str(), gb.c_str()));
            break;
        case EV_LEVEL:
            chat.AddSystem(Format(TR("%s: оружие уровня %d.", "%s: weapons of level %d."), ga.c_str(), (int)value));
            break;
        case EV_CONVOY:
            chat.AddSystem(Format(TR("Военный конвой с оружием у «%s»! Пригоните грузовик на базу банды.",
                                     "A military convoy with weapons near %s! Drive its truck to your gang's base."), z.c_str()));
            break;
        case EV_CONVOY_TAKEN:
            chat.AddSystem(Format(TR("%s пригнали грузовик с оружием.", "%s brought the weapons truck in."), ga.c_str()));
            break;
        case EV_CONVOY_GONE:
            chat.AddSystem(TR("Военный конвой ушёл.", "The military convoy is gone."));
            break;
        case EV_OUT:
            chat.AddSystem(Format(TR("%s выбыли из войны: у них не осталось территорий.", "%s are out of the war: no territory left."), ga.c_str()));
            break;
        case EV_WIN:
            chat.AddSystem(Format(TR("ПОБЕДА: %s! Территорий: %d.", "VICTORY: %s! Territories: %d."), ga.c_str(), (int)value));
            break;
        case EV_EMMET:
            chat.AddSystem(value ? TR("Эммет выдал броню и оружие получше.", "Emmet gave you armour and better guns.")
                                 : TR("Эммет выдал броню.", "Emmet gave you armour."));
            break;
        case EV_DENIED: {
            const char* why = "";
            switch (value) {
                case NO_MONEY: why = TR("в банке банды не хватает денег", "the gang has not enough money"); break;
                case NO_ZONE: why = TR("встаньте на территорию банды", "stand in a gang territory"); break;
                case OWN_ZONE: why = TR("это ваша территория", "it is your own territory"); break;
                case CAPT_ON: why = TR("капт уже идёт", "an attack is on already"); break;
                case HAVE_IT: why = TR("уже куплено", "bought already"); break;
                case NOT_HERE: why = TR("Эммет недалеко от Грув-стрит", "Emmet is near Grove Street"); break;
                case MAX_LEVEL: why = TR("лучше оружия у банды не будет", "the gang's weapons are the best they get"); break;
            }
            chat.AddError(Format(TR("Нельзя: %s.", "Not now: %s."), why));
            break;
        }
    }
}

void HostBuy(uint8_t player, uint8_t gang, uint8_t item, int zone);

// ---------------------------------------------------------------- this game's player in the war
bool g_entered = false;       // came into the war in this world
uint8_t g_kitLevel = 0;       // the weapons it was given
bool g_wasDead = false;
uint64_t g_lookMs = 0, g_densityMs = 0, g_emmetAskMs = 0;
bool g_specialsLoaded = false;
bool g_menu = false;

// A new game's prologue ("intro", a mission): ended as it begins, everything it took given back (control, the screen,
// the radar), and the mission flag off (the story then never moves on: no mission markers)
void EndPrologue() {
    bool found = false;
    for (void* s = Ref<void*>(0xA8B42C); s; s = Field<void*>(s, 0x00))  // CTheScripts::pActiveScripts
        if (Field<uint8_t>(s, 0xDC) && !_strnicmp((const char*)s + 0x08, "intro", 8)) found = true;  // m_bIsMission, m_szName
    if (!found) return;
    Cmd(0x0459).S8("intro").Run();  // TERMINATE_ALL_SCRIPTS_WITH_THIS_NAME
    Ref<int32_t>(0xA49960 + (uintptr_t)Ref<int32_t>(0xA476AC)) = 0;  // the script's ONMISSION (CTheScripts::OnAMissionFlag)
    Cmd(0x01B4).I(0).I(1).Run();       // SET_PLAYER_CONTROL
    Cmd(0x02A3).I(0).Run();            // SWITCH_WIDESCREEN off
    Cmd(0x02EB).Run();                 // RESTORE_CAMERA_JUMPCUT
    Cmd(0x016A).I(500).I(1).Run();     // DO_FADE in
    Cmd(0x0581).I(1).Run();            // DISPLAY_RADAR
    Cmd(0x0826).I(1).Run();            // DISPLAY_HUD
    g_entered = false;
    if (Net().IsHost()) flow::RequestFreshWorld();  // the partners get this world (the flow waited for a prologue's end)
    LOGI("Gang war: the new game's prologue left out");
}

void Teleport(const CVector& to, float heading) {
    CPlayerPed* me = FindPlayerPed(0);
    if (!me) return;
    int h = GetPedRef(me);
    if (CVehicle* in = me->VehicleIfIn()) Cmd(0x0362).I(h).F(in->GetPosition().x).F(in->GetPosition().y).F(in->GetPosition().z + 1.0f).Run();
    Cmd(0x04BB).I(0).Run();                            // SET_AREA_VISIBLE
    Cmd(0x0860).I(h).I(0).Run();                       // SET_CHAR_AREA_VISIBLE
    Cmd(0x03CB).F(to.x).F(to.y).F(to.z).Run();         // LOAD_SCENE
    CVector p = Ground(to);
    Cmd(0x00A1).I(h).F(p.x).F(p.y).F(p.z).Run();       // SET_CHAR_COORDINATES
    Cmd(0x0173).I(h).F(heading).Run();                 // SET_CHAR_HEADING
    Cmd(0x0373).Run();                                 // SET_CAMERA_BEHIND_PLAYER
}

// the leaders' special characters, in their own slots in every game (no story uses them in this mode)
void LoadSpecials() {
    if (g_specialsLoaded) return;
    void* dir = Ref<void*>(0x8E48D0);  // CStreaming::ms_pExtraObjectsDir
    for (int g = 0; g < kGangs; ++g) {
        std::string name = ToLower(Info(g).leader);
        if (!dir || !CallMethod<0x532450, void*>(dir, name.c_str())) continue;  // CDirectory::FindItem
        Call<0x409D10>((int)Info(g).leaderModel, name.c_str(), 2);  // CStreaming::RequestSpecialModel (required)
    }
    LoadAllRequestedModels(false);
    g_specialsLoaded = true;
}

uint16_t ModelFor(uint8_t player) {
    uint8_t gang = GangOf(player);
    if (gang >= kGangs) return 0;
    const GangInfo& gi = Info(gang);
    if (Leader(player) && IsModelLoaded(gi.leaderModel)) return gi.leaderModel;
    return gi.models[player % 3];
}

void Wear() {
    CPlayerPed* me = FindPlayerPed(0);
    uint16_t model = ModelFor(Net().MyId());
    if (!me || !model || me->InVehicle() || me->ModelIndex() == model || me->Health() <= 0.0f) return;
    if (model < 290 && !LoadModelNow(model)) return;
    Cmd(0x09C7).I(0).I(model).Run();  // SET_PLAYER_MODEL
    LOGI("Gang war: our player is model %d", model);
}

void GiveKit() {
    CPlayerPed* me = FindPlayerPed(0);
    uint8_t gang = MyGang();
    if (!me || gang >= kGangs) return;
    int h = GetPedRef(me);
    Cmd(0x048F).I(h).Run();  // REMOVE_ALL_CHAR_WEAPONS
    int last = 0;
    for (int w : KitOf(gang).w)
        if (w && GunReady(w)) {
            Cmd(0x01B2).I(h).I(w).I(AmmoFor(w)).Run();  // GIVE_WEAPON_TO_CHAR
            if (w != 16 && w != 18) last = w;
        }
    if (last) Cmd(0x01B9).I(h).I(last).Run();  // SET_CURRENT_CHAR_WEAPON
    g_kitLevel = State(gang).level;
}

// the street's gangs: our own respect us, the others hate us
void Relations(uint8_t mine) {
    for (int g = 0; g < kGangs; ++g) {
        int type = Info(g).pedType;
        for (int a = 0; a <= 4; ++a) Cmd(0x0747).I(a).I(type).I(0).Run();  // CLEAR_RELATIONSHIP towards PLAYER1
        Cmd(0x0746).I(g == mine ? 0 : 4).I(type).I(0).Run();              // SET_RELATIONSHIP: respect / hate
    }
}

void EnterWar() {
    CPlayerPed* me = FindPlayerPed(0);
    uint8_t gang = MyGang();
    if (!me || gang >= kGangs) return;
    LoadSpecials();
    Relations(gang);
    Teleport(gangmode::BaseSpot(Net().MyId()), Rand(0.0f, 360.0f));
    Wear();
    GiveKit();
    me->Health() = std::max(me->Health(), 100.0f);
    if (State(gang).flags & F_ARMOUR) me->Armour() = 100.0f;
    g_entered = true;
    LOGI("Gang war: our player came into the war for %s", Info(gang).name[1]);
}

// ---- the city of the war, in every game
// Few people and cars on the streets (a passer-by or a car only now and then)
constexpr float kPedDensity = 0.08f, kCarDensity = 0.10f;

// The engine's gang war statics: on, with no training mission and no zones of a mission's own, the radar and the map
// draw every territory's square in its gang's colour (CRadar::DrawRadarGangOverlay asks
// CGangWars::CanPlayerStartAGangWarHere, which with bTrainingMission allows only the training's zone: 0.5.0 set it and
// no square showed). The engine's own wars stay out: CGangWars::Update is not run in this mode (HookGangWarsUpdate).
void KeepGangWarStatics() {
    Ref<float>(0x96AB5C) = 0.0f;  // CGangWars::Provocation
    Ref<uint8_t>(0x96AB90) = 1;   // bGangWarsActive
    Ref<uint8_t>(0x96AB91) = 0;   // bTrainingMission
    Ref<int32_t>(0x96AB94) = 0;   // NumSpecificZones
}

// The whole map known from the start: the ten by ten squares of the map's fog all visited
void RevealMap() {
    for (int i = 0; i < 100; ++i) Ref<uint8_t>(0xBA3730 + i) = 1;  // CTheZones::ZonesVisited[10][10]
    Ref<int32_t>(0xBA372C) = 100;                                 // CTheZones::ZonesRevealed
}

// No doors with their yellow markers (barbers, clothes shops, burger bars, Ammu-Nations, houses): nothing of them is in
// the war. A street door's CEntryExit::bEnableAccess (+0x30, 0x4000) off: CEntryExitManager::Update neither draws its
// marker nor takes a player through. Kept off (a script or a restart turns them on).
constexpr uint16_t kEnexEnableAccess = 0x4000;
void ShutStreetDoors() {
    auto* pool = Ref<CPoolHeader*>(0x96A7D8);  // CEntryExitManager::mp_poolEntryExits, CEntryExit 0x3C bytes
    for (int i = 0; pool && i < pool->size; ++i) {
        if (pool->byteMap[i] < 0) continue;
        uintptr_t e = (uintptr_t)pool->objects + (uintptr_t)i * 0x3C;
        if (Ref<int8_t>(e + 0x32) == 0) Ref<uint16_t>(e + 0x30) &= (uint16_t)~kEnexEnableAccess;  // m_nArea 0: outside
    }
}

// No weapons lying about the city (the street's pickups that come back: their guns, models 321-372)
void ClearStreetWeapons() {
    for (int i = 0; i < 620; ++i) {
        uintptr_t p = 0x9788C0 + (uintptr_t)i * 0x20;  // CPickups::aPickUps: +0x18 model, +0x1C type
        uint8_t type = Ref<uint8_t>(p + 0x1C);
        int model = Ref<int16_t>(p + 0x18);
        if ((type == 2 || type == 15) && model >= 321 && model <= 372) CallMethod<0x4556C0>((void*)p);  // CPickup::Remove
    }
}

// A gang whose armour was bought: all its men have it, those on the street (this game's own) as well, once each
std::set<CPed*> g_armoured;
void ArmourGangMen() {
    std::set<CPed*> now;
    CPoolHeader* pool = PedPool();
    for (int i = 0; pool && i < pool->size; ++i) {
        if (pool->byteMap[i] < 0) continue;
        auto* p = (CPed*)(pool->objects + (size_t)i * kPedPoolElemSize);
        if (p->IsPlayer() || p->Health() <= 0.0f || Players::Instance().FindByPed(p)) continue;
        int gang = -1;
        for (int g = 0; g < kGangs; ++g)
            if (p->PedType() == Info(g).pedType) gang = g;
        if (gang < 0 || !(State(gang).flags & F_ARMOUR)) continue;
        now.insert(p);
        if (!g_armoured.count(p)) p->Armour() = std::max(p->Armour(), 100.0f);
    }
    g_armoured.swap(now);
}

void KeepWorld() {
    Cmd(0x03DE).F(kPedDensity).Run();  // SET_PED_DENSITY_MULTIPLIER
    Cmd(0x01EB).F(kCarDensity).Run();  // SET_CAR_DENSITY_MULTIPLIER
    KeepGangWarStatics();
    RevealMap();
    ShutStreetDoors();
    ClearStreetWeapons();
    ArmourGangMen();
}

void UpdatePlayer() {
    CPlayerPed* me = FindPlayerPed(0);
    if (!me) return;
    uint8_t gang = MyGang();
    if (gang >= kGangs) return;
    uint64_t now = NowMs();
    if (!g_entered) {
        if (me->Health() > 0.0f && !GetPad(0)->DisablePlayerControls) EnterWar();
        return;
    }
    // back at the base after a death (the game's hospital first)
    bool dead = me->Health() <= 0.0f;
    if (g_wasDead && !dead) {  // the game put it back at the base already (Players): the scene loaded, the guns
        if ((me->GetPosition() - gangmode::BaseSpot(Net().MyId())).Length2D() > 15.0f)
            Teleport(gangmode::BaseSpot(Net().MyId()), Rand(0.0f, 360.0f));
        GiveKit();
        if (State(gang).flags & F_ARMOUR) me->Armour() = 100.0f;  // the gang's armour: at every start
    }
    g_wasDead = dead;
    if (State(gang).level != g_kitLevel && !dead && !me->InVehicle()) GiveKit();
    if (now - g_lookMs > 2000) {
        g_lookMs = now;
        Wear();
    }
    if (now - g_densityMs > 3000) {
        g_densityMs = now;
        KeepWorld();
    }
    // Grove Street at Emmet's: armour (and once a weapons upgrade for the gang)
    if (gang == GROVE && !dead && !me->InVehicle() && (me->GetPosition() - kEmmet).Length() < 4.0f && now - g_emmetAskMs > 5 * kMin) {
        g_emmetAskMs = now;
        if (Net().IsHost()) HostBuy(Net().MyId(), gang, IT_EMMET, -1);
        else
            Send(OP_BUY, [](Writer& w) {
                w.U8(IT_EMMET);
                w.U16(0xFFFF);
            }, Net().HostId());
    }
}

void Buy(Item item) {
    CPlayerPed* me = FindPlayerPed(0);
    if (!me) return;
    int zone = TerritoryAt(me->GetPosition());
    Send(OP_BUY, [&](Writer& w) {
        w.U8(item);
        w.U16(zone < 0 ? 0xFFFF : (uint16_t)zone);
    }, Net().HostId());
}

// ---------------------------------------------------------------- the host's war
// what every game shows of an attack and of the convoy (the host's own too: its messages go to the others)
struct CaptView {
    bool on = false;
    uint8_t att = NONE, def = NONE, defLeft = 0, attLeft = 0;
    int zone = -1;
    CVector mid;
} g_captView;
bool g_convoyOn = false;
CVector g_convoyAt;

struct HostGang {
    uint64_t nextIncome = 0, nextAiMs = 0;
};
HostGang g_host[kGangs];
bool g_hostStarted = false;
uint64_t g_stateMs = 0;
bool g_over = false;

void SendEvent(uint8_t ev, uint8_t a, uint8_t b, int zone, int32_t value, int to) {
    if (to < 0 || to == Net().MyId()) ShowEvent(ev, a, b, zone, value);
    if (to == Net().MyId()) return;
    Send(OP_EVENT, [&](Writer& w) {
        w.U8(ev), w.U8(a), w.U8(b), w.U16(zone < 0 ? 0xFFFF : (uint16_t)zone), w.I32(value);
    }, to);
}

std::vector<int> ZonesOf(int gang) {
    std::vector<int> out;
    for (uint16_t z : Territories())
        if (OwnerOf(z) == gang) out.push_back(z);
    return out;
}

void CountZones() {
    for (int g = 0; g < kGangs; ++g) HostState(g).zones = 0;
    for (uint16_t z : Territories())
        if (uint8_t o = OwnerOf(z); o < kGangs) ++HostState(o).zones;
}

// the war begins: who plays what, the territories of Los Santos (and those of a gang left out freed), the money
void HostBegin() {
    // whoever picked nothing: the gang with the fewest players
    for (auto& [id, peer] : Net().Peers())
        if (GangOf(id) == NONE) {
            int best = 0;
            for (int g = 1; g < kGangs; ++g)
                if (PlayersOf(g) < PlayersOf(best)) best = g;
            HostAssign(id, (uint8_t)best);
        }
    if (MyGang() == NONE) {
        int best = 0;
        for (int g = 1; g < kGangs; ++g)
            if (PlayersOf(g) < PlayersOf(best)) best = g;
        HostAssign(Net().MyId(), (uint8_t)best);
    }
    std::vector<uint16_t> zones;
    std::set<int> seen;
    for (int i = 0, n = std::min<int>(Ref<int16_t>(0xBA3794), 380); i < n; ++i) {  // CTheZones::NavigationZoneArray
        uintptr_t z = 0xBA3798 + (uintptr_t)i * 0x20;
        int info = Ref<int16_t>(z + 0x1C);
        float x = (Ref<int16_t>(z + 0x10) + Ref<int16_t>(z + 0x16)) * 0.5f, y = (Ref<int16_t>(z + 0x12) + Ref<int16_t>(z + 0x18)) * 0.5f;
        if (info <= 0 || info >= 380 || seen.count(info) || x < 44.0f || x > 2997.0f || y < -2892.0f || y > -768.0f) continue;
        seen.insert(info);
        const uint8_t* d = (const uint8_t*)(0xBA1DF0 + (uintptr_t)info * 0x11);  // CTheZones::ZoneInfoArray: gang densities
        bool gangs = false;
        for (int g = 0; g < kGangs; ++g) gangs |= d[Info(g).density] > 0;
        if (gangs) zones.push_back((uint16_t)info);
    }
    // No gangs in the zones yet (a game whose script sets them later): the districts of the city instead, each the gang's
    // whose base is nearest
    bool byBases = zones.size() < 10;
    if (byBases) {
        zones.clear();
        seen.clear();
        for (int i = 0, n = std::min<int>(Ref<int16_t>(0xBA3794), 380); i < n; ++i) {
            uintptr_t z = 0xBA3798 + (uintptr_t)i * 0x20;
            int info = Ref<int16_t>(z + 0x1C);
            float w = (float)(Ref<int16_t>(z + 0x16) - Ref<int16_t>(z + 0x10)), h = (float)(Ref<int16_t>(z + 0x18) - Ref<int16_t>(z + 0x12));
            float x = (Ref<int16_t>(z + 0x10) + Ref<int16_t>(z + 0x16)) * 0.5f, y = (Ref<int16_t>(z + 0x12) + Ref<int16_t>(z + 0x18)) * 0.5f;
            if (info <= 0 || info >= 380 || seen.count(info) || w > 600.0f || h > 600.0f || x < 44.0f || x > 2997.0f || y < -2892.0f ||
                y > -768.0f)
                continue;
            seen.insert(info);
            zones.push_back((uint16_t)info);
        }
        LOGW("Gang war: no gang territories in the zones, %d districts shared out by the bases", (int)zones.size());
    }
    SetTerritories(zones);
    if (byBases)
        for (uint16_t z : zones) {
            CVector m = TerritoryMiddle(z);
            int best = 0;
            for (int g = 1; g < kGangs; ++g)
                if ((Info(g).base - m).Length2D() < (Info(best).base - m).Length2D()) best = g;
            SetOwner(z, (uint8_t)best);
        }
    for (int g = 0; g < kGangs; ++g) {
        GangState& st = HostState(g);
        st = GangState{};
        st.bank = 500;
        bool played = PlayersOf(g) > 0 || Rules().ai[g];
        st.flags = (played ? F_PLAYED : 0) | (PlayersOf(g) == 0 && Rules().ai[g] ? F_AI : 0);
        for (uint16_t z : zones)
            if (OwnerOf(z) == g) SetOwner(z, played ? (uint8_t)g : (uint8_t)NONE);  // left out: its territories are free
        g_host[g].nextIncome = NowMs() + IncomeEvery(g);
        g_host[g].nextAiMs = NowMs() + (uint64_t)RandInt(60, 120) * 1000;
    }
    // squares only for the war's territories (the new game's own radar modes of other zones off)
    for (int i = 0, n = std::clamp<int>(Ref<int16_t>(0xBA1DE8), 0, 380); i < n; ++i)  // CTheZones::TotalNumberOfZoneInfos
        if (std::find(zones.begin(), zones.end(), (uint16_t)i) == zones.end()) {
            uint8_t* z = (uint8_t*)(0xBA1DF0 + (uintptr_t)i * 0x11);
            z[0x0F] &= (uint8_t)~0x60;
        }
    CountZones();
    KeepGangWarStatics();
    g_hostStarted = true;
    g_over = false;
    uint32_t seconds = Rules().rule == WIN_TIME ? (uint32_t)Rules().minutes * 60 : 0;
    HostStart(seconds);
    SendRoster();
    SendEvent(EV_BEGIN);
    LOGI("Gang war: begins with %d territories of Los Santos", (int)zones.size());
}

// ---- the police bribed: cops in a territory hunt its gang for 5 minutes
struct Bribe {
    int zone = -1;
    uint8_t target = NONE;
    uint64_t until = 0;
    std::vector<CPed*> cops;
} g_bribe;

void EndBribe() {
    for (CPed* p : g_bribe.cops)
        if (IsPedValid(p)) Cmd(0x01C2).I(GetPedRef(p)).Run();  // MARK_CHAR_AS_NO_LONGER_NEEDED
    g_bribe = Bribe{};
}

CPed* NearestOf(const CVector& at, float within, const std::function<bool(CPed*)>& want) {
    CPed* best = nullptr;
    float bestD = within;
    CPoolHeader* pool = PedPool();
    for (int i = 0; pool && i < pool->size; ++i) {
        if (pool->byteMap[i] < 0) continue;
        auto* p = (CPed*)(pool->objects + (size_t)i * kPedPoolElemSize);
        if (p->Health() <= 0.0f || !want(p)) continue;
        float d = (p->GetPosition() - at).Length();
        if (d < bestD) bestD = d, best = p;
    }
    return best;
}

// a man of that gang: one of its players, or a person of its ped type
bool OfGang(CPed* p, int gang) {
    if (p == FindPlayerPed(0)) return MyGang() == gang;
    if (RemotePlayer* rp = Players::Instance().FindByPed(p)) return GangOf(rp->id) == gang;
    if (p->IsPlayer()) return false;
    return gang < kGangs && p->PedType() == Info(gang).pedType;
}

void StartBribe(uint8_t gang, int zone) {
    EndBribe();
    uint8_t target = OwnerOf(zone);
    CVector mid = TerritoryMiddle(zone);
    if (!LoadModelNow(MODEL_COP) || !GunReady(22) || !GunReady(25)) return;
    g_bribe.zone = zone, g_bribe.target = target, g_bribe.until = NowMs() + 5 * kMin;
    for (int k = 0; k < 6 && HasRoom(PedPool(), kKeepPeds + 2, "ped"); ++k) {
        CVector at = WalkNear(mid + CVector{Rand(-30.0f, 30.0f), Rand(-30.0f, 30.0f), 0.0f});
        int h = -1;
        Cmd(0x009A).I(PED_TYPE_COP).I(MODEL_COP).F(at.x).F(at.y).F(at.z).Out(&h).Run();  // CREATE_CHAR
        if (h < 0) continue;
        Cmd(0x01B2).I(h).I(k % 2 ? 25 : 22).I(200).Run();  // GIVE_WEAPON_TO_CHAR
        Cmd(0x02E2).I(h).I(60).Run();                       // SET_CHAR_ACCURACY
        if (CPed* p = GetPed(h)) Peds::Instance().Share(p), g_bribe.cops.push_back(p);
    }
    SendEvent(EV_BRIBE, gang, target, zone);
}

void UpdateBribe() {
    if (g_bribe.zone < 0) return;
    if (NowMs() > g_bribe.until) return EndBribe();
    for (CPed* p : g_bribe.cops) {
        if (!IsPedValid(p) || p->Health() <= 0.0f) continue;
        CPed* t = NearestOf(p->GetPosition(), 80.0f, [](CPed* q) { return OfGang(q, g_bribe.target); });
        int h = GetPedRef(p);
        if (t) Cmd(0x05E2).I(h).I(GetPedRef(t)).Run();  // TASK_KILL_CHAR_ON_FOOT
        else Cmd(0x05DE).I(h).Run();                    // TASK_WANDER_STANDARD
    }
}

// ---- an attack on a territory
// The attackers' cars (each with its men) come from 150-220 m off, from the side of their base, and drive to a road in
// the territory; there their men get out and fight. The owners defend in two waves of men about the territory's middle.
// Taken: all the defenders down (a free territory: 20 s held) with someone of the attackers still there; lost: nobody
// of the attackers left there for 15 s, or 7 minutes.
struct Capt {
    bool on = false;
    uint8_t att = NONE, def = NONE;
    int zone = -1;
    CVector mid;
    uint64_t startMs = 0, arrivedMs = 0, emptySince = 0, holdSince = 0;
    int wave = 0;  // defenders' waves made
    std::vector<int> cars;               // handles
    std::vector<CPed*> attackers, defenders;
    bool out = false;                    // the attackers got out of their cars
} g_capt;

bool Alive(CPed* p) { return p && IsPedValid(p) && p->Health() > 0.0f; }

int MakeMan(int gang, const CVector& at, int car, int seat) {  // seat: -2 on foot, -1 the driver, else a passenger seat
    const GangInfo& gi = Info(gang);
    int model = gi.models[RandInt(0, 2)];
    if (!HasRoom(PedPool(), kKeepPeds + 2, "ped") || !LoadModelNow(model)) return -1;
    int h = -1;
    if (seat == -2) Cmd(0x009A).I(gi.pedType).I(model).F(at.x).F(at.y).F(at.z).Out(&h).Run();  // CREATE_CHAR
    else if (seat == -1) Cmd(0x0129).I(car).I(gi.pedType).I(model).Out(&h).Run();                // CREATE_CHAR_INSIDE_CAR
    else Cmd(0x01C8).I(car).I(gi.pedType).I(model).I(seat).Out(&h).Run();                         // CREATE_CHAR_AS_PASSENGER
    if (h < 0) return -1;
    Fighter f = FighterOf(gang);
    int gun = MainGun(gang);
    if (GunReady(gun)) {
        Cmd(0x01B2).I(h).I(gun).I(500).Run();  // GIVE_WEAPON_TO_CHAR
        Cmd(0x01B9).I(h).I(gun).Run();         // SET_CURRENT_CHAR_WEAPON
    }
    Cmd(0x0223).I(h).I(f.health).Run();                // SET_CHAR_HEALTH
    Cmd(0x02E2).I(h).I(f.accuracy).Run();              // SET_CHAR_ACCURACY
    Cmd(0x060F).I(h).F(120.0f).Run();                  // SET_SENSE_RANGE
    Cmd(0x087E).I(h).I(0).Run();                       // SET_CHAR_DROPS_WEAPONS_WHEN_DEAD: none
    Cmd(0x060B).I(h).I(65542).Run();                   // SET_CHAR_DECISION_MAKER "Empty": no running off
    if (CPed* p = GetPed(h)) {
        p->Armour() = (State(gang).flags & F_ARMOUR) ? 100.0f : (float)f.armour;
        Peds::Instance().Share(p);
    }
    return h;
}

void EndCapt(bool won) {
    if (!g_capt.on) return;
    if (won) {
        SetOwner(g_capt.zone, g_capt.att);
        CountZones();
        SendEvent(EV_CAPT_WON, g_capt.att, g_capt.def, g_capt.zone);
    } else {
        SetFlashing(g_capt.zone, false);
        SendEvent(EV_CAPT_LOST, g_capt.att, g_capt.def, g_capt.zone);
    }
    for (CPed* p : g_capt.attackers)
        if (IsPedValid(p)) Cmd(0x01C2).I(GetPedRef(p)).Run();  // MARK_CHAR_AS_NO_LONGER_NEEDED
    for (CPed* p : g_capt.defenders)
        if (IsPedValid(p)) Cmd(0x01C2).I(GetPedRef(p)).Run();
    for (int c : g_capt.cars)
        if (GetVehicle(c)) Cmd(0x01C3).I(c).Run();  // MARK_CAR_AS_NO_LONGER_NEEDED
    Send(OP_CAPT, [](Writer& w) { w.U8(0); });
    radar::RemoveIcon(9800);
    g_captView = CaptView{};
    LOGI("Gang war: the attack of %s on zone %d %s", Info(g_capt.att).name[1], g_capt.zone, won ? "took it" : "failed");
    g_capt = Capt{};
}

void SpawnDefenders(int count) {
    if (g_capt.def >= kGangs) return;
    for (int k = 0; k < count; ++k) {
        float a = Rand(0.0f, 6.2831853f), d = Rand(5.0f, 30.0f);
        CVector at = WalkNear(g_capt.mid + CVector{std::cos(a) * d, std::sin(a) * d, 0.0f});
        int h = MakeMan(g_capt.def, at, -1, -2);
        if (CPed* p = h >= 0 ? GetPed(h) : nullptr) g_capt.defenders.push_back(p);
    }
    ++g_capt.wave;
}

bool StartCapt(uint8_t gang, int zone) {
    if (g_capt.on) return false;
    const GangInfo& gi = Info(gang);
    Capt c;
    c.on = true, c.att = gang, c.def = OwnerOf(zone), c.zone = zone, c.startMs = NowMs();
    if (c.def < kGangs && !(State(c.def).flags & F_PLAYED)) c.def = NONE;
    c.mid = Ground(RoadNear(Ground(TerritoryMiddle(zone))));
    g_capt = c;
    // the cars: from the side of the attackers' base
    CVector from = gi.base - g_capt.mid;
    from.z = 0;
    float len = from.Length();
    CVector dir = len > 1.0f ? from * (1.0f / len) : CVector{1, 0, 0};
    bool bikes = CarOf(gang) == MODEL_FREEWAY;
    int cars = gang == GROVE && State(gang).capts < 4 ? 3 : gang == VAGOS ? 5 : bikes ? 6 : 4;
    int per = bikes ? 2 : 4;
    int model = CarOf(gang);
    if (!LoadModelNow(model)) model = 567;  // a Savanna then
    if (!LoadModelNow(model)) {
        g_capt = Capt{};
        return false;
    }
    for (int k = 0; k < cars; ++k) {
        float side = (float)(k - cars / 2) * 0.12f;
        CVector d{dir.x * std::cos(side) - dir.y * std::sin(side), dir.x * std::sin(side) + dir.y * std::cos(side), 0.0f};
        CVector at = RoadNear(g_capt.mid + d * Rand(150.0f, 220.0f));
        int car = -1;
        Cmd(0x00A5).I(model).F(at.x).F(at.y).F(at.z + 1.0f).Out(&car).Run();  // CREATE_CAR
        if (car < 0) continue;
        CVector to = g_capt.mid - at;
        Cmd(0x0175).I(car).F(std::atan2(-to.x, to.y) * 57.29578f).Run();  // SET_CAR_HEADING
        if (CVehicle* v = GetVehicle(car)) Vehicles::Instance().EnsureNetworked(v);
        CVehicle* made = GetVehicle(car);
        for (int s = -1; s < per - 1 && made && s < (int)made->MaxPassengers(); ++s) {
            int h = MakeMan(gang, at, car, s);
            if (CPed* p = h >= 0 ? GetPed(h) : nullptr) g_capt.attackers.push_back(p);
        }
        Cmd(0x00AD).I(car).F(bikes ? 30.0f : 24.0f).Run();                              // SET_CAR_CRUISE_SPEED
        Cmd(0x00AE).I(car).I(2).Run();                                                   // SET_CAR_DRIVING_STYLE: round the cars
        Cmd(0x00A7).I(car).F(g_capt.mid.x).F(g_capt.mid.y).F(g_capt.mid.z).Run();       // CAR_GOTO_COORDINATES
        g_capt.cars.push_back(car);
    }
    if (g_capt.cars.empty()) {
        g_capt = Capt{};
        return false;
    }
    SpawnDefenders(g_capt.def == VAGOS ? 8 : 6);
    radar::SetIcon(9800, g_capt.mid, 19 /* ENEMYATTACK */);
    SetFlashing(zone, true);
    SendEvent(EV_CAPT_START, gang, g_capt.def, zone);
    LOGI("Gang war: %s attack zone %d (%s) with %d cars", gi.name[1], zone, TerritoryName(zone).c_str(), (int)g_capt.cars.size());
    return true;
}

bool InZone(const CVector& p) { return TerritoryAt(p) == g_capt.zone || (p - g_capt.mid).Length2D() < 45.0f; }

void UpdateCapt() {
    if (!g_capt.on) return;
    uint64_t now = NowMs();
    if (now - g_capt.startMs > 7 * kMin) return EndCapt(false);
    std::vector<PlayerPed> players = LivePlayers();
    // the cars there (or long on their way): everyone out, into the fight
    if (!g_capt.out) {
        bool there = now - g_capt.startMs > 75000;
        for (int c : g_capt.cars)
            if (CVehicle* v = GetVehicle(c); v && (v->GetPosition() - g_capt.mid).Length2D() < 35.0f) there = true;
        if (there) {
            g_capt.out = true;
            g_capt.arrivedMs = now;
            for (CPed* p : g_capt.attackers)
                if (Alive(p) && p->InVehicle()) Cmd(0x05CD).I(GetPedRef(p)).I(GetVehicleRef(p->Vehicle())).Run();  // TASK_LEAVE_CAR
        } else if (CarOf(g_capt.att) == MODEL_FREEWAY) {  // the Aztecas shoot from their bikes on the way
            for (CPed* p : g_capt.attackers) {
                if (!Alive(p) || !p->InVehicle()) continue;
                CPed* t = NearestOf(p->GetPosition(), 35.0f, [](CPed* q) { return g_capt.def < kGangs && OfGang(q, g_capt.def); });
                if (t) Cmd(0x0713).I(GetPedRef(p)).I(GetPedRef(t)).I(-1).F(0.0f).F(0.0f).F(0.0f).F(40.0f).I(4).I(0).I(80).Run();  // TASK_DRIVE_BY
            }
        }
    }
    // who goes for whom: the nearest of the other side within 120 m (players among them); with none, to the middle
    int attLeft = 0, defLeft = 0;
    for (CPed* p : g_capt.attackers) {
        if (!Alive(p)) continue;
        ++attLeft;
        if (!g_capt.out || p->InVehicle()) continue;
        CPed* t = NearestOf(p->GetPosition(), 120.0f, [](CPed* q) {
            return g_capt.def < kGangs ? OfGang(q, g_capt.def) : std::find(g_capt.defenders.begin(), g_capt.defenders.end(), q) != g_capt.defenders.end();
        });
        int h = GetPedRef(p);
        if (t) Cmd(0x05E2).I(h).I(GetPedRef(t)).Run();  // TASK_KILL_CHAR_ON_FOOT
        else if ((p->GetPosition() - g_capt.mid).Length2D() > 8.0f)
            Cmd(0x05D3).I(h).F(g_capt.mid.x + Rand(-5.0f, 5.0f)).F(g_capt.mid.y + Rand(-5.0f, 5.0f)).F(g_capt.mid.z).I(6).I(6000).Run();  // TASK_GO_STRAIGHT_TO_COORD
    }
    for (CPed* p : g_capt.defenders) {
        if (!Alive(p)) continue;
        ++defLeft;
        CPed* t = NearestOf(p->GetPosition(), 120.0f, [](CPed* q) { return OfGang(q, g_capt.att); });
        if (t) Cmd(0x05E2).I(GetPedRef(p)).I(GetPedRef(t)).Run();
    }
    // the second wave when the first is mostly down
    if (g_capt.def < kGangs && g_capt.wave == 1 && g_capt.out && defLeft <= 2) SpawnDefenders(g_capt.def == VAGOS ? 8 : 6);
    bool attHere = false;
    for (CPed* p : g_capt.attackers) attHere |= Alive(p) && g_capt.out && InZone(p->GetPosition());
    for (const PlayerPed& pp : players) attHere |= pp.gang == g_capt.att && InZone(pp.ped->GetPosition());
    if (!attHere && g_capt.out) {
        if (!g_capt.emptySince) g_capt.emptySince = now;
        if (now - g_capt.emptySince > 15000) return EndCapt(false);
    } else {
        g_capt.emptySince = 0;
    }
    bool defendersDone = g_capt.def >= kGangs ? true : g_capt.wave >= 2 && defLeft == 0;
    if (defendersDone && attHere) {
        if (g_capt.def < kGangs) return EndCapt(true);
        if (!g_capt.holdSince) g_capt.holdSince = now;
        if (now - g_capt.holdSince > 20000) return EndCapt(true);
    } else {
        g_capt.holdSince = 0;
    }
    g_captView.on = true, g_captView.att = g_capt.att, g_captView.def = g_capt.def, g_captView.zone = g_capt.zone;
    g_captView.defLeft = (uint8_t)std::min(defLeft, 255), g_captView.attLeft = (uint8_t)std::min(attLeft, 255), g_captView.mid = g_capt.mid;
    static uint64_t sentMs = 0;
    if (now - sentMs > 2000) {
        sentMs = now;
        Send(OP_CAPT, [&](Writer& w) {
            w.U8(1), w.U8(g_capt.att), w.U8(g_capt.def), w.U16((uint16_t)g_capt.zone), w.U8((uint8_t)std::min(defLeft, 255)),
                w.U8((uint8_t)std::min(attLeft, 255));
            w.Vec({g_capt.mid.x, g_capt.mid.y, g_capt.mid.z});
        });
    }
}

// ---- a purchase (a player's, or the computer's for its gang)
void HostBuy(uint8_t player, uint8_t gang, uint8_t item, int zone) {
    if (!g_hostStarted || g_over || gang >= kGangs || item < IT_CAPT || item >= kItems) return;
    GangState& st = HostState(gang);
    auto deny = [&](uint8_t why) {
        if (player != NONE) SendEvent(EV_DENIED, gang, NONE, -1, why, player);
    };
    if (item == IT_EMMET) {  // Grove Street's: free
        if (gang != GROVE) return;
        bool upgrade = !(st.flags & F_EMMET) && st.level < Info(gang).maxLevel;
        if (upgrade) {
            st.flags |= F_EMMET;
            ++st.level;
            SendEvent(EV_LEVEL, gang, NONE, -1, st.level);
        }
        Send(OP_GIFT, [](Writer& w) { w.U8(GIFT_ARMOUR); }, player);
        if (player == Net().MyId())
            if (CPlayerPed* me = FindPlayerPed(0)) me->Armour() = 100.0f;
        SendEvent(EV_EMMET, gang, NONE, -1, upgrade ? 1 : 0, player);
        SendState();
        return;
    }
    int price = Price(gang, (Item)item);
    if (st.bank < price) return deny(NO_MONEY);
    if (item == IT_CAPT || item == IT_BRIBE) {
        if (zone < 0 || std::find(Territories().begin(), Territories().end(), (uint16_t)zone) == Territories().end()) return deny(NO_ZONE);
        if (OwnerOf(zone) == gang) return deny(OWN_ZONE);
        if (item == IT_CAPT && g_capt.on) return deny(CAPT_ON);
        if (item == IT_BRIBE && OwnerOf(zone) >= kGangs) return deny(NO_ZONE);
    }
    if (item == IT_ARMOUR && (st.flags & F_ARMOUR)) return deny(HAVE_IT);
    if (item == IT_PRODUCTION && (st.flags & F_PRODUCTION)) return deny(HAVE_IT);
    if (item == IT_QUALITY && (st.flags & F_QUALITY)) return deny(HAVE_IT);
    switch (item) {
        case IT_CAPT:
            if (!StartCapt(gang, zone)) return;
            ++st.capts;
            break;
        case IT_ARMOUR:  // for good: now, at every start of its players (UpdatePlayer) and on all its men
            st.flags |= F_ARMOUR;
            for (CPed* p : g_capt.attackers)
                if (Alive(p) && g_capt.att == gang) p->Armour() = 100.0f;
            for (CPed* p : g_capt.defenders)
                if (Alive(p) && g_capt.def == gang) p->Armour() = 100.0f;
            for (auto& [id, peer] : Net().Peers())
                if (GangOf(id) == gang) Send(OP_GIFT, [](Writer& w) { w.U8(GIFT_ARMOUR); }, id);
            if (MyGang() == gang)
                if (CPlayerPed* me = FindPlayerPed(0)) me->Armour() = 100.0f;
            break;
        case IT_BRIBE: StartBribe(gang, zone); break;
        case IT_PRODUCTION: st.flags |= F_PRODUCTION; break;
        case IT_QUALITY: st.flags |= F_QUALITY; break;
    }
    st.bank -= price;
    SendEvent(EV_BOUGHT, gang, NONE, zone, item);
    SendState();
    LOGI("Gang war: %s bought item %d for $%d", Info(gang).name[1], item, price);
}

// ---- the computer's gangs: now and then an attack on the nearest enemy territory, upgrades when the money is there
void UpdateAi(int g) {
    GangState& st = HostState(g);
    HostGang& hg = g_host[g];
    if (!(st.flags & F_AI) || (st.flags & F_OUT) || NowMs() < hg.nextAiMs) return;
    hg.nextAiMs = NowMs() + (uint64_t)RandInt(40, 90) * 1000;
    if (!(st.flags & F_PRODUCTION) && st.bank >= Price(g, IT_PRODUCTION) + 500) return HostBuy(NONE, (uint8_t)g, IT_PRODUCTION, -1);
    if (!(st.flags & F_QUALITY) && st.bank >= Price(g, IT_QUALITY) + 500) return HostBuy(NONE, (uint8_t)g, IT_QUALITY, -1);
    if (!(st.flags & F_ARMOUR) && st.bank >= Price(g, IT_ARMOUR) + 600) return HostBuy(NONE, (uint8_t)g, IT_ARMOUR, -1);
    if (g_capt.on || st.bank < Price(g, IT_CAPT) || RandInt(0, 99) < 35) return;
    std::vector<int> mine = ZonesOf(g);
    std::vector<CVector> from;
    for (int z : mine) from.push_back(TerritoryMiddle(z));
    if (from.empty()) from.push_back(Info(g).base);
    int best = -1;
    float bestD = 1e9f;
    for (uint16_t z : Territories()) {
        if (OwnerOf(z) == g) continue;
        CVector m = TerritoryMiddle(z);
        float d = 1e9f;
        for (const CVector& f : from) d = std::min(d, (f - m).Length2D());
        d += Rand(0.0f, 150.0f);  // not always the very nearest
        if (d < bestD) bestD = d, best = z;
    }
    if (best >= 0) HostBuy(NONE, (uint8_t)g, IT_CAPT, best);
}

// ---- the military convoy: a truck with weapons; its driver's gang gets better ones at its base
struct Convoy {
    uint16_t netId = 0;
    int handle = -1;
    uint64_t until = 0, nextMs = 0;
} g_convoy;

void EndConvoy(bool told) {
    if (g_convoy.handle >= 0 && GetVehicle(g_convoy.handle)) Cmd(0x01C3).I(g_convoy.handle).Run();  // no longer needed
    g_convoy.netId = 0, g_convoy.handle = -1;
    g_convoy.nextMs = NowMs() + (uint64_t)RandInt(12, 16) * kMin;
    Send(OP_CONVOY, [](Writer& w) { w.U8(0); });
    radar::RemoveIcon(9801);
    g_convoyOn = false;
    if (told) SendEvent(EV_CONVOY_GONE);
}

void UpdateConvoy() {
    uint64_t now = NowMs();
    if (!g_convoy.netId) {
        if (!g_convoy.nextMs) g_convoy.nextMs = now + 8 * kMin;
        if (now < g_convoy.nextMs || !LoadModelNow(MODEL_BARRACKS) || !LoadModelNow(MODEL_ARMY)) return;
        CVector at;
        for (int t = 0; t < 12; ++t) {  // a road in the city away from every base
            at = RoadNear({Rand(300.0f, 2800.0f), Rand(-2600.0f, -900.0f), 20.0f});
            bool away = true;
            for (int g = 0; g < kGangs; ++g) away &= (Info(g).base - at).Length2D() > 400.0f;
            if (away) break;
        }
        int car = -1, crew = -1;
        Cmd(0x00A5).I(MODEL_BARRACKS).F(at.x).F(at.y).F(at.z + 1.0f).Out(&car).Run();  // CREATE_CAR
        if (car < 0) {
            g_convoy.nextMs = now + kMin;
            return;
        }
        Cmd(0x0129).I(car).I(PED_TYPE_MISSION1).I(MODEL_ARMY).Out(&crew).Run();  // CREATE_CHAR_INSIDE_CAR
        if (CPed* p = crew >= 0 ? GetPed(crew) : nullptr) Peds::Instance().Share(p);
        Cmd(0x00AD).I(car).F(12.0f).Run();  // SET_CAR_CRUISE_SPEED
        Cmd(0x00A8).I(car).Run();           // CAR_WANDER_RANDOMLY
        CVehicle* v = GetVehicle(car);
        NetVehicle* nv = v ? Vehicles::Instance().EnsureNetworked(v) : nullptr;
        g_convoy.handle = car;
        g_convoy.netId = nv ? nv->netId : 0;
        g_convoy.until = now + 10 * kMin;
        if (!g_convoy.netId) {
            EndConvoy(false);
            return;
        }
        SendEvent(EV_CONVOY, NONE, NONE, TerritoryAt(at) >= 0 ? TerritoryAt(at) : -1);
        LOGI("Gang war: a military convoy at %.0f, %.0f", at.x, at.y);
        return;
    }
    NetVehicle* nv = Vehicles::Instance().Find(g_convoy.netId);
    if (!nv || !Vehicles::Alive(*nv) || nv->veh->Status() == 5 /* wrecked */ || now > g_convoy.until) return EndConvoy(true);
    CVector at = nv->veh->GetPosition();
    radar::SetIcon(9801, at, 51 /* the truck's: RADAR_SPRITE_TRUCK */);
    g_convoyOn = true, g_convoyAt = at;
    static uint64_t sentMs = 0;
    if (now - sentMs > 1000) {
        sentMs = now;
        Send(OP_CONVOY, [&](Writer& w) {
            w.U8(1);
            w.Vec({at.x, at.y, at.z});
        });
    }
    CPed* driver = nv->veh->Driver();
    uint8_t who = NONE;
    if (driver == FindPlayerPed(0)) who = Net().MyId();
    else if (RemotePlayer* rp = driver ? Players::Instance().FindByPed(driver) : nullptr) who = rp->id;
    uint8_t gang = who != NONE ? GangOf(who) : (uint8_t)NONE;
    if (gang >= kGangs || (Info(gang).base - at).Length2D() > kBaseRadius) return;
    GangState& st = HostState(gang);
    SendEvent(EV_CONVOY_TAKEN, gang);
    if (st.level < Info(gang).maxLevel) {
        ++st.level;
        SendEvent(EV_LEVEL, gang, NONE, -1, st.level);
    }
    SendState();
    EndConvoy(false);
}

// ---- the money, the war's end
// the Ballas' protection: the shops, bars and clubs whose doors stand in their territories (the doors outside whose
// other side is one of the game's 24/7s, bars or clubs)
int ProtectedPlaces() {
    struct Shop {
        int8_t area;
        CVector at;
    };
    static const Shop kShops[] = {
        {4, {-27.38f, -27.64f, 1003.56f}}, {6, {-26.72f, -55.99f, 1003.55f}}, {10, {6.08f, -28.63f, 1003.55f}},
        {16, {-25.37f, -139.65f, 1003.55f}}, {17, {-25.39f, -185.91f, 1003.55f}}, {18, {-30.95f, -89.61f, 1003.55f}},
        {11, {502.33f, -70.68f, 998.76f}}, {17, {493.47f, -23.01f, 1000.68f}}, {2, {1213.43f, -6.68f, 1000.92f}},
        {3, {1210.26f, -29.30f, 1000.88f}}};
    auto* pool = Ref<CPoolHeader*>(0x96A7D8);  // CEntryExitManager::mp_poolEntryExits, CEntryExit 0x3C bytes
    int n = 0;
    for (int i = 0; pool && i < pool->size; ++i) {
        if (pool->byteMap[i] < 0) continue;
        uintptr_t e = (uintptr_t)pool->objects + (uintptr_t)i * 0x3C;
        uintptr_t link = Ref<uintptr_t>(e + 0x38);  // m_pLink
        if (Ref<int8_t>(e + 0x32) != 0 || !link) continue;  // a street door
        int8_t area = Ref<int8_t>(link + 0x32);
        CVector inside = DoorSpot((void*)link);
        bool shop = false;
        for (const Shop& s : kShops) shop |= s.area == area && (s.at - inside).Length() < 40.0f;
        if (shop && OwnerOf(TerritoryAt(DoorSpot((void*)e))) == BALLAS) ++n;
    }
    return n;
}

void UpdateIncome(int g) {
    GangState& st = HostState(g);
    HostGang& hg = g_host[g];
    if (!(st.flags & F_PLAYED) || (st.flags & F_OUT) || NowMs() < hg.nextIncome) return;
    hg.nextIncome = NowMs() + IncomeEvery(g);
    int value = IncomeValue(g);
    if (g == BALLAS) value += 50 * ProtectedPlaces();
    st.bank += value;
    ++st.sold;
    SendEvent(EV_INCOME, (uint8_t)g, NONE, -1, value);
}

void CheckWin() {
    int total = (int)Territories().size(), alive = 0, last = -1;
    for (int g = 0; g < kGangs; ++g) {
        GangState& st = HostState(g);
        if (!(st.flags & F_PLAYED) || (st.flags & F_OUT)) continue;
        if (st.zones == 0 && !(g_capt.on && g_capt.att == g)) {
            st.flags |= F_OUT;
            SendEvent(EV_OUT, (uint8_t)g);
            continue;
        }
        ++alive, last = g;
    }
    int winner = -1;
    if (alive == 1) winner = last;
    for (int g = 0; g < kGangs && winner < 0; ++g) {
        const GangState& st = State(g);
        if (!(st.flags & F_PLAYED) || (st.flags & F_OUT)) continue;
        if (Rules().rule == WIN_ALL && st.zones >= total && total > 0) winner = g;
        if (Rules().rule == WIN_COUNT && st.zones >= Rules().count) winner = g;
    }
    if (winner < 0 && Rules().rule == WIN_TIME && SecondsLeft() == 0) {
        int most = -1;
        for (int g = 0; g < kGangs; ++g)
            if ((State(g).flags & F_PLAYED) && (most < 0 || State(g).zones > State(most).zones)) most = g;
        winner = most;
    }
    if (winner < 0) return;
    g_over = true;
    EndCapt(false);
    EndBribe();
    SendEvent(EV_WIN, (uint8_t)winner, NONE, -1, State(winner).zones);
    HostStop();
    LOGI("Gang war: won by %s", Info(winner).name[1]);
}

void UpdateHost() {
    uint64_t now = NowMs();
    if (!g_hostStarted) {
        CPlayerPed* me = FindPlayerPed(0);
        if (me && me->Health() > 0.0f && !GetPad(0)->DisablePlayerControls && Territories().empty()) HostBegin();
        return;
    }
    if (g_over) return;
    for (int g = 0; g < kGangs; ++g) {
        UpdateIncome(g);
        UpdateAi(g);
    }
    static uint64_t fightMs = 0;
    if (now - fightMs > 1000) {
        fightMs = now;
        UpdateCapt();
        UpdateConvoy();
    }
    static uint64_t bribeMs = 0;
    if (now - bribeMs > 2000) {
        bribeMs = now;
        UpdateBribe();
    }
    if (now - g_stateMs > 2000) {
        g_stateMs = now;
        CountZones();
        CheckWin();
        SendState();
    }
}

// ---------------------------------------------------------------- every game: messages, the screen, the menu
void HandleFeature(uint8_t from, uint8_t op, Reader& r) {
    bool fromHost = from == Net().HostId();
    if (op == OP_BUY) {
        uint8_t item = r.U8();
        uint16_t zone = r.U16();
        if (r.Ok() && Net().IsHost()) HostBuy(from, GangOf(from), item, zone == 0xFFFF ? -1 : zone);
    } else if (op == OP_EVENT) {
        uint8_t ev = r.U8(), a = r.U8(), b = r.U8();
        uint16_t zone = r.U16();
        int32_t value = r.I32();
        if (r.Ok() && fromHost) ShowEvent(ev, a, b, zone == 0xFFFF ? -1 : zone, value);
    } else if (op == OP_GIFT) {
        uint8_t what = r.U8();
        if (r.Ok() && fromHost && what == GIFT_ARMOUR)
            if (CPlayerPed* me = FindPlayerPed(0); me && me->Health() > 0.0f) me->Armour() = 100.0f;
    } else if (op == OP_CAPT) {
        uint8_t on = r.U8();
        if (!r.Ok() || !fromHost) return;
        if (!on) {
            g_captView = CaptView{};
            radar::RemoveIcon(9800);
            return;
        }
        CaptView c;
        c.on = true, c.att = r.U8(), c.def = r.U8(), c.zone = r.U16(), c.defLeft = r.U8(), c.attLeft = r.U8();
        Vec3 m = r.Vec();
        if (!r.Ok()) return;
        c.mid = {m.x, m.y, m.z};
        g_captView = c;
        radar::SetIcon(9800, c.mid, 19 /* ENEMYATTACK */);
    } else if (op == OP_CONVOY) {
        uint8_t on = r.U8();
        if (!r.Ok() || !fromHost) return;
        g_convoyOn = on != 0;
        if (g_convoyOn) {
            Vec3 v = r.Vec();
            if (!r.Ok()) return;
            g_convoyAt = {v.x, v.y, v.z};
            radar::SetIcon(9801, g_convoyAt, 51);
        } else {
            radar::RemoveIcon(9801);
        }
    }
}

void DrawHud() {
    uint8_t gang = MyGang();
    if (gang >= kGangs) return;
    auto& g = ui::Gfx();
    float s = g.Scale(), W = g.Width();
    const GangState& st = State(gang);
    const GangInfo& gi = Info(gang);
    float x = W - 22 * s, y = g.Height() * 0.30f, lh = g.LineHeight(ui::FONT_SMALL);
    g.Text(ui::FONT_NORMAL, x, y, ShortName(gang), gi.argb, ui::TEXT_RIGHT | ui::TEXT_OUTLINE);
    y += g.LineHeight(ui::FONT_NORMAL);
    std::vector<std::string> lines = {
        Format(TR("Банк: $%d", "Bank: $%d"), st.bank),
        Format(TR("Районов: %d из %d", "Territories: %d of %d"), st.zones, (int)Territories().size()),
        Format(TR("Оружие: уровень %d", "Weapons: level %d"), st.level),
    };
    if (Leader(Net().MyId())) lines.push_back(TR("Вы — лидер банды", "You lead the gang"));
    if (CPlayerPed* me = FindPlayerPed(0)) {
        int zone = TerritoryAt(me->GetPosition());
        if (zone >= 0) {
            uint8_t o = OwnerOf(zone);
            lines.push_back(Format("%s · %s", TerritoryName(zone).c_str(), o < kGangs ? ShortName(o).c_str() : TR("ничей", "nobody's")));
        }
    }
    const Settings& rules = Rules();
    if (rules.rule == WIN_TIME && Started()) {
        uint32_t left = SecondsLeft();
        lines.push_back(Format(TR("До конца: %d:%02d", "Time left: %d:%02d"), (int)(left / 60), (int)(left % 60)));
    } else if (rules.rule == WIN_COUNT) {
        lines.push_back(Format(TR("Цель: %d районов", "Goal: %d territories"), (int)rules.count));
    } else {
        lines.push_back(TR("Цель: все районы", "Goal: every territory"));
    }
    lines.push_back(TR("F5 — меню банды", "F5: the gang's menu"));
    for (const std::string& l : lines) {
        g.Text(ui::FONT_SMALL, x, y, l, 0xE0FFFFFF, ui::TEXT_RIGHT | ui::TEXT_OUTLINE);
        y += lh;
    }
    // the lines at the top of the screen, as the arena's countdown: an attack, the convoy and how far it is
    float top = 14 * s;
    if (g_captView.on) {
        std::string line = Format(TR("КАПТ · %s → «%s» · защитников %d · атакующих %d", "ATTACK · %s → %s · defenders %d · attackers %d"),
                                  ShortName(g_captView.att).c_str(), TerritoryName(g_captView.zone).c_str(), g_captView.defLeft,
                                  g_captView.attLeft);
        g.Text(ui::FONT_NORMAL, W * 0.5f, top, line, g_captView.att < kGangs ? Info(g_captView.att).argb : 0xFFFFD060,
               ui::TEXT_CENTER | ui::TEXT_OUTLINE);
        top += g.LineHeight(ui::FONT_NORMAL);
        ui::DrawWorldMarks({g_captView.mid}, 0xC0FF5050, 2.0f, 0.8f);
    }
    if (g_convoyOn) {
        CPlayerPed* me = FindPlayerPed(0);
        CVehicle* in = me ? me->VehicleIfIn() : nullptr;
        bool driving = in && in->ModelIndex() == MODEL_BARRACKS && (in->GetPosition() - g_convoyAt).Length() < 15.0f;
        std::string line;
        if (driving) {
            int toBase = me ? (int)(me->GetPosition() - gi.base).Length2D() : 0;
            line = Format(TR("ВОЕННЫЙ КОНВОЙ · вы за рулём · до базы %d м", "MILITARY CONVOY · you drive it · %d m to the base"), toBase);
        } else {
            int away = me ? (int)(me->GetPosition() - g_convoyAt).Length2D() : 0;
            line = Format(TR("ВОЕННЫЙ КОНВОЙ · %d м · угоните грузовик на базу", "MILITARY CONVOY · %d m · take its truck to your base"), away);
        }
        g.Text(ui::FONT_NORMAL, W * 0.5f, top, line, 0xFFF0D030, ui::TEXT_CENTER | ui::TEXT_OUTLINE);
        if (!driving) ui::DrawWorldMarks({g_convoyAt}, 0xC0F0D030, 3.0f, 0.9f);
        else ui::DrawWorldMarks({gi.base}, ui::WithAlpha(gi.argb, 0.75f), 2.0f, 1.0f);
    }
}

void DrawMenu() {
    uint8_t gang = MyGang();
    if (!g_menu || gang >= kGangs) return;
    auto& g = ui::Gfx();
    float s = g.Scale(), w = 520 * s, x = g.Width() * 0.5f - w * 0.5f, y = g.Height() * 0.22f;
    float lh = g.LineHeight(ui::FONT_NORMAL), sh = g.LineHeight(ui::FONT_SMALL);
    float h = 30 * s + lh * 2 + (lh + sh + 6 * s) * 5 + sh * 2;
    g.Rect(x, y, w, h, 0xE0000000);
    g.RectOutline(x, y, w, h, Info(gang).argb, std::max(1.0f, s));
    float cy = y + 14 * s, cx = x + 20 * s;
    g.Text(ui::FONT_BIG, cx, cy, Name(gang), Info(gang).argb, ui::TEXT_SHADOW);
    cy += g.LineHeight(ui::FONT_BIG);
    g.Text(ui::FONT_NORMAL, cx, cy, Format(TR("В банке банды: $%d", "The gang's money: $%d"), State(gang).bank), 0xFF7FE07F, ui::TEXT_SHADOW);
    cy += lh + 8 * s;
    for (int i = IT_CAPT; i <= IT_QUALITY; ++i) {
        uint8_t fl = State(gang).flags;
        bool have = (i == IT_ARMOUR && (fl & F_ARMOUR)) || (i == IT_PRODUCTION && (fl & F_PRODUCTION)) || (i == IT_QUALITY && (fl & F_QUALITY));
        int price = Price(gang, (Item)i);
        bool can = !have && State(gang).bank >= price;
        std::string line = Format("%d  %s — %s", i, kItemNames[i][Lang()], have ? TR("куплено", "bought") : Format("$%d", price).c_str());
        g.Text(ui::FONT_NORMAL, cx, cy, line, can ? 0xFFFFFFFF : 0xFF8D9CAD, ui::TEXT_SHADOW);
        cy += lh;
        g.Text(ui::FONT_SMALL, cx + 22 * s, cy, kItemHints[i][Lang()], 0xFF8D9CAD, ui::TEXT_SHADOW);
        cy += sh + 6 * s;
    }
    g.Text(ui::FONT_SMALL, cx, cy, TR("Цифра — купить, F5 или Esc — закрыть", "A digit buys, F5 or Esc closes"), 0xFFACCBF1, ui::TEXT_SHADOW);
}

// ---------------------------------------------------------------- the engine's own, in this mode
// CGangWars::Update: the story's wars (gangs attacking the player's territories, its offensives) and their end, which
// gives every zone the colours of the story's three gangs again: not in this mode, the war is ours
using VoidFn = void(__cdecl*)();
VoidFn g_origWarsUpdate = nullptr;
void __cdecl HookGangWarsUpdate() {
    if (On() && Started()) return;
    g_origWarsUpdate();
}
// CTheZones::FillZonesWithGangColours: the squares' colours from the gangs' densities, the Ballas, Grove Street and the
// Vagos only (the Aztecas' territories went blank): the war colours its own (SetOwner)
using FillFn = void(__cdecl*)(bool);
FillFn g_origFill = nullptr;
void __cdecl HookFillZones(bool disable) {
    if (On() && Started()) return;
    g_origFill(disable);
}
// CDarkel::RegisterKillByPlayer: a passer-by killed by this game's player pays $50-150 (in tens)
using KillFn = void(__cdecl*)(CPed*, int, bool, int);
KillFn g_origKill = nullptr;
void __cdecl HookKillByPlayer(CPed* ped, int weapon, bool headshot, int player) {
    g_origKill(ped, weapon, headshot, player);
    if (!On() || !Started() || player != 0 || !ped || ped->IsPlayer() || Players::Instance().FindByPed(ped)) return;
    int type = ped->PedType();
    if (type != 4 && type != 5) return;  // PED_TYPE_CIVMALE, PED_TYPE_CIVFEMALE: the people of the street
    int money = RandInt(5, 15) * 10;
    Cmd(0x0109).I(0).I(money).Run();  // ADD_SCORE
}

class GangWar : public Module {
public:
    const char* Name() const override { return "GangWar"; }
    void OnInstallHooks() override {
        SetHandler(&HandleFeature);
        mem::HookFn(0x446610, &HookGangWarsUpdate, &g_origWarsUpdate, "CGangWars::Update");
        mem::HookFn(0x572440, &HookFillZones, &g_origFill, "CTheZones::FillZonesWithGangColours");
        mem::HookFn(0x43DCD0, &HookKillByPlayer, &g_origKill, "CDarkel::RegisterKillByPlayer");
    }
    void OnGameStarted() override {
        g_entered = false, g_wasDead = false, g_kitLevel = 0;
        g_specialsLoaded = false;
        g_armoured.clear();
    }
    void OnSessionEnd() override {
        g_entered = g_menu = false;
        g_hostStarted = g_over = false;
        g_capt = Capt{};
        g_bribe = Bribe{};
        g_convoy = Convoy{};
        g_captView = CaptView{};
        g_convoyOn = false;
        for (auto& h : g_host) h = HostGang{};
        radar::RemoveIcon(9800);
        radar::RemoveIcon(9801);
    }
    void OnPostProcess() override {
        if (!On()) return;
        EndPrologue();
        if (!core::IsPlayerReady()) return;
        if (Net().IsHost()) UpdateHost();
        if (Started()) UpdatePlayer();
    }
    void OnRender() override {
        if (!Started() || MenuActive()) return;
        DrawHud();
        DrawMenu();
    }
    bool OnCommand(const std::string& cmd, const std::vector<std::string>& args) override {
        static const char* const kWord[2] = TRP("банда", "gang");
        if (cmd != "gang" && cmd != kWord[0]) return false;
        if (!Started()) ui::TheChat().AddError(TR("Война банд не идёт.", "No gang war is on."));
        else g_menu = !g_menu;
        return true;
    }
};

GangWar g_module;
struct Reg {
    Reg() { core::RegisterModule(&g_module); }
} g_reg;
}  // namespace

namespace gangwar {
bool KeyDown(int vk) {
    if (!Started() || MenuActive() || MyGang() >= kGangs) return false;
    if (vk == VK_F5) {
        g_menu = !g_menu;
        return true;
    }
    if (!g_menu) return false;
    if (vk == VK_ESCAPE) {
        g_menu = false;
        return true;
    }
    if (vk >= '1' && vk <= '5') {
        Item item = (Item)(IT_CAPT + (vk - '1'));
        if (Net().IsHost()) {
            CPlayerPed* me = FindPlayerPed(0);
            HostBuy(Net().MyId(), MyGang(), item, me ? TerritoryAt(me->GetPosition()) : -1);
        } else {
            Buy(item);
        }
        g_menu = false;
        return true;
    }
    return false;
}
}  // namespace gangwar

}  // namespace coop
