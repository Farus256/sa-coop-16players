// The host's mission shows a scene in the game itself (no cutscene file, the world keeps going): its script takes the
// camera (SET_FIXED_CAMERA_POSITION, POINT_CAMERA_AT_POINT/CHAR/CAR, the camera's moves and zooms) and gives it back
// (RESTORE_CAMERA). The partners saw none of it: their camera stayed behind their own player, wherever that was.
// Now the host's game tells them each of those steps (G_SCRIPT_CAM) as its mission's script takes it, and their camera
// does the same: a partner further than 20 m from the host is brought next to him (into his car when there is a
// seat), its controls are held while the scene lasts, and the camera comes back to the player when the host's does
// (or when the host's mission is over). The subtitles, the voices and the widescreen borders of such scenes come as
// they did before (Missions).
//
// A host in a car with no free seat (the mission's people fill it: "Drive-Thru", "Drive-By") takes nobody into its
// scene: the partner was taken out of its own car, stood by his and held there, and the car drove off without it. It
// keeps its camera, its car and its controls and follows him. The host's camera given back while a cutscene still
// played here (it ends a moment later than the host's) was dropped with the rest, and the partner stood held until the
// end of the mission: now the hold goes with it, and with any giving back of this game's camera (the cutscene's end).
#include <cmath>

#include "common/api/protocol/Protocol.h"
#include "common/lib/log/Log.h"
#include "common/lib/util/Util.h"
#include "entities/mission/Mission.h"
#include "entities/ped/Peds.h"
#include "entities/player/Players.h"
#include "entities/vehicle/Vehicles.h"
#include "shared/api/net-session/Session.h"
#include "shared/game/Game.h"
#include "shared/game/Script.h"
#include "shared/lib/core/Core.h"
#include "shared/lib/memory/Memory.h"

namespace coop {

namespace {
using namespace game;
using game::script::Cmd;

enum : uint8_t { OP_FIXED = 1, OP_POINT = 2, OP_ENTITY = 3, OP_RESTORE = 4, OP_MOVE = 5, OP_TRACK = 6, OP_FOV = 7 };
enum : uint8_t { TARGET_PLAYER = 1, TARGET_PED = 2, TARGET_VEHICLE = 3 };
// CCamera's whoIsInControlOfTheCamera: a script, as the script commands pass it (POINT_CAMERA_AT_POINT 0x47C5EC). It
// was 2, the cinematic camera's (Obbe), and CCamera::CamControl gives that one back in its very frame (0x52B64F) when
// no cinematic camera may run: the partner's camera came straight back, its controls stayed held to the mission's end
constexpr int kScriptControl = 1;
constexpr uint16_t kOwnControls = 0x4000;        // our bit of CPad::DisablePlayerControls
constexpr float kBringFrom = 20.0f;              // a partner further than this from the host is brought to him
void* Camera() { return (void*)addr::TheCamera; }
Vec3 V(const CVector& v) { return {v.x, v.y, v.z}; }
CVector G(const Vec3& v) { return {v.x, v.y, v.z}; }

// ---------------------------------------------------------------- host: what its mission's script does
bool Ours() { return Net().Online() && Net().IsHost() && CurrentScript() && Field<uint8_t>(CurrentScript(), 0xDC) && HostReplicating(); }

void Send(Writer& w) { Net().Send(proto::CH_RELIABLE, w); }

using FixedFn = void(__fastcall*)(void*, void*, const CVector*, const CVector*);
FixedFn g_origFixed = nullptr;
void __fastcall HookFixed(void* cam, void* edx, const CVector* pos, const CVector* up) {
    g_origFixed(cam, edx, pos, up);
    if (!Ours()) return;
    Writer w(proto::G_SCRIPT_CAM);
    w.U8(OP_FIXED);
    w.Vec(V(*pos));
    w.Vec(V(*up));
    Send(w);
}

using PointFn = void(__fastcall*)(void*, void*, const CVector*, int, int);
PointFn g_origPoint = nullptr;
void __fastcall HookPoint(void* cam, void* edx, const CVector* at, int sw, int who) {
    g_origPoint(cam, edx, at, sw, who);
    if (!Ours()) return;
    Writer w(proto::G_SCRIPT_CAM);
    w.U8(OP_POINT);
    w.Vec(V(*at));
    w.U8((uint8_t)sw);
    Send(w);
}

using EntityFn = void(__fastcall*)(void*, void*, CEntity*, int, int, int);
EntityFn g_origEntity = nullptr;
void __fastcall HookEntity(void* cam, void* edx, CEntity* target, int mode, int sw, int who) {
    g_origEntity(cam, edx, target, mode, sw, who);
    if (!Ours() || !target) return;
    uint8_t kind = 0;
    uint16_t id = 0;
    if (target == (CEntity*)FindPlayerPed(0)) {
        kind = TARGET_PLAYER, id = Net().MyId();
    } else if (RemotePlayer* rp = target->IsPed() ? Players::Instance().FindByPed((CPed*)target) : nullptr) {
        kind = TARGET_PLAYER, id = rp->id;
    } else if (NetPed* np = target->IsPed() ? Peds::Instance().Find((CPed*)target) : nullptr) {
        kind = TARGET_PED, id = np->netId;
    } else if (NetVehicle* nv = target->IsVehicle() ? Vehicles::Instance().EnsureNetworked((CVehicle*)target) : nullptr) {
        kind = TARGET_VEHICLE, id = nv->netId;
    }
    Writer w(proto::G_SCRIPT_CAM);
    w.U8(OP_ENTITY);
    w.U8(kind);
    w.U16(id);
    w.U16((uint16_t)mode);
    w.U8((uint8_t)sw);
    w.Vec(V(target->GetPosition()));  // where it is: the camera looks there when the partner's game has no copy of it
    Send(w);
}

using VoidFn = void(__fastcall*)(void*, void*);
VoidFn g_origRestore = nullptr, g_origRestoreCut = nullptr;
void Let();
void SendRestore(bool cut) {
    Let();  // partner: this game's camera is given back (a cutscene's end), the host's scene holds it no more
    if (!Ours()) return;
    Writer w(proto::G_SCRIPT_CAM);
    w.U8(OP_RESTORE);
    w.U8(cut);
    Send(w);
}
void __fastcall HookRestore(void* cam, void* edx) {
    g_origRestore(cam, edx);
    SendRestore(false);
}
void __fastcall HookRestoreCut(void* cam, void* edx) {
    g_origRestoreCut(cam, edx);
    SendRestore(true);
}

using LinearFn = void(__fastcall*)(void*, void*, CVector*, CVector*, float, bool);
LinearFn g_origMove = nullptr, g_origTrack = nullptr;
void SendLinear(uint8_t op, const CVector* to, const CVector* from, float ms, bool ease) {
    if (!Ours()) return;
    Writer w(proto::G_SCRIPT_CAM);
    w.U8(op);
    w.Vec(V(*to));
    w.Vec(V(*from));
    w.F32(ms);
    w.U8(ease);
    Send(w);
}
void __fastcall HookMove(void* cam, void* edx, CVector* to, CVector* from, float ms, bool ease) {
    g_origMove(cam, edx, to, from, ms, ease);
    SendLinear(OP_MOVE, to, from, ms, ease);
}
void __fastcall HookTrack(void* cam, void* edx, CVector* to, CVector* from, float ms, bool ease) {
    g_origTrack(cam, edx, to, from, ms, ease);
    SendLinear(OP_TRACK, to, from, ms, ease);
}

using FovFn = void(__fastcall*)(void*, void*, float, float, float, bool);
FovFn g_origFov = nullptr;
void __fastcall HookFov(void* cam, void* edx, float a, float b, float ms, bool ease) {
    g_origFov(cam, edx, a, b, ms, ease);
    if (!Ours()) return;
    Writer w(proto::G_SCRIPT_CAM);
    w.U8(OP_FOV);
    w.F32(a);
    w.F32(b);
    w.F32(ms);
    w.U8(ease);
    Send(w);
}

// ---------------------------------------------------------------- partner: the same camera
bool g_active = false;  // the host's scene holds our camera
uint64_t g_activeMs = 0;

void Hold(bool on) {
    CPad* pad = GetPad(0);
    if (on) pad->DisablePlayerControls |= kOwnControls;
    else pad->DisablePlayerControls &= (uint16_t)~kOwnControls;
}

// the scene is over here without a word of the camera: the controls come back, the camera is as it is
void Let() {
    if (!g_active || Net().IsHost()) return;
    g_active = false;
    Hold(false);
    LOGI("Host's scene: our camera was given back here, our control too");
}

// the host sits in a car whose seats are all taken (by the mission's people) and we are not in it: no place for us in
// his scene, we go on our own
bool NoSeatByHost() {
    RemotePlayer* host = Players::Instance().Get(Net().HostId());
    CPlayerPed* me = FindPlayerPed(0);
    CVehicle* v = host && host->ped ? host->ped->VehicleIfIn() : nullptr;
    if (!v || !me || me->VehicleIfIn() == v) return false;
    for (int s = 0; s < v->MaxPassengers() && s < 8; ++s)
        if (!v->Passengers()[s]) return false;
    return true;
}

// next to the host (in his car, when it has a free seat), as the scene starts
void Bring() {
    RemotePlayer* host = Players::Instance().Get(Net().HostId());
    CPlayerPed* me = FindPlayerPed(0);
    if (!host || !host->ped || !me || me->Health() <= 0.0f || Players::Instance().LocalDowned()) return;
    CPed* hp = host->ped;
    if (hp->AreaCode() == me->AreaCode() && (hp->GetPosition() - me->GetPosition()).Length() < kBringFrom) return;
    int handle = GetPedRef(me);
    CVector p = me->GetPosition();
    if (me->InVehicle()) Cmd(0x0362).I(handle).F(p.x).F(p.y).F(p.z + 1.0f).Run();  // WARP_CHAR_FROM_CAR_TO_COORD
    if (CVehicle* v = hp->VehicleIfIn()) {
        for (int s = 0; s < v->MaxPassengers() && s < 8; ++s)
            if (!v->Passengers()[s]) {
                Cmd(0x0430).I(handle).I(GetVehicleRef(v)).I(s).Run();  // WARP_CHAR_INTO_CAR_AS_PASSENGER
                LOGI("Host's scene: brought into the host's car (seat %d)", s);
                return;
            }
    }
    float h = hp->CurrentRotation();
    CVector at = hp->GetPosition() + CVector{std::cos(h) * 1.5f, std::sin(h) * 1.5f, 0};  // at his right hand
    Cmd(0x04BB).I(hp->AreaCode()).Run();                         // SET_AREA_VISIBLE
    Cmd(0x0860).I(handle).I(hp->AreaCode()).Run();               // SET_CHAR_AREA_VISIBLE
    Cmd(0x00A1).I(handle).F(at.x).F(at.y).F(at.z - 1.0f).Run();  // SET_CHAR_COORDINATES
    Cmd(0x0173).I(handle).F(h * 57.29578f).Run();                // SET_CHAR_HEADING
    LOGI("Host's scene: brought next to the host");
}

// false: this scene is not ours (the host's car is full)
bool Begin() {
    g_activeMs = NowMs();
    if (g_active) return true;
    if (NoSeatByHost()) {
        static uint64_t saidMs = 0;
        if (NowMs() - saidMs > 5000) LOGI("Host's scene: his car is full, we keep our camera and control");
        saidMs = NowMs();
        return false;
    }
    g_active = true;
    Bring();
    Hold(true);
    LOGI("Host's scene: the camera follows the host's");
    return true;
}

void End(bool cut) {
    if (!g_active) return;
    g_active = false;
    Hold(false);
    if (cut) CallMethod<0x50BAB0>(Camera());  // CCamera::RestoreWithJumpCut
    else CallMethod<0x50B930>(Camera());      // CCamera::Restore
    LOGI("Host's scene over: the camera is ours again");
}

CEntity* Target(uint8_t kind, uint16_t id) {
    switch (kind) {
        case TARGET_PLAYER:
            if (id == Net().MyId()) return FindPlayerPed(0);
            if (RemotePlayer* rp = Players::Instance().Get((uint8_t)id)) return rp->ped;
            return nullptr;
        case TARGET_PED: {
            int h = Peds::Instance().HandleOf(id);
            return h >= 0 ? GetPed(h) : nullptr;
        }
        case TARGET_VEHICLE: {
            NetVehicle* nv = Vehicles::Instance().Find(id);
            return nv && Vehicles::Alive(*nv) ? nv->veh : nullptr;
        }
    }
    return nullptr;
}

void HandleCam(uint8_t from, Reader& r) {
    if (from != Net().HostId() || Net().IsHost()) return;
    uint8_t op = r.U8();
    if (!core::IsPlayerReady() || Ref<uint8_t>(0xB5F851)) {  // a cutscene: its end gives the camera back itself
        if (op == OP_RESTORE) Let();
        return;
    }
    switch (op) {
        case OP_FIXED: {
            CVector pos = G(r.Vec()), up = G(r.Vec());
            if (!r.Ok() || !Begin()) return;
            CallMethod<0x50BEC0>(Camera(), &pos, &up);  // CCamera::SetCamPositionForFixedMode
            break;
        }
        case OP_POINT: {
            CVector at = G(r.Vec());
            int sw = r.U8();
            if (!r.Ok() || !Begin()) return;
            CallMethod<0x50C8B0>(Camera(), &at, sw, kScriptControl);  // CCamera::TakeControlNoEntity
            break;
        }
        case OP_ENTITY: {
            uint8_t kind = r.U8();
            uint16_t id = r.U16();
            int mode = r.U16(), sw = r.U8();
            CVector at = G(r.Vec());
            if (!r.Ok() || !Begin()) return;
            if (CEntity* e = Target(kind, id)) CallMethod<0x50C7C0>(Camera(), e, mode, sw, kScriptControl);  // CCamera::TakeControl
            else CallMethod<0x50C8B0>(Camera(), &at, sw, kScriptControl);  // no copy of it here: where it is
            break;
        }
        case OP_RESTORE: {
            bool cut = r.U8() != 0;
            if (r.Ok()) End(cut);
            break;
        }
        case OP_MOVE:
        case OP_TRACK: {
            CVector to = G(r.Vec()), fromV = G(r.Vec());
            float ms = r.F32();
            bool ease = r.U8() != 0;
            if (!r.Ok() || !Begin()) return;
            if (op == OP_MOVE) CallMethod<0x50D160>(Camera(), &to, &fromV, ms, ease);  // CCamera::VectorMoveLinear
            else CallMethod<0x50D1D0>(Camera(), &to, &fromV, ms, ease);                // CCamera::VectorTrackLinear
            break;
        }
        case OP_FOV: {
            float a = r.F32(), b = r.F32(), ms = r.F32();
            bool ease = r.U8() != 0;
            if (r.Ok() && g_active) CallMethod<0x50D280>(Camera(), a, b, ms, ease);  // CCamera::LerpFOV (not on a camera of our own)
            break;
        }
    }
}

class MissionCamera : public Module {
public:
    const char* Name() const override { return "MissionCamera"; }
    void OnInstallHooks() override {
        mem::HookFn(0x50BEC0, &HookFixed, &g_origFixed, "CCamera::SetCamPositionForFixedMode");
        mem::HookFn(0x50C8B0, &HookPoint, &g_origPoint, "CCamera::TakeControlNoEntity");
        mem::HookFn(0x50C7C0, &HookEntity, &g_origEntity, "CCamera::TakeControl");
        mem::HookFn(0x50B930, &HookRestore, &g_origRestore, "CCamera::Restore");
        mem::HookFn(0x50BAB0, &HookRestoreCut, &g_origRestoreCut, "CCamera::RestoreWithJumpCut");
        mem::HookFn(0x50D160, &HookMove, &g_origMove, "CCamera::VectorMoveLinear");
        mem::HookFn(0x50D1D0, &HookTrack, &g_origTrack, "CCamera::VectorTrackLinear");
        mem::HookFn(0x50D280, &HookFov, &g_origFov, "CCamera::LerpFOV");
        Net().SetHandler(proto::G_SCRIPT_CAM, &HandleCam);
    }
    void OnSessionEnd() override { Reset(); }
    void OnGameShutdown() override {
        g_active = false;
        Hold(false);
    }
    void OnHostChanged(uint8_t) override { Reset(); }
    void OnPeerLeave(uint8_t id) override {
        if (id == Net().HostId()) Reset();
    }
    void OnPostProcess() override {
        if (!g_active || !core::IsPlayerReady()) return;
        Hold(true);  // the pause menu or a script may have given the controls back
        // the host's mission is over (failed, passed) with the camera still taken: ours comes back
        if (!HostMissionRecent(3000) && NowMs() - g_activeMs > 10000) End(true);
    }

private:
    static void Reset() {
        if (g_active && core::IsPlayerReady()) End(true);
        g_active = false;
        Hold(false);
    }
};

MissionCamera g_module;
struct Reg {
    Reg() { core::RegisterModule(&g_module); }
} g_reg;
}  // namespace

}  // namespace coop
