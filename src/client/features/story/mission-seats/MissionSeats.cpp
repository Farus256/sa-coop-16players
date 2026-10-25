#include "features/story/mission-seats/MissionSeats.h"

#include <cmath>
#include <iterator>
#include <vector>

#include "common/api/protocol/Protocol.h"
#include "common/lib/log/Log.h"
#include "common/lib/util/Util.h"
#include "entities/player/Players.h"
#include "entities/vehicle/Vehicles.h"
#include "shared/api/net-session/Session.h"
#include "shared/config/Config.h"
#include "shared/game/Script.h"
#include "shared/lib/core/Core.h"
#include "shared/ui/chat/Chat.h"

namespace coop {

namespace {
using namespace game;
using game::script::Cmd;

enum : uint8_t { KIND_CAR = 1, KIND_ATTACH = 2, KIND_DETACH = 3 };
constexpr float kCallRadius = 200.0f;   // the partners this far from the vehicle come; the others stay where they are
constexpr uint64_t kCallWaitMs = 10000;  // a partner waits this long for its copy of the vehicle (or a cutscene's end)
constexpr int kPedTurretOffset = 0x76C, kPedTurretA = 0x778, kPedTurretB = 0x77C;  // CPed::m_vecTurretOffset...

// places next to CJ's on a vehicle he stands on (its sides, then behind him, then before him)
constexpr float kBeside[][2] = {{0.9f, 0.0f}, {-0.9f, 0.0f},  {0.9f, -1.0f}, {-0.9f, -1.0f}, {0.0f, -1.0f},
                                {0.9f, 1.0f}, {-0.9f, 1.0f}, {0.0f, 1.0f},  {0.45f, -2.0f}, {-0.45f, -2.0f}};

struct Place {
    uint8_t player = 0;
    int8_t seat = -1;  // -1: stand on the vehicle
    CVector offset{};
    uint16_t turretA = 0;
    float turretB = 0;
};

// ---------------------------------------------------------------- host
CVehicle* g_carWas = nullptr;
CPhysical* g_onWas = nullptr;
uint16_t g_standingOn = 0;  // the vehicle CJ stands on (its net id): the partners come off it with him

bool Callable(const RemotePlayer& rp, const CVector& at, int8_t area) {
    const OnFootState& st = rp.state;
    return rp.ped && rp.hasState && NowMs() - rp.stateRecvMs < 3000 && rp.mode == RemotePlayer::Mode::OnFoot &&
           !(st.flags & (OnFootState::F_DEAD | OnFootState::F_DOWNED | OnFootState::F_ATTACHED)) && st.area == area &&
           (st.pos - at).Length() < kCallRadius;
}

void Call(uint8_t kind, CVehicle* veh, CPlayerPed* cj) {
    NetVehicle* nv = Vehicles::Instance().EnsureNetworked(veh);
    if (!nv) return;
    // a bike's pillion is where CJ stands himself ("Just Business"), or the seat of the mission's own man, who gets on
    // after CJ was put on it (OG Loc behind him in "OG Loc": a partner warped there left him with no seat, and the
    // mission stood). No seat nor place on a bike for anyone else: they get a bike of their own (Players::FollowHost)
    if (veh->IsBike()) return;
    std::vector<int> seats;  // free passenger seats here
    for (int s = 0; s < veh->MaxPassengers() && s < 8; ++s)
        if (!veh->Passengers()[s]) seats.push_back(s);
    std::vector<Place> places;
    size_t beside = 0;
    for (auto& [id, rp] : Players::Instance().All()) {  // by player number: every game assigns the same
        if (!Callable(rp, veh->GetPosition(), cj->AreaCode())) continue;
        Place p;
        p.player = id;
        if (places.size() < seats.size()) {
            p.seat = (int8_t)seats[places.size()];
        } else if (kind == KIND_ATTACH && beside < std::size(kBeside)) {
            CVector base = Field<CVector>(cj, kPedTurretOffset);
            p.offset = base + CVector{kBeside[beside][0], kBeside[beside][1], 0.0f};
            p.turretA = Field<uint16_t>(cj, kPedTurretA);
            p.turretB = Field<float>(cj, kPedTurretB);
            ++beside;
        } else {
            continue;  // no place left
        }
        places.push_back(p);
    }
    if (kind == KIND_ATTACH) g_standingOn = nv->netId;
    if (places.empty()) return;
    Writer w(proto::G_SEAT_CALL);
    w.U8(kind);
    w.U16(nv->netId);
    w.U8((uint8_t)places.size());
    for (const Place& p : places) {
        w.U8(p.player);
        w.I8(p.seat);
        w.Vec({p.offset.x, p.offset.y, p.offset.z});
        w.U16(p.turretA);
        w.F32(p.turretB);
    }
    Net().Send(proto::CH_RELIABLE, w);
    LOGI("The mission %s CJ %s vehicle %u: %zu partner(s) called", kind == KIND_CAR ? "put" : "stood",
         kind == KIND_CAR ? "in" : "on", nv->netId, places.size());
}

void CallOff() {
    Writer w(proto::G_SEAT_CALL);
    w.U8(KIND_DETACH);
    w.U16(g_standingOn);
    w.U8(0);
    Net().Send(proto::CH_RELIABLE, w);
    LOGI("The mission took CJ off vehicle %u: the partners come off it", g_standingOn);
    g_standingOn = 0;
}

// ---------------------------------------------------------------- partner
struct Pending {
    bool active = false;
    uint8_t kind = 0;
    uint16_t netId = 0;
    Place place;
    uint64_t until = 0;
} g_pending;
uint16_t g_myStand = 0;      // the vehicle this game's player was stood on by a call (its net id)
uint64_t g_hostOffMs = 0;    // since when the host does not stand on a vehicle while this player does

// the gun to shoot from the vehicle with: the one in hand, else the best one there is ammunition for
int PickGun(CPed* me) {
    const CWeapon& cur = me->ActiveWeapon();
    if (cur.type >= 22 && cur.type <= 38 && cur.ammoTotal > 0) return cur.type;  // pistol .. minigun
    constexpr int kSlots[] = {5, 4, 3, 2, 6, 7};  // assault rifle, SMG, shotgun, pistol, rifle, heavy
    for (int slot : kSlots) {
        const CWeapon& w = me->Weapons()[slot];
        if (w.type > 0 && w.ammoTotal > 0) return w.type;
    }
    return 0;
}

void HandleCall(uint8_t from, Reader& r) {
    if (from != Net().HostId() || Net().IsHost()) return;
    uint8_t kind = r.U8();
    uint16_t netId = r.U16();
    uint8_t n = r.U8();
    if (kind == KIND_DETACH) {
        g_pending.active = false;
        CPlayerPed* me = core::IsPlayerReady() ? FindPlayerPed(0) : nullptr;
        if (me && g_myStand && me->AttachedTo()) {
            DetachPed(me);
            LOGI("The host came off the vehicle: so does this player");
        }
        g_myStand = 0;
        return;
    }
    for (uint8_t i = 0; i < n && r.Ok(); ++i) {
        Place p;
        p.player = r.U8();
        p.seat = r.I8();
        Vec3 o = r.Vec();
        p.offset = {o.x, o.y, o.z};
        p.turretA = r.U16();
        p.turretB = r.F32();
        if (!r.Ok() || p.player != Net().MyId()) continue;
        g_pending = {true, kind, netId, p, NowMs() + kCallWaitMs};
        LOGI("The host's mission calls this player %s vehicle %u (seat %d)", p.seat >= 0 ? "into" : "onto", netId, p.seat);
    }
}

// false: not now (try again), true: done or never
bool TryPending(CPlayerPed* me) {
    const Pending& p = g_pending;
    NetVehicle* nv = Vehicles::Instance().Find(p.netId);
    if (!nv || !Vehicles::Alive(*nv)) return false;  // its copy is on its way
    if (Ref<uint8_t>(0xB5F851)) return false;         // CCutsceneMgr::ms_running: after the scene
    CVehicle* veh = nv->veh;
    if (me->Health() <= 0.0f || me->InVehicle() || me->AttachedTo() || Players::Instance().LocalDowned() ||
        me->AreaCode() != veh->AreaCode() || (me->GetPosition() - veh->GetPosition()).Length() > kCallRadius) {
        LOGI("Not called into vehicle %u: this player is busy or far", p.netId);
        return true;
    }
    int handle = GetPedRef(me);
    if (p.place.seat >= 0) {
        int seat = p.place.seat;
        if (seat >= veh->MaxPassengers() || veh->Passengers()[seat]) {  // taken here: any other one
            seat = -1;
            for (int s = 0; s < veh->MaxPassengers() && s < 8 && seat < 0; ++s)
                if (!veh->Passengers()[s]) seat = s;
        }
        if (seat < 0) return true;
        Cmd(0x0430).I(handle).I(nv->handle).I(seat).Run();  // WARP_CHAR_INTO_CAR_AS_PASSENGER
        LOGI("Called into the host's car (vehicle %u, seat %d)", p.netId, seat);
        return true;
    }
    // the gun in hand first: the turret's ammunition replaces the one in hand and gives back what it had (a gun of
    // another slot would lose its own)
    int gun = PickGun(me);
    if (gun) Cmd(0x01B9).I(handle).I(gun).Run();  // SET_CURRENT_CHAR_WEAPON
    const CVector& o = p.place.offset;
    Cmd(0x0464).I(handle).I(nv->handle).F(o.x).F(o.y).F(o.z).I(p.place.turretA).F(p.place.turretB * 57.29578f).I(gun).Run();  // ATTACH_CHAR_TO_CAR
    if (me->AttachedTo() == veh) {
        g_myStand = p.netId;
        g_hostOffMs = 0;
        ui::TheChat().AddSystem(TR("Вы на машине вместе с хостом: стреляйте по преследователям. Слезть: F / Enter.",
                                   "You ride the vehicle with the host: shoot at the chasers. To get off: F / Enter."));
        LOGI("Called onto the host's vehicle %u (%.1f %.1f %.1f), gun %d", p.netId, o.x, o.y, o.z, gun);
    }
    return true;
}

// The player comes off the vehicle it was stood on with the host (the host's call; or when the host is no longer on
// one, the vehicle is gone, or the player presses the enter-car key).
void KeepStanding(CPlayerPed* me) {
    if (!g_myStand) return;
    if (!me->AttachedTo()) {  // the vehicle went (its reference nulled it) or a script took it off
        me->Flags() |= 1;     // bUsesCollision
        g_myStand = 0;
        return;
    }
    RemotePlayer* host = Players::Instance().Get(Net().HostId());
    bool hostOn = host && host->hasState && (host->state.flags & OnFootState::F_ATTACHED);
    g_hostOffMs = hostOn ? 0 : (g_hostOffMs ? g_hostOffMs : NowMs());
    CPad* pad = GetPad(0);
    bool leave = pad->NewState.ButtonTriangle && !pad->OldState.ButtonTriangle && !pad->DisablePlayerControls;
    if (leave || !host || (g_hostOffMs && NowMs() - g_hostOffMs > 2000)) {
        DetachPed(me);
        g_myStand = 0;
        LOGI("Off the host's vehicle (%s)", leave ? "the player's key" : "the host is not on it");
    }
}

class MissionSeats : public Module {
public:
    const char* Name() const override { return "MissionSeats"; }
    void OnInstallHooks() override { Net().SetHandler(proto::G_SEAT_CALL, &HandleCall); }
    void OnSessionEnd() override { Reset(); }
    void OnGameShutdown() override { Reset(); }
    void OnPostProcess() override {
        if (!Net().Online() || Net().IsHost() || !core::IsPlayerReady()) return;
        CPlayerPed* me = FindPlayerPed(0);
        if (!me) return;
        if (g_pending.active) {
            if (NowMs() > g_pending.until) {
                g_pending.active = false;
                LOGW("The host's vehicle %u never came: not called into it", g_pending.netId);
            } else if (TryPending(me)) {
                g_pending.active = false;
            }
        }
        KeepStanding(me);
    }

private:
    static void Reset() {
        g_pending.active = false;
        g_myStand = 0;
        g_standingOn = 0;
        g_carWas = nullptr;
        g_onWas = nullptr;
    }
};

MissionSeats g_module;
struct Reg {
    Reg() { core::RegisterModule(&g_module); }
} g_reg;
}  // namespace

namespace seats {

void BeforeScript(CPlayerPed* cj) {
    g_carWas = cj->VehicleIfIn();
    g_onWas = cj->AttachedTo();
}

void AfterScript(CPlayerPed* cj) {
    if (!Net().Online() || FindPlayerPed(0) != cj) return;
    CVehicle* car = cj->VehicleIfIn();
    CPhysical* on = cj->AttachedTo();
    if (car && car != g_carWas) Call(KIND_CAR, car, cj);
    if (on && on != g_onWas && on->IsVehicle()) Call(KIND_ATTACH, (CVehicle*)on, cj);
    else if (!on && g_onWas && g_standingOn) CallOff();
}

}  // namespace seats

}  // namespace coop
