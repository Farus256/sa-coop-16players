// The gangs' bases.
//
// Its people: 5-10 of the gang's men (more with its weapons' level, the Vagos one more) walk the pavements about its
// base, as its own people of the street do (the game's wandering; one gone too far comes back), and a dead one is
// replaced after 90 s. They are made and run by the game of the player nearest the base (within 180 m; it keeps them
// till its player is 320 m off): the host says whose (OP_GUARD), as players come and go. A game far off has no ground
// there, and its men would stand frozen.
//
// The door: each base has a way into a house, a door of the city's own near it (a house's front door within 80 m:
// its own interior behind it), else a marker at the base into a house of the gang's kind (the Johnsons', a crack den,
// the Vagos' house of "Burning Desire", Ryder's). The engine's doors are all shut in this mode (no yellow markers):
// these are the gang's own, a ring in its colour outside and inside, and only its players go through.
#include <algorithm>
#include <cmath>
#include <cstring>

#include "common/api/protocol/Protocol.h"
#include "common/lib/log/Log.h"
#include "common/lib/util/Util.h"
#include "entities/gang-mode/GangMode.h"
#include "features/gang-war/War.h"
#include "shared/api/net-session/Session.h"
#include "shared/game/Game.h"
#include "shared/game/Script.h"
#include "shared/lib/core/Core.h"

namespace coop::gangwar::war {

namespace {
using namespace game;
using namespace gangmode;
using game::script::Cmd;

// ---------------------------------------------------------------- the people about the base
constexpr float kGuardTake = 180.0f, kGuardKeep = 320.0f;
constexpr float kRoam = 70.0f;  // farther off than this from the base: back towards it
constexpr uint64_t kGuardBackMs = 90000;

uint8_t g_guardRunner[kGangs] = {NONE, NONE, NONE, NONE};  // whose game keeps each base's people (as the host says)
uint64_t g_guardSentMs = 0;

struct Guards {
    std::vector<CPed*> men;
    std::vector<uint64_t> backAt;   // when a dead one is replaced
    std::vector<uint8_t> going;     // walking back towards the base
};
Guards g_guards[kGangs];

int GuardCount(int gang) { return std::min(10, 5 + (int)State(gang).level + (gang == VAGOS ? 1 : 0)); }

// a pavement near the base: the walkers' path node nearest a point 15-50 m off
CVector Pavement(const CVector& base, float from, float to) {
    float a = Rand(0.0f, 6.2831853f), d = Rand(from, to);
    return WalkNear(base + CVector{std::cos(a) * d, std::sin(a) * d, 0.0f});
}

void Release(int gang) {
    Guards& G = g_guards[gang];
    for (CPed* p : G.men)
        if (p && IsPedValid(p)) Cmd(0x009B).I(GetPedRef(p)).Run();  // DELETE_CHAR: the new keeper's game makes its own
    G = Guards{};
}

void KeepGuards(int gang) {
    Guards& G = g_guards[gang];
    const CVector& base = Info(gang).base;
    int want = GuardCount(gang);
    for (size_t i = want; i < G.men.size(); ++i)
        if (G.men[i] && IsPedValid(G.men[i])) Cmd(0x01C2).I(GetPedRef(G.men[i])).Run();
    G.men.resize(want, nullptr);
    G.backAt.resize(want, 0);
    G.going.resize(want, 0);
    uint64_t now = NowMs();
    for (int i = 0; i < want; ++i) {
        CPed*& p = G.men[i];
        if (p && !Alive(p)) {
            if (IsPedValid(p)) Cmd(0x01C2).I(GetPedRef(p)).Run();  // MARK_CHAR_AS_NO_LONGER_NEEDED: the body goes
            p = nullptr;
            G.backAt[i] = now + kGuardBackMs;
        }
        if (!p) {
            if (now < G.backAt[i]) continue;
            CVector at = Pavement(base, 12.0f, 45.0f);
            int h = MakeMan(gang, at, -1, -2, true);
            if (h < 0) continue;
            p = GetPed(h);
            Cmd(0x05DE).I(h).Run();  // TASK_WANDER_STANDARD: along the pavement
            G.going[i] = 0;
            continue;
        }
        int h = GetPedRef(p);
        float d = (p->GetPosition() - base).Length2D();
        if (!G.going[i] && d > kRoam) {
            CVector to = Pavement(base, 5.0f, 25.0f);
            Cmd(0x05D3).I(h).F(to.x).F(to.y).F(to.z).I(4).I(60000).Run();  // TASK_GO_STRAIGHT_TO_COORD: walking back
            G.going[i] = 1;
        } else if (G.going[i] && d < 30.0f) {
            Cmd(0x05DE).I(h).Run();  // wandering again
            G.going[i] = 0;
        }
    }
}

// ---------------------------------------------------------------- the doors
// CEntryExitManager::mp_poolEntryExits, CEntryExit 0x3C bytes: +0x20 exit position, +0x2C exit angle, +0x30 flags
// (0x4000 bEnableAccess), +0x32 area, +0x38 link
constexpr uint16_t kEnexEnableAccess = 0x4000;
CPoolHeader* EnexPool() { return Ref<CPoolHeader*>(0x96A7D8); }
uintptr_t Enex(int i) { return (uintptr_t)EnexPool()->objects + (uintptr_t)i * 0x3C; }
int EnexIndex(uintptr_t e) {
    CPoolHeader* pool = EnexPool();
    if (!pool || e < (uintptr_t)pool->objects) return -1;
    int i = (int)((e - (uintptr_t)pool->objects) / 0x3C);
    return i < pool->size && pool->byteMap[i] >= 0 ? i : -1;
}

struct Spot {
    CVector at;
    float heading = 0.0f;
};
struct BaseDoor {
    bool found = false;
    int inIdx = -1;      // the interior's own door (shut: ours stands at it)
    CVector outMark, inMark;
    Spot outArrive, inArrive;
    int8_t inArea = 0;
    bool outGround = false, inGround = false;  // the markers' height looked up (the ground is there)
};
BaseDoor g_doors[kGangs];
bool g_doorsLooked = false;
bool g_waitLeave = false;  // just came through: not again till off the marker
uint64_t g_lastThroughMs = 0;

// a house of the gang's kind, where no door of the city stands near its base: an area and a point inside it
struct Inside {
    int8_t area;
    CVector at;
};
const Inside kInside[kGangs] = {
    {3, {2496.0f, -1694.0f, 1014.7f}},  // the Johnsons' house
    {5, {318.6f, 1118.2f, 1083.9f}},    // a crack den
    {5, {2350.3f, -1181.6f, 1028.0f}},  // the house of "Burning Desire"
    {2, {2454.7f, -1700.9f, 1013.5f}},  // Ryder's house
};

// where one comes out of a door pair on that side: the pair's exit position nearer to that side's door
Spot ArriveAt(uintptr_t side, uintptr_t other) {
    CVector door = DoorSpot((void*)side);
    CVector a = Ref<CVector>(side + 0x20), b = other ? Ref<CVector>(other + 0x20) : a;
    bool useA = (a - door).Length() <= (b - door).Length();
    uintptr_t e = useA ? side : other;
    return {useA ? a : b, Ref<float>(e + 0x2C)};
}

void LookForDoors() {
    CPoolHeader* pool = EnexPool();
    if (!pool) return;
    g_doorsLooked = true;
    for (int g = 0; g < kGangs; ++g) {
        BaseDoor d;
        const CVector& base = Info(g).base;
        // a door of the city near the base, with an interior behind it
        float bestD = 80.0f;
        int best = -1;
        for (int i = 0; i < pool->size; ++i) {
            if (pool->byteMap[i] < 0) continue;
            uintptr_t e = Enex(i), link = Ref<uintptr_t>(e + 0x38);
            if (Ref<int8_t>(e + 0x32) != 0 || !link || EnexIndex(link) < 0 || Ref<int8_t>(link + 0x32) == 0) continue;
            CVector at = DoorSpot((void*)e);
            float dist = (at - base).Length2D();
            if (dist < bestD && std::fabs(at.z - base.z) < 10.0f) bestD = dist, best = i;
        }
        if (best >= 0) {
            uintptr_t e = Enex(best), link = Ref<uintptr_t>(e + 0x38);
            d.found = true;
            d.inIdx = EnexIndex(link);
            d.outMark = DoorSpot((void*)e), d.inMark = DoorSpot((void*)link);
            d.outArrive = ArriveAt(e, link), d.inArrive = ArriveAt(link, e);
            d.inArea = Ref<int8_t>(link + 0x32);
            LOGI("Gang war: the base of %s has the door at %.1f,%.1f (%.0f m off) into area %d", Info(g).name[1], d.outMark.x, d.outMark.y,
                 bestD, d.inArea);
        } else {
            // a marker at the base into the gang's kind of house: that interior's own door inside
            const Inside& in = kInside[g];
            float inD = 40.0f;
            int inside = -1;
            for (int i = 0; i < pool->size; ++i) {
                if (pool->byteMap[i] < 0 || Ref<int8_t>(Enex(i) + 0x32) != in.area) continue;
                float dist = (DoorSpot((void*)Enex(i)) - in.at).Length();
                if (dist < inD) inD = dist, inside = i;
            }
            if (inside >= 0) {
                uintptr_t e = Enex(inside), link = Ref<uintptr_t>(e + 0x38);
                d.found = true;
                d.inIdx = inside;
                d.inMark = DoorSpot((void*)e);
                d.inArrive = ArriveAt(e, EnexIndex(link) >= 0 ? link : 0);
                d.inArea = in.area;
                d.outMark = base + CVector{3.0f, 0.0f, 0.0f};
                d.outArrive = {base, 0.0f};
                LOGI("Gang war: the base of %s has no door near: a marker into area %d", Info(g).name[1], d.inArea);
            } else {
                LOGW("Gang war: the base of %s has no door and no house of its kind found", Info(g).name[1]);
            }
        }
        g_doors[g] = d;
    }
}

using PlaceMarkerSetFn = void(__cdecl*)(uint32_t, uint16_t, CVector*, float, uint8_t, uint8_t, uint8_t, uint8_t, uint16_t, float,
                                        int16_t);
void Ring(uint32_t id, CVector at, int gang) {
    const GangInfo& gi = Info(gang);
    ((PlaceMarkerSetFn)0x725BA0)(id, 1 /* a cylinder */, &at, 1.0f, gi.r, gi.g, gi.b, 170, 2048, 0.1f, 0);  // C3dMarkers::PlaceMarkerSet
}
}  // namespace

// ---------------------------------------------------------------- the host
void HostUpdateGuards() {
    std::vector<PlayerSpot> spots = PlayerSpots();
    bool changed = false;
    for (int g = 0; g < kGangs; ++g) {
        uint8_t want = NONE;
        const GangState& st = State(g);
        if ((st.flags & F_PLAYED) && !(st.flags & F_OUT)) {
            const CVector& base = Info(g).base;
            uint8_t cur = g_guardRunner[g];
            for (const PlayerSpot& p : spots)
                if (p.id == cur && (p.pos - base).Length2D() < kGuardKeep) want = cur;
            if (want == NONE) {
                float bestD = kGuardTake;
                for (const PlayerSpot& p : spots)
                    if (float d = (p.pos - base).Length2D(); d < bestD) bestD = d, want = p.id;
            }
        }
        if (want != g_guardRunner[g]) g_guardRunner[g] = want, changed = true;
    }
    if (!changed && NowMs() - g_guardSentMs < 10000) return;
    g_guardSentMs = NowMs();
    Send(OP_GUARD, [](Writer& w) {
        for (uint8_t r : g_guardRunner) w.U8(r);
    });
}

// ---------------------------------------------------------------- every game
void UpdateGuards() {
    static uint64_t last = 0;
    if (NowMs() - last < 1500) return;
    last = NowMs();
    CPlayerPed* me = FindPlayerPed(0);
    for (int g = 0; g < kGangs; ++g) {
        bool mine = g_guardRunner[g] == Net().MyId() && me && me->AreaCode() == 0 &&
                    (me->GetPosition() - Info(g).base).Length2D() < kGuardKeep && (State(g).flags & F_PLAYED) && !(State(g).flags & F_OUT);
        if (mine) KeepGuards(g);
        else if (!g_guards[g].men.empty()) Release(g);
    }
}

bool HandleGuards(uint8_t from, uint8_t op, Reader& r) {
    if (op != OP_GUARD) return false;
    uint8_t runners[kGangs];
    for (uint8_t& x : runners) x = r.U8();
    if (r.Ok() && from == Net().HostId() && !Net().IsHost()) std::copy(std::begin(runners), std::end(runners), std::begin(g_guardRunner));
    return true;
}

void ShutStreetDoors() {
    CPoolHeader* pool = EnexPool();
    for (int i = 0; pool && i < pool->size; ++i) {
        if (pool->byteMap[i] < 0) continue;
        uintptr_t e = Enex(i);
        if (Ref<int8_t>(e + 0x32) == 0) Ref<uint16_t>(e + 0x30) &= (uint16_t)~kEnexEnableAccess;  // m_nArea 0: outside
    }
    for (const BaseDoor& d : g_doors)  // the bases' interiors' own doors: ours stand there
        if (d.found && d.inIdx >= 0 && pool && pool->byteMap[d.inIdx] >= 0) Ref<uint16_t>(Enex(d.inIdx) + 0x30) &= (uint16_t)~kEnexEnableAccess;
}

void UpdateBaseDoors() {
    if (!g_doorsLooked) {
        LookForDoors();
        ShutStreetDoors();
    }
    CPlayerPed* me = FindPlayerPed(0);
    uint8_t mine = MyGang();
    if (!me || mine >= kGangs || me->Health() <= 0.0f || me->InVehicle() || GetPad(0)->DisablePlayerControls) return;
    const BaseDoor& d = g_doors[mine];
    if (!d.found) return;
    CVector p = me->GetPosition();
    bool outside = me->AreaCode() == 0;
    const CVector& mark = outside ? d.outMark : d.inMark;
    if (!outside && me->AreaCode() != d.inArea) return;
    bool on = (p - mark).Length2D() < 1.2f && std::fabs(p.z - mark.z) < 2.5f;
    if (!on) {
        g_waitLeave = false;
        return;
    }
    if (g_waitLeave || NowMs() - g_lastThroughMs < 1500) return;
    g_waitLeave = true;
    g_lastThroughMs = NowMs();
    const Spot& to = outside ? d.inArrive : d.outArrive;
    Teleport(outside ? d.inArea : 0, to.at, to.heading);
    LOGI("Gang war: through the base's door %s", outside ? "in" : "out");
}

void PlaceBaseMarkers() {
    CPlayerPed* me = FindPlayerPed(0);
    if (!me || !g_doorsLooked) return;
    CVector p = me->GetPosition();
    for (int g = 0; g < kGangs; ++g) {
        BaseDoor& d = g_doors[g];
        if (!d.found) continue;
        bool outside = me->AreaCode() == 0;
        if (!outside && me->AreaCode() != d.inArea) continue;
        CVector& mark = outside ? d.outMark : d.inMark;
        bool& grounded = outside ? d.outGround : d.inGround;
        if ((p - mark).Length() > 60.0f) continue;
        if (!grounded && (p - mark).Length() < 40.0f) {  // its ground is loaded now
            float z = -100.0f;
            Cmd(0x02CE).F(mark.x).F(mark.y).F(mark.z + 1.5f).Out(&z).Run();  // GET_GROUND_Z_FOR_3D_COORD
            if (z > -50.0f && std::fabs(z - mark.z) < 3.0f) mark.z = z;
            grounded = true;
        }
        Ring(0x47570000u + (uint32_t)g * 2 + (outside ? 0 : 1), mark, g);
    }
}

bool BaseDoorAt(int gang, CVector& at) {
    if (gang < 0 || gang >= kGangs || !g_doors[gang].found) return false;
    at = g_doors[gang].outMark;
    return true;
}

void ResetBase() {
    for (int g = 0; g < kGangs; ++g) {
        g_guards[g] = Guards{};
        g_guardRunner[g] = NONE;
        g_doors[g] = BaseDoor{};
    }
    g_doorsLooked = false, g_waitLeave = false;
    g_guardSentMs = 0;
}

}  // namespace coop::gangwar::war
