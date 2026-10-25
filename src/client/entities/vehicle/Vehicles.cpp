#include "entities/vehicle/Vehicles.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <vector>

#include "common/api/protocol/Protocol.h"
#include "common/lib/log/Log.h"
#include "common/lib/util/Util.h"
#include "entities/ped/Peds.h"
#include "entities/player/Players.h"
#include "shared/api/net-session/Session.h"
#include "shared/game/Script.h"
#include "shared/lib/memory/Memory.h"

namespace coop {

using namespace game;
using game::script::Cmd;

Vehicles& Vehicles::Instance() {
    static Vehicles v;
    return v;
}

namespace {
struct Reg {
    Reg() { core::RegisterModule(&Vehicles::Instance()); }
} g_reg;

constexpr uint8_t STATUS_PHYSICS = 3, STATUS_WRECKED = 5;
constexpr uint8_t VEHICLE_CREATED_RANDOM = 1, VEHICLE_CREATED_MISSION = 2, VEHICLE_CREATED_PARKED = 3;
constexpr float kTrafficIn = 170.0f;   // owned vehicles closer than this to a remote player are announced (> car spawn range)
constexpr float kTrafficOut = 220.0f;  // and dropped from the network beyond this from all of them
// Another player's vehicle has a copy in this game while this game's player is near it (or someone sits in it): with
// up to 16 players apart, each other's traffic would fill the vehicle pool (110) otherwise. Beyond that it is a record.
constexpr float kMirrorIn = 190.0f, kMirrorOut = 250.0f;
constexpr float kSyncRange = 300.0f;  // the owner sends an empty vehicle's state to the players this near (> kMirrorOut)
constexpr uint8_t kOrphan = 0xFE;     // its owner left: the nearest player that has it takes it
constexpr uintptr_t kIgnoreEntity = 0xB7CD68;  // CWorld::pIgnoreEntity: a line of sight passes through it
constexpr int kIgnoredCollision = 0x128;       // CPhysical::m_pEntityIgnoredCollision

struct ColPoint {
    CVector point;
    uint8_t rest[0x2C - 12];
};
constexpr uint64_t kOrphanMs = 3000;  // ... or it goes from the games after this

void SetVehicleCreatedBy(CVehicle* v, uint8_t by) { CallMethod<0x6D5D70>(v, (int)by); }  // keeps CCarCtrl counters right

// A script of this game holds the car: its mission's list (CTheScripts::MissionCleanUp, 75 x {type, handle}, 1 a car)
// or the car STORE_CAR_CHAR_IS_IN stored (CTheScripts::StoreVehicleIndex). Such a car stays a mission car here.
bool ScriptHolds(CVehicle* v) {
    int h = GetVehicleRef(v);
    if (h == Ref<int32_t>(0xA447EC)) return true;
    for (int i = 0; i < 75; ++i)
        if (Ref<uint8_t>(0xA90850 + (uintptr_t)i * 8) == 1 && Ref<int32_t>(0xA90850 + (uintptr_t)i * 8 + 4) == h) return true;
    return false;
}

bool HasPlayerInside(CVehicle* v) {
    if (v->Driver() && v->Driver()->IsPlayer()) return true;
    for (int i = 0; i < 8; ++i)
        if (v->Passengers()[i] && v->Passengers()[i]->IsPlayer()) return true;
    return false;
}

float HeadingOf(const Quat& q) {  // the forward axis' direction on the map (radians)
    float fx = 2 * (q.x * q.y - q.w * q.z), fy = 1 - 2 * (q.x * q.x + q.z * q.z);
    return std::atan2(-fx, fy);
}

float WrapPi(float a) {
    // in one step: subtracting 2pi in a loop never ends for a large angle (1e9, the "not set yet" heading of a held
    // cutscene partner, is a multiple of the float step there and froze the game in the cutscene)
    if (!std::isfinite(a)) return 0.0f;
    return std::remainder(a, 6.2831853f);
}

bool HasSteeredWheels(CVehicle* v) {
    int t = v->VehicleSubClass();
    return t == VEHICLE_TYPE_AUTOMOBILE || t == VEHICLE_TYPE_MTRUCK || t == VEHICLE_TYPE_QUAD || t == VEHICLE_TYPE_BIKE ||
           t == VEHICLE_TYPE_BMX || t == VEHICLE_TYPE_TRAILER;
}

void SetVehicleMatrix(CVehicle* v, const CVector& pos, const Quat& q) {
    CMatrix* m = v->Matrix();
    if (!m) return;
    q.ToMatrix(*m);
    m->pos = pos;
    CallMethod<0x446F90>(v);  // CEntity::UpdateRW
    CallMethod<0x532B00>(v);  // CEntity::UpdateRwFrame
}
constexpr int kDamageManager = 0x5A0, kBikeTyres = 0x65C;  // CAutomobile::m_damageManager, CBike::m_nWheelStatus

bool HasDamageManager(CVehicle* v) {  // CAutomobile and its subclasses
    switch (v->VehicleSubClass()) {
    case VEHICLE_TYPE_AUTOMOBILE: case VEHICLE_TYPE_MTRUCK: case VEHICLE_TYPE_QUAD: case VEHICLE_TYPE_HELI:
    case VEHICLE_TYPE_PLANE: case VEHICLE_TYPE_TRAILER: return true;
    }
    return false;
}

// shows the damage of the owner's vehicle: parts are reset, then rebuilt from the damage state the way
// a loaded save does it (no debris flies off, the parts are just gone or bent)
void ApplyDamage(CVehicle* v, const uint8_t (&damage)[20]) {
    uint8_t* dm = (uint8_t*)v + kDamageManager + 4;
    if (!memcmp(dm, damage, sizeof(damage))) return;
    CallMethod<0x6A3440>(v);  // CAutomobile::Fix
    memcpy(dm, damage, sizeof(damage));
    switch (v->VehicleSubClass()) {  // SetupDamageAfterLoad of the exact class
    case VEHICLE_TYPE_HELI: CallMethod<0x6C4550>(v); break;
    case VEHICLE_TYPE_PLANE: CallMethod<0x6CAC10>(v); break;
    case VEHICLE_TYPE_QUAD: CallMethod<0x6CE340>(v); break;
    default: CallMethod<0x6B3E90>(v); break;
    }
}

VehicleLook CaptureLook(CVehicle* v) {
    VehicleLook l;
    if (HasDamageManager(v)) memcpy(l.damage, (uint8_t*)v + kDamageManager + 4, sizeof(l.damage));
    if (v->VehicleSubClass() == VEHICLE_TYPE_BIKE) memcpy(l.bikeTyres, (uint8_t*)v + kBikeTyres, 2);
    memcpy(l.colors, v->Colors(), 4);
    memcpy(l.upgrades, v->Upgrades(), sizeof(l.upgrades));
    l.lock = (uint8_t)v->DoorLock();
    int pj = -1;
    Cmd(0x0988).I(GetVehicleRef(v)).Out(&pj).Run();  // GET_CURRENT_VEHICLE_PAINTJOB
    l.paintjob = (int8_t)pj;
    return l;
}
}  // namespace

bool VehicleLook::operator==(const VehicleLook& o) const {
    return !memcmp(colors, o.colors, 4) && !memcmp(upgrades, o.upgrades, sizeof(upgrades)) && paintjob == o.paintjob &&
           !memcmp(damage, o.damage, sizeof(damage)) && !memcmp(bikeTyres, o.bikeTyres, 2) && lock == o.lock;
}

bool IsSyncableVehicle(CVehicle* v) { return !v->IsTrain(); }

float SteerForMotion(CVehicle* v, const CVector& vel, float yawRate) {
    CMatrix* m = v->Matrix();
    if (!m || !HasSteeredWheels(v)) return 0.0f;
    float fwd = vel.x * m->forward.x + vel.y * m->forward.y + vel.z * m->forward.z;
    if (std::fabs(fwd) < 1.0f) return 0.0f;  // standing or creeping: no telling
    constexpr float kWheelbase = 2.6f;       // a car's; near enough for the look of it
    // positive: to the left (CAutomobile turns its front wheels by (-sin, cos) of the angle), as the yaw turns left
    return std::clamp(std::atan(kWheelbase * yawRate / fwd), -0.6f, 0.6f);
}

// A mirror has the AI-driven status (STATUS_PHYSICS), so every physics step ran its AI steering
// (CCarCtrl::SteerAICarWithPhysics): with no mission it brakes and pulls the handbrake, and the next frame's
// network speed pushed it on, so a mirror lurched like a car that keeps braking. Mirrors are driven by the network.
using SteerFn = void(__cdecl*)(CVehicle*);
SteerFn g_origSteerAI = nullptr;
void __cdecl HookSteerAICarWithPhysics(CVehicle* v) {
    if (Net().Online() && v)
        if (NetVehicle* nv = Vehicles::Instance().Find(v); nv && Vehicles::Instance().IsMirror(*nv)) return;
    g_origSteerAI(v);
}

// CHeli::HeliSearchLights (0xC1C990) has 4 places and CHeli::AddHeliSearchLight does not check: with the other game's
// helicopters copied here a night chase can light more, and a 5th wrote over what follows the array
using AddSearchLightFn = void(__cdecl*)(const CVector&, const CVector&, float, float, uint32_t, uint8_t, uint8_t);
AddSearchLightFn g_origAddSearchLight = nullptr;
void __cdecl HookAddSearchLight(const CVector& from, const CVector& to, float radius, float power, uint32_t corona,
                                uint8_t clip, uint8_t shadow) {
    if (Ref<uint32_t>(0xC1C96C) < 4) g_origAddSearchLight(from, to, radius, power, corona, clip, shadow);  // NumberOfSearchLights
}

// The horn sounds for as long as the counter is up only in the player's own car (STATUS_PLAYER); in any other car the
// audio plays a random honking pattern. A mirror honks while its owner's horn does, as the owner hears it.
using HornStateFn = void(__fastcall*)(void*, void*, bool*, void*);
HornStateFn g_origHornState = nullptr;
void __fastcall HookGetHornState(void* audio, void* edx, bool* on, void* params) {
    g_origHornState(audio, edx, on, params);
    if (*on || !Net().Online()) return;
    auto* v = Field<CVehicle*>(params, 0x10);  // tVehicleParams::Vehicle
    if (NetVehicle* nv = Vehicles::Instance().Find(v); nv && Vehicles::Instance().IsMirror(*nv) && nv->hasState)
        *on = (nv->state.flags & VehicleState::F_HORN) != 0;
}

void Vehicles::OnInstallHooks() {
    mem::HookFn(0x437C20, &HookSteerAICarWithPhysics, &g_origSteerAI, "CCarCtrl::SteerAICarWithPhysics");
    mem::HookFn(0x4F61E0, &HookGetHornState, &g_origHornState, "CAEVehicleAudioEntity::GetHornState");
    mem::HookFn(0x6C45B0, &HookAddSearchLight, &g_origAddSearchLight, "CHeli::AddHeliSearchLight");
    Net().SetHandler(proto::G_VEH_CREATE, [](uint8_t from, Reader& r) { Instance().HandleCreate(from, r); });
    Net().SetHandler(proto::G_VEH_REMOVE, [](uint8_t from, Reader& r) { Instance().HandleRemove(from, r); });
    Net().SetHandler(proto::G_VEH_SYNC, [](uint8_t from, Reader& r) { Instance().HandleSync(from, r); });
    Net().SetHandler(proto::G_VEH_OWNER, [](uint8_t from, Reader& r) { Instance().HandleOwner(from, r); });
    Net().SetHandler(proto::G_VEH_STATUS, [](uint8_t from, Reader& r) { Instance().HandleStatus(from, r); });
    Net().SetHandler(proto::G_VEH_HANDOVER, [](uint8_t from, Reader& r) { Instance().HandleHandover(from, r); });
    Net().SetHandler(proto::G_VEH_SCRIPT_FREE, [](uint8_t from, Reader& r) { Instance().HandleScriptFree(from, r); });
    Net().SetHandler(proto::G_VEH_CLEAR, [](uint8_t from, Reader& r) { Instance().HandleClear(from, r); });
}

NetVehicle* Vehicles::Find(uint16_t netId) {
    auto it = m_byId.find(netId);
    return it == m_byId.end() ? nullptr : &it->second;
}

NetVehicle* Vehicles::Find(CVehicle* v) {
    auto it = m_byPtr.find(v);
    return it == m_byPtr.end() ? nullptr : Find(it->second);
}

bool Vehicles::IsMirror(const NetVehicle& nv) const { return nv.owner != Net().MyId(); }

NetVehicle* Vehicles::EnsureNetworked(CVehicle* v) {
    // a train is the Trains module's (CREATE_CAR made a train off its rails in the other game): a player getting into its
    // cab announced it as a car
    if (!v || !IsVehicleValid(v) || !IsSyncableVehicle(v)) return nullptr;
    if (NetVehicle* nv = Find(v)) return nv;
    if (!Net().Online()) return nullptr;
    uint16_t id;
    do {
        id = proto::EntityId(Net().MyId(), m_nextId++);
    } while (m_byId.count(id) || m_pending.count(id));
    NetVehicle& nv = m_byId[id];
    nv.netId = id;
    nv.veh = v;
    nv.handle = GetVehicleRef(v);
    nv.owner = Net().MyId();
    nv.model = v->ModelIndex();
    nv.lastChangeMs = NowMs();
    m_byPtr[v] = id;
    Announce(nv);
    LOGI("Vehicle %04X (model %d) is now networked", id, nv.model);
    return &nv;
}

void Vehicles::Announce(const NetVehicle& nv, proto::PlayerMask to) {
    CVehicle* v = nv.veh;
    VehicleSpawn sp;
    sp.netId = nv.netId;
    sp.model = nv.model;
    sp.owner = nv.owner;
    sp.pos = v->GetPosition();
    if (CMatrix* m = v->Matrix()) sp.rot = Quat::FromMatrix(*m);
    memcpy(sp.colors, v->Colors(), 4);
    sp.health = v->Health();
    sp.area = v->AreaCode();
    sp.lockState = (uint8_t)v->DoorLock();
    sp.script = nv.createdByUs ? nv.script : v->CreatedBy() == VEHICLE_CREATED_MISSION;
    Writer w(proto::G_VEH_CREATE);
    sp.Write(w);
    Net().SendTo(to, proto::CH_RELIABLE, w);
}

void Vehicles::Reannounce(uint8_t to) {
    for (auto& [id, nv] : m_byId)
        if (!IsMirror(nv) && IsVehicleValid(nv.veh)) {
            Announce(nv, proto::Bit(to));
            nv.lookTo &= ~proto::Bit(to);
            nv.syncTo &= ~proto::Bit(to);
            SendLook(nv, proto::Bit(to));
        }
}

proto::PlayerMask Vehicles::Riders(uint16_t netId) const {
    proto::PlayerMask mask = 0;
    for (auto& [id, rp] : Players::Instance().All())
        if (rp.hasState && rp.mode != RemotePlayer::Mode::OnFoot && rp.vehNetId == netId) mask |= proto::Bit(id);
    return mask;
}

// the players an owned vehicle's state and look go to: those near enough to have a copy of it, and its riders
proto::PlayerMask Vehicles::StateTargets(const NetVehicle& nv) const {
    return PlayersNear(nv.veh->GetPosition(), nv.veh->AreaCode(), kSyncRange, false) | Riders(nv.netId);
}

void Vehicles::SendLook(NetVehicle& nv, proto::PlayerMask to) {
    VehicleLook now = CaptureLook(nv.veh);
    if (!nv.hasLook || !(now == nv.look)) nv.lookTo = 0;  // changed: everyone needs it again
    nv.look = now;
    nv.hasLook = true;
    to &= Net().PeersMask() & ~nv.lookTo;
    if (!to) return;
    nv.lookTo |= to;
    Writer w(proto::G_VEH_STATUS);
    w.U16(nv.netId);
    w.Raw(nv.look.colors, 4);
    for (int16_t u : nv.look.upgrades) w.I16(u);
    w.I8(nv.look.paintjob);
    w.Raw(nv.look.damage, sizeof(nv.look.damage));
    w.Raw(nv.look.bikeTyres, 2);
    w.U8(nv.look.lock);
    Net().SendTo(to, proto::CH_RELIABLE, w);
}

void Vehicles::HandleStatus(uint8_t from, Reader& r) {
    uint16_t id = r.U16();
    VehicleLook l;
    r.Raw(l.colors, 4);
    for (int16_t& u : l.upgrades) u = r.I16();
    l.paintjob = r.I8();
    r.Raw(l.damage, sizeof(l.damage));
    r.Raw(l.bikeTyres, 2);
    l.lock = r.U8();
    if (!r.Ok()) return;
    if (auto it = m_pending.find(id); it != m_pending.end() && it->second.sp.owner == from) {  // for when it is made
        it->second.look = l;
        it->second.hasLook = true;
        return;
    }
    NetVehicle* nv = Find(id);
    if (!nv || nv->owner != from) return;
    nv->look = l;
    nv->hasLook = true;
}

// mod shop work done in the owner's game: same parts (models streamed in first), paintjob, colours
void Vehicles::ApplyLook(NetVehicle& nv) {
    CVehicle* v = nv.veh;
    memcpy(v->Colors(), nv.look.colors, 4);
    int h = nv.handle;
    for (int slot = 0; slot < 15; ++slot) {
        int16_t want = nv.look.upgrades[slot], have = v->Upgrades()[slot];
        if (want == have) continue;
        if (have >= 0) Cmd(0x06E8).I(h).I(have).Run();  // REMOVE_VEHICLE_MOD
        // a part of a mod this game lacks crashes the streamer; IsValidModForVehicle: what a mod shop here offers it
        if (want < 0 || !IsValidModel(want) || !Call<0x49B010, bool>(want, v)) continue;
        Cmd(0x06E9).I(want).Run();                          // REQUEST_VEHICLE_MOD
        if (!Cmd(0x06EA).I(want).Run()) continue;           // HAS_VEHICLE_MOD_LOADED: try again next frame
        int dummy = 0;
        Cmd(0x06E7).I(h).I(want).Out(&dummy).Run();         // ADD_VEHICLE_MOD
    }
    int pj = -1;
    Cmd(0x0988).I(h).Out(&pj).Run();  // GET_CURRENT_VEHICLE_PAINTJOB
    // CVehicle::SetRemap reads the model's four paintjobs at that index, unchecked
    if (nv.look.paintjob >= 0 && nv.look.paintjob < 4 && pj != nv.look.paintjob)
        Cmd(0x06ED).I(h).I(nv.look.paintjob).Run();  // GIVE_VEHICLE_PAINTJOB
    if (HasDamageManager(v)) ApplyDamage(v, nv.look.damage);
    else if (v->VehicleSubClass() == VEHICLE_TYPE_BIKE) memcpy((uint8_t*)v + kBikeTyres, nv.look.bikeTyres, 2);
    // the lock it had when it became known stayed: a mission car locked then held the partner who got in, after the
    // mission it was opened only in the owner's game (the Savanna of "Cesar Vialpando"); same rule as at the spawn
    v->DoorLock() = nv.look.lock == 2 && !nv.script ? 1 : nv.look.lock;
}

void Vehicles::TakeOwnership(NetVehicle& nv) {
    if (nv.owner == Net().MyId()) return;
    nv.owner = Net().MyId();
    nv.hasState = false;
    nv.lastChangeMs = NowMs();
    nv.hasLook = false;  // this game's look of it goes to the players near it
    nv.lookTo = nv.syncTo = 0;
    Release(nv);
    Writer w(proto::G_VEH_OWNER);
    w.U16(nv.netId);
    w.U8(nv.owner);
    Net().Send(proto::CH_RELIABLE, w);
    LOGI("Took ownership of vehicle %04X", nv.netId);
}

CVehicle* Vehicles::FindAdoptable(uint16_t model, const CVector& pos) {
    CPoolHeader* pool = VehiclePool();
    if (!pool) return nullptr;
    CPlayerPed* me = FindPlayerPed(0);
    CVehicle* best = nullptr;
    float bestD = 3.0f;
    for (int i = 0; i < pool->size; ++i) {
        if (pool->byteMap[i] < 0) continue;
        auto* v = (CVehicle*)(pool->objects + (size_t)i * kVehiclePoolElemSize);
        if (v->ModelIndex() != model || m_byPtr.count(v)) continue;
        if (me && me->InVehicle() && me->Vehicle() == v) continue;
        // a script's car stays its own: released as an ordinary car later, its script lost it and crashed
        if (v->CreatedBy() == VEHICLE_CREATED_MISSION) continue;
        // a parked car both games have (a car generator's): one with people in it or on the move is this game's traffic,
        // its driver kept driving it against the copy's states
        if (v->Driver() || v->NumPassengers() || v->MoveSpeed().Length() > 0.01f) continue;
        float d = (v->GetPosition() - pos).Length();
        if (d < bestD) {
            bestD = d;
            best = v;
        }
    }
    return best;
}

// The vehicle pool's band between the game's own reserve (CCarCtrl::RemoveCarsIfThePoolGetsFull, 8) and ours held a
// copy back for good (a mission's cars among the last announced). It takes the place of our own empty random car the
// other game has no copy of, out of view, the farthest.
static bool FreeRandomCar() {
    CPoolHeader* pool = VehiclePool();
    CPlayerPed* me = FindPlayerPed(0);
    CVehicle* far = nullptr;
    float farD = 0.0f;
    for (int i = 0; pool && me && i < pool->size; ++i) {
        if (pool->byteMap[i] < 0) continue;
        auto* v = (CVehicle*)(pool->objects + (size_t)i * kVehiclePoolElemSize);
        if (v->CreatedBy() != VEHICLE_CREATED_RANDOM || v->Driver() || v->NumPassengers() || Vehicles::Instance().Find(v) ||
            v->IsTrain() || CallMethod<0x534540, bool>(v))  // CEntity::GetIsOnScreen
            continue;
        if (float d = (v->GetPosition() - me->GetPosition()).Length(); d > farD) farD = d, far = v;
    }
    if (!far) return false;
    LOGD("Vehicle pool full: our empty car %.0f m away makes room for a copy", farD);
    Cmd(0x00A6).I(GetVehicleRef(far)).Run();  // DELETE_CAR
    return true;
}

CVehicle* Vehicles::SpawnMirror(const VehicleSpawn& sp) {
    if (!IsModelLoaded(sp.model)) {
        RequestModel(sp.model, 0);  // streamed in the background, CreateMirror is retried every frame
        return nullptr;
    }
    if (!HasRoom(VehiclePool(), kKeepVehicles, "vehicle") && !FreeRandomCar()) return nullptr;
    int handle = -1;
    Cmd(0x00A5).I(sp.model).F(sp.pos.x).F(sp.pos.y).F(sp.pos.z).Out(&handle).Run();  // CREATE_CAR
    CVehicle* v = GetVehicle(handle);
    if (!v) return nullptr;
    // CREATE_CAR marks the car as held by a script (CVehicle::bIsLocked), and a tow truck or a tractor never hooks such
    // a car (CAutomobile::TowTruckControl, 0x6A4366); a mission's car stays so, as in its owner's game
    if (!sp.script) v->VehicleFlagsLo() &= ~8u;
    SetVehicleMatrix(v, sp.pos, sp.rot);
    memcpy(v->Colors(), sp.colors, 4);
    v->Health() = sp.health;
    v->AreaCode() = sp.area;
    // a random car locked there does not lock the other player out; a mission's locked car stays shut for him too
    v->DoorLock() = sp.lockState == 2 && !sp.script ? 1 : sp.lockState;
    return v;
}

void Vehicles::HandleCreate(uint8_t from, Reader& r) {
    VehicleSpawn sp;
    if (!sp.Read(r) || from == Net().MyId()) return;
    if (!core::IsPlayerReady()) return;
    if (NetVehicle* existing = Find(sp.netId)) {
        // announced again: its owner may have changed. Not a car of ours or another model: the id came twice (a game
        // that started over counts its ids from the start), and it handed our car to the other player.
        if (IsMirror(*existing) && existing->model == sp.model) existing->owner = sp.owner;
        return;
    }
    if (!IsVehicleModel(sp.model)) {
        LOGW("Vehicle %04X has an unknown model %d", sp.netId, sp.model);
        return;
    }
    PendingVehicle& pv = m_pending[sp.netId];  // an announcement again keeps the look and state it has
    pv.sp = sp;
    if (WantMirror(sp.netId, sp.pos, false) && CreateMirror(pv)) m_pending.erase(sp.netId);
}

bool Vehicles::WantMirror(uint16_t netId, const CVector& pos, bool spawned) const {
    CPlayerPed* me = FindPlayerPed(0);
    if (!me) return false;
    if (Riders(netId)) return true;  // a player's car follows it anywhere (its ped sits in it, its blip with it)
    return (pos - me->GetPosition()).Length() < (spawned ? kMirrorOut : kMirrorIn);
}

bool Vehicles::CreateMirror(const PendingVehicle& pv) {
    const VehicleSpawn& sp = pv.sp;
    if (Find(sp.netId)) return true;  // announced again meanwhile and made already
    bool adopted = false;
    CVehicle* v = FindAdoptable(sp.model, sp.pos);
    if (v) {
        adopted = true;
        SetVehicleMatrix(v, sp.pos, sp.rot);
        memcpy(v->Colors(), sp.colors, 4);  // the same parked car can have other random colours here
        v->Health() = sp.health;
    } else {
        v = SpawnMirror(sp);
    }
    if (!v) return false;
    NetVehicle& nv = m_byId[sp.netId];
    nv.netId = sp.netId;
    nv.veh = v;
    nv.handle = GetVehicleRef(v);
    nv.owner = sp.owner;
    nv.model = sp.model;
    nv.createdByUs = !adopted;
    nv.script = sp.script;
    if (adopted) {
        Protect(nv);
    } else if (void* clump = v->RwObject()) {
        Call<0x732B00>(clump, 0);  // CVisibilityPlugins::SetClumpAlpha: fades in (ApplyMirror)
        nv.fadeMs = NowMs();
    }
    if (pv.hasState) {  // where the owner has it now, at once
        nv.state = pv.state;
        nv.hasState = true;
        nv.lastTime = pv.state.time;
        nv.recvMs = pv.recvMs;
    }
    if (pv.hasLook) {
        nv.look = pv.look;
        nv.hasLook = true;
    }
    m_byPtr[v] = sp.netId;
    LOGI("Vehicle %04X (model %d) %s for player %d", sp.netId, sp.model, adopted ? "adopted" : "created", sp.owner);
    return true;
}

// Far from this game's player: the copy goes (one this game had before is its own again) and the record waits, kept up
// to date by the owner's states for when the player comes near again.
void Vehicles::Park(NetVehicle& nv) {
    PendingVehicle pv;
    VehicleSpawn& sp = pv.sp;
    sp.netId = nv.netId;
    sp.model = nv.model;
    sp.owner = nv.owner;
    sp.script = nv.script;
    if (IsVehicleValid(nv.veh)) {
        CVehicle* v = nv.veh;
        sp.pos = v->GetPosition();
        if (CMatrix* m = v->Matrix()) sp.rot = Quat::FromMatrix(*m);
        memcpy(sp.colors, v->Colors(), 4);
        sp.health = v->Health();
        sp.area = v->AreaCode();
        sp.lockState = (uint8_t)v->DoorLock();
    }
    if (nv.hasState) {
        pv.hasState = true;
        pv.state = nv.state;
        pv.recvMs = nv.recvMs;
        sp.pos = nv.state.pos;
        sp.rot = nv.state.rot;
    }
    pv.look = nv.look;
    pv.hasLook = nv.hasLook;
    uint16_t id = nv.netId;
    bool gone = nv.createdByUs;
    if (!gone) Release(nv);
    LOGD("Vehicle %04X (model %d) is far: %s, the record stays", id, nv.model, gone ? "its copy goes" : "this game's again");
    Forget(id, gone);
    m_pending[id] = pv;
}

void Vehicles::Forget(uint16_t netId, bool deleteVehicle) {
    auto it = m_byId.find(netId);
    if (it == m_byId.end()) return;
    NetVehicle& nv = it->second;
    if (deleteVehicle && nv.veh && IsVehicleValid(nv.veh)) {
        // a deleted car takes its occupants along: the other player's character gets out first
        for (auto& [id, rp] : Players::Instance().All())
            if (rp.ped && IsPedValid(rp.ped) && rp.ped->InVehicle() && rp.ped->Vehicle() == nv.veh) {
                CVector p = rp.ped->GetPosition();
                Cmd(0x0362).I(rp.pedHandle).F(p.x).F(p.y).F(p.z).Run();  // WARP_CHAR_FROM_CAR_TO_COORD
            }
        StopEntering(nv.veh);
        Cmd(0x00A6).I(GetVehicleRef(nv.veh)).Run();  // DELETE_CAR
    }
    m_byPtr.erase(nv.veh);
    m_byId.erase(it);
}

// A mirror is the other game's car: this game's population and scripts leave it alone (a car of the population taken
// over by the other player was deleted here as soon as this player got far, with the other player's character in it).
void Vehicles::Protect(NetVehicle& nv) {
    CVehicle* v = nv.veh;
    if (!IsVehicleValid(v) || v->CreatedBy() == VEHICLE_CREATED_MISSION) return;
    if (!nv.createdByUs && !nv.origBy) nv.origBy = v->CreatedBy();
    SetVehicleCreatedBy(v, VEHICLE_CREATED_MISSION);
}

// Ours again: what it was in this game before, and a mirror spawned here an ordinary car (a script's car of the other
// game stays a mission car: dropping it here would make the owner drop it in the middle of its mission)
void Vehicles::Release(NetVehicle& nv) {
    CVehicle* v = nv.veh;
    if (nv.crashProofHere && IsVehicleValid(v)) v->PhysicalFlags() &= ~CPhysical::PF_COLLISION_PROOF;
    nv.crashProofHere = false;
    uint8_t by = nv.createdByUs ? (nv.script ? VEHICLE_CREATED_MISSION : VEHICLE_CREATED_RANDOM) : nv.origBy;
    if (IsVehicleValid(v) && by && v->CreatedBy() != by && !ScriptHolds(v)) SetVehicleCreatedBy(v, by);
    nv.origBy = 0;
}

// The owner let the vehicle go: a mirror spawned here goes too (unless this player sits in it), an instance this game had
// before (adopted, or ours until the other player took it) is this game's again, a script's car its script's.
void Vehicles::Drop(uint16_t netId) {
    NetVehicle* nv = Find(netId);
    if (!nv) return;
    CPlayerPed* me = FindPlayerPed(0);
    bool inside = me && me->InVehicle() && me->Vehicle() == nv->veh;
    bool gone = nv->createdByUs && !inside;
    if (!gone) Release(*nv);
    Forget(netId, gone);
}

std::vector<uint16_t> Vehicles::ClearMirrors(const CVector& at, float radius) {
    std::vector<uint16_t> ids;
    for (auto& [id, nv] : m_byId) {
        CVehicle* v = nv.veh;
        // one the mission took for itself stays (CESAR1 stores the lowrider the partner drives, $8788: deleted here, the
        // mission saw it wrecked and never faded the screen back in)
        if (!IsMirror(nv) || nv.script || !IsVehicleValid(v) || v->Driver() || ScriptHolds(v)) continue;
        bool empty = true;
        for (int i = 0; i < 8; ++i) empty = empty && !v->Passengers()[i];
        CVector d = v->GetPosition() - at;
        if (empty && d.x * d.x + d.y * d.y < radius * radius) ids.push_back(id);
    }
    for (uint16_t id : ids) Forget(id, true);
    return ids;
}

// the owner's side of ClearMirrors: what the host's copy no longer has goes here too, as the area's ordinary cars went
void Vehicles::HandleClear(uint8_t from, Reader& r) {
    uint8_t n = r.U8();
    for (uint8_t i = 0; i < n && r.Ok() && from == Net().HostId(); ++i) {
        uint16_t id = r.U16();
        NetVehicle* nv = r.Ok() ? Find(id) : nullptr;
        if (!nv || IsMirror(*nv) || !IsVehicleValid(nv->veh)) continue;
        if (nv->veh->Driver() || HasPlayerInside(nv->veh)) {  // got in meanwhile: the host gets its copy back
            Announce(*nv);
            continue;
        }
        LOGI("Vehicle %04X (model %d) cleared away by the host's mission", id, nv->model);
        StopEntering(nv->veh);
        Cmd(0x00A6).I(nv->handle).Run();  // DELETE_CAR: its removal goes out with the other vanished ones (Update)
    }
}

void Vehicles::ForgetOwnedBy(uint8_t owner) {
    std::vector<uint16_t> ids;
    for (auto& [vid, nv] : m_byId)
        if (nv.owner == owner) ids.push_back(vid);
    for (auto it = m_pending.begin(); it != m_pending.end();) it = it->second.sp.owner == owner ? m_pending.erase(it) : std::next(it);
    for (uint16_t vid : ids) Drop(vid);
}

// Its owner deleted an ordinary car by this game's player: after the host's death its game clears the streets, and a
// police car could not go to this game (its crew changes games only to chase): it vanished in front of the player. It
// stays as this game's car, with whoever sits in it (Peds keeps them), as on a hand-over. A mission's or a wreck still goes,
// and so does an aircraft: a police helicopter kept here hung in the air with no police AI to fly it.
bool Vehicles::KeepsWhenDeleted(const NetVehicle& nv) const {
    CPlayerPed* me = FindPlayerPed(0);
    CVehicle* v = nv.veh;
    if (!me || !IsMirror(nv) || !IsVehicleValid(v) || nv.script || v->Status() == STATUS_WRECKED || ScriptHolds(v)) return false;
    int type = v->VehicleSubClass();
    bool aircraft = type == VEHICLE_TYPE_HELI || type == VEHICLE_TYPE_PLANE || type == VEHICLE_TYPE_FHELI || type == VEHICLE_TYPE_FPLANE;
    float mine = (v->GetPosition() - me->GetPosition()).Length();
    if (aircraft || type == VEHICLE_TYPE_TRAIN || v->AreaCode() != me->AreaCode() || mine >= 80.0f) return false;
    // several players by it: only the nearest one's game keeps it (the lower number of two as near), not two copies
    for (auto& [id, rp] : Players::Instance().All()) {
        if (id == nv.owner || !rp.hasState || rp.state.area != me->AreaCode()) continue;
        float theirs = (v->GetPosition() - rp.state.pos).Length();
        if (theirs < mine - 1.0f || (std::fabs(theirs - mine) <= 1.0f && id < Net().MyId())) return false;
    }
    return true;
}

void Vehicles::HandleRemove(uint8_t from, Reader& r) {
    uint16_t id = r.U16();
    if (!r.Ok()) return;
    m_pending.erase(id);
    NetVehicle* nv = Find(id);
    if (!nv || nv->owner != from) return;
    CPlayerPed* me = FindPlayerPed(0);
    if (KeepsWhenDeleted(*nv)) {
        LOGI("Vehicle %04X (model %d): its owner deleted it by us: kept", id, nv->model);
        return TakeOver(*nv, from);
    }
    // Its owner deleted it with our player riding inside ("Home Invasion" deletes the van at the lockup): ours stayed in
    // it as its own car, jammed in a garage door that is shut here, and fell through the floor. Out next to its owner's
    // player, the mission put that one where the scene goes on.
    if (me && IsVehicleValid(nv->veh) && me->InVehicle() && me->Vehicle() == nv->veh && nv->veh->Driver() != me) {
        CVector p = nv->veh->GetPosition();
        if (const RemotePlayer* rp = Players::Instance().Get(from);
            rp && rp->hasState && rp->mode == RemotePlayer::Mode::OnFoot && (rp->state.pos - p).Length() < 50.0f)
            p = rp->state.pos + CVector{1.0f, 0.0f, 0.0f};
        Cmd(0x0362).I(GetPedRef(me)).F(p.x).F(p.y).F(p.z - 1.0f).Run();  // WARP_CHAR_FROM_CAR_TO_COORD (feet)
        LOGI("Vehicle %04X: its owner deleted it with us inside: out by its player", id);
    }
    Drop(id);
}

// An ordinary car of this game (traffic, a parked one) the other player is at while this game's player is far: this
// game would drop it soon (CCarCtrl removes cars beyond its player's reach) and it vanished in front of the other player.
// The other game takes it over with whoever sits in it, as its own, and this game's one goes.
static bool CanGiveAway(CVehicle* v, bool evenInView = false) {
    uint8_t by = v->CreatedBy();
    if ((by != VEHICLE_CREATED_RANDOM && by != VEHICLE_CREATED_PARKED) || v->Status() == STATUS_WRECKED ||
        Field<CVehicle*>(v, 0x4C4) || Field<CVehicle*>(v, 0x4C8) ||  // towing or towed: the pair stays together
        !CallMethod<0x6D1180, bool>(v) ||                             // CVehicle::CanBeDeleted: the engine's own test
        (!evenInView && CallMethod<0x534540, bool>(v)) || ScriptHolds(v))  // CEntity::GetIsOnScreen: it does not vanish in view
        return false;
    CPed* seats[9] = {v->Driver()};
    for (int i = 0; i < 8; ++i) seats[i + 1] = v->Passengers()[i];
    for (CPed* p : seats) {  // its occupants go along: only this game's own passers-by the other game knows
        NetPed* np = p ? Peds::Instance().Find(p) : nullptr;
        if (p && (p->IsPlayer() || Players::Instance().FindByPed(p) || !np || np->owner != Net().MyId() ||
                  p->CreatedBy() != 1 /* PED_GAME */ || !Peds::Instance().CanGiveAwayType(*np)))
            return false;
    }
    return true;
}

void Vehicles::GiveAway(NetVehicle& nv, uint8_t to) {
    CVehicle* v = nv.veh;
    CPed* seats[9] = {v->Driver()};
    for (int i = 0; i < 8; ++i) seats[i + 1] = v->Passengers()[i];
    for (CPed* p : seats)  // the peds first: the car's message drives them on
        if (NetPed* np = p ? Peds::Instance().Find(p) : nullptr) Peds::Instance().GiveAway(np->netId, to);
    Writer w(proto::G_VEH_HANDOVER);
    w.U16(nv.netId);
    w.U8(to);
    Net().Send(proto::CH_RELIABLE, w);
    LOGI("Vehicle %04X (model %d) handed over to player %d", nv.netId, nv.model, to);
    // this game's one goes; the record stays, the new owner's car, for when this game's player comes back
    nv.owner = to;
    nv.createdByUs = true;
    nv.hasState = false;
    Park(nv);
}

// This game's player goes into another place: its cars near the other players go to them now (see Peds)
int Vehicles::HandOverLeftBehind() {
    std::vector<std::pair<NetVehicle*, uint8_t>> give;
    for (auto& [id, nv] : m_byId) {
        if (IsMirror(nv) || !IsVehicleValid(nv.veh) || !CanGiveAway(nv.veh, true)) continue;
        if (GetVehicleRef(nv.veh) == m_lastDriven || GetVehicleRef(nv.veh) == m_kept) continue;  // the player's car waits for it
        CVector p = nv.veh->GetPosition();
        uint8_t best = 255;
        float bestD = 140.0f;
        for (auto& [pid, rp] : Players::Instance().All()) {
            if (!rp.hasState || rp.state.area != nv.veh->AreaCode() || NowMs() - rp.stateRecvMs > 1500 || !rp.ped) continue;
            float d = (rp.state.pos - p).Length2D();
            if (d < bestD) bestD = d, best = pid;
        }
        if (best != 255) give.push_back({&nv, best});
    }
    for (auto& [nv, to] : give) GiveAway(*nv, to);
    return (int)give.size();
}

void Vehicles::Count(int& own, int& copies) const {
    own = copies = 0;
    for (auto& [id, nv] : m_byId)
        if (!IsMirror(nv)) ++own;
        else if (nv.veh) ++copies;
}

bool Vehicles::QueueGiveAway(CVehicle* v, uint8_t to) {
    NetVehicle* nv = Find(v);
    if (!nv || IsMirror(*nv) || !CanGiveAway(v)) return false;
    m_giveAway[v] = to;
    return true;
}

void Vehicles::HandleHandover(uint8_t from, Reader& r) {
    uint16_t id = r.U16();
    uint8_t to = r.U8();
    if (!r.Ok()) return;
    if (auto it = m_pending.find(id); it != m_pending.end()) {  // no copy of it here
        if (it->second.sp.owner != from) return;
        if (to != Net().MyId()) {
            it->second.sp.owner = to;  // the new owner's car from now on
            return;
        }
        // given to this game, which has no copy to take over (its model was still streaming): it is gone for all
        m_pending.erase(it);
        Writer w(proto::G_VEH_REMOVE);
        w.U16(id);
        Net().Send(proto::CH_RELIABLE, w);
        return;
    }
    NetVehicle* nv = Find(id);
    if (!nv) return;
    if (nv->owner != from) {  // this game took it just before: the giver deleted its own, it gets this one back
        if (nv->owner == Net().MyId() && IsVehicleValid(nv->veh)) Announce(*nv);
        return;
    }
    if (to != Net().MyId()) {  // a third player's game takes it over: this copy follows that one now
        nv->owner = to;
        nv->hasState = false;
        return;
    }
    if (!IsVehicleValid(nv->veh)) return Forget(id, true);
    TakeOver(*nv, from);
}

void Vehicles::TakeOver(NetVehicle& nv, uint8_t from) {
    CVehicle* v = nv.veh;
    int h = nv.handle;
    uint16_t id = nv.netId;
    nv.owner = Net().MyId();
    nv.hasState = false;
    nv.lastChangeMs = NowMs();
    nv.lookTo = nv.syncTo = 0;
    nv.script = false;
    Release(nv);  // a mirror's collision proof stayed on the car, and this game's own traffic never took crash damage
    if (!ScriptHolds(v)) SetVehicleCreatedBy(v, VEHICLE_CREATED_RANDOM);  // this game's traffic now
    if (CPed* driver = v->Driver(); driver && !driver->IsPlayer() && !Players::Instance().FindByPed(driver)) {
        Cmd(0x00A8).I(h).Run();           // CAR_WANDER_RANDOMLY
        Cmd(0x00AD).I(h).F(12.0f).Run();  // SET_CAR_CRUISE_SPEED
        Cmd(0x00AE).I(h).I(0).Run();      // SET_CAR_DRIVING_STYLE: stops for cars and lights
    }
    LOGI("Vehicle %04X (model %d) taken over from player %d", id, (int)v->ModelIndex(), from);
}

void Vehicles::HandleSync(uint8_t from, Reader& r) {
    uint16_t id = r.U16();
    VehicleState st;
    if (!st.Read(r)) return;
    ApplyDriverState(from, id, st);
}

void Vehicles::ApplyDriverState(uint8_t from, uint16_t netId, const VehicleState& st) {
    NetVehicle* nv = Find(netId);
    if (!nv) {  // a record without a copy here: where it is, for when it is made
        auto it = m_pending.find(netId);
        if (it == m_pending.end() || it->second.sp.owner != from) return;
        PendingVehicle& pv = it->second;
        if (pv.hasState && (int32_t)(st.time - pv.state.time) < 0) return;
        pv.state = st;
        pv.hasState = true;
        pv.recvMs = NowMs();
        pv.sp.pos = st.pos;
        pv.sp.rot = st.rot;
        pv.sp.health = st.health;
        return;
    }
    if (nv->owner != from) return;
    if (nv->hasState && (int32_t)(st.time - nv->lastTime) < 0) return;
    // how it speeds up or brakes and turns between the owner's states (its own turn speed is 0 for AI traffic on rails)
    if (nv->hasState) {
        float dt = (float)(st.time - nv->lastTime) / 1000.0f;
        if (dt > 0.02f && dt < 0.6f) {
            CVector a = (st.vel - nv->state.vel) * (1.0f / dt);
            float len = a.Length();
            if (len > 15.0f) a = a * (15.0f / len);  // a crash, not an acceleration to carry on
            nv->accel = nv->accel * 0.5f + a * 0.5f;
            float yaw = std::fabs(st.turn.z) > 0.01f ? st.turn.z : WrapPi(HeadingOf(st.rot) - HeadingOf(nv->state.rot)) / dt;
            nv->yawRate = nv->yawRate * 0.5f + std::clamp(yaw, -3.0f, 3.0f) * 0.5f;
        } else {
            nv->accel = CVector{};
            nv->yawRate = st.turn.z;
        }
    }
    if ((st.flags ^ (nv->hasState ? nv->state.flags : 0)) & VehicleState::F_HORN)
        LOGD("Vehicle %04X: horn %s", netId, (st.flags & VehicleState::F_HORN) ? "on" : "off");
    if ((st.extras ^ (nv->hasState ? nv->state.extras : 0)) & VehicleState::X_LIGHT)
        LOGD("Vehicle %04X: searchlight %s", netId, (st.extras & VehicleState::X_LIGHT) ? "on" : "off");
    nv->lastTime = st.time;
    nv->state = st;
    nv->hasState = true;
    nv->recvMs = NowMs();
}

void Vehicles::HandleOwner(uint8_t from, Reader& r) {
    uint16_t id = r.U16();
    uint8_t owner = r.U8();
    if (!r.Ok()) return;
    if (auto it = m_pending.find(id); it != m_pending.end()) {
        it->second.sp.owner = owner;
        it->second.hasState = false;
        return;
    }
    NetVehicle* nv = Find(id);
    if (!nv) return;
    CPlayerPed* me = FindPlayerPed(0);
    bool iDrive = me && nv->veh && me->InVehicle() && me->Vehicle() == nv->veh && nv->veh->Driver() == me;
    // two games took over the car of a player that left: the lower number keeps it
    bool keepClaim = nv->owner == Net().MyId() && NowMs() - nv->claimMs < kOrphanMs && owner == from && from > Net().MyId();
    if (keepClaim && !iDrive) {
        Writer w(proto::G_VEH_OWNER);
        w.U16(id);
        w.U8(Net().MyId());
        Net().Send(proto::CH_RELIABLE, w);
        return;
    }
    if (iDrive && owner != Net().MyId()) {
        if (owner > Net().MyId() || from != owner) {
            // we are driving it right now: keep ownership, tell the others again
            Writer w(proto::G_VEH_OWNER);
            w.U16(id);
            w.U8(Net().MyId());
            Net().Send(proto::CH_RELIABLE, w);
            return;
        }
        // It drives it too, in its game (both took the seat at once, or it pulled us out and our game never saw it).
        // Each kept answering the other's claim while both drove, each showed the other standing frozen beside the car.
        // The lower id keeps it (the host); our player gives way and sits beside it if there is room.
        int h = GetPedRef(me);
        CVector p = me->GetPosition();
        Cmd(0x0362).I(h).F(p.x).F(p.y).F(p.z + 1.0f).Run();  // WARP_CHAR_FROM_CAR_TO_COORD
        if (SeatOrAny(nv->veh, 0) == 0 && !nv->veh->Passengers()[0])
            Cmd(0x0430).I(h).I(nv->handle).I(0).Run();  // WARP_CHAR_INTO_CAR_AS_PASSENGER
        LOGI("Vehicle %04X: player %d drives it as well, ours gives way", id, owner);
    }
    bool wasMine = nv->owner == Net().MyId();
    nv->owner = owner;
    nv->hasState = false;
    if (owner == Net().MyId()) {
        Release(*nv);
    } else if (wasMine) {
        nv->heldHere = IsVehicleValid(nv->veh) && ScriptHolds(nv->veh);
        Protect(*nv);
    }
}

// The script that held this car here let go (its mission ended): the owner's copy, kept a mission car for it, is an
// ordinary car again (else it never went away and stood twice when announced again)
void Vehicles::HandleScriptFree(uint8_t from, Reader& r) {
    uint16_t id = r.U16();
    NetVehicle* nv = r.Ok() ? Find(id) : nullptr;
    if (!nv || IsMirror(*nv) || !IsVehicleValid(nv->veh)) return;
    nv->script = false;
    if (nv->createdByUs && nv->veh->CreatedBy() == VEHICLE_CREATED_MISSION && !ScriptHolds(nv->veh))
        SetVehicleCreatedBy(nv->veh, VEHICLE_CREATED_RANDOM);
}

// The parts a driver moves: nozzles, hoists and forks (m_wMiscComponentAngle), the landing gear, a turret. Nothing ticks
// them on a vehicle without a player's status, so the owner's values stand here.
void ApplyParts(CVehicle* v, const VehicleState& st) {
    // only what this vehicle has as well (VehicleState::Capture's rules): a net id that meant another vehicle here wrote
    // a plane's gear (CPlane::SetGearUp) or a tank's turret into a bike or a boat
    int type = v->VehicleSubClass(), model = v->ModelIndex();
    if ((st.extras & VehicleState::X_BIKE) && (type == VEHICLE_TYPE_BIKE || type == VEHICLE_TYPE_BMX)) {
        Field<float>(v, 0x648) = Field<float>(v, 0x64C) = st.lean;  // LeanAngle, DesiredLeanAngle: as its owner shows it
        Field<float>(v, 0x650) = st.leanFwd;                          // LeanFwd
    }
    if ((st.extras & VehicleState::X_MISC) && (type <= VEHICLE_TYPE_PLANE || type == VEHICLE_TYPE_TRAILER))
        Field<uint16_t>(v, 0x86C) = st.misc;
    if ((st.extras & VehicleState::X_GEAR) && type == VEHICLE_TYPE_PLANE) {
        float& gear = Field<float>(v, 0x9CC);
        if (st.gear >= 1.0f && gear < 1.0f) CallMethod<0x6CAC20>(v);         // CPlane::SetGearUp: no wheels
        else if (st.gear == 0.0f && gear != 0.0f) CallMethod<0x6CAC70>(v);  // CPlane::SetGearDown
        gear = st.gear;
        Field<float>(v, 0x994) = std::clamp(st.gas - st.brake, -1.0f, 1.0f);  // m_fAccelerationBreakStatus: jet flames
    }
    if ((st.extras & VehicleState::X_TURRET) && (model == 432 || model == 601 || model == 407)) {
        Field<float>(v, 0x94C) = st.turretYaw;
        Field<float>(v, 0x950) = st.turretPitch;
    }
}

// Hydraulics of a car a remote player drives. Its game keeps the wheel heights only while computing them from the
// right stick, so they are computed here from his stick the same way; a mirror has the mission-car status for which
// CAutomobile::HydraulicControl takes them from CVehicle::m_aSpecialHydraulicData (4 slots for the whole game).
void ApplyHydraulics(CVehicle* v, const VehicleState& st) {
    if (v->VehicleSubClass() != VEHICLE_TYPE_AUTOMOBILE || !(Field<uint32_t>(v, 0x38C) & 0x20000)) return;  // bHydraulicInst
    RemotePlayer* rp = v->Driver() ? Players::Instance().FindByPed(v->Driver()) : nullptr;
    if (!rp) return;
    const PadState& pad = rp->state.pad;
    float wheel[4] = {1, 1, 1, 1};  // front left, rear left, front right, rear right; all up while the jump is held
    if (!(pad.buttons & PadState::B_SHOCKR)) {
        float lr = pad.rx, ud = pad.ry;
        float jump = std::sqrt(ud * ud + lr * lr) * 1.5f / 128.0f, a = std::atan2(lr, ud) - 0.7853982f;
        float rr = std::cos(a) * jump, fr = std::sin(a) * jump;
        wheel[0] = std::max(0.0f, -rr);
        wheel[1] = std::max(0.0f, -fr);
        wheel[2] = std::max(0.0f, fr);
        wheel[3] = std::max(0.0f, rr);
    }
    bool active = st.misc != 0 || wheel[0] + wheel[1] + wheel[2] + wheel[3] > 0.0f;
    // the lowrider contest showed the copy flying above its owner's car: how far apart and what the wheels do then
    float zHere = v->GetPosition().z, vzHere = v->MoveSpeed().z * 50.0f;
    static uint64_t loggedMs = 0;
    if ((std::fabs(zHere - st.pos.z) > 1.0f || std::fabs(st.vel.z) > 3.0f) && NowMs() - loggedMs > 250) {
        loggedMs = NowMs();
        LOGI("Hydraulics of player %d's car: z here %.2f, owner %.2f; vz here %.2f, owner %.2f; wheels %.2f %.2f %.2f %.2f, "
             "misc %u, jump %d", rp->id, zHere, st.pos.z, vzHere, st.vel.z, wheel[0], wheel[1], wheel[2], wheel[3], st.misc,
             (pad.buttons & PadState::B_SHOCKR) != 0);
    }
    int8_t slot = Field<int8_t>(v, 0x48B);  // CVehicle::m_vehicleSpecialColIndex
    if (active && slot < 0 && CallMethod<0x6DF3D0, bool>(v)) slot = Field<int8_t>(v, 0x48B);  // GetSpecialColModel
    if (slot >= 0 && slot < 4) memcpy((void*)(0xC1CB60 + (uintptr_t)slot * 0x28 + 0x18), wheel, sizeof(wheel));
}

void Vehicles::ApplyMirror(NetVehicle& nv, float dt) {
    if (!nv.hasState) return;
    CVehicle* v = nv.veh;
    const VehicleState& st = nv.state;
    CPlayerPed* me = FindPlayerPed(0);

    if (nv.heldHere && !ScriptHolds(v)) {  // the script here let go of it (its mission ended): the owner may drop it
        nv.heldHere = false;
        Writer w(proto::G_VEH_SCRIPT_FREE);
        w.U16(nv.netId);
        Net().Send(proto::CH_RELIABLE, w);
    }
    if (v->CreatedBy() != VEHICLE_CREATED_MISSION) Protect(nv);  // a script let it go meanwhile (its mission ended)
    // A crash dents and burns a car in its owner's game. Here one heavy hit (a Rhino's ram counts 15 times) blew the copy
    // up at once (CAutomobile::VehicleDamage), its people died here with it, and the wreck stayed while the car drove on.
    if (!(v->PhysicalFlags() & CPhysical::PF_COLLISION_PROOF)) {
        v->PhysicalFlags() |= CPhysical::PF_COLLISION_PROOF;
        nv.crashProofHere = true;
    }
    // burnt out: only where it lies is followed. Each game's blast throws the wreck its own way, and the copy stayed
    // where its own blast threw it (cars on their roofs here, on their wheels there)
    bool wreck = v->Status() == STATUS_WRECKED;
    if (!wreck && (st.flags & VehicleState::F_WRECKED)) {
        Cmd(0x020B).I(nv.handle).Run();  // EXPLODE_CAR: it blew up in the owner's game
        return;
    }
    // towed in its owner's game: hooked here to the copy of its tower too. A tow truck or a tractor hooks a car only by
    // its driver's pad (CAutomobile::TowTruckControl); linked, the two stop colliding and the hoist holds it up
    CVehicle* tower = nullptr;
    if (!wreck && (st.extras & VehicleState::X_TOW))
        if (NetVehicle* tn = Find(st.tower); tn && tn->veh != v && IsVehicleValid(tn->veh)) tower = tn->veh;
    if (tower && !Field<CVehicle*>(v, 0x4C4) && !Field<CVehicle*>(tower, 0x4C8))  // m_pTowingVehicle, m_pVehicleBeingTowed
        if (CallVirtual<61, bool>(v, tower, false))                                 // CVehicle::SetTowLink (vtable +0xF4)
            LOGD("Vehicle %04X hooked to %04X here", nv.netId, st.tower);
    // a remote player (or nobody) drives it: no local pad input must reach this vehicle. A trailer keeps its own
    // status: an abandoned one looks for a cab to hitch (CTrailer::ScanForTowLink), whoever's game that cab is in.
    // A towed one keeps STATUS_IS_TOWED: with any other its ProcessControl breaks the link (0x6B32A7)
    if (!wreck && v->Driver() != me && v->VehicleSubClass() != VEHICLE_TYPE_TRAILER && !tower) v->SetStatus(STATUS_PHYSICS);

    // spawned here a moment ago: it comes into sight over half a second (CVehicle::UpdateClumpAlpha goes on from where
    // it is, for the cars that have it), not out of nowhere in front of the player
    if (nv.fadeMs) {
        uint64_t t = NowMs() - nv.fadeMs;
        if (void* clump = v->RwObject()) Call<0x732B00>(clump, (int)std::min<uint64_t>(255, t * 255 / 500));  // SetClumpAlpha
        if (t >= 500) nv.fadeMs = 0;
    }
    // from the state's leaving its owner's game: the time it took to come here too (0.4.0). From its arrival only, a car
    // was shown a network trip behind its owner's: at 100 km/h some 3 m, a car's length, with the usual ping.
    float age = std::min(0.45f, (float)(NowMs() - nv.recvMs) / 1000.0f + Net().LatencyFrom(nv.owner));
    // Where the owner's car is now: on from the last state with its speed and how it was speeding up or braking. With
    // the speed alone a braking car went on past the place it stops at, a car behind another drove into it here and
    // was pushed back by the next state: the queues at the lights kept bumping into each other. Braking, it goes no
    // farther than where that braking stops it.
    CVector disp = st.vel * age + nv.accel * (0.5f * age * age);
    CVector vNow = st.vel + nv.accel * age;
    float speed = st.vel.Length();
    if (speed > 0.1f) {
        float along = (nv.accel.x * st.vel.x + nv.accel.y * st.vel.y + nv.accel.z * st.vel.z) / speed;
        if (along < -0.5f) {
            float stop = speed * speed / (-2.0f * along), d = disp.Length();
            if (d > stop) disp = disp * (stop / d);
            if (age >= speed / -along) vNow = CVector{};
        }
    }
    CVector target = st.pos + disp;
    // The turn goes on too. The rotation was pulled to where the owner's car faced when the state left, while the physics
    // turned it on by its turn speed: between states a turning car lagged behind and jumped ahead with each new one. The
    // follow camera hid it, but a passenger aiming out of the window (the aim camera sits on the passenger, MODE_
    // AIMWEAPON_FROMCAR) saw the picture shake 10-30 times a second.
    Quat targetRot = st.rot.Advanced(CVector{st.turn.x, st.turn.y, std::fabs(st.turn.z) > 0.01f ? st.turn.z : nv.yawRate}, age);
    CVector cur = v->GetPosition();
    CVector err = target - cur;
    float dist = err.Length();
    bool resting = st.vel.Length() < 0.05f && dist < 0.15f;
    // no ground under it in this game (its collision is not loaded here): physics would drop it through the map,
    // so it stands exactly where its owner has it
    if (NowMs() - nv.groundCheckMs > 250) {
        nv.groundCheckMs = NowMs();
        bool found = false;
        Call<0x5696C0, float>(target.x, target.y, target.z + 2.0f, &found, (CEntity**)nullptr);  // FindGroundZFor3DCoord
        nv.groundless = !found;
    }
    // Held up above its owner's car while that one stays level: it stands on something only this game has there (a car
    // the owner's mission had moved away, an object). The lowrider contest of "Cesar Vialpando" kept the host's car
    // 1-3 m up in the partner's game, sent down at 4 m/s every frame and never getting there. It goes through what is
    // under it (the game lets them collide again once they part).
    bool heldUp = !nv.groundless && dist < 10.0f && cur.z - target.z > 0.5f && std::fabs(st.vel.z) < 1.0f;
    nv.aboveSinceMs = heldUp ? (nv.aboveSinceMs ? nv.aboveSinceMs : NowMs()) : 0;
    if (heldUp && NowMs() - nv.aboveSinceMs > 500) {
        nv.aboveSinceMs = NowMs();
        ColPoint cp{};
        CEntity* under = nullptr;
        CVector down = cur - CVector{0.0f, 0.0f, 3.0f};
        Ref<CEntity*>(kIgnoreEntity) = v;
        bool hit = Call<0x56BA00, bool>(&cur, &down, &cp, &under, false, true, false, true, false, false, false, false);  // ProcessLineOfSight
        Ref<CEntity*>(kIgnoreEntity) = nullptr;
        if (hit && under && under != v && (under->IsVehicle() || under->IsObject())) {
            Field<CEntity*>(v, kIgnoredCollision) = under;
            if (under != nv.goneThrough) {
                nv.goneThrough = under;
                LOGI("Vehicle %04X stood %.1f m above its owner's on a %s (model %d) that is not there in its game: through it",
                     nv.netId, cur.z - target.z, under->IsVehicle() ? "vehicle" : "object", under->ModelIndex());
            }
        }
    }
    if (nv.groundless) {
        SetVehicleMatrix(v, target, targetRot);
        v->MoveSpeed() = CVector{};
        v->TurnSpeed() = CVector{};
    } else if (dist > 10.0f) {
        SetVehicleMatrix(v, target, targetRot);
        v->MoveSpeed() = st.vel * (1.0f / 50.0f);
        v->TurnSpeed() = st.turn * (1.0f / 50.0f);
    } else if (!resting) {
        CVector cmd = vNow + err * 4.0f;  // converge in ~1/4 s
        v->MoveSpeed() = cmd * (1.0f / 50.0f);
        v->TurnSpeed() = st.turn * (1.0f / 50.0f);
        if (CMatrix* m = v->Matrix()) {
            // the physics step turns it on by the turn speed after this, as far as the next frame's target moves on
            Quat q = Quat::Slerp(Quat::FromMatrix(*m), targetRot, std::min(1.0f, dt * 12.0f));
            q.ToMatrix(*m);
            CallMethod<0x446F90>(v);
        }
    }
    if (wreck) return;
    // Under 250 health a car burns and blows up 5 s later by its own timer: the copy went up apart from its owner's car
    // (often still driving) and stayed a wreck. It burns and smokes here as there, and blows up with it (F_WRECKED).
    int type = v->VehicleSubClass();
    if (type == VEHICLE_TYPE_BIKE || type == VEHICLE_TYPE_BMX)
        Field<float>(v, 0x7BC) = std::min(Field<float>(v, 0x7BC), 4000.0f);  // CBike::m_BlowUpTimer
    else if (type != VEHICLE_TYPE_BOAT && type != VEHICLE_TYPE_TRAIN)
        Field<float>(v, 0x8E4) = std::min(Field<float>(v, 0x8E4), 4000.0f);  // CAutomobile::m_fBurnTimer
    if (nv.hasLook && NowMs() - nv.lookCheckMs >= 500) {
        nv.lookCheckMs = NowMs();
        ApplyLook(nv);
    }
    // The front wheels as the owner's: an AI car on rails there (out of its player's way, CCarCtrl::UpdateCarOnRails) has
    // them straight and only its body turns, so a corner was taken with the wheels straight. Then they follow the turn.
    float wantSteer = std::fabs(st.steer) > 0.02f ? st.steer : SteerForMotion(v, st.vel, nv.yawRate);
    nv.steerShown += (wantSteer - nv.steerShown) * std::min(1.0f, dt * 10.0f);
    v->SteerAngle() = nv.steerShown;
    v->GasPedal() = st.gas;
    v->BrakePedal() = st.brake;
    // The handbrake as the owner's. Nothing clears it on a copy (only a player's car takes it from the pad, CAutomobile::
    // ProcessControlInputs), and once this game set it (a player's copy getting in) a car with a passenger went on with
    // its wheels locked, a bike bent over, for the other players.
    v->SetHandbrake((st.flags & VehicleState::F_HANDBRAKE) != 0);
    v->Health() = st.health;
    v->SetEngineOn((st.flags & VehicleState::F_ENGINE) != 0);
    v->SetSiren((st.flags & VehicleState::F_SIREN) != 0);
    int16_t alarm = Field<int16_t>(v, 0x45C);  // CVehicle::m_nAlarmState: a car alarm keeps its time in the horn counter
    if (alarm == 0 || alarm == -1) Field<uint32_t>(v, 0x514) = (st.flags & VehicleState::F_HORN) ? 1 : 0;  // the siren's yelp asks it
    ApplyParts(v, st);
    ApplyHydraulics(v, st);
}

void Vehicles::OnPreProcess() {
    if (!Net().Online() || !core::IsPlayerReady()) return;
    uint64_t now = NowMs();
    for (auto it = m_pending.begin(); it != m_pending.end();) {
        PendingVehicle& pv = it->second;
        if (pv.sp.owner == kOrphan) {  // a record of a car whose owner left: nobody here can take it
            it = m_pending.erase(it);
            continue;
        }
        it = WantMirror(pv.sp.netId, pv.sp.pos, false) && CreateMirror(pv) ? m_pending.erase(it) : std::next(it);
    }
    // copies far from this game's player go (twice a second is enough)
    if (now - m_lastPark >= 500) {
        m_lastPark = now;
        CPlayerPed* me = FindPlayerPed(0);
        std::vector<uint16_t> far, unclaimed;
        for (auto& [id, nv] : m_byId) {
            if (!IsMirror(nv) || !IsVehicleValid(nv.veh)) continue;
            if (nv.owner == kOrphan) {
                if (now - nv.orphanMs > kOrphanMs) unclaimed.push_back(id);
                continue;
            }
            bool mine = me->InVehicle() && me->Vehicle() == nv.veh;
            if (!mine && !WantMirror(id, nv.veh->GetPosition(), true)) far.push_back(id);
        }
        for (uint16_t id : far)
            if (NetVehicle* nv = Find(id)) Park(*nv);
        for (uint16_t id : unclaimed) {
            LOGI("Vehicle %04X: nobody took it after its owner left, it goes", id);
            Drop(id);
        }
    }
    float dt = TimeStep() / 50.0f;
    std::vector<uint16_t> dead;
    for (auto& [id, nv] : m_byId) {
        if (!nv.veh || !IsVehicleValid(nv.veh) || nv.veh->ModelIndex() != nv.model) {
            dead.push_back(id);
            continue;
        }
        if (IsMirror(nv)) ApplyMirror(nv, dt);
    }
    for (uint16_t id : dead) {
        NetVehicle* nv = Find(id);
        if (nv && !IsMirror(*nv)) {
            Writer w(proto::G_VEH_REMOVE);
            w.U16(id);
            Net().Send(proto::CH_RELIABLE, w);
        }
        if (nv) m_byPtr.erase(nv->veh);
        m_byId.erase(id);
    }
}

void Vehicles::SendState(NetVehicle& nv) {
    VehicleState st = VehicleState::Capture(nv.veh);
    st.time = (uint32_t)NowMs();
    if (CVehicle* t = Field<CVehicle*>(nv.veh, 0x4C4))  // CVehicle::m_pTowingVehicle: the other game hooks its copies too
        if (NetVehicle* tn = Find(t)) {
            st.extras |= VehicleState::X_TOW;
            st.tower = tn->netId;
        }
    nv.sentHealth = st.health;
    Writer w(proto::G_VEH_SYNC);
    w.U16(nv.netId);
    st.Write(w);
    Net().SendTo(nv.syncTo, proto::CH_STATE, w);
}

void Vehicles::ScanTraffic() {
    CPoolHeader* pool = VehiclePool();
    if (!pool) return;
    std::vector<uint16_t> gone;
    for (auto& [id, nv] : m_byId)
        if (!IsMirror(nv) && IsVehicleValid(nv.veh) && !HasPlayerInside(nv.veh) &&
            RemoteDistance(nv.veh->GetPosition(), nv.veh->AreaCode()) > kTrafficOut)
            gone.push_back(id);
    for (uint16_t id : gone) {
        Writer w(proto::G_VEH_REMOVE);
        w.U16(id);
        Net().Send(proto::CH_RELIABLE, w);
        // a copy of the other game's car this player took: off the network an ordinary car (a mission one stayed forever)
        if (NetVehicle* nv = Find(id); nv && nv->createdByUs && IsVehicleValid(nv->veh) &&
                                       nv->veh->CreatedBy() == VEHICLE_CREATED_MISSION && !ScriptHolds(nv->veh) &&
                                       GetVehicleRef(nv->veh) != m_kept)
            SetVehicleCreatedBy(nv->veh, VEHICLE_CREATED_RANDOM);
        Forget(id, false);
    }
    for (int i = 0; i < pool->size; ++i) {
        if (pool->byteMap[i] < 0) continue;
        auto* v = (CVehicle*)(pool->objects + (size_t)i * kVehiclePoolElemSize);
        if (m_byPtr.count(v) || !IsSyncableVehicle(v)) continue;
        if (RemoteDistance(v->GetPosition(), v->AreaCode()) < kTrafficIn) EnsureNetworked(v);
    }
}

// The car the player came in waits outside while the player is in an interior. An interior lies kilometres from its
// door (CCarCtrl::PossiblyRemoveVehicle measures on the map), so the game dropped the car, or handed it to the other
// player's game as an ordinary car that went as soon as that player got far too: both found nothing outside.
void Vehicles::KeepCarWhileInside(CPlayerPed* me) {
    if (me->InVehicle() && me->Vehicle() && me->Vehicle()->Driver() == me) m_lastDriven = GetVehicleRef(me->Vehicle());
    int8_t area = me->AreaCode();
    if (area != 0 && m_lastArea == 0 && m_kept < 0) {
        CVehicle* v = GetVehicle(m_lastDriven);
        uint8_t by = v ? v->CreatedBy() : 0;
        if (v && v->AreaCode() == 0 && (by == VEHICLE_CREATED_RANDOM || by == VEHICLE_CREATED_PARKED)) {
            m_kept = m_lastDriven;
            m_keptBy = by;
            SetVehicleCreatedBy(v, VEHICLE_CREATED_MISSION);
            LOGI("The player's car (model %d) is kept outside while the player is in interior %d", v->ModelIndex(), area);
        }
    } else if (area == 0 && m_lastArea != 0) {
        ReleaseKeptCar();
    }
    m_lastArea = area;
}

void Vehicles::ReleaseKeptCar() {
    CVehicle* v = m_kept >= 0 ? GetVehicle(m_kept) : nullptr;
    if (v && v->CreatedBy() == VEHICLE_CREATED_MISSION && !ScriptHolds(v)) SetVehicleCreatedBy(v, m_keptBy);
    m_kept = -1;
}

namespace {
// AI traffic out of the way of its game's physics runs on rails (STATUS_SIMPLE, CCarCtrl::UpdateCarOnRails): its body
// follows the road, its steering angle stays 0, and it took the corners with its front wheels straight. Near a player
// (ours, or another one, whose game shows its state) the wheels follow its turn. Before the states go out: they carry it.
struct RailSteer {
    float heading = 0, yaw = 0, steer = 0;
    CVector pos, vel;  // its speed by its moves (a car on rails may have none of its own)
    uint64_t ms = 0;
};
std::map<CVehicle*, RailSteer> g_railSteer;
constexpr uint8_t STATUS_SIMPLE = 2;

void SteerRailCars(CPlayerPed* me) {
    CPoolHeader* pool = VehiclePool();
    if (!pool || !me) return;
    uint64_t now = NowMs();
    for (auto it = g_railSteer.begin(); it != g_railSteer.end();)
        it = IsVehicleValid(it->first) && it->first->Status() == STATUS_SIMPLE ? std::next(it) : g_railSteer.erase(it);
    CVector mp = me->GetPosition();
    for (int i = 0; i < pool->size; ++i) {
        if (pool->byteMap[i] < 0) continue;
        auto* v = (CVehicle*)(pool->objects + (size_t)i * kVehiclePoolElemSize);
        CMatrix* m = v->Matrix();
        if (!m || v->Status() != STATUS_SIMPLE || !v->Driver() || !HasSteeredWheels(v)) continue;
        if (NetVehicle* nv = Vehicles::Instance().Find(v); nv && Vehicles::Instance().IsMirror(*nv)) continue;
        CVector p = v->GetPosition();
        if ((p - mp).Length() > 150.0f && RemoteDistance(p, v->AreaCode()) > 150.0f) continue;
        float heading = std::atan2(-m->forward.x, m->forward.y);
        RailSteer& r = g_railSteer[v];
        if (r.ms) {
            float dt = (float)(now - r.ms) / 1000.0f;
            if (dt > 0.005f && dt < 0.5f) {
                r.yaw = r.yaw * 0.7f + std::clamp(WrapPi(heading - r.heading) / dt, -3.0f, 3.0f) * 0.3f;
                r.vel = r.vel * 0.7f + (p - r.pos) * (0.3f / dt);
            }
        }
        r.heading = heading;
        r.pos = p;
        r.ms = now;
        CVector vel = v->MoveSpeed().Length() > 0.01f ? v->MoveSpeed() * 50.0f : r.vel;
        float want = SteerForMotion(v, vel, r.yaw);
        r.steer += (want - r.steer) * 0.2f;
        v->SteerAngle() = r.steer;
    }
}
}  // namespace

void Vehicles::OnPostProcess() {
    if (!Net().Online() || !core::IsPlayerReady()) return;
    CPlayerPed* me = FindPlayerPed(0);
    uint64_t now = NowMs();
    SteerRailCars(me);
    KeepCarWhileInside(me);
    for (auto& [v, to] : m_giveAway)  // still ours and still free to go
        if (NetVehicle* nv = IsVehicleValid(v) ? Find(v) : nullptr; nv && !IsMirror(*nv) && CanGiveAway(v)) GiveAway(*nv, to);
    m_giveAway.clear();
    // a trailer or a car this player's vehicle tows is simulated here from now on
    if (CVehicle* mine = me->InVehicle() && me->Vehicle() && me->Vehicle()->Driver() == me ? me->Vehicle() : nullptr) {
        int n = 0;  // a baggage train: each trailer tows the next
        for (auto* towed = Field<CVehicle*>(mine, 0x4C8); towed && n < 4; towed = Field<CVehicle*>(towed, 0x4C8), ++n)
            if (NetVehicle* tn = Find(towed); tn && IsMirror(*tn)) TakeOwnership(*tn);  // CVehicle::m_pVehicleBeingTowed
    }
    if (now - m_lastScan >= 250) {
        m_lastScan = now;
        ScanTraffic();
    }
    for (auto& [id, nv] : m_byId) {
        if (IsMirror(nv)) {
            // keep the visual controls after the physics step (wheels, brake lights)
            if (nv.hasState) {
                nv.veh->SteerAngle() = nv.steerShown;
                nv.veh->BrakePedal() = nv.state.brake;
                nv.veh->SetHandbrake((nv.state.flags & VehicleState::F_HANDBRAKE) != 0);
                ApplyParts(nv.veh, nv.state);  // the step just leaned a bike again by its speed change
                // CHeli::ProcessControl just switched its searchlight off (no police AI, no player here) and
                // CHeli::PreRender (0x6C5420) adds the light to this frame at night: as the owner's
                if (nv.veh->VehicleSubClass() == VEHICLE_TYPE_HELI) {
                    Field<uint8_t>(nv.veh, 0xA11) = (nv.state.extras & VehicleState::X_LIGHT) != 0;  // m_bSearchLightEnabled
                    Field<CVector>(nv.veh, 0x9F4) = nv.state.lightTarget;                          // m_vecSearchLightTarget
                    Field<float>(nv.veh, 0xA00) = nv.state.lightPower;                             // m_fSearchLightIntensity
                }
            }
            continue;
        }
        // owned: mod shop changes, to the players near it (and to each one that comes near: it is made there then)
        if (now - nv.lookCheckMs >= 1000) {
            nv.lookCheckMs = now;
            SendLook(nv, StateTargets(nv));
        }
        // owned: the local driver's state goes in G_PLAYER_DRIVER (Players); here only unoccupied vehicles
        if (me && nv.veh->Driver() == me) {
            nv.lastChangeMs = now;
            continue;
        }
        // A car of the host's mission (its id has the host's number) the partner drove was the partner's game's from then
        // on, and the host's mission moving, freezing, mending or deleting it only touched its copy there, which the
        // owner's states undid. Left standing empty for 2 s, it goes back to the host.
        if (nv.script && !Net().IsHost() && proto::EntityMaker(nv.netId) == Net().HostId() && !nv.veh->Driver() &&
            !HasPlayerInside(nv.veh) && now - nv.lastChangeMs > 2000) {
            nv.owner = Net().HostId();
            nv.hasState = false;
            Protect(nv);
            Writer w(proto::G_VEH_OWNER);
            w.U16(nv.netId);
            w.U8(nv.owner);
            Net().Send(proto::CH_RELIABLE, w);
            LOGI("Vehicle %04X of the host's mission goes back to the host", nv.netId);
            continue;
        }
        float speed = nv.veh->MoveSpeed().Length() * 50.0f;
        // an overturned or burning car loses ~200 hp/s standing still: its copy's smoke (white to black by health)
        // and fire lagged 2 s behind
        if (speed > 0.1f || std::fabs(nv.veh->Health() - nv.sentHealth) >= 1.0f) nv.lastChangeMs = now;
        bool active = now - nv.lastChangeMs < 3000;
        bool towed = Field<CVehicle*>(nv.veh, 0x4C4) != nullptr;  // CVehicle::m_pTowingVehicle
        // moving near another player: 20 states a second (the traffic around that player keeps its distances there)
        bool close = active && RemoteDistance(nv.veh->GetPosition(), nv.veh->AreaCode()) < 80.0f;
        uint64_t interval = towed || close ? 50 : active ? 100 : 2000;
        proto::PlayerMask targets = StateTargets(nv);
        bool newcomer = (targets & ~nv.syncTo) != 0;  // came near: where it stands now, at once
        if (now - nv.lastSendMs >= interval || newcomer) {
            nv.lastSendMs = now;
            nv.syncTo = targets;
            SendState(nv);
        }
    }
}

void Vehicles::OnSessionEnd() {
    ReleaseKeptCar();
    // mirrors stay in the world as this game's cars (the other game's scripts hold nothing here any more)
    for (auto& [id, nv] : m_byId)
        if (IsMirror(nv)) {
            nv.script = false;
            Release(nv);
        }
    m_byId.clear();
    m_byPtr.clear();
    m_pending.clear();
}

void Vehicles::OnPeerLeave(uint8_t id) {
    for (auto it = m_pending.begin(); it != m_pending.end();)
        it = it->second.sp.owner == id ? m_pending.erase(it) : std::next(it);
    // the vehicles of the leaving player stay in the world (its scripts are gone with it): the player nearest to each
    // takes it over, the others' copies follow that one; whoever sits in it first
    for (auto& [vid, nv] : m_byId) {
        if (nv.owner != id) continue;
        nv.owner = kOrphan;
        nv.hasState = false;
        nv.script = false;
        nv.orphanMs = NowMs();
        if (IsVehicleValid(nv.veh) && ShouldAdopt(nv, id)) Claim(nv);
    }
}

bool Vehicles::ShouldAdopt(const NetVehicle& nv, uint8_t leaver) const {
    CPlayerPed* me = FindPlayerPed(0);
    if (!me) return false;
    if (me->InVehicle() && me->Vehicle() == nv.veh) return true;
    CVector p = nv.veh->GetPosition();
    float mine = (p - me->GetPosition()).Length();
    for (auto& [pid, rp] : Players::Instance().All()) {
        if (pid == leaver || !rp.hasState || !Net().Peer(pid)) continue;
        if (rp.mode != RemotePlayer::Mode::OnFoot && rp.vehNetId == nv.netId) return false;  // it sits in it
        float d = (p - rp.state.pos).Length();
        if (d < mine || (d == mine && pid < Net().MyId())) return false;
    }
    return true;
}

void Vehicles::Claim(NetVehicle& nv) {
    nv.owner = Net().MyId();
    nv.claimMs = NowMs();
    nv.lastChangeMs = NowMs();
    nv.hasLook = false;
    nv.lookTo = nv.syncTo = 0;
    Release(nv);
    Writer w(proto::G_VEH_OWNER);
    w.U16(nv.netId);
    w.U8(nv.owner);
    Net().Send(proto::CH_RELIABLE, w);
    LOGI("Vehicle %04X (model %d): its owner left, this game took it", nv.netId, nv.model);
}

void Vehicles::OnSessionStart() {
    // another start than the last process's: the others may still know its ids, and the cars of a player that left
    // keep theirs (with its number in them) when it comes back
    m_nextId = (uint16_t)NowMs();
}

void Vehicles::OnGameStarted() {
    g_railSteer.clear();
    m_lastDriven = m_kept = -1;  // a new world: the old handles mean other cars
    m_lastArea = 0;
    m_byId.clear();
    m_byPtr.clear();
    m_pending.clear();
    m_giveAway.clear();
}

void Vehicles::OnGameShutdown() { OnGameStarted(); }

}  // namespace coop
