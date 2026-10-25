// Population control for co-op:
//  * where players are together, one game generates the random peds, traffic and parked cars for all of them (they see
//    its population through Peds/Vehicles sync): the host's, else the lowest-numbered one's that generates its own;
//    a wanted player's game makes its own (police chases);
//  * entities owned by this game are not removed while the other player is close to them
//    (CPopulation::ManagePed / CCarCtrl::PossiblyRemoveVehicle measure distance to the nearest player);
//  * farther from this game's player, an ordinary ped or car the other player is nearer to goes to the other game
//    instead of being dropped here (it vanished in front of the other player);
//  * no random Ballas on Grove Street (Ganton);
//  * a player going into an interior leaves its people and cars near the others to their games (they do not vanish);
//  * the game's new passers-by come a little farther off: in sight 46-53 m away instead of 42.5-50.5, and out of
//    sight 37-46 m away instead of 15-25 (CPopulation::AddToPopulation): they no longer appear right behind a player
//    who looks away and back. Out of sight they are kept to 50 m (the game dropped them past 25 m within 4 s).
//    Nobody new comes nearer than 42 m to a player, in sight or not, and none goes within 40 m of one (FreeRandomPed of
//    Peds frees only one farther off).
#include <algorithm>
#include <cmath>

#include "features/population/Population.h"
#include "common/api/protocol/Protocol.h"
#include "common/lib/log/Log.h"
#include "common/lib/util/Util.h"
#include "entities/ped/Peds.h"
#include "entities/mission/Mission.h"
#include "entities/player/Players.h"
#include "entities/vehicle/Vehicles.h"
#include "entities/world-share/WorldShare.h"
#include "shared/api/net-session/Session.h"
#include "shared/game/Game.h"
#include "shared/lib/core/Core.h"
#include "shared/lib/memory/Memory.h"

namespace coop {

namespace {
using namespace game;

// a player closer than this to one whose game generates: that population (spawned 40-55 m around it) covers both
constexpr float kShareRadius = 60.0f;
constexpr float kShareRadiusOff = 90.0f;  // hysteresis
// Traffic spawns farther out (100 m and more around its player) and drives through: where two games made their own
// within that reach, each one's cars met the other's copies, which go by the network and give way to nobody, and they
// crashed into each other at the junctions. The cars of one game give way to each other as in the single-player game.
constexpr float kCarShareRadius = 110.0f;
constexpr float kCarShareRadiusOff = 160.0f;

// Grove Street's own neighbourhood (Ganton, both its zones): random gang members made around a player there are the
// Families' own, never Ballas (CPopCycle::PickGangToCreateMembersOf picks the gang of both people and gang cars by the
// zone's densities and how many of each are around already, and gave Ballas on the cul-de-sac). The story's and the
// gang wars' Ballas are made by scripts and stay.
constexpr float kGantonMinX = 2220.0f, kGantonMaxX = 2640.0f, kGantonMinY = -1855.0f, kGantonMaxY = -1600.0f;
constexpr int PED_TYPE_GANG1 = 7, PED_TYPE_GANG2 = 8;  // Ballas, Grove Street Families
using PickGangFn = int(__cdecl*)();
PickGangFn g_origPickGang = nullptr;
int __cdecl HookPickGang() {
    int gang = g_origPickGang();
    if (gang != PED_TYPE_GANG1) return gang;
    CPlayerPed* me = FindPlayerPed(0);
    if (!me || me->AreaCode() != 0) return gang;
    CVector p = me->GetPosition();
    if (p.x < kGantonMinX || p.x > kGantonMaxX || p.y < kGantonMinY || p.y > kGantonMaxY) return gang;
    return PED_TYPE_GANG2;
}

bool g_suppress = false;      // the passers-by are another player's game's
bool g_suppressCars = false;  // and the traffic and parked cars
// in an interior only the game of the player who came in first makes its people: the second one filled the attractors
// the copies stand on (copies use none) before they were even spawned, and the host's generation never stopped
bool g_guest = false;
// partner: the host's game made the people of this interior (G_INTERIOR_POP)
int8_t g_hostPopArea = 0;
CVector g_hostPopAt;
uint64_t g_hostPopMs = 0;

bool OtherPlayerInside(const CVector& at);

int LocalWantedLevel() {
    void* wanted = Call<0x56E230, void*>(-1);  // FindPlayerWanted
    return wanted ? Field<int32_t>(wanted, 0x2C) : 0;
}

// A game leaves the population to a player before it (MakesBefore) whose game makes its own (its state says so) close
// by; one that leaves it too does not count, so a line of players apart gets people all along.
void UpdateSuppression() {
    bool want = false, wantCars = false;
    uint8_t by = 255;
    if (Net().Online() && !Net().IsHost() && core::IsPlayerReady() && LocalWantedLevel() == 0) {
        CPlayerPed* me = FindPlayerPed(0);
        float limit = g_suppress ? kShareRadiusOff : kShareRadius, carLimit = g_suppressCars ? kCarShareRadiusOff : kCarShareRadius;
        for (auto& [id, rp] : Players::Instance().All()) {
            if (!rp.hasState || !(rp.state.flags & OnFootState::F_POPULATION) || !MakesBefore(id) ||
                NowMs() - rp.stateRecvMs > 3000 || rp.state.area != me->AreaCode())
                continue;
            float d = (rp.state.pos - me->GetPosition()).Length2D();
            if (d < carLimit) wantCars = true;
            if (d < limit) {
                want = true;
                by = id;
            }
        }
    }
    if (Net().Online() && core::IsPlayerReady()) {  // an interior another game populated first: its people, not ours
        CPlayerPed* me = FindPlayerPed(0);
        if (me->AreaCode() == 0) g_guest = false;
        else want = want || (g_guest && OtherPlayerInside(me->GetPosition()));
        // indoors during the host's mission: its interiors hold who its script put there. The game fills a house's chairs
        // and television (its attractors) by the density, and "Home Invasion" puts it back to 1 while the host is out:
        // the partner alone in the house had people standing about
        if (!Net().IsHost() && me->AreaCode() != 0 && HostMissionRecent(0)) want = true;
    }
    if (want != g_suppress)
        LOGI("Population generation %s", want ? (by != 255 ? Format("handed to player %d", by).c_str() : "the interior's other game's") : "local");
    if (wantCars != g_suppressCars) LOGI("Traffic generation %s", wantCars ? "handed to a nearby player's game" : "local");
    Share().population = !want;
    // The engine adds one ped a frame at the edge of view: the empty streets around would fill slowly. The countdowns of
    // a game's start (and of a respawn) fill them at once, but they place people 10 m away in view and park cars next to
    // the player ("NPCs and cars pop up in front of me"): only behind a dark screen.
    // Not indoors: the countdown filled the house of "Home Invasion" with people behind its fades.
    if (g_suppress && !want && Ref<float>(addr::TheCamera + 0xBFC) > 200.0f &&  // CCamera's fade alpha
        core::IsPlayerReady() && FindPlayerPed(0)->AreaCode() == 0) {
        Ref<uint8_t>(0xC0EC23) = 2;  // CPopulation::m_CountDownToPedsAtStart
        Ref<uint8_t>(0x9690C0) = 2;  // CCarCtrl::CountDownToCarsAtStart
    }
    g_suppress = want;
    g_suppressCars = wantCars;
}

using UpdatePopFn = void(__cdecl*)(bool);
UpdatePopFn g_origPopUpdate = nullptr;
void __cdecl HookPopulationUpdate(bool generate) { g_origPopUpdate(generate && !g_suppress); }

using VoidFn = void(__cdecl*)();
VoidFn g_origGenerateRandomCars = nullptr;
void __cdecl HookGenerateRandomCars() {
    if (g_suppressCars) return;
    g_origGenerateRandomCars();
}

VoidFn g_origCarGenerators = nullptr;
void __cdecl HookCarGeneratorsProcess() {
    if (g_suppressCars) return;
    g_origCarGenerators();
}

// the other player is in the game (states keep coming): what it is given goes nowhere while it loads a world
bool CanTake(uint8_t who) {
    const RemotePlayer* rp = Players::Instance().Get(who);
    return rp && rp->ped && NowMs() - rp->stateRecvMs < 1500;
}

// nearest remote player position (same interior) within maxDist, or nullptr
const CVector* NearestRemote(const CVector& p, float maxDist, int8_t area, uint8_t* who = nullptr) {
    const CVector* best = nullptr;
    float bestD = maxDist;
    for (auto& [id, rp] : Players::Instance().All()) {
        if (!rp.hasState || rp.state.area != area) continue;
        float d = (rp.state.pos - p).Length2D();
        if (d < bestD) {
            bestD = d;
            best = &rp.state.pos;
            if (who) *who = id;
        }
    }
    return best;
}

// The other player looks at p: within 60 m of its camera and ~60 degrees of where it looks (its state carries the
// camera). The engine's culling knows only this game's camera: off it and over 25 m a ped went in 4 s, in front of the
// other player ("they vanish when my partner turns around").
bool RemoteSees(const CVector& p, int8_t area, float within = 60.0f) {
    for (auto& [id, rp] : Players::Instance().All()) {
        if (!rp.hasState || rp.state.area != area || NowMs() - rp.stateRecvMs > 1500) continue;
        CVector d = p - (rp.state.pos + rp.state.cam.sourceOffset);
        const CVector& f = rp.state.cam.front;
        float len = d.Length();
        if (len < within && d.x * f.x + d.y * f.y + d.z * f.z > len * 0.5f) return true;
    }
    return false;
}

using ManagePedFn = void(__cdecl*)(CPed*, const CVector*);
ManagePedFn g_origManagePed = nullptr;
constexpr int kTimeTillWeNeedThisPed = 0x54C;  // CPed::m_nTimeTillWeNeedThisPed
constexpr float kKeepOffScreen = 50.0f;
void __cdecl HookManagePed(CPed* ped, const CVector* centre) {
    // out of sight within 50 m: still needed (they come 37-46 m away now, AddToPopulation)
    if (ped && centre && ped->Health() > 0.0f && (ped->GetPosition() - *centre).Length2D() < kKeepOffScreen)
        Field<uint32_t>(ped, kTimeTillWeNeedThisPed) = std::max(Field<uint32_t>(ped, kTimeTillWeNeedThisPed), TimeMs() + 1000);
    // a script let it go to fade out (REMOVE_CHAR_ELEGANTLY: CPed::bFadeOut, +0x470 bit 3): kept or handed over, a
    // failed mission's Catalina stood twice at the restart
    if (Net().Online() && ped && centre && !(ped->PedFlags2() & 8)) {
        CVector p = ped->GetPosition();
        float local = (p - *centre).Length2D();
        int8_t area = AreaOf(ped);
        uint8_t who = 0;
        const CVector* r = NearestRemote(p, local, area, &who);
        // far from this game's player and clearly nearer the other one: the other game keeps it (the margin keeps a ped
        // from going back and forth between two players standing together). Past this game's ground (collision loads
        // ~120 m around its player) it goes even in view here: it would fall through the map
        if (r && local > 60.0f && (p - *r).Length2D() < local - 30.0f && CanTake(who) &&
            Peds::Instance().QueueGiveAway(ped, who, local >= 100.0f))
            return;
        // the other player still needs it (next to it, or in its view) while this game has ground under it
        if (local < 100.0f && ped->Health() > 0.0f && ((r && (p - *r).Length2D() < 50.0f) || RemoteSees(p, area))) return;
        if (r) {
            g_origManagePed(ped, r);
            return;
        }
    }
    g_origManagePed(ped, centre);
}

// (The original's hand-over at a door, CEntryExit::TransitionFinished -> RemoveAllRandomPeds, is ours below:
// HookRemoveAllRandomPeds hands the people and the cars over, Peds::HandOverLeftBehind.)

using PossiblyRemoveFn = void(__cdecl*)(CVehicle*);
PossiblyRemoveFn g_origPossiblyRemove = nullptr;
void __cdecl HookPossiblyRemoveVehicle(CVehicle* v) {
    // kept for the other player while this game has ground under it (see HookManagePed), then given to it; a wreck
    // goes as usual (kept, they filled the vehicle pool around two players)
    uint8_t who = 0;
    // with its people, it does not go within 40 m of this game's player either
    if (v && v->Status() != 5 /* STATUS_WRECKED */ && v->Driver() && FindPlayerPed(0) &&
        (v->GetPosition() - FindPlayerPed(0)->GetPosition()).Length() < 40.0f)
        return;
    if (Net().Online() && v && v->Status() != 5 /* STATUS_WRECKED */)
        if (const CVector* r = NearestRemote(v->GetPosition(), 140.0f, v->AreaCode(), &who)) {
            float local = (v->GetPosition() - FindPlayerPed(0)->GetPosition()).Length2D();
            if (local < 100.0f) return;
            if ((v->GetPosition() - *r).Length2D() < local - 40.0f && CanTake(who) && Vehicles::Instance().QueueGiveAway(v, who))
                return;
        }
    g_origPossiblyRemove(v);
}

// The people of an entrance's interior (CEntryExit::GenerateAmbientPeds: a police station's officers, a bar's
// customers) come in the game of the player who is there first, and the other player sees that game's (Peds): each game
// had its own, and none of the other's.
// `at`: where in the interior (one area holds several interiors far apart)
bool OtherPlayerInside(const CVector& at) {
    int area = Ref<int32_t>(0xB72914);  // CGame::currArea: the interior this game's player is in (or just came into)
    if (!Net().Online() || !core::IsPlayerReady() || area == 0) return false;
    for (auto& [id, rp] : Players::Instance().All())
        if (rp.hasState && rp.state.area == area && (rp.state.pos - at).Length() < 80.0f) return true;
    return false;
}

bool HostPopulated(int8_t area, const CVector& at) {
    return !Net().IsHost() && NowMs() - g_hostPopMs < 5000 && g_hostPopArea == area && (g_hostPopAt - at).Length() < 80.0f;
}

// A house's people (InteriorGroup_c::SetupPeds, its family about the furniture) during the host's mission: none here.
using SetupPedsFn = void(__fastcall*)(void*, void*);
SetupPedsFn g_origSetupPeds = nullptr;
void __fastcall HookSetupPeds(void* group, void* edx) {
    if (Net().Online() && !Net().IsHost() && HostMissionRecent(0)) return;
    g_origSetupPeds(group, edx);
}

using PopulateInteriorFn = void(__cdecl*)(int, CVector);
PopulateInteriorFn g_origPopulateInterior = nullptr;
void __cdecl HookPopulateInterior(int count, CVector at) {  // CEntryExit::GenerateAmbientPeds, on coming in
    int8_t area = (int8_t)Ref<int32_t>(0xB72914);  // CGame::currArea
    // our player is still at the door outside, `at` is in the interior
    g_guest = OtherPlayerInside(at) || HostPopulated(area, at);
    if (g_guest) {
        LOGI("The interior's people are the other game's (%d not made here)", count);
        return;
    }
    // the host's mission runs: its interiors hold who its script put there ("Home Invasion"'s house, through the door the
    // mission opens, had a family walking about in the partner's game only)
    if (!Net().IsHost() && Net().Online() && HostMissionRecent(0)) {
        LOGI("The host's mission runs: no people of our own in this interior (%d not made)", count);
        return;
    }
    g_origPopulateInterior(count, at);
    if (Net().Online() && Net().IsHost()) {  // two through the door at once: each saw the other still outside
        Writer w(proto::G_INTERIOR_POP);
        w.I8(area);
        w.Vec({at.x, at.y, at.z});
        Net().Send(proto::CH_RELIABLE, w);
    }
}

// CPopulation::Update keeps the interior's attractors filled (Ammu-Nation's customers) by this game's own count: the
// other game's people there are copies on no attractor, so a player who came in second put a second one on each
using AttractorPedsFn = int(__cdecl*)(CVector, float, float, float, float, int, int);
AttractorPedsFn g_origAttractorPeds = nullptr;
int __cdecl HookAttractorPeds(CVector at, float r1, float r2, float r3, float r4, int dm, int count) {
    return OtherPlayerInside(at) ? 0 : g_origAttractorPeds(at, r1, r2, r3, r4, dm, count);
}

// We came in together with the host, each before the other's state said so: the host's people stay, ours go
void HandleInteriorPop(uint8_t from, Reader& r) {
    int8_t area = r.I8();
    Vec3 v = r.Vec();
    if (!r.Ok() || from != Net().HostId() || Net().IsHost() || !core::IsPlayerReady()) return;
    CVector at{v.x, v.y, v.z};
    g_hostPopArea = area, g_hostPopAt = at, g_hostPopMs = NowMs();
    CPlayerPed* me = FindPlayerPed(0);
    if (g_guest || area == 0 || me->AreaCode() != area || (me->GetPosition() - at).Length() > 80.0f) return;
    int n = 0;
    CPoolHeader* pool = PedPool();
    for (int i = 0; pool && i < pool->size; ++i) {
        if (pool->byteMap[i] < 0) continue;
        auto* ped = (CPed*)(pool->objects + (size_t)i * kPedPoolElemSize);
        NetPed* np = Peds::Instance().Find(ped);
        if (ped->IsPlayer() || ped->CreatedBy() != 1 /* PED_GAME */ || ped->InVehicle() || (np && np->owner != Net().MyId()) ||
            (ped->GetPosition() - at).Length() > 80.0f || !CallMethod<0x5DF100, bool>(ped))  // CPed::CanBeDeleted
            continue;
        Call<0x610F20>(ped);  // CPopulation::RemovePed (the host's copies of ours go as they vanish: Peds)
        ++n;
    }
    g_guest = true;
    LOGI("Came into interior %d with the host: its game's people stay, ours (%d) go", area, n);
}

// The furnished interiors (InteriorGroup_c: the shops' cashiers and customers, the houses' residents, the offices'
// workers) are each game's own. They live off their interior's own logic (the cashier who puts his hands up and pays out
// at gunpoint, the residents who wake up at a burglar), and the copy of another game's cashier reacted to nothing: the
// player who came in second could not rob the shop, nor deal with anyone in it. Every game makes its own and none is
// announced (Peds::KeepLocal).
constexpr int kGroupNumPeds = 0x36, kGroupPeds = 0x38, kGroupMaxPeds = 16;  // InteriorGroup_c::m_numPeds, m_peds[16]
using SetupPedsFn = void(__fastcall*)(void*, void*);
SetupPedsFn g_origSetupInteriorPeds = nullptr;
void __fastcall HookSetupInteriorPeds(void* group, void* edx) {  // InteriorGroup_c::Update asks again every frame
    g_origSetupInteriorPeds(group, edx);
    if (!group) return;
    int n = std::clamp<int>(Field<int8_t>(group, kGroupNumPeds), 0, kGroupMaxPeds);
    for (int i = 0; i < n; ++i)
        if (CPed* p = Field<CPed*>(group, kGroupPeds + i * 4)) Peds::Instance().KeepLocal(p);
}

// CPopulation::RemoveAllRandomPeds: on going into an interior (and out of one) the game drops all its random people,
// and the cars they drove stood empty. Near another player who stays out there they vanished in front of that player:
// those people and cars go to that player's game instead (Peds::HandOverLeftBehind), the rest go as before.
using VoidCdeclFn = void(__cdecl*)();
VoidCdeclFn g_origRemoveAllRandomPeds = nullptr;
void __cdecl HookRemoveAllRandomPeds() {
    if (Net().Online() && core::IsPlayerReady()) Peds::Instance().HandOverLeftBehind();
    g_origRemoveAllRandomPeds();
}

using AddToPopulationFn = bool(__cdecl*)(float, float, float, float);
AddToPopulationFn g_origAddToPopulation = nullptr;
bool __cdecl HookAddToPopulation(float minOn, float maxOn, float minOff, float maxOff) {
    if (maxOn > minOn && minOn > 30.0f)  // the usual call (the start of the game's spreads them from 10 m: left alone)
        minOn = std::max(42.0f, std::min(minOn * 1.08f, maxOn * 1.05f - 1.0f)), maxOn = std::max(maxOn * 1.05f, minOn + 6.0f);
    // out of sight: 15-25 m was right behind the player, and 5 of them stood there as soon as it looked back; no
    // nearer than 42 m now
    if (maxOff > minOff && minOff >= 10.0f) minOff = std::max({minOff, maxOff * 1.48f, 42.0f}), maxOff = std::max(maxOff * 1.85f, minOff + 6.0f);
    return g_origAddToPopulation(minOn, maxOn, minOff, maxOff);
}

// This game judges a new person's or car's spot by its own camera alone: the other player nearby saw people appear
// 5-45 m in front of it and cars just ahead (their copies). A spot in its view waits for the next try, as one in ours.
int g_skipped[2] = {};  // spots skipped for the other player's view: people, cars (logged every 10 s)
uint64_t g_skippedMs = 0;

bool SeenByOther(const CVector& p, float within) {
    if (!Net().Online() || !core::IsPlayerReady() || !RemoteSees(p, FindPlayerPed(0)->AreaCode(), within)) return false;
    ++g_skipped[within > 50.0f ? 1 : 0];
    if (NowMs() - g_skippedMs > 10000) {
        LOGD("Spots in the other player's view skipped: %d for people, %d for cars", g_skipped[0], g_skipped[1]);
        g_skipped[0] = g_skipped[1] = 0;
        g_skippedMs = NowMs();
    }
    return true;
}

using ClearForPedFn = bool(__cdecl*)(const CVector&, float, int, CEntity**, bool, bool, bool);
ClearForPedFn g_origClearForPed = nullptr;
bool __cdecl HookClearForPed(const CVector& p, float r, int n, CEntity** out, bool cars, bool peds, bool objects) {
    return !SeenByOther(p, 42.5f) && g_origClearForPed(p, r, n, out, cars, peds, objects);
}

using CarCoorsFn = bool(__cdecl*)(CVector, float, float, float, bool, float, float, CVector*, void*, void*, float*, bool, bool);
CarCoorsFn g_origCarCoors = nullptr;
bool __cdecl HookCarCoors(CVector at, float r, float dx, float dy, bool anyDir, float a6, float a7, CVector* out, void* node1,
                          void* node2, float* dist, bool invert, bool room) {
    return g_origCarCoors(at, r, dx, dy, anyDir, a6, a7, out, node1, node2, dist, invert, room) && !SeenByOther(*out, 90.0f);
}

class Population : public Module {
public:
    const char* Name() const override { return "Population"; }
    void OnInstallHooks() override {
        mem::HookFn(0x614720, &HookAddToPopulation, &g_origAddToPopulation, "CPopulation::AddToPopulation");
        mem::HookFn(0x6122C0, &HookRemoveAllRandomPeds, &g_origRemoveAllRandomPeds, "CPopulation::RemoveAllRandomPeds");
        mem::RedirectCallFn(0x59690F, &HookSetupInteriorPeds, &g_origSetupInteriorPeds);  // InteriorGroup_c::Update
        mem::RedirectCallFn(0x43E983, &HookPopulateInterior, &g_origPopulateInterior);    // CEntryExit::GenerateAmbientPeds
        mem::RedirectCallFn(0x616837, &HookAttractorPeds, &g_origAttractorPeds);          // CPopulation::Update
        // CPedPlacement::IsPositionClearForPed in AddToPopulation where "not clear" gives up (its other two calls use
        // the entities it lists, or go on when it says no), CCarCtrl::GenerateCarCreationCoors2 in GenerateOneRandomCar
        for (uintptr_t at : {0x614C8F, 0x6151FA}) mem::RedirectCallFn(at, &HookClearForPed, &g_origClearForPed);
        mem::RedirectCallFn(0x430455, &HookCarCoors, &g_origCarCoors);
        Net().SetHandler(proto::G_INTERIOR_POP, &HandleInteriorPop);
        mem::HookFn(0x616650, &HookPopulationUpdate, &g_origPopUpdate, "CPopulation::Update");
        mem::HookFn(0x4341C0, &HookGenerateRandomCars, &g_origGenerateRandomCars, "CCarCtrl::GenerateRandomCars");
        mem::HookFn(0x6F3F40, &HookCarGeneratorsProcess, &g_origCarGenerators, "CTheCarGenerators::Process");
        mem::HookFn(0x611FC0, &HookManagePed, &g_origManagePed, "CPopulation::ManagePed");
        mem::HookFn(0x424F80, &HookPossiblyRemoveVehicle, &g_origPossiblyRemove, "CCarCtrl::PossiblyRemoveVehicle");
        mem::HookFn(0x60F8D0, &HookPickGang, &g_origPickGang, "CPopCycle::PickGangToCreateMembersOf");
    }
    void OnPreProcess() override { UpdateSuppression(); }
    void OnSessionEnd() override {
        g_suppress = g_suppressCars = g_guest = false;
        Share().population = true;
    }
};

Population g_pop;
struct Reg {
    Reg() { core::RegisterModule(&g_pop); }
} g_reg;
}  // namespace

bool PopulationSuppressed() { return g_suppress; }

}  // namespace coop
