#include "entities/player/Players.h"

#include <windows.h>

#include <algorithm>
#include <cstdlib>
#include <map>
#include <set>
#include <cmath>

#include "common/api/protocol/Protocol.h"
#include "common/lib/log/Log.h"
#include "common/lib/util/Util.h"
#include "entities/mission/Mission.h"
#include "entities/ped/Peds.h"
#include "entities/session/Flow.h"
#include "entities/vehicle/Vehicles.h"
#include "entities/world-share/WorldShare.h"
#include "shared/api/net-session/Session.h"
#include "shared/config/Config.h"
#include "shared/game/Anim.h"
#include "shared/game/Script.h"
#include "shared/lib/memory/Memory.h"
#include "shared/ui/chat/Chat.h"
#include "shared/ui/renderer/Renderer.h"

namespace coop {

using namespace game;
using game::script::Cmd;

Players& Players::Instance() {
    static Players p;
    return p;
}

namespace {
struct Reg {
    Reg() { core::RegisterModule(&Players::Instance()); }
} g_reg;

constexpr int kRemoteSlot = 1;  // CWorld::Players index used for the remote player while it is processed
constexpr int kLastWeapon = 46;  // eWeaponType: the parachute
constexpr int kCellphone = 330;  // MODEL_CELLPHONE
constexpr float kTeleportDist = 5.0f;

// ------------------------------------------------------------------ context switching
// CStats::StatTypesInt (0xB79000, 223 ints) followed by CStats::StatTypesFloat (0xB79380, 82 floats): the remote
// player's skills, stamina, fat... while it is processed. Its running, swimming and shooting advance the progress
// counters after them (m_SprintStaminaCounter.. 0xB794D8, UpdateFatAndMuscleStats' state 0xB79534): those stay ours,
// and so do the stat update boxes (CStats::bShowUpdateStats, 0x8CDE56: "Stamina +", "Respect - Upgraded" texts).
constexpr uintptr_t kStatsBlock = 0xB79000;
constexpr size_t kStatsBlockSize = 0x4C8, kStatsKeptSize = 0x538;
constexpr uintptr_t kShowUpdateStats = 0x8CDE56;

// The remote player is a PLAYER1 ped as well, and what its processing does to the player's aim stays its own:
// CCamera::m_PlayerWeaponMode (cleared when it does not aim), m_vecAimingTargetCoors, and the lock-on crosshair
// CWeaponEffects gCrossHair[0] (the engine picks it by the ped type)
constexpr uintptr_t kCamWeaponMode = addr::TheCamera + 0x830, kCamAimingCoors = addr::TheCamera + 0x854;
constexpr uintptr_t kCrossHair = 0xC8A838;
// Goggles it puts on switch the screen's night or thermal vision (CPed::AddGogglesModel points the ped's
// m_pGogglesState, +0x500, at CPostEffects::m_bNightVision / m_bInfraredVision): the screen is this game's player's
constexpr uintptr_t kVision = 0xC402B8;
// its sniper shots shake the camera (CamShakeNoPos: TheCamera +0x12C force, +0x5C start) and every gun the pads
constexpr uintptr_t kCamShake = addr::TheCamera + 0x12C, kCamShakeStart = addr::TheCamera + 0x5C;

// ------------------------------------------------------------------ the remote players' player data
// Every remote player is a CPlayerPed of player slot 1 (CREATE_PLAYER 1). The game keeps a player's wanted level,
// clothes, movement, look pitch, gun spin... in the CPlayerPedData its ped points to (CPed +0x480): the slot's own
// (CPlayerInfo +0x04). Each remote player gets a copy of its own, so that several of them keep apart; the slot's stays
// for the engine's code that reaches it through the slot while a remote player is processed (FindPlayerWanted).
constexpr size_t kPlayerDataSize = 0xAC;
constexpr int kPlayerDataOffset = 0x04;  // CPlayerInfo::m_PlayerData
constexpr int kGroupField = 0x38;        // CPlayerPedData::m_nPlayerGroup
// the entity pointers in it that the game registers (CEntity::RegisterReference: nulled when the entity goes):
// m_pArrestingCop, m_pDangerCar, m_pCurrentProstitutePed, m_pLastProstituteShagged
constexpr int kDataRefs[] = {0x08, 0x30, 0xA4, 0xA8};
constexpr int kHsMissileTarget = 0x98;   // m_LastHSMissileTarget: compared only, not registered
constexpr uintptr_t kActiveGroups = 0xC098E0, kGroups = 0xC09920;  // CPedGroups::ms_activeGroups, ms_groups
constexpr size_t kGroupSize = 0x2D4, kGroupCount = 8;

void* SlotData() { return (uint8_t*)PlayerInfo(kRemoteSlot) + kPlayerDataOffset; }
int32_t& GroupOf(void* playerData) { return Field<int32_t>(playerData, kGroupField); }
int32_t LocalGroup() { return GroupOf((uint8_t*)PlayerInfo(0) + kPlayerDataOffset); }

// CREATE_PLAYER makes the new player a ped group of its own (its constructor: CPedGroups::AddGroup) and the remote
// player leaves it at once (SpawnRemote): 8 groups for the whole game, and gang members walking together take them.
// With none free the constructor would write before the array: the spawn waits for one. -1: none.
int FreePedGroup() {
    for (size_t i = 0; i < kGroupCount; ++i)
        if (!Ref<uint8_t>(kActiveGroups + i)) return (int)i;
    return -1;
}

// The remote players count as members of this game's player's group (they have none of their own). The group's "keep
// up" call (CPlayerPed::ProcessControl, every 128 frames when the group lags behind its leader) would come from each of
// them in CJ's voice, and the members would climb into a remote player's car: off while one is processed.
uint8_t* LocalGroupFollowFlag() {
    int32_t g = LocalGroup();
    if (g < 0 || g >= (int32_t)kGroupCount || !Ref<uint8_t>(kActiveGroups + g)) return nullptr;
    return (uint8_t*)(kGroups + (uintptr_t)g * kGroupSize + 4);  // CPedGroup::m_bMembersEnterLeadersVehicle
}

// A group nobody uses (the last free one), for a remote player while it is processed: what its processing tells its
// group (it got into a car: the members get in with it) reached this game's player's members all the same, and OG Loc,
// a member of the host's group in "OG Loc", got off the host's bike and went after the partner. -1: none free.
int IdlePedGroup() {
    for (int i = (int)kGroupCount - 1; i >= 0; --i)
        if (!Ref<uint8_t>(kActiveGroups + i)) return i;
    return -1;
}

// a remote player's group is the idle one for the scope (its processing, a warp into a car), this game's player's after:
// its own data and the slot's it is processed in
struct QuietGroup {
    void* data;
    int32_t saved = -1, slotSaved = -1;
    explicit QuietGroup(CPed* ped) : data(ped ? ped->PlayerData() : nullptr) {
        if (!data) return;
        saved = GroupOf(data), slotSaved = GroupOf(SlotData());
        if (int idle = IdlePedGroup(); idle >= 0) GroupOf(data) = GroupOf(SlotData()) = idle;
    }
    ~QuietGroup() {
        if (!data) return;
        GroupOf(data) = saved;
        GroupOf(SlotData()) = slotSaved;
    }
};

// CPedGroupMembership of group g (CPedGroup +8): m_apMembers[8] from +4, the leader the last (+0x20)
uintptr_t Membership(int32_t g) { return kGroups + (uintptr_t)g * kGroupSize + 8; }
CPed*& LeaderOf(uintptr_t membership) { return Ref<CPed*>(membership + 0x20); }
bool OurMembership(void* m) {
    int32_t g = LocalGroup();
    return g >= 0 && g < (int32_t)kGroupCount && (uintptr_t)m == Membership(g);
}

// A remote player is never a leader or a member of this game's player's group. Made its leader, the group followed it:
// OG Loc, the host's man in "OG Loc", ran after the partner and got into the partner's car instead of the host's bike.
using SetLeaderFn = void(__fastcall*)(void*, void*, CPed*);
SetLeaderFn g_origSetLeader = nullptr;
using AddMemberFn = void(__fastcall*)(void*, void*, CPed*, int);
AddMemberFn g_origAddMember = nullptr;
void KeptOut(const char* what, void* from) {
    static int said = 0;
    if (said++ < 20) LOGW("A remote player was made %s of our group (from %p): kept out", what, from);
}
void __fastcall HookSetLeader(void* membership, void* edx, CPed* ped) {  // CPedGroupMembership::SetLeader
    if (ped && OurMembership(membership) && Players::Instance().FindByPed(ped)) return KeptOut("the leader", __builtin_return_address(0));
    g_origSetLeader(membership, edx, ped);
}
void __fastcall HookAddMember(void* membership, void* edx, CPed* ped, int index) {  // CPedGroupMembership::AddMember
    if (ped && OurMembership(membership) && Players::Instance().FindByPed(ped)) return KeptOut("a member", __builtin_return_address(0));
    g_origAddMember(membership, edx, ped, index);
}
// whatever got past that (a reload, a respawn): this game's player leads its group again
void KeepOurLeader(CPlayerPed* me) {
    int32_t g = LocalGroup();
    if (g < 0 || g >= (int32_t)kGroupCount || !Ref<uint8_t>(kActiveGroups + g)) return;
    uintptr_t m = Membership(g);
    CPed* leader = LeaderOf(m);
    if (leader == me || !leader || !Players::Instance().FindByPed(leader)) return;
    LOGW("A remote player led our group: our player leads it again");
    g_origSetLeader((void*)m, nullptr, me);
}

void ReleaseOwnData(RemotePlayer& rp) {
    if (!rp.hasData) return;
    for (int off : kDataRefs)
        if (CEntity*& e = Field<CEntity*>(rp.data, off)) {
            CallMethod<0x571A00>(e, &e);  // CEntity::CleanUpOldReference: this memory is no longer a reference
            e = nullptr;
        }
    CallMethod<0x56F890>((void*)rp.data);  // CPlayerPedData::DeAllocateData: its wanted level and clothes
    rp.hasData = false;
}

// the ped's own copy of the player data its constructor set up in the slot
void TakeOwnData(RemotePlayer& rp, CPlayerPed* ped) {
    ReleaseOwnData(rp);
    memcpy(rp.data, SlotData(), kPlayerDataSize);
    Field<void*>(rp.data, 0x00) = nullptr;  // m_pWanted
    Field<void*>(rp.data, 0x04) = nullptr;  // m_pPedClothesDesc
    for (int off : kDataRefs) Field<void*>(rp.data, off) = nullptr;  // registered at the slot's address, not here
    Field<void*>(rp.data, kHsMissileTarget) = nullptr;
    CallMethod<0x571880>((void*)rp.data);  // CPlayerPedData::AllocateData: a wanted level and clothes of its own
    GroupOf(rp.data) = LocalGroup();
    ped->PlayerData() = rp.data;
    rp.hasData = true;
}

struct LocalContext {
    uint8_t stats[kStatsKeptSize];
    uint8_t showUpdateStats;
    CPad pads[2];  // whole: buttons, controls, shake
    int8_t focus;
    CPlayerPed* slotPed;
    uint16_t camMode;
    uint8_t weaponMode[12], aimingCoors[12], crossHair[0x2C], vision[2];
    float shake;
    uint32_t shakeStart;
    float myHealth;  // an ambulance it gets into heals "the player" (CVehicle::SetDriver's freebies: Players[0])
    float fov, orientation, alpha, beta;
    CVector front, source, up;
    uint8_t mouse3rd;
    float chairX, chairY;
};

void SaveLocal(LocalContext& c) {
    memcpy(c.stats, (void*)kStatsBlock, kStatsKeptSize);
    c.showUpdateStats = Ref<uint8_t>(kShowUpdateStats);
    memcpy(c.pads, GetPad(0), sizeof c.pads);
    c.focus = PlayerInFocus();
    c.slotPed = PlayerInfo(kRemoteSlot)->ped;
    c.camMode = cam::Mode();
    memcpy(c.weaponMode, (void*)kCamWeaponMode, sizeof c.weaponMode);
    memcpy(c.aimingCoors, (void*)kCamAimingCoors, sizeof c.aimingCoors);
    memcpy(c.crossHair, (void*)kCrossHair, sizeof c.crossHair);
    memcpy(c.vision, (void*)kVision, sizeof c.vision);
    c.shake = Ref<float>(kCamShake);
    c.shakeStart = Ref<uint32_t>(kCamShakeStart);
    c.myHealth = PlayerInfo(0)->ped ? PlayerInfo(0)->ped->Health() : 0.0f;
    c.fov = cam::Fov();
    c.orientation = cam::Orientation();
    c.alpha = cam::Alpha();
    c.beta = cam::Beta();
    c.front = cam::Front();
    c.source = cam::Source();
    c.up = cam::Up();
    c.mouse3rd = cam::UseMouse3rdPerson();
    c.chairX = cam::CrosshairMultX();
    c.chairY = cam::CrosshairMultY();
}

void RestoreLocal(const LocalContext& c) {
    memcpy((void*)kStatsBlock, c.stats, kStatsKeptSize);
    Ref<uint8_t>(kShowUpdateStats) = c.showUpdateStats;
    memcpy(GetPad(0), c.pads, sizeof c.pads);
    PlayerInFocus() = c.focus;
    PlayerInfo(kRemoteSlot)->ped = c.slotPed;
    cam::Mode() = c.camMode;
    memcpy((void*)kCamWeaponMode, c.weaponMode, sizeof c.weaponMode);
    memcpy((void*)kCamAimingCoors, c.aimingCoors, sizeof c.aimingCoors);
    memcpy((void*)kCrossHair, c.crossHair, sizeof c.crossHair);
    memcpy((void*)kVision, c.vision, sizeof c.vision);
    Ref<float>(kCamShake) = c.shake;
    Ref<uint32_t>(kCamShakeStart) = c.shakeStart;
    if (PlayerInfo(0)->ped) PlayerInfo(0)->ped->Health() = c.myHealth;
    cam::Fov() = c.fov;
    cam::Orientation() = c.orientation;
    cam::Alpha() = c.alpha;
    cam::Beta() = c.beta;
    cam::Front() = c.front;
    cam::Source() = c.source;
    cam::Up() = c.up;
    cam::UseMouse3rdPerson() = c.mouse3rd;
    cam::CrosshairMultX() = c.chairX;
    cam::CrosshairMultY() = c.chairY;
}

void ApplyRemote(RemotePlayer& rp) {
    if (rp.stats.size() == kStatsBlockSize) memcpy((void*)kStatsBlock, rp.stats.data(), kStatsBlockSize);  // its skills, stamina...
    Ref<uint8_t>(kShowUpdateStats) = 0;
    CPad* p0 = GetPad(0);
    CPad* p1 = GetPad(1);
    p0->NewState = p1->NewState = rp.padNew;
    p0->OldState = p1->OldState = rp.padOld;
    // its owner's activity holds it (a fall, a parachute, a script's animation): there its pad moves nothing either
    if (rp.animHeld) p0->NewState = p1->NewState = p0->OldState = p1->OldState = CControllerState{};
    p0->DisablePlayerControls = 0;
    memset(&p0->bApplyBrakes, 0, 8);
    PlayerInFocus() = (int8_t)kRemoteSlot;
    PlayerInfo(kRemoteSlot)->ped = rp.ped;
    GroupOf(SlotData()) = LocalGroup();  // this game's player's group (a reload gives it another)
    if (rp.hasData) GroupOf(rp.data) = LocalGroup();
    const CameraState& c = rp.state.cam;
    cam::Mode() = c.mode;
    cam::WeaponMode() = c.weaponMode;
    cam::Fov() = c.fov;
    cam::Orientation() = c.orientation;
    cam::Alpha() = c.alpha;
    cam::Beta() = c.beta;
    cam::Front() = c.front;
    cam::Source() = rp.ped->GetPosition() + c.sourceOffset;
    cam::Up() = c.up;
    cam::UseMouse3rdPerson() = c.useMouse3rd ? 1 : 0;
    cam::CrosshairMultX() = c.chairX;
    cam::CrosshairMultY() = c.chairY;
    if (rp.ped->PlayerData()) rp.ped->LookPitch() = c.lookPitch;
    // A task walks its owner (the walk-in through a shop's door, a script's go-to) while its pad is still: the copy slid
    // along standing and spun round. Its stick here is the one that walks it the same way: the game turns a player to
    // atan2(-x, -y) of the stick less the camera's orientation, a stick of 60 walks and of 120 runs (as fast as its owner
    // goes: its move state said run while the door's walk-in walked it).
    if ((rp.state.flags & OnFootState::F_TASK_WALK) && !rp.animHeld && rp.mode == RemotePlayer::Mode::OnFoot) {
        float speed = rp.state.vel.Length2D();
        float stick = speed > 2.6f ? 120.0f : speed > 0.3f ? 60.0f : 0.0f;
        float a = rp.state.rotation + c.orientation;
        p0->NewState.LeftStickX = p1->NewState.LeftStickX = (int16_t)(-std::sin(a) * stick);
        p0->NewState.LeftStickY = p1->NewState.LeftStickY = (int16_t)(-std::cos(a) * stick);
    }
}

// A driver's drive-by (CVehicle::DoDriveByShootings: looking aside and firing out of the window) runs only for a car of
// STATUS_PLAYER, and the remote player's car is a mirror here: it runs for it under its pad and camera. The shots only
// show: the shooter's game reports its hits.
void DriveBy(CPlayerPed* ped) {
    CVehicle* v = ped->InVehicle() ? ped->Vehicle() : nullptr;
    if (!v || v->Driver() != ped) return;
    uint8_t& typeStatus = Field<uint8_t>(v, 0x36);
    uint8_t saved = typeStatus;
    typeStatus &= 0x07;  // m_nStatus (bits 3..7): STATUS_PLAYER
    // with the usual car camera the side comes from the camera's looking left/right, and the camera is ours: the
    // remote player's look buttons set it for the call
    auto* camLook = (uint8_t*)(cam::Active() + 0x08);
    uint8_t look[2] = {camLook[0], camLook[1]};
    const CControllerState& pad = GetPad(0)->NewState;
    camLook[0] = pad.LeftShoulder2 && !pad.RightShoulder2;
    camLook[1] = pad.RightShoulder2 && !pad.LeftShoulder2;
    int clip = ped->ActiveWeapon().ammoInClip;
    CallMethod<0x741FD0>(v);
    camLook[0] = look[0], camLook[1] = look[1];
    typeStatus = saved;
    if (ped->ActiveWeapon().ammoInClip != clip) LOGD("Remote player drive-by: a shot");
}

// A passenger's drive-by is the game's split-screen mode's (CTaskSimpleCarDrive::ProcessPed 0x6449AB: a player in a
// passenger seat leans out of the window, CTaskSimpleGangDriveBy). In co-op it starts for a passenger holding the right
// mouse button (R1 on a pad) with any gun in hand, not only an SMG (the task takes any instant-hit weapon), and ends
// when the button is let go. The checks of the SMG slot and of the split-screen camera after the call are dropped.
// The player processed is the one in focus: the other player's copy is processed under its own pad, so its drive-by
// shows here too (its game folds the mouse button into R1, FillCommon).
std::map<CPed*, bool> g_ourDriveBy;  // passengers whose drive-by this started (a mission's is left alone)
bool AimFromSeat(const CPad* pad) { return pad->NewState.RightShoulder1 || pad->NewState.m_bVehicleMouseLook; }

bool __cdecl PassengerDriveBy() {
    const CPad* pad = GetPad(0);
    CPlayerPed* ped = FindPlayerPed(-1);
    if (!Net().Online() || !ped || pad->DisablePlayerControls || !AimFromSeat(pad) || cam::Mode() == 0x37) return false;  // MODE_AIMWEAPON_FROMCAR
    const CWeapon& w = ped->ActiveWeapon();
    void* info = w.type ? Call<0x743C60, void*>(w.type, (uint8_t)1) : nullptr;  // CWeaponInfo::GetWeaponInfo
    if (!info || Field<int32_t>(info, 0) != 1 || w.ammoTotal <= 0) return false;  // m_nWeaponFire: FIRE_INSTANT_HIT
    g_ourDriveBy[ped] = true;
    return true;
}

using GangDriveByFn = bool(__fastcall*)(void*, void*, CPed*);
GangDriveByFn g_origGangDriveBy = nullptr;
bool __fastcall HookGangDriveByProcess(void* task, void* edx, CPed* ped) {
    auto ours = g_ourDriveBy.find(ped);
    // let go: the task's own way out, command 7 (m_nNextCommand; a player's pad sets only 1 and 2 below 4). The rest of
    // the lean-out animation brings the passenger back in, and the task finishes when it is over. Finishing it at once
    // (m_bIsFinished) left the passenger leaning out of the window with the gun until two clicks.
    if (ours != g_ourDriveBy.end() && !AimFromSeat(GetPad(0))) Field<int8_t>(task, 0x0F) = 7;
    bool done = g_origGangDriveBy(task, edx, ped);
    if (done && ours != g_ourDriveBy.end()) g_ourDriveBy.erase(ours);
    return done;
}

// CCarEnterExit::IsPlayerToQuitCarEnter: a player gives up getting into a car by its pad (the stick pushed away a
// moment after it began). A copy runs under its owner's pad, which drives off or turns the moment the owner sits in
// the car: the copy gave up at the door. A copy gets in as its owner did; its owner's exit says when it gets out.
using QuitEnterFn = bool(__cdecl*)(CPed*, CVehicle*, int32_t, void*);
QuitEnterFn g_origQuitEnter = nullptr;
bool __cdecl HookQuitCarEnter(CPed* ped, CVehicle* v, int32_t start, void* task) {
    if (ped && ped != FindPlayerPed(0) && Players::Instance().FindByPed(ped)) return false;
    return g_origQuitEnter(ped, v, start, task);
}

// Two players dead or busted at about the same time come back at the same hospital or police station door, one inside
// the other: the one whose game was done first stood stuck in the other, still in its respawn view, until that ended.
// Ours comes back 1.5 m to the right when the other player's copy is at that door already (both at once: KeepApart).
using RestoreFn = void(__cdecl*)(CPlayerPed*, CVector, float);
RestoreFn g_origRestore = nullptr;
void __cdecl HookRestorePlayer(CPlayerPed* player, CVector pos, float heading) {
    // Killed or busted in the host's mission (its enemies go for every PLAYER1): back by the host, not at a hospital
    // across the map while the mission goes on. Only one who died near the host: where it was is still its place now.
    if (Net().Online() && !Net().IsHost() && player == FindPlayerPed(0) && HostMissionRecent(0))
        if (const RemotePlayer* host = Players::Instance().Get(Net().HostId());
            host && host->ped && host->hasState && host->mode == RemotePlayer::Mode::OnFoot && host->state.area == 0 &&
            !(host->state.flags & OnFootState::F_DEAD) && (player->GetPosition() - host->state.pos).Length() < 300.0f) {
            float h = host->state.rotation;
            pos = host->state.pos + CVector{std::cos(h), std::sin(h), 0.0f} * 2.0f;  // at its right hand
            heading = h * 57.29578f;
            LOGI("Respawn by the host: its mission goes on");
        }
    if (Net().Online() && player == FindPlayerPed(0))
        for (auto& [id, rp] : Players::Instance().All()) {
            if (!rp.ped) continue;
            CVector d = rp.ped->GetPosition() - pos;
            if (d.Length2D() > 1.5f || std::fabs(d.z) > 3.0f) continue;
            float h = heading * 0.0174533f;  // degrees: 0 faces north, the right is (cos, sin)
            pos = pos + CVector{std::cos(h), std::sin(h), 0.0f} * 1.5f;
            LOGI("Respawn 1.5 m aside: player %d stands at the door", id);
            break;
        }
    g_origRestore(player, pos, heading);
}

// The copy dies by TASK_DIE, and that task always falls the same way (KO_shot_front): its owner fell otherwise (a
// spin, a skid back, hit in the face). Our TASK_DIE gets the owner's animation (G_PLAYER_DEATH) in its constructor.
int g_dieAnim = -1;  // set around our TASK_DIE: the default group's animation to fall with
using DieCtorFn = void*(__fastcall*)(void*, void*, int, int, int, float, float, bool, bool, int, bool);
DieCtorFn g_origDieCtor = nullptr;
void* __fastcall HookDieCtor(void* task, void* edx, int weapon, int group, int anim, float delta, float speed, bool stealth,
                             bool falling, int dir, bool railing) {
    if (g_dieAnim >= 0) group = 0, anim = g_dieAnim;  // ANIM_GROUP_DEFAULT
    return g_origDieCtor(task, edx, weapon, group, anim, delta, speed, stealth, falling, dir, railing);
}

// DELETE_CHAR deletes whoever it is given (CTheScripts::RemoveThisPed). A mission that clears "the player's car" for
// the people it brings deletes its passengers: in "Sweet's Girl" the host rode with the partner driving, the host's own
// CJ was deleted, and the host's game crashed on its next look at him (the partner's screen stayed dark). A player, ours
// or the other one's copy, only gets out of the car; vanilla scripts never delete a player.
using RemovePedFn = void(__cdecl*)(CPed*);
RemovePedFn g_origRemoveThisPed = nullptr;
void __cdecl HookDeleteChar(CPed* ped) {
    if (ped && ped->IsPlayer()) {
        if (ped->InVehicle()) {
            CVector p = ped->GetPosition();
            Cmd(0x0362).I(GetPedRef(ped)).F(p.x).F(p.y).F(p.z + 1.0f).Run();  // WARP_CHAR_FROM_CAR_TO_COORD
        }
        LOGW("A script deleted a player (%s): out of its car instead", ped == FindPlayerPed(0) ? "ours" : "a copy");
        return;
    }
    g_origRemoveThisPed(ped);
}

using ProcessControlFn = void(__fastcall*)(CPlayerPed*, void*);
ProcessControlFn g_origProcessControl = nullptr;
bool g_inRemoteContext = false;
void FireOwedShot(RemotePlayer& rp);

void __fastcall HookPlayerProcessControl(CPlayerPed* self, void* edx) {
    RemotePlayer* rp = g_inRemoteContext ? nullptr : Players::Instance().FindByPed(self);
    if (!rp) {
        g_origProcessControl(self, edx);
        return;
    }
    LocalContext saved;
    SaveLocal(saved);
    ApplyRemote(*rp);
    uint8_t* follow = LocalGroupFollowFlag();
    uint8_t followSaved = follow ? *follow : 0;
    if (follow) *follow = 0;
    {
        QuietGroup quiet(self);
        g_inRemoteContext = true;
        g_origProcessControl(self, edx);
        DriveBy(self);  // still under its pad and camera
        FireOwedShot(*rp);
        g_inRemoteContext = false;
    }
    if (follow) *follow = followSaved;
    RestoreLocal(saved);
    // Aiming, the game turned the copy to where its owner's camera aims from where the copy stands: up to 7 degrees off.
    // A half turn on foot could also go round the other way. It faces as its owner.
    if (rp->mode == RemotePlayer::Mode::OnFoot && rp->hasState && !rp->animHeld && !self->InVehicle() &&
        !(rp->state.flags & OnFootState::F_DEAD) &&
        ((rp->state.pad.buttons & PadState::B_RS1) || std::fabs(WrapAngle(rp->state.rotation - self->CurrentRotation())) > 0.8f))
        self->CurrentRotation() = self->AimingRotation() = rp->state.rotation;
    Field<bool*>(self, 0x500) = nullptr;  // CPed::m_pGogglesState: taking its goggles off must not switch ours off
}

// The remote player's gang work (CPlayerPed::ProcessGroupBehaviour, called from its ProcessControl): recruiting this
// game's Grove members with its buttons, and the Grove gang's hate for cops from its wanted level here, which is not
// its real one. Its own game runs its gang.
using GroupBehaviourFn = void(__fastcall*)(CPlayerPed*, void*, CPad*);
GroupBehaviourFn g_origGroupBehaviour = nullptr;
void __fastcall HookGroupBehaviour(CPlayerPed* self, void* edx, CPad* pad) {
    if (!g_inRemoteContext) g_origGroupBehaviour(self, edx, pad);
}

// Its photos are its game's (a snapshot it gets there comes over as a pickup): here CWeapon::TakePhotograph flashed
// this screen, saved it to this player's gallery and paid this player for a snapshot in this view
using TakePhotoFn = bool(__fastcall*)(void*, void*, CEntity*, CVector*);
TakePhotoFn g_origTakePhoto = nullptr;
bool __fastcall HookTakePhotograph(void* weapon, void* edx, CEntity* owner, CVector* point) {
    return !g_inRemoteContext && g_origTakePhoto(weapon, edx, owner, point);
}

using GetInfoFn = CPlayerInfo*(__fastcall*)(CPlayerPed*, void*);
GetInfoFn g_origGetPlayerInfo = nullptr;
CPlayerInfo* __fastcall HookGetPlayerInfoForThisPlayerPed(CPlayerPed* self, void* edx) {
    if (Players::Instance().FindByPed(self)) return PlayerInfo(kRemoteSlot);
    return g_origGetPlayerInfo(self, edx);
}

// Every PLAYER1 ped gets real-time shadow number 0, and the remote player is one too (enemies attack it). The two
// players took that shadow from each other, and the one left holding it without owning it crashed the game when it was
// deleted: CPhysical::~CPhysical returns the shadow to its owner (a world load, the game's exit). The remote player
// takes a free shadow like any other ped.
using GetShadowFn = void*(__fastcall*)(void*, void*, CPed*);
GetShadowFn g_origGetShadow = nullptr;
void* __fastcall HookGetRealTimeShadow(void* manager, void* edx, CPed* e) {  // CRealTimeShadowManager::GetRealTimeShadow
    bool remote = e->IsPed() && e->PedType() == PED_TYPE_PLAYER1 && e != FindPlayerPed(0);
    if (remote) e->PedType() = PED_TYPE_PLAYER2;
    void* shadow = g_origGetShadow(manager, edx, e);
    if (remote) e->PedType() = PED_TYPE_PLAYER1;
    return shadow;
}

// Every vehicle's sound (CAEVehicleAudioEntity::Service) takes a PLAYER1 driver for the player: the remote player's
// car turned this game's radio on and took over the player's engine sounds, and with both players driving the two
// cars fought over one radio. For its sound the remote driver is not a player.
using VehicleAudioFn = void(__fastcall*)(void*, void*);
VehicleAudioFn g_origVehicleAudio = nullptr;
void __fastcall HookVehicleAudioService(void* audio, void* edx) {
    auto* veh = Field<CVehicle*>(audio, 0x4);  // CAEAudioEntity::m_pEntity
    CPed* driver = veh ? veh->Driver() : nullptr;
    bool remote = driver && driver->PedType() == PED_TYPE_PLAYER1 && driver != FindPlayerPed(0);
    if (remote) driver->PedType() = PED_TYPE_PLAYER_NETWORK;
    g_origVehicleAudio(audio, edx);
    if (remote) driver->PedType() = PED_TYPE_PLAYER1;
}

// The other players' weapon icon goes next to the health bar over their heads. The HUD's sprite code draws it right
// after the HUD: the icons are textures of the weapon models' dictionaries ("<model>ICON"), the fist is a HUD sprite.
void DrawWeaponIcon(int weapon, float x, float y, float w, float h, uint8_t alpha) {
    if (weapon < 0 || weapon > kLastWeapon) return;  // from the network
    struct { void* texture; } sprite{};  // CSprite2d
    void* info = Call<0x743C60, void*>(weapon, (uint8_t)1);  // CWeaponInfo::GetWeaponInfo, standard skill
    int model = info ? Field<int32_t>(info, 0x0C) : -1;     // m_nModelId1
    if (model <= 0) {
        sprite.texture = Ref<void*>(0xBAB1FC);  // CHud::Sprites[SPRITE_FIST]
    } else {
        void* mi = Ref<void*>(addr::ModelInfoPtrs + (uintptr_t)model * 4);
        auto* txds = Ref<CPoolHeader*>(0xC8800C);  // CTxdStore::ms_pTxdPool
        int slot = mi ? Field<int16_t>(mi, 0x0A) : -1;  // m_nTxdIndex
        if (!txds || slot < 0 || slot >= txds->size || txds->byteMap[slot] < 0) return;
        void* dict = Ref<void*>((uintptr_t)txds->objects + (uintptr_t)slot * 0xC);  // TxdDef::m_pRwDictionary
        uint32_t key = Call<0x53CF70, uint32_t>(Field<uint32_t>(mi, 0x04), "ICON");  // CKeyGen::AppendStringToKey
        if (dict) sprite.texture = Call<0x734E50, void*>(dict, key);               // RwTexDictionaryFindHashNamedTexture
    }
    if (!sprite.texture) return;
    float rect[4] = {x, y + h, x + w, y};  // CRect: left, bottom, right, top
    CRGBA color{255, 255, 255, alpha};
    CallMethod<0x728350>(&sprite, rect, &color);  // CSprite2d::Draw(const CRect&, const CRGBA&)
}

// and its wanted level left of the health bar: the HUD's own stars (CHud::DrawWanted: "]" of FONT_GOTHIC in gold),
// smaller. CFont::DrawFonts prints them right after the HUD.
void DrawWantedStars(int level, float right, float midY, uint8_t alpha) {
    if (level <= 0) return;
    float scaleX = ScreenWidth() / 640.0f * 0.605f * 0.6f, scaleY = ScreenHeight() / 448.0f * 1.21f * 0.6f;
    CRGBA gold = Ref<CRGBA>(0xBAB22C + 6 * 4);  // CHudColours HudColour: HUD_COLOUR_GOLD
    gold.a = alpha;
    Call<0x7195C0>(false, false);          // CFont::SetBackground
    Call<0x719380>(scaleX, scaleY);        // CFont::SetScale
    Call<0x719610>(2);                     // CFont::SetAlignment: right
    Call<0x7195B0>(true);                  // CFont::SetProportional
    Call<0x719490>(0);                     // CFont::SetFontStyle: FONT_GOTHIC
    Call<0x719590>(1);                     // CFont::SetOutlinePosition
    Call<0x719510>(CRGBA{0, 0, 0, alpha});  // CFont::SetDropColor
    Call<0x719430>(gold);                  // CFont::SetColor
    char text[8] = {}, gxt[8] = {};
    memset(text, ']', (size_t)std::min(level, 6));
    Call<0x718600>((const char*)text, gxt);              // AsciiToGxtChar
    Call<0x71A700>(right, midY - 8.0f * scaleY, (const char*)gxt);  // CFont::PrintString
}

using HudDrawFn = void(__cdecl*)();
HudDrawFn g_origHudDraw = nullptr;
void __cdecl HookHudDraw() {
    g_origHudDraw();
    if (Net().Online() && !Cfg().cleanScreen) Players::Instance().DrawWeaponIcons();
}

// A save keeps every PLAYER1 ped on foot (CPools::SavePedPool), and the remote player is one: the host's saves and the
// worlds it sent carried a copy of the partner. Loading made every such ped a CPlayerPed of player 0 sharing its data,
// so a second CJ walked by this game's pad, took the player's clothes and cleared its crosshair. Saves leave the remote
// players out, and a save that has copies already loads only the story's CJ ($PLAYER_ACTOR, main.scm's $12).
using BoolFn = bool(__cdecl*)();
BoolFn g_origSavePedPool = nullptr, g_origLoadPedPool = nullptr;
bool __cdecl HookSavePedPool() {
    std::vector<CPed*> hidden;
    for (auto& [id, rp] : Players::Instance().All())
        if (rp.ped && IsPedValid(rp.ped) && rp.ped->PedType() == PED_TYPE_PLAYER1) {
            rp.ped->PedType() = PED_TYPE_PLAYER_NETWORK;
            hidden.push_back(rp.ped);
        }
    bool ok = g_origSavePedPool();
    for (CPed* p : hidden) p->PedType() = PED_TYPE_PLAYER1;
    return ok;
}

int g_loadCount = 0, g_loadSeen = 0;  // CPools::LoadPedPool: its player peds (it loads no other ones)
CPlayerPed* g_loadKept = nullptr;
std::vector<CPlayerPed*> g_loadDropped;

using LoadDataFn = void(__cdecl*)(void*, int);
LoadDataFn g_origLoadPedCount = nullptr;
void __cdecl HookLoadPedCount(void* dst, int size) {  // CGenericGameStorage::LoadDataFromWorkBuffer: the count
    g_origLoadPedCount(dst, size);
    g_loadCount = size == 4 ? *(int32_t*)dst : 0;
    g_loadSeen = 0;
}

using PlayerCtorFn = CPlayerPed*(__fastcall*)(CPlayerPed*, void*, int, bool);
PlayerCtorFn g_origLoadedPlayerCtor = nullptr;
CPlayerPed* __fastcall HookLoadedPlayerCtor(CPlayerPed* self, void* edx, int playerId, bool groupCreated) {
    ++g_loadSeen;
    // the one $PLAYER_ACTOR names, or the last as the game would have it
    bool keep = g_loadCount <= 1 ||
                (!g_loadKept && (GetPedRef(self) == Ref<int32_t>(addr::ScriptSpace + 12) || g_loadSeen >= g_loadCount));
    if (keep) {
        g_loadKept = self;
        return g_origLoadedPlayerCtor(self, edx, playerId, groupCreated);
    }
    g_loadDropped.push_back(self);  // a ped of player 1 with no group: nothing of player 0's is touched, it goes after
    return g_origLoadedPlayerCtor(self, edx, kRemoteSlot, true);
}

bool __cdecl HookLoadPedPool() {
    g_loadCount = g_loadSeen = 0;
    g_loadKept = nullptr;
    g_loadDropped.clear();
    bool ok = g_origLoadPedPool();
    if (g_loadKept && !g_loadDropped.empty()) {
        PlayerInfo(0)->ped = g_loadKept;  // every loaded player ped was made player 0 in turn
        for (CPlayerPed* p : g_loadDropped)
            if (IsPedValid(p)) {
                WorldRemove(p);
                CallVirtual<0, void>(p, 1u);  // scalar deleting destructor
            }
        LOGI("The save had %d player characters: the story's CJ stays, the copies are removed", g_loadCount);
    }
    g_loadDropped.clear();
    g_loadKept = nullptr;
    g_loadCount = 0;
    return ok;
}

// a player's jetpack is the simplest active task (CPedIntelligence::GetTaskJetPack)
bool HasJetpack(CPed* ped) { return ped->Intelligence() && CallMethod<0x601110, void*>(ped->Intelligence()); }

int WeaponModel(int weaponType) {
    int model = -1;
    Cmd(0x0781).I(weaponType).Out(&model).Run();  // GET_WEAPONTYPE_MODEL
    return model;
}

uint8_t ClampByte(float v) { return (uint8_t)std::clamp(v, 0.0f, 255.0f); }

constexpr int TASK_ENTER_CAR_AS_PASSENGER = 700;
constexpr int TASK_ENTER_CAR_AS_DRIVER = 701;
constexpr int TASK_LEAVE_CAR = 704;
// what the script commands (TASK_ENTER_CAR_AS_PASSENGER / _DRIVER, the G key's too) give: the plain one under it
constexpr int TASK_ENTER_CAR_AS_PASSENGER_TIMED = 712, TASK_ENTER_CAR_AS_DRIVER_TIMED = 713;

bool EnteringCar(CPed* ped) {
    int t = ActivePrimaryTaskType(ped);
    return t == TASK_ENTER_CAR_AS_PASSENGER || t == TASK_ENTER_CAR_AS_DRIVER || t == TASK_ENTER_CAR_AS_PASSENGER_TIMED ||
           t == TASK_ENTER_CAR_AS_DRIVER_TIMED;
}
constexpr uint64_t kEnterGraceMs = 4000;  // let the enter animation finish before warping

uint8_t WantedLevel(CPlayerPed* me) {
    void* wanted = me->PlayerData() ? me->Wanted() : nullptr;
    return wanted ? (uint8_t)std::clamp(Field<int32_t>(wanted, 0x2C), 0, 6) : 0;  // CWanted::m_nWantedLevel
}

// what its game makes for the players near it (the only flags a vehicle state carries)
uint16_t MakesFlags() {
    return (Share().population ? OnFootState::F_POPULATION : 0) | (Share().trains ? OnFootState::F_TRAINS : 0);
}
constexpr uint16_t kMakesFlags = OnFootState::F_POPULATION | OnFootState::F_TRAINS;
constexpr int kMakesShift = 7;  // they go in a byte of their own: F_POPULATION is 1 there, F_TRAINS 2

// A ped getting into a car stops it (CTaskComplexEnterCar::PrepareVehicleForPedEnter: the autopilot's cruise speed 0,
// the old one kept in the task's m_CruiseSpeed, +0x48) and gives the speed back once it sits inside. For a player that
// happens only if it gave up getting in (0x63E7A4 tests !bInVehicle for players: a slip of Rockstar's, the single-player
// game never puts the player beside a driver). A player who got in beside a passer-by left the car standing for good,
// its driver frozen at the wheel, in the game that drives it (ours for our traffic, the other one's for its traffic,
// where our player's copy got in). Here the speed goes back as it does for anyone else.
constexpr int kAutopilotCruise = 0x390 + 0x40;  // CVehicle::m_autoPilot.m_nCruiseSpeed
constexpr int kEnterCruise = 0x48;               // CTaskComplexEnterCar::m_CruiseSpeed
constexpr uint8_t kDefaultCruise = 10;           // what the game gives a car it sets driving again (TASK_STEAL_CAR...)
std::map<CVehicle*, uint8_t> g_enterCruise;      // the speed a car had before a player started getting into it
std::set<CVehicle*> g_cruiseGiven;               // cars started again without a record: once each, the game may stop them

void* FindTaskOf(CPed* ped, int type) {  // CTaskManager::FindActiveTaskByType
    void* intel = ped ? ped->Intelligence() : nullptr;
    return intel ? CallMethod<0x681740, void*>((uint8_t*)intel + 4, type) : nullptr;
}

void KeepDriverGoing(CPed* player) {
    if (!player || !IsPedValid(player)) return;
    if (void* enter = FindTaskOf(player, 700 /* TASK_COMPLEX_ENTER_CAR_AS_PASSENGER */)) {
        auto* v = Field<CVehicle*>(enter, 0x0C);
        float was = Field<float>(enter, kEnterCruise);
        if (v && was > 0.0f) g_enterCruise[v] = (uint8_t)std::min(was, 255.0f);
        return;
    }
    CVehicle* v = player->InVehicle() ? player->Vehicle() : nullptr;
    CPed* driver = v ? v->Driver() : nullptr;
    if (!v || !driver || driver == player || driver->IsPlayer() || Players::Instance().FindByPed(driver)) return;
    // getting out: the car waits for it standing (CTaskSimpleCarWaitToSlowDown), as it should; a driver dying stops too
    if (FindTaskOf(player, 704 /* TASK_COMPLEX_LEAVE_CAR */) || driver->Health() <= 0.0f) return;
    auto rec = g_enterCruise.find(v);
    uint8_t was = rec != g_enterCruise.end() ? rec->second : 0;
    if (rec != g_enterCruise.end()) g_enterCruise.erase(rec);
    NetVehicle* nv = Vehicles::Instance().Find(v);
    if (nv && Vehicles::Instance().IsMirror(*nv)) return;  // the owner's game drives it (and gives the speed back there)
    uint8_t& cruise = Field<uint8_t>(v, kAutopilotCruise);
    if (cruise != 0) return;
    // a car stopped this way before the fix: a passer-by's goes on at the game's usual speed; a mission's driver is its
    // script's to start again
    if (!was && driver->CreatedBy() != 2 /* PED_MISSION */ && g_cruiseGiven.insert(v).second) was = kDefaultCruise;
    if (!was) return;
    cruise = was;
    LOGI("The driver of the car %s sits in goes on (cruise speed %u)", player == FindPlayerPed(0) ? "our player" : "a player's copy",
         (unsigned)was);
}

// A passenger leaning out of the window with a gun (CTaskSimpleGangDriveBy, the split-screen mode's drive-by the mod
// gives to any instant-hit gun) aims and fires with an empty hand: getting in took the gun's model out of the hand
// (CPed::RemoveWeaponWhenEnteringVehicle) and the task puts back only the SMG the split-screen mode allowed. The gun in
// hand is shown while it leans out, ours and the other players' copies alike, and goes again when it sits back.
constexpr int kWeaponModelId = 0x740;  // CPed::m_nWeaponModelId
std::set<CPed*> g_driveByGun;          // the passengers whose gun this showed

void ShowDriveByGun(CPed* ped) {
    if (!ped || !IsPedValid(ped)) return;
    bool leaning = ped->InVehicle() && ped->Vehicle() && ped->Vehicle()->Driver() != ped &&
                   FindTaskOf(ped, 1022 /* TASK_SIMPLE_GANG_DRIVEBY */);
    const CWeapon& w = ped->ActiveWeapon();
    void* info = w.type > 0 && w.type <= kLastWeapon ? Call<0x743C60, void*>(w.type, (uint8_t)1) : nullptr;  // CWeaponInfo::GetWeaponInfo
    int model = info ? Field<int32_t>(info, 0x0C) : -1;  // m_nModelId1
    int32_t& shown = Field<int32_t>(ped, kWeaponModelId);
    if (leaning) {
        if (model > 0 && shown != model && IsModelLoaded(model)) {
            CallMethod<0x5E5ED0>(ped, model);  // CPed::AddWeaponModel
            g_driveByGun.insert(ped);
        }
        return;
    }
    if (g_driveByGun.erase(ped) && ped->InVehicle() && shown > 0) CallMethod<0x5E3990>(ped, shown);  // CPed::RemoveWeaponModel
}

void KeepDriversGoing() {
    for (auto it = g_enterCruise.begin(); it != g_enterCruise.end();)  // gone meanwhile (the pointer may be another car's)
        it = IsVehicleValid(it->first) ? std::next(it) : g_enterCruise.erase(it);
    for (auto it = g_cruiseGiven.begin(); it != g_cruiseGiven.end();) it = IsVehicleValid(*it) ? std::next(it) : g_cruiseGiven.erase(it);
    for (auto it = g_driveByGun.begin(); it != g_driveByGun.end();) it = IsPedValid(*it) ? std::next(it) : g_driveByGun.erase(it);
    KeepDriverGoing(FindPlayerPed(0));
    ShowDriveByGun(FindPlayerPed(0));
    for (auto& [id, rp] : Players::Instance().All())
        if (rp.ped) {
            KeepDriverGoing(rp.ped);
            ShowDriveByGun(rp.ped);
        }
}

// ---- shots. A remote player's shots are its pad replayed here: a tap of the fire button whose press was lost on the
// way (the state goes unreliably) was a shot the others never saw nor heard, a pistol's or a shotgun's most of all.
// Each game counts its player's shots (CWeapon::Fire, the guns), the count goes in its state, and a copy that fired
// fewer than its owner fires the missing one itself (towards where its owner's camera looks), once its pad had its
// chance. Its damage counts nowhere here (Damage drops a remote player's), its flash and its sound are what show.
uint8_t g_localShots = 0;  // this game's player's shots, counted round
bool CountedGun(int type) { return (type >= 22 && type <= 34) || type == 38; }  // the projectiles and fire go their own way

using WeaponFireFn = bool(__fastcall*)(CWeapon*, void*, CEntity*, CVector*, CVector*, CEntity*, CVector*, CVector*);
WeaponFireFn g_origWeaponFire = nullptr;
bool g_forcingShot = false;
// People shooting at a player aim at its head now and then: the sharper the shooter, the more often (its accuracy
// / 200, 12%-50% of the shots), at the head bone (CPed::GetBonePosition) instead of the body.
bool AimsAtHead(CEntity* by, CEntity* target, int type) {
    if (!by || !target || !by->IsPed() || !target->IsPed() || !CountedGun(type) || type == 38) return false;
    auto* shooter = (CPed*)by;
    if (shooter->IsPlayer() || Players::Instance().FindByPed(shooter)) return false;
    auto* victim = (CPed*)target;
    if (victim != (CPed*)FindPlayerPed(0) && !Players::Instance().FindByPed(victim)) return false;
    float chance = std::clamp((float)shooter->WeaponAccuracy() / 200.0f, 0.12f, 0.5f);
    return (float)(std::rand() % 1000) / 1000.0f < chance;
}

bool __fastcall HookWeaponFire(CWeapon* w, void* edx, CEntity* by, CVector* start, CVector* barrel, CEntity* target,
                               CVector* targetPos, CVector* alt) {
    int type = w->type;
    CVector head;
    if (AimsAtHead(by, target, type)) {
        CallMethod<0x5E4280>(target, &head, 5 /* BONE_HEAD */, false);  // CPed::GetBonePosition
        if (head.Length() > 1.0f) {
            head = head + CVector{((float)(std::rand() % 100) - 50.0f) * 0.0016f, ((float)(std::rand() % 100) - 50.0f) * 0.0016f, 0.0f};
            target = nullptr;  // at the point, not at the body
            targetPos = &head;
        }
    }
    bool fired = g_origWeaponFire(w, edx, by, start, barrel, target, targetPos, alt);
    if (!fired || !by || !by->IsPed() || !CountedGun(type) || !Net().Online()) return fired;
    if (by == (CEntity*)FindPlayerPed(0) && !g_inRemoteContext) {
        ++g_localShots;
    } else if (RemotePlayer* rp = Players::Instance().FindByPed((CPed*)by)) {
        rp->copyFiredMs = NowMs();
        if (rp->shotsOwed && !g_forcingShot) --rp->shotsOwed;
    }
    return fired;
}

void NoteShots(RemotePlayer& rp, uint8_t shots) {
    if (rp.shotsKnown) {
        uint8_t more = (uint8_t)(shots - rp.shotsSeen);
        if (more && more < 64) {
            rp.shotsOwed = (uint8_t)std::min(3, rp.shotsOwed + more);
            rp.shotsOwedMs = NowMs();
        }
    }
    rp.shotsSeen = shots;
    rp.shotsKnown = true;
}

// in the copy's own processing (its pad, camera and stats in place): the shot its pad did not fire
void FireOwedShot(RemotePlayer& rp) {
    uint64_t now = NowMs();
    if (!rp.shotsOwed || now - rp.shotsOwedMs < 150 || now - rp.copyFiredMs < 150) return;
    CPed* ped = rp.ped;
    CWeapon& w = ped->ActiveWeapon();
    if (!CountedGun(w.type) || w.type != rp.state.weapon || ped->Health() <= 0.0f ||
        (ped->InVehicle() && !FindTaskOf(ped, 1022 /* TASK_SIMPLE_GANG_DRIVEBY */))) {
        rp.shotsOwed = 0;  // nothing to fire it with here: let it go
        return;
    }
    const CameraState& c = rp.state.cam;
    CVector from = ped->GetPosition() + c.sourceOffset, to = from + c.front.Normalized() * 80.0f;
    if (w.ammoInClip <= 0) w.ammoInClip = 1;
    if (w.state != 0 && w.state != 1) w.state = 0;  // WEAPONSTATE_READY: a reload here is not one there
    g_forcingShot = true;
    bool fired = CallMethod<0x742300, bool>(&w, (CEntity*)ped, (CVector*)nullptr, (CVector*)nullptr, (CEntity*)nullptr, &to,
                                             (CVector*)nullptr);  // CWeapon::Fire
    g_forcingShot = false;
    --rp.shotsOwed;
    rp.shotsOwedMs = now;
    rp.copyFiredMs = now;
    LOGD("Remote player %d: a missed shot fired (%s)", rp.id, fired ? "ok" : "refused");
}

void FillCommon(OnFootState& st, CPlayerPed* me) {
    st.flags = MakesFlags();
    st.shots = g_localShots;
    st.health = ClampByte(me->Health());
    st.wanted = WantedLevel(me);
    st.armour = ClampByte(me->Armour());
    CWeapon& w = me->ActiveWeapon();
    st.weapon = (uint8_t)w.type;
    st.ammo = (uint16_t)std::clamp(w.ammoTotal, 0, 65535);
    st.area = me->AreaCode();
    CPad* pad = GetPad(0);
    if (pad->DisablePlayerControls == 0 && !game::MenuActive()) st.pad = PadState::From(pad->NewState);
    // a passenger aims out of the window with the mouse's look button, which the pad state has not: R1 (PassengerDriveBy)
    if (me->InVehicle() && me->Vehicle() && me->Vehicle()->Driver() != me && pad->NewState.m_bVehicleMouseLook)
        st.pad.buttons |= PadState::B_RS1;
    CameraState& c = st.cam;
    c.mode = cam::Mode();
    c.weaponMode = cam::WeaponMode();
    c.fov = cam::Fov();
    c.front = cam::Front();
    c.sourceOffset = cam::Source() - me->GetPosition();
    c.up = cam::Up();
    c.orientation = cam::Orientation();
    c.alpha = cam::Alpha();
    c.beta = cam::Beta();
    c.lookPitch = me->PlayerData() ? me->LookPitch() : 0.0f;
    c.useMouse3rd = cam::UseMouse3rdPerson() != 0;
    c.chairX = cam::CrosshairMultX();
    c.chairY = cam::CrosshairMultY();
}

void WriteCommon(Writer& w, const OnFootState& st) {
    w.U8(st.shots);
    st.pad.Write(w);
    st.cam.Write(w);
    w.U8(st.health);
    w.U8(st.armour);
    w.U8(st.weapon);
    w.U16(st.ammo);
    w.I8(st.area);
    w.U8(st.wanted);
    w.U8((uint8_t)((st.flags & kMakesFlags) >> kMakesShift));
}

void ReadCommon(Reader& r, OnFootState& st) {
    st.shots = r.U8();
    st.pad.Read(r);
    st.cam.Read(r);
    st.health = r.U8();
    st.armour = r.U8();
    st.weapon = r.U8();
    st.ammo = r.U16();
    st.area = r.I8();
    st.wanted = r.U8();
    st.flags = (uint16_t)(r.U8() << kMakesShift) & kMakesFlags;
}
}  // namespace

// A model in memory, else asked for (it comes in a few frames, and a later try uses it): LoadModelNow loads the whole
// streaming queue with it, and the other player's weapon or phone stood this game still for a second or more (issue #43)
bool ModelReady(int model) {
    if (IsModelLoaded(model)) return true;
    RequestModel(model, 0);
    return false;
}

// a go-to task walks the player (900-939: TASK_SIMPLE_GO_TO_POINT .. TASK_COMPLEX_GOTO_DOOR_AND_OPEN 932 ..), not its pad;
// so does an entrance to the end of its walk-in (CEntryExitManager::ms_exitEnterState): the fade-out walks it on under
// TASK_SIMPLE_UNINTERRUPTABLE with its pad back on, and the copy ran off by that stick and the door's fixed camera
bool TaskWalks(CPed* ped) {
    int t = ActivePrimaryTaskType(ped);
    return (t >= 900 && t < 940) || (ped == FindPlayerPed(0) && Ref<int32_t>(0x96A7CC) != 0);
}

int ActivePrimaryTaskType(CPed* ped, void** taskOut) {
    if (taskOut) *taskOut = nullptr;
    void* intel = ped ? ped->Intelligence() : nullptr;
    if (!intel) return -1;
    for (int i = 0; i < 5; ++i) {
        void* task = Field<void*>(intel, 0x04 + i * 4);  // CTaskManager::m_aPrimaryTasks
        if (!task) continue;
        if (taskOut) *taskOut = task;
        return CallVirtual<4, int>(task);  // CTask::GetTaskType
    }
    return -1;
}

void StopEntering(CVehicle* v) {
    CPoolHeader* pool = PedPool();
    for (int i = 0; pool && i < pool->size; ++i) {
        if (pool->byteMap[i] < 0) continue;
        auto* ped = (CPed*)(pool->objects + (size_t)i * kPedPoolElemSize);
        void* task = nullptr;
        int type = ActivePrimaryTaskType(ped, &task);
        bool entering = type == TASK_ENTER_CAR_AS_PASSENGER || type == TASK_ENTER_CAR_AS_DRIVER ||
                        type == TASK_ENTER_CAR_AS_PASSENGER_TIMED || type == TASK_ENTER_CAR_AS_DRIVER_TIMED;
        if (entering && Field<CVehicle*>(task, 0x0C) == v) CallMethod<0x601640>(ped->Intelligence(), true);  // FlushImmediately
    }
}

int PassengerSeatOf(CVehicle* v, CPed* ped) {
    if (!v || !ped) return -2;
    if (v->Driver() == ped) return -1;
    for (int i = 0; i < 8; ++i)
        if (v->Passengers()[i] == ped) return i;
    return -2;
}

int SeatOfTargetDoor(int door) {
    switch (door) {
        case 11: return 1;  // rear left
        case 9: return 2;   // rear right
        default: return 0;  // front right (8) and anything else
    }
}

int SeatOrAny(CVehicle* v, int seat) { return v && seat >= 0 && seat < v->MaxPassengers() ? seat : -1; }

void DetachPed(CPed* ped) {
    CPhysical* to = ped ? ped->AttachedTo() : nullptr;
    if (!to) return;
    CallMethod<0x571A00>(to, (CEntity**)&ped->AttachedTo());  // CEntity::CleanUpOldReference
    Cmd(0x0465).I(GetPedRef(ped)).Run();                      // DETACH_CHAR_FROM_CAR
    ped->AttachedTo() = nullptr;
    if (ped->IsPlayer()) ped->Flags() |= 1;  // bUsesCollision: attaching a player turns it off, detaching never back
}

float RemoteDistance(const CVector& p, int8_t area, uint8_t* who) {
    float best = 1e9f;
    for (auto& [id, rp] : Players::Instance().All())
        if (float d = (rp.state.pos - p).Length(); rp.hasState && rp.state.area == area && d < best) {
            best = d;
            if (who) *who = id;
        }
    return best;
}

proto::PlayerMask PlayersNear(const CVector& p, int8_t area, float radius, bool unknownToo) {
    proto::PlayerMask mask = 0;
    const auto& remote = Players::Instance().All();
    for (auto& [id, peer] : Net().Peers()) {
        auto it = remote.find(id);
        // one that has not been heard from for a while is loading or in the menu: where it appears is unknown
        bool known = it != remote.end() && it->second.hasState && NowMs() - it->second.stateRecvMs < 3000;
        if (known ? it->second.state.area == area && (it->second.state.pos - p).Length() < radius : unknownToo)
            mask |= proto::Bit(id);
    }
    return mask;
}

// ------------------------------------------------------------------ module
void Players::OnInstallHooks() {
    mem::HookFn(0x60EA90, &HookPlayerProcessControl, &g_origProcessControl, "CPlayerPed::ProcessControl");
    mem::HookFn(0x5FB9C0, &HookSetLeader, &g_origSetLeader, "CPedGroupMembership::SetLeader");
    mem::HookFn(0x5F6AE0, &HookAddMember, &g_origAddMember, "CPedGroupMembership::AddMember");
    void* coopGame = nullptr;  // CGameLogic::IsCoopGameGoingOn, always false (Patches)
    mem::RedirectCall(0x644A1A, (const void*)&PassengerDriveBy, &coopGame);
    mem::Nop(0x644A27, 28);  // the SMG slot holds a weapon with ammo (PassengerDriveBy checks the gun in hand)
    mem::Nop(0x644A69, 6);   // jne: the camera is MODE_TWOPLAYER_IN_CAR_AND_SHOOTING
    mem::HookFn(0x62D3B0, &HookGangDriveByProcess, &g_origGangDriveBy, "CTaskSimpleGangDriveBy::ProcessPed");
    mem::HookFn(0x64F240, &HookQuitCarEnter, &g_origQuitEnter, "CCarEnterExit::IsPlayerToQuitCarEnter");
    mem::HookFn(0x442060, &HookRestorePlayer, &g_origRestore, "CGameLogic::RestorePlayerStuffDuringResurrection");
    mem::HookFn(0x630040, &HookDieCtor, &g_origDieCtor, "CTaskComplexDie::CTaskComplexDie");
    mem::RedirectCallFn(0x4677A5, &HookDeleteChar, &g_origRemoveThisPed);  // DELETE_CHAR -> CTheScripts::RemoveThisPed
    mem::HookFn(0x609FF0, &HookGetPlayerInfoForThisPlayerPed, &g_origGetPlayerInfo,
                "CPlayerPed::GetPlayerInfoForThisPlayerPed");
    mem::HookFn(0x706970, &HookGetRealTimeShadow, &g_origGetShadow, "CRealTimeShadowManager::GetRealTimeShadow");
    mem::HookFn(0x502280, &HookVehicleAudioService, &g_origVehicleAudio, "CAEVehicleAudioEntity::Service");
    mem::HookFn(0x58FAE0, &HookHudDraw, &g_origHudDraw, "CHud::Draw");
    mem::HookFn(0x742300, &HookWeaponFire, &g_origWeaponFire, "CWeapon::Fire");
    mem::RedirectCallFn(0x60F595, &HookGroupBehaviour, &g_origGroupBehaviour);  // CPlayerPed::ProcessControl
    mem::RedirectCallFn(0x74278B, &HookTakePhotograph, &g_origTakePhoto);      // CWeapon::Fire, the camera
    mem::RedirectCallFn(0x5D0880, &HookSavePedPool, &g_origSavePedPool);  // CPools::Save
    mem::RedirectCallFn(0x5D0890, &HookLoadPedPool, &g_origLoadPedPool);  // CPools::Load
    mem::RedirectCallFn(0x5D2D95, &HookLoadPedCount, &g_origLoadPedCount);  // in CPools::LoadPedPool
    mem::RedirectCallFn(0x5D2E07, &HookLoadedPlayerCtor, &g_origLoadedPlayerCtor);
    Net().SetHandler(proto::G_PLAYER_ONFOOT, [](uint8_t from, Reader& r) { Instance().HandleOnFoot(from, r); });
    Net().SetHandler(proto::G_TELEPORT, [](uint8_t from, Reader& r) { Instance().HandleTeleport(from, r); });
    Net().SetHandler(proto::G_PLAYER_DEATH, [](uint8_t from, Reader& r) {
        int16_t anim = r.I16();
        uintptr_t grp = Ref<uintptr_t>(0xB4EA34);  // CAnimManager::ms_aAnimAssocGroups[0]: the default group (ped.ifp)
        int first = grp ? (int)Ref<uint32_t>(grp + 0x0C) : 0, count = grp ? (int)Ref<uint32_t>(grp + 0x08) : 0;
        RemotePlayer* rp = Instance().Get(from);
        if (!r.Ok() || !rp || anim < first || anim >= first + count) return;
        rp->dieAnim = anim;
        rp->dieMs = NowMs();
    });
    Net().SetHandler(proto::G_PLAYER_DRIVER, [](uint8_t from, Reader& r) { Instance().HandleDriver(from, r); });
    Net().SetHandler(proto::G_PLAYER_PASSENGER, [](uint8_t from, Reader& r) { Instance().HandlePassenger(from, r); });
    Net().SetHandler(proto::G_PLAYER_ENTER_CAR, [](uint8_t from, Reader& r) { Instance().HandleEnterCar(from, r); });
    Net().SetHandler(proto::G_PLAYER_EXIT_CAR, [](uint8_t from, Reader& r) { Instance().HandleExitCar(from, r); });
}

RemotePlayer* Players::FindByPed(CPed* ped) {
    if (!ped) return nullptr;
    for (auto& [id, rp] : m_remote)
        if (rp.ped == ped) return &rp;
    return nullptr;
}

RemotePlayer* Players::Get(uint8_t id) {
    auto it = m_remote.find(id);
    return it == m_remote.end() ? nullptr : &it->second;
}

// as SET_PLAYER_MODEL (0x47A820) changes it: a new clump, the walk set kept. A special character (290-299) must be
// in memory already: it is loaded by name (Peds::SpecialModelFor). For the other players' copies only.
bool Players::SetModel(CPlayerPed* ped, int model) {
    if (ped->ModelIndex() == model) return true;
    bool special = model >= 290 && model <= 299;
    if (model != 0 && (!IsPedModel(model) || (special ? !IsModelLoaded(model) : !LoadModelNow(model)))) return false;
    int32_t walk = Field<int32_t>(ped, 0x4D4);  // CPed::m_nAnimGroup
    // The copy's tasks hold animations of the old clump (a car door's, a fight's, the look-at's bones) and touched them
    // freed later: SET_PLAYER_MODEL is for an idle player. Flushed first, out of a car first (the flush takes the drive
    // task, the next state seats it again).
    if (ped->InVehicle()) {
        CVector p = ped->GetPosition();
        Cmd(0x0362).I(GetPedRef(ped)).F(p.x).F(p.y).F(p.z + 1.0f).Run();  // WARP_CHAR_FROM_CAR_TO_COORD
    }
    CallMethod<0x601640>(ped->Intelligence(), true);  // CPedIntelligence::FlushImmediately
    CallVirtual<8>(ped);                        // CEntity::DeleteRwObject
    Field<uint16_t>(ped, 0x22) = 0xFFFF;        // CEntity::m_nModelIndex: SetModelIndex rebuilds only a new one
    CallVirtual<5>(ped, (uint32_t)model);       // CPed::SetModelIndex
    Field<int32_t>(ped, 0x4D4) = walk;
    return true;
}

void Players::OnSessionStart() { m_lastSend = 0; }

void Players::OnSessionEnd() {
    g_driveByGun.clear();
    for (auto& [id, rp] : m_remote) DestroyRemote(rp);
    m_remote.clear();
    g_ourDriveBy.clear();
}

void Players::OnPeerLeave(uint8_t id) {
    auto it = m_remote.find(id);
    if (it == m_remote.end()) return;
    DestroyRemote(it->second);
    m_remote.erase(it);
}

void Players::OnGameStarted() {
    Cmd(0x0501).I(0).I(1).Run();  // SET_PLAYER_CAN_DO_DRIVE_BY: passengers may shoot from the window
    g_ourDriveBy.clear();
    g_enterCruise.clear();
    g_cruiseGiven.clear();
    g_driveByGun.clear();
    m_enterReported = nullptr;
    m_lastCar = -1;  // another world
    m_exitReported = false;
    // world was (re)created: our peds are gone, they will be respawned from the next states
    for (auto& [id, rp] : m_remote) {
        rp.ped = nullptr;
        rp.pedHandle = -1;
        rp.blip = 0;
        ReleaseOwnData(rp);
    }
    // a reload of the host's world after its progress: back where the partner stood, as it was
    if (flow::SavedSpot s; Net().Online() && !Net().IsHost() && flow::TakeSavedSpot(s)) {
        CPlayerPed* me = FindPlayerPed(0);
        int h = GetPedRef(me);
        Cmd(0x04BB).I(s.area).Run();                          // SET_AREA_VISIBLE
        Cmd(0x0860).I(h).I(s.area).Run();                     // SET_CHAR_AREA_VISIBLE
        Cmd(0x03CB).F(s.x).F(s.y).F(s.z).Run();               // LOAD_SCENE: its ground first
        Cmd(0x00A1).I(h).F(s.x).F(s.y).F(s.z - 1.0f).Run();   // SET_CHAR_COORDINATES
        LightArea(s.area, {s.x, s.y, s.z});  // the host's world came with the light of where the host stood
        UseStreetDoor(s.area, {s.x, s.y, s.z}, {s.x, s.y, s.z});
        me->CurrentRotation() = me->AimingRotation() = s.heading;
        me->Health() = s.health;
        me->Armour() = s.armour;
        Cmd(0x048F).I(h).Run();  // REMOVE_ALL_CHAR_WEAPONS: the host's ones came with its world
        for (int i = 0; i < 13; ++i)
            if (s.weapons[i] > 0) {
                int model = WeaponModel(s.weapons[i]);
                if (model > 0 && !IsModelLoaded(model)) LoadModelNow(model);
                Cmd(0x01B2).I(h).I(s.weapons[i]).I(std::max(s.ammo[i], 1)).Run();  // GIVE_WEAPON_TO_CHAR
            }
        Cmd(0x01B9).I(h).I(s.activeWeapon).Run();  // SET_CURRENT_CHAR_WEAPON
        if (s.carModel > 0 && IsVehicleModel(s.carModel) && HasRoom(VehiclePool(), kKeepVehicles, "vehicle") &&
            LoadModelNow(s.carModel)) {  // the car left next to it
            int car = -1;
            Cmd(0x00A5).I(s.carModel).F(s.carX).F(s.carY).F(s.carZ).Out(&car).Run();  // CREATE_CAR
            if (CVehicle* v = GetVehicle(car)) {
                Cmd(0x0175).I(car).F(s.carHeading * 57.29578f).Run();  // SET_CAR_HEADING
                memcpy(v->Colors(), s.carColors, 4);
                v->Health() = s.carHealth;
                Cmd(0x01C3).I(car).Run();  // MARK_CAR_AS_NO_LONGER_NEEDED: an ordinary car, as before
                m_lastCar = car;
                LOGI("The car left next to the partner (model %d) is back", s.carModel);
            }
        }
        LOGI("Back at the spot before the reload of the host's world");
        return;
    }
    // the host's world puts the partner where the host stands: a step aside, where nothing is in the way
    if (Net().Online() && !Net().IsHost()) {
        CPlayerPed* me = FindPlayerPed(0);
        CVector p = me->GetPosition();
        float h = me->CurrentRotation();
        for (float side : {1.2f, -1.2f}) {
            CVector q = p + CVector{std::cos(h) * side, std::sin(h) * side, 0};
            if (!Call<0x56A490, bool>(&p, &q, true, true, false, true, false, true, false)) continue;  // GetIsLineOfSightClear
            Cmd(0x00A1).I(GetPedRef(me)).F(q.x).F(q.y).F(q.z - 1.0f).Run();  // SET_CHAR_COORDINATES
            break;
        }
    }
}

void Players::OnGameShutdown() {
    // the remote players go before the world is torn down (a world loaded in the game: the prologue's end)
    for (auto& [id, rp] : m_remote) {
        DestroyRemote(rp);
        rp.blip = 0;
    }
    PlayerInfo(kRemoteSlot)->ped = nullptr;
}

void Players::SpawnRemote(RemotePlayer& rp) {
    if (rp.ped || !rp.hasState || !core::IsPlayerReady()) return;
    CPlayerInfo* slot = PlayerInfo(kRemoteSlot);
    if (slot->ped) return;  // slot busy (should not happen)
    if (!HasRoom(PedPool(), 2, "ped")) return;  // the other player above everything else, still not into the last slots
    int freeGroup = FreePedGroup();
    if (freeGroup < 0) {
        if (NowMs() - rp.groupWaitLogMs > 10000) {
            rp.groupWaitLogMs = NowMs();
            LOGW("Remote player %d waits for a free ped group to be made", rp.id);
        }
        return;
    }
    // should its constructor set the player up as the leader of the group the slot names before it makes one
    // (SetInitialState), that is the one it makes next
    GroupOf(SlotData()) = freeGroup;
    const CVector p = rp.state.pos;
    int playerIndex = -1, handle = -1;
    // made high above its place: CREATE_PLAYER clears the space around itself of this game's random peds and cars
    // (the taxi the other player sits in), and it goes where it is right after
    Cmd(0x0053).I(kRemoteSlot).F(p.x).F(p.y).F(p.z + 300.0f).Out(&playerIndex).Run();  // CREATE_PLAYER
    Cmd(0x01F5).I(kRemoteSlot).Out(&handle).Run();                            // GET_PLAYER_CHAR
    Cmd(0x0501).I(kRemoteSlot).I(1).Run();  // SET_PLAYER_CAN_DO_DRIVE_BY: its drive-bys show here (CPlayerInfo +0x153)
    // no group of its own: the one its constructor made goes at once, and it counts as one of this game's player's
    // (whatever asks a player for its group finds one that exists; its recruiting, ProcessGroupBehaviour, is off)
    int32_t made = GroupOf(SlotData()), local = LocalGroup();
    if (made >= 0 && made < (int32_t)kGroupCount && made != local) Call<0x5FB870>(made);  // CPedGroups::RemoveGroup
    GroupOf(SlotData()) = local;
    auto* ped = (CPlayerPed*)GetPed(handle);
    if (!ped) {
        LOGE("Failed to create the remote player ped");
        slot->ped = nullptr;
        return;
    }
    // registered in CWorld::Players[1] only while processed (see HookPlayerProcessControl)
    slot->ped = nullptr;
    TakeOwnData(rp, ped);
    Cmd(0x00A1).I(handle).F(p.x).F(p.y).F(p.z - 1.0f).Run();  // SET_CHAR_COORDINATES (feet)
    ped->PedType() = PED_TYPE_PLAYER1;  // enemies hostile to the player attack the partner too
    ped->SetProofs(true, true, true, true, true);  // damage is decided by its owner
    ped->SetStreamingDontDelete(true);
    ped->AreaCode() = rp.state.area;
    rp.ped = ped;
    // CEntity::RegisterReference: the engine nulls it when it deletes the copy itself (with a car a mission deletes:
    // CWorld::Process removes the people of a deleted car), before the network handlers of the next frame use it
    CallMethod<0x571B70>(ped, (CEntity**)&rp.ped);
    rp.pedHandle = handle;
    rp.padNew = rp.padOld = CControllerState{};
    // radar blip, unless the player hid them (partner_blip = 0: a shoot-out not knowing where the other is, issue #44)
    int blip = 0;
    if (Cfg().partnerBlip) Cmd(0x0187).I(handle).Out(&blip).Run();  // ADD_BLIP_FOR_CHAR
    if (blip) {
        Cmd(0x07E0).I(blip).I(1).Run();       // SET_BLIP_AS_FRIENDLY
        Cmd(0x0165).I(blip).I((int)ui::ArgbToRgba(PlayerColor(rp.id))).Run();  // CHANGE_BLIP_COLOUR: an RGBA is drawn as is
        Cmd(0x0168).I(blip).I(2).Run();       // CHANGE_BLIP_SCALE
        Cmd(0x018B).I(blip).I(2).Run();       // CHANGE_BLIP_DISPLAY: radar only, no 3D arrow
        rp.blip = blip;
    }
    LOGI("Remote player %d spawned (ped %p, handle %d)", rp.id, (void*)ped, handle);
}

void Players::DestroyRemote(RemotePlayer& rp) {
    if (rp.blip) {
        Cmd(0x0164).I(rp.blip).Run();  // REMOVE_BLIP
        rp.blip = 0;
    }
    if (rp.ped && IsPedValid(rp.ped)) {
        CPed* ped = rp.ped;
        DetachPed(ped);  // the vehicle keeps a reference to it otherwise
        if (ped->InVehicle()) {  // the car kept it in its seat count (CTheScripts::RemoveThisPed takes it out first too)
            CVector p = ped->GetPosition();
            Cmd(0x0362).I(rp.pedHandle).F(p.x).F(p.y).F(p.z + 1.0f).Run();  // WARP_CHAR_FROM_CAR_TO_COORD
        }
        if (PlayerInfo(kRemoteSlot)->ped == ped) PlayerInfo(kRemoteSlot)->ped = nullptr;
        if (CPlayerPed* me = FindPlayerPed(0); me && Field<CEntity*>(me, 0x128) == ped) Field<CEntity*>(me, 0x128) = nullptr;  // KeepApart
        WorldRemove(ped);
        Call<0x565510>(ped);            // CWorld::RemoveReferencesToDeletedObject: who stood on it or touched it
        CallVirtual<0, void>(ped, 1u);  // scalar deleting destructor (returns the slot to the ped pool)
        LOGI("Remote player %d removed", rp.id);
    }
    rp.ped = nullptr;
    rp.pedHandle = -1;
    rp.downShown = false;
    ReleaseOwnData(rp);  // after the ped: its destructor may still read it
}

void Players::HandleOnFoot(uint8_t from, Reader& r) {
    if (from == Net().MyId()) return;
    OnFootState st;
    if (!st.Read(r)) return;
    RemotePlayer& rp = Touch(from);
    // unreliable channel: ignore states older than the newest applied one
    if (rp.hasState && (int32_t)(st.time - rp.lastStateTime) < 0) return;
    rp.lastStateTime = st.time;
    // enter/exit are replicated as events; with the target held the button is the fight's second attack (a kick,
    // CTaskSimplePlayerOnFoot: MeleeAttackJustDown 4), which only the pad replay shows
    if (!(st.pad.buttons & PadState::B_RS1)) st.pad.buttons &= ~PadState::B_TRIANGLE;
    NoteShots(rp, st.shots);
    rp.state = st;
    rp.hasState = true;
    rp.stateRecvMs = NowMs();
    rp.padFresh = true;
    rp.mode = RemotePlayer::Mode::OnFoot;
}

RemotePlayer& Players::Touch(uint8_t from) {
    RemotePlayer& rp = m_remote[from];
    rp.id = from;
    return rp;
}

void Players::HandleDriver(uint8_t from, Reader& r) {
    if (from == Net().MyId()) return;
    uint16_t netId = r.U16();
    VehicleState vs;
    vs.Read(r);
    OnFootState st;
    ReadCommon(r, st);
    if (!r.Ok()) return;
    RemotePlayer& rp = Touch(from);
    if (rp.hasState && (int32_t)(vs.time - rp.lastStateTime) < 0) return;
    rp.lastStateTime = vs.time;
    st.time = vs.time;
    st.pos = vs.pos;
    st.vel = vs.vel;
    st.pad.buttons &= ~PadState::B_TRIANGLE;
    NoteShots(rp, st.shots);
    rp.state = st;
    rp.hasState = true;
    rp.stateRecvMs = NowMs();
    rp.padFresh = true;
    rp.mode = RemotePlayer::Mode::Driver;
    rp.vehNetId = netId;
    rp.seat = -1;
    Vehicles::Instance().ApplyDriverState(from, netId, vs);
}

void Players::HandlePassenger(uint8_t from, Reader& r) {
    if (from == Net().MyId()) return;
    uint16_t netId = r.U16();
    int8_t seat = r.I8();
    uint32_t time = r.U32();
    Vec3 p = r.Vec();
    OnFootState st;
    ReadCommon(r, st);
    if (!r.Ok()) return;
    RemotePlayer& rp = Touch(from);
    if (rp.hasState && (int32_t)(time - rp.lastStateTime) < 0) return;
    rp.lastStateTime = time;
    st.time = time;
    st.pos = {p.x, p.y, p.z};
    st.pad.buttons &= ~PadState::B_TRIANGLE;
    NoteShots(rp, st.shots);
    rp.state = st;
    rp.hasState = true;
    rp.stateRecvMs = NowMs();
    rp.padFresh = true;
    rp.mode = RemotePlayer::Mode::Passenger;
    rp.vehNetId = netId;
    rp.seat = seat;
}

void Players::HandleEnterCar(uint8_t from, Reader& r) {
    uint16_t netId = r.U16();
    int8_t seat = r.I8();
    if (!r.Ok() || from == Net().MyId()) return;
    RemotePlayer* rp = Get(from);
    NetVehicle* nv = Vehicles::Instance().Find(netId);
    // a car deleted in the last frame is still listed (Vehicles drops it in its OnPreProcess, after the messages)
    if (!rp || !rp->ped || !nv || !Vehicles::Alive(*nv)) return;
    rp->enterTaskMs = NowMs();
    if (seat < 0)
        Cmd(0x05CB).I(rp->pedHandle).I(nv->handle).I(10000).Run();  // TASK_ENTER_CAR_AS_DRIVER
    else
        Cmd(0x05CA).I(rp->pedHandle).I(nv->handle).I(10000).I(SeatOrAny(nv->veh, seat)).Run();  // TASK_ENTER_CAR_AS_PASSENGER
}

void Players::HandleExitCar(uint8_t from, Reader& r) {
    uint16_t netId = r.U16();
    if (!r.Ok() || from == Net().MyId()) return;
    RemotePlayer* rp = Get(from);
    NetVehicle* nv = Vehicles::Instance().Find(netId);
    if (!rp || !rp->ped || !nv || !nv->veh) return;
    if (rp->ped->InVehicle() && rp->ped->Vehicle() == nv->veh) {
        rp->exitTaskMs = NowMs();
        Cmd(0x05CD).I(rp->pedHandle).I(nv->handle).Run();  // TASK_LEAVE_CAR
    }
}

void Players::HandleTeleport(uint8_t from, Reader& r) {
    uint8_t target = r.U8();
    CVector pos{r.F32(), r.F32(), r.F32()};
    int8_t area = r.I8();
    if (!r.Ok() || target != Net().MyId() || !core::IsPlayerReady()) return;
    CPlayerPed* me = FindPlayerPed(0);
    int handle = GetPedRef(me);
    CVector was = me->GetPosition();
    Cmd(0x04BB).I(area).Run();                 // SET_AREA_VISIBLE
    Cmd(0x0860).I(handle).I(area).Run();       // SET_CHAR_AREA_VISIBLE
    Cmd(0x00A1).I(handle).F(pos.x).F(pos.y).F(pos.z).Run();  // SET_CHAR_COORDINATES
    LightArea(area, pos);
    if (const RemotePlayer* rp = Get(from)) UseStreetDoor(area, pos, rp->streetPos.x != 0.0f ? rp->streetPos : was);
    ui::TheChat().AddSystem(Format(TR("%s переместил вас к себе.", "%s brought you over."), Net().PeerName(from).c_str()));
}

void Players::ApplyState(RemotePlayer& rp) {
    CPlayerPed* ped = rp.ped;
    const OnFootState& st = rp.state;

    // pad: "old" is always the previous frame, "new" only changes when a packet arrived
    rp.padOld = rp.padNew;
    if (rp.padFresh) {
        st.pad.To(rp.padNew);
        rp.padFresh = false;
    }

    // death is decided by the owner's game (the local ped has all proofs): play it once; after the
    // respawn a fresh ped replaces the dead one, which cannot be revived
    if (st.flags & OnFootState::F_DEAD) {
        if (!rp.deadShown) {
            // its game's fall comes in a message of its own, a moment after the state or before it: waited for briefly
            if (!rp.deadSinceMs) rp.deadSinceMs = NowMs();
            bool known = NowMs() - rp.dieMs < 3000;
            if (!known && NowMs() - rp.deadSinceMs < 300) return;
            rp.deadShown = true;
            LOGI("Remote player %d died (%s %d)", rp.id, known ? "its own fall" : "the default fall", known ? rp.dieAnim : 15);
            g_dieAnim = known ? rp.dieAnim : -1;
            Cmd(0x05BE).I(rp.pedHandle).Run();  // TASK_DIE
            g_dieAnim = -1;
        }
        return;
    }
    rp.deadSinceMs = 0;
    if (rp.deadShown) {
        rp.deadShown = false;
        DestroyRemote(rp);  // SpawnRemote runs next frame
        return;
    }

    ped->Health() = (float)st.health;
    ped->Armour() = (float)st.armour;
    if (st.area == 0) {
        rp.streetPos = st.pos;
    } else if (rp.doorArea != st.area || rp.doorPed != ped) {  // just went in: its blip at its door, not at the raw spot
        rp.doorArea = st.area;
        rp.doorPed = ped;
        void* door = StreetDoorOf(st.area, st.pos, rp.streetPos);
        Field<void*>(ped, 0x78C) = door;  // CPed::m_pEnex
        LOGD("Remote player %d is in area %d: its door %.8s", rp.id, st.area, door ? (const char*)door : "-");
    }
    if (st.area == 0 && rp.doorArea != 0) rp.doorArea = 0, Field<void*>(ped, 0x78C) = nullptr;
    // Through a shop's door the game's walk-in (CTaskComplexGotoDoorAndOpen) turns its player round after the push on the
    // door and walks it back to the street spot while its screen is dark: the other player saw it come back out. The copy
    // goes out of sight once it turns from the door, until its owner is inside (KeepApart hides it).
    if ((st.flags2 & OnFootState::F2_DOOR) && rp.mode == RemotePlayer::Mode::OnFoot) {
        // out on the other side (on its way out it comes out facing the street): the turn counts from there
        if (!rp.doorMs || st.area != rp.doorFromArea)
            rp.doorMs = NowMs(), rp.doorHeading = st.rotation, rp.doorFromArea = st.area, rp.goneInDoor = false;
        else if (std::fabs(WrapAngle(st.rotation - rp.doorHeading)) > 2.0f) rp.goneInDoor = true;
    } else {
        rp.doorMs = 0, rp.goneInDoor = false;
    }
    if (!Ref<uint8_t>(0xB5F851)) ped->AreaCode() = st.area;  // CCutsceneMgr::ms_running: the scene sets it (Missions)

    // crouching toggles on the crouch button's press, which the copy replays from the owner's pad: a press lost on the
    // way (or a press and its release in one frame) left the copy standing while its owner crouched, and every next
    // press the other way round. A difference that lasts is the owner's state put right.
    bool duck = (st.flags & OnFootState::F_DUCKING) != 0;
    if (duck == ped->IsDucking() || rp.mode != RemotePlayer::Mode::OnFoot) {
        rp.duckOffMs = 0;
    } else if (!rp.duckOffMs) {
        rp.duckOffMs = NowMs();
    } else if (NowMs() - rp.duckOffMs > 400 && ped->Intelligence()) {
        if (duck) CallMethod<0x601230>(ped->Intelligence(), (uint16_t)0);  // CPedIntelligence::SetTaskDuckSecondary
        else CallMethod<0x601390>(ped->Intelligence());                    // CPedIntelligence::ClearTaskDuckSecondary
        rp.duckOffMs = 0;
        LOGD("Remote player %d: crouch %s to match its owner", rp.id, duck ? "on" : "off");
    }

    // a phone call (CTaskComplexUseMobilePhone): the phone is put in the hand as a weapon's model, and the weapon's
    // comes back after it (CPed__removeCellphone); the animations go with Anims
    bool phone = (st.flags & OnFootState::F_PHONE) != 0;
    if (phone != (Field<int32_t>(ped, 0x740) == kCellphone) && ModelReady(kCellphone)) {
        if (phone) {
            CallMethod<0x5E5ED0>(ped, kCellphone);  // CPed::AddWeaponModel
        } else {
            CallMethod<0x5E3990>(ped, kCellphone);                       // CPed::RemoveWeaponModel
            CallMethod<0x5E6280>(ped, (int)ped->ActiveWeapon().type);  // CPed::SetCurrentWeapon: its gun again
        }
        LOGI("Remote player %d: phone %s", rp.id, phone ? "out" : "away");
    }

    // the jetpack flies by the player's buttons, which this ped gets: it only needs one. Dropping it (the enter key)
    // happens here as there; whatever is left over goes with the flag
    bool jetpack = (st.flags & OnFootState::F_JETPACK) != 0;
    if (jetpack != HasJetpack(ped) && rp.mode == RemotePlayer::Mode::OnFoot && NowMs() - rp.jetpackMs > 1000 &&
        (!jetpack || ModelReady(370))) {  // the jetpack's model
        rp.jetpackMs = NowMs();
        if (jetpack) {
            Cmd(0x07A7).I(rp.pedHandle).Run();   // TASK_JETPACK
        } else {
            Cmd(0x0792).I(rp.pedHandle).Run();   // CLEAR_CHAR_TASKS_IMMEDIATELY
        }
        LOGI("Remote player %d: jetpack %s", rp.id, jetpack ? "on" : "off");
    }

    // weapon in hand
    CWeapon& active = ped->ActiveWeapon();
    int weapon = st.weapon <= kLastWeapon ? st.weapon : 0;  // from the network
    if (int model = weapon != 0 ? WeaponModel(weapon) : 0;
        active.type != weapon && NowMs() - rp.lastWeaponTry > 500 && (model <= 0 || ModelReady(model))) {
        rp.lastWeaponTry = NowMs();
        if (weapon != 0) Cmd(0x01B2).I(rp.pedHandle).I(weapon).I(std::max<int>(st.ammo, 1)).Run();  // GIVE_WEAPON_TO_CHAR
        Cmd(0x01B9).I(rp.pedHandle).I(weapon).Run();  // SET_CURRENT_CHAR_WEAPON
    } else if (weapon != 0 && active.type == weapon) {
        active.ammoTotal = std::max<int>(st.ammo, 1);  // never run dry locally: the owner decides
        if (active.ammoInClip <= 0) active.ammoInClip = 1;
    }

    // the copy on a bike that is not ours (it is the other game's) falls only when its game says so: knocked off here
    // by this game's impacts it ran riderless until the next warp. On our bike it stays knockable, else our own
    // driver never fell (DamageKnockOffRider returns when either rider can never be knocked off).
    CVehicle* rv = ped->InVehicle() ? ped->Vehicle() : nullptr;
    uint32_t never = rv && rv->IsBike() && rv->Driver() != FindPlayerPed(0) ? 1u << 27 : 0;
    ped->PedFlags3() = (ped->PedFlags3() & ~(3u << 27)) | never;
    if (rp.mode != RemotePlayer::Mode::OnFoot) {
        if (ped->AttachedTo()) DetachPed(ped);
        ApplyVehicleSeat(rp);
        // A car through a door of its own (a mod garage moves it into CARMOD1-3, a stadium's events) changes its driver's
        // and its area in the driver's game only: the copy (culled by area) vanished, and our player riding along saw
        // the empty sky of an interior. The car takes its driver's area here, and so do we when we ride in it.
        if (CVehicle* v = rp.mode == RemotePlayer::Mode::Driver && ped->InVehicle() ? ped->Vehicle() : nullptr;
            v && v->Driver() == ped && v->AreaCode() != rp.state.area && !Ref<uint8_t>(0xB5F851)) {  // not in a cutscene
            v->AreaCode() = ped->AreaCode() = rp.state.area;
            if (CPlayerPed* me = FindPlayerPed(0); me->InVehicle() && me->Vehicle() == v && me->AreaCode() != rp.state.area) {
                int h = GetPedRef(me);
                Cmd(0x04BB).I(rp.state.area).Run();          // SET_AREA_VISIBLE
                Cmd(0x0860).I(h).I(rp.state.area).Run();     // SET_CHAR_AREA_VISIBLE
                LightArea(rp.state.area, me->GetPosition());
            }
            LOGI("Player %d's car went into area %d: here too", rp.id, rp.state.area);
        }
        return;
    }
    ShowDowned(rp);
    if (FollowAttach(rp)) {
        float r = LerpAngle(ped->CurrentRotation(), st.rotation, 0.6f);
        ped->CurrentRotation() = ped->AimingRotation() = r;
        return;
    }
    // in its cab here; a train this game has no copy of (far away): it goes along as on foot, for its blip
    if ((st.flags & OnFootState::F_IN_TRAIN) && RideTrain(rp)) return;
    // thrown off its bike in its game (on foot without an exit message) while our player rides behind it: ours falls too
    if (CPlayerPed* me = FindPlayerPed(0); rv && rv->IsBike() && rv->Driver() == ped && me->InVehicle() &&
                                           me->Vehicle() == rv && NowMs() - rp.exitTaskMs > 1000) {
        Cmd(0x0622).I(GetPedRef(me)).I(GetVehicleRef(rv)).Run();  // TASK_LEAVE_CAR_IMMEDIATELY
        rp.exitTaskMs = NowMs();
        LOGI("Player %d fell off the bike in its game: ours gets off too", rp.id);
    }
    // on foot but still sitting in a vehicle here: the exit animation gets a few seconds, then warp out
    if (ped->InVehicle()) {
        if (NowMs() - rp.exitTaskMs > kEnterGraceMs) {
            Cmd(0x0362).I(rp.pedHandle).F(st.pos.x).F(st.pos.y).F(st.pos.z - 1.0f).Run();  // WARP_CHAR_FROM_CAR_TO_COORD (feet)
            rp.exitTaskMs = NowMs();
        }
        return;
    }

    if (Ref<uint8_t>(0xB5F851)) return;  // CCutsceneMgr::ms_running: Missions places the ped in the scene
    if (RideVehicle(rp)) return;
    // getting into a car: its task walks it to the door, turns it and opens the door; the pull to its owner's place
    // and facing (its owner is at the door or in the seat already) held it on the spot until the warp
    if (EnteringCar(ped) && NowMs() - rp.enterTaskMs < kEnterGraceMs && (st.pos - ped->GetPosition()).Length() < 6.0f) return;

    // position: predict, then correct smoothly (teleport on large errors)
    // from the state's leaving its owner: the time it took to come too (0.4.0; the copy was a network trip behind)
    float age = std::min(0.4f, (float)(NowMs() - rp.stateRecvMs) / 1000.0f + Net().LatencyFrom(rp.id));
    CVector target = st.pos + st.vel * age;
    CVector cur = ped->GetPosition();
    CVector err = target - cur;
    float dist2d = err.Length2D();
    // Where this game has no ground under it (one of you indoors: the other's map is unloaded; or far away), its
    // physics would drop it through the map, and the pull would hold it there, under the teleport distance.
    CPlayerPed* me = FindPlayerPed(0);
    bool found = false;
    float ground = Call<0x5696C0, float>(target.x, target.y, target.z + 1.0f, &found, (CEntity**)nullptr);  // FindGroundZFor3DCoord
    bool groundless = st.area != me->AreaCode() || !found || (target - me->GetPosition()).Length2D() > 150.0f;
    bool sunk = found && cur.z < ground + 0.3f && target.z > ground + 0.5f;  // a standing ped is ~1 m above its ground
    // A climb puts the copy on its wall's handholds by itself, a moment before or after its owner: pulled to its owner's
    // last spot meanwhile, it was dragged back over the edge it had just climbed and fell (FALL_glide) onto the top
    void* intel = ped->Intelligence();
    if (intel && CallMethod<0x681740, void*>((uint8_t*)intel + 4, 254)) rp.climbMs = NowMs();  // TASK_SIMPLE_CLIMB
    bool climbing = NowMs() - rp.climbMs < 500 && dist2d < 2.0f && std::fabs(err.z) < 2.5f;
    if (climbing) {
        // left to its own climb and the half second after it
    } else if (dist2d > kTeleportDist || std::fabs(err.z) > kTeleportDist || sunk) {
        Cmd(0x00A1).I(rp.pedHandle).F(st.pos.x).F(st.pos.y).F(st.pos.z - 1.0f).Run();  // SET_CHAR_COORDINATES
        ped->MoveSpeed() = st.vel * (1.0f / 50.0f);
    } else if (groundless) {
        SetEntityPosition(ped, target);
        ped->MoveSpeed() = CVector{};
    } else if (dist2d > 0.05f || std::fabs(err.z) > 1.0f) {
        float k = std::min(1.0f, TimeStep() * 0.25f);  // ~ 12 per second at 50 steps
        CVector corr{err.x * k, err.y * k, std::fabs(err.z) > 1.0f ? err.z * k : 0.0f};
        SetEntityPosition(ped, cur + corr);
        if (err.z > 1.0f && ped->MoveSpeed().z < 0.0f) ped->MoveSpeed().z = 0.0f;  // below its place: no falling on
    }
    // standing still: keep the facing in sync (movement sets it while active). Aiming, the owner's game turns its player
    // with its camera, and that camera exists only there: here the player kept facing where it began to aim and only
    // its gun went up and down. It faces as its owner, quickly.
    bool moving = std::abs(st.pad.lx) > 10 || std::abs(st.pad.ly) > 10;
    bool aiming = (st.pad.buttons & PadState::B_RS1) != 0;
    if (aiming || !moving) {
        float r = LerpAngle(ped->CurrentRotation(), st.rotation, aiming ? 0.6f : 0.2f);
        ped->CurrentRotation() = ped->AimingRotation() = r;
    }
}

// Wounded (Revive): its owner's game holds it on the ground with its controls off, so its pad is still here and only
// the pose needs showing: it falls as its owner did, lies there, gets up when it is revived (a death is F_DEAD).
void Players::ShowDowned(RemotePlayer& rp) {
    bool down = (rp.state.flags & OnFootState::F_DOWNED) != 0;
    // nobody here aims at it either (CPed::bNeverEverTargetThisPed)
    rp.ped->PedFlags2() = down ? (rp.ped->PedFlags2() | (1u << 28)) : (rp.ped->PedFlags2() & ~(1u << 28));
    if (down) {
        if (rp.downShown && NowMs() - rp.downCheckMs < 1000) return;
        rp.downCheckMs = NowMs();
        if (rp.downShown && Cmd(0x0611).I(rp.pedHandle).S(kDownAnim).Run()) return;  // IS_CHAR_PLAYING_ANIM
        if (rp.ped->InVehicle()) return;
        Cmd(0x0605).I(rp.pedHandle).S(kDownAnim).S("PED").F(4.0f).I(0).I(0).I(0).I(1).I(-1).Run();  // TASK_PLAY_ANIM, held
        if (!rp.downShown) LOGI("Remote player %d is wounded", rp.id);
        rp.downShown = true;
    } else if (rp.downShown) {
        rp.downShown = false;
        Cmd(0x0792).I(rp.pedHandle).Run();  // CLEAR_CHAR_TASKS_IMMEDIATELY
        Cmd(0x0605).I(rp.pedHandle).S(kGetUpAnim).S("PED").F(4.0f).I(0).I(0).I(0).I(0).I(-1).Run();  // TASK_PLAY_ANIM, once
        LOGI("Remote player %d got up", rp.id);
    }
}

// A script of its owner's game stood it on a vehicle (the back of a truck, a bike's pillion): here it stands on that
// vehicle's copy the same way, where the pull to its sent place made it shake and fall behind a moving car. Attaching a
// player sets the camera's weapon mode (the turret's) for this game's player: ours is put back.
bool Players::FollowAttach(RemotePlayer& rp) {
    const OnFootState& st = rp.state;
    CPed* ped = rp.ped;
    NetVehicle* nv = (st.flags & OnFootState::F_ATTACHED) ? Vehicles::Instance().Find(st.attachVeh) : nullptr;
    CVehicle* veh = nv && Vehicles::Alive(*nv) && !ped->InVehicle() ? nv->veh : nullptr;
    if (ped->AttachedTo() && ped->AttachedTo() != veh) {
        DetachPed(ped);
        LOGI("Remote player %d: off the vehicle", rp.id);
    }
    if (!veh) return false;
    if (ped->AttachedTo() != veh) {
        uint8_t mode[12];
        memcpy(mode, (void*)(addr::TheCamera + 0x830), sizeof mode);  // CCamera::m_PlayerWeaponMode
        const CVector& o = st.attachOffset;
        int weapon = st.weapon <= kLastWeapon ? st.weapon : 0;
        Cmd(0x0464).I(rp.pedHandle).I(nv->handle).F(o.x).F(o.y).F(o.z).I(st.attachA).F(st.attachB * 57.29578f).I(weapon).Run();  // ATTACH_CHAR_TO_CAR
        memcpy((void*)(addr::TheCamera + 0x830), mode, sizeof mode);
        LOGI("Remote player %d stands on vehicle %u (%.1f %.1f %.1f)", rp.id, st.attachVeh, o.x, o.y, o.z);
    }
    return ped->AttachedTo() == veh;
}

// Standing on a moving vehicle (a car's roof, the back of a pickup): its place goes in the vehicle's own frame and it
// stands there on this game's copy of it. Its place in the world came with the network's delay while the copy moves
// on: it was pulled along behind the car, fell off and was dragged down the road.
bool Players::RideVehicle(RemotePlayer& rp) {
    const OnFootState& st = rp.state;
    if (!(st.flags & OnFootState::F_ON_VEHICLE)) return false;
    NetVehicle* nv = Vehicles::Instance().Find(st.onVeh);
    if (!nv || !Vehicles::Alive(*nv) || nv->veh->MoveSpeed().Length() * 50.0f < 0.5f) return false;  // standing still: as usual
    CPed* ped = rp.ped;
    SetEntityPosition(ped, ToWorld(nv->veh, st.onLocal));
    ped->MoveSpeed() = nv->veh->MoveSpeed();
    float r = LerpAngle(ped->CurrentRotation(), st.rotation, 0.3f);
    ped->CurrentRotation() = ped->AimingRotation() = r;
    return true;
}

// Trains are not networked as vehicles (Trains.cpp copies them): the player riding one sits in the cab of the train
// here nearest to its place. False: no train here near its place.
bool Players::RideTrain(RemotePlayer& rp) {
    CVehicle* train = Call<0x6F7090, CVehicle*>(rp.state.pos, true);  // CTrain::FindNearestTrain(position, an engine)
    CPlayerPed* ped = rp.ped;
    if (!train || (train->GetPosition() - rp.state.pos).Length() > 25.0f) return false;
    if ((ped->InVehicle() && ped->Vehicle() == train) || NowMs() - rp.enterTaskMs < 1500) return true;
    rp.enterTaskMs = NowMs();
    CVector p = ped->GetPosition();
    if (ped->InVehicle()) {
        Cmd(0x0362).I(rp.pedHandle).F(p.x).F(p.y).F(p.z).Run();  // WARP_CHAR_FROM_CAR_TO_COORD
        return true;
    }
    if (CPed* d = train->Driver(); d && d->IsPlayer()) return true;      // taken here
    else if (d) Cmd(0x009B).I(GetPedRef(d)).Run();                       // DELETE_CHAR: the copy's own driver
    Field<uint8_t>(train, 0x42A) |= 2;  // bHasBeenOwnedByPlayer: a player warped in reports a car theft (0x64BBBA) on ours
    QuietGroup quiet(rp.ped);  // its getting in calls nobody of ours
    Cmd(0x036A).I(rp.pedHandle).I(GetVehicleRef(train)).Run();          // WARP_CHAR_INTO_CAR
    return true;
}

void Players::ApplyVehicleSeat(RemotePlayer& rp) {
    NetVehicle* nv = Vehicles::Instance().Find(rp.vehNetId);
    if (!nv || !Vehicles::Alive(*nv)) return;  // the vehicle announcement has not arrived yet
    CPlayerPed* ped = rp.ped;
    int seat = PassengerSeatOf(nv->veh, ped);
    bool seated = ped->InVehicle() && ped->Vehicle() == nv->veh && seat == rp.seat;
    if (seated) return;
    // give the enter animation time while it goes on; otherwise put the player in place directly. A copy whose
    // animation stands still (it never got to open the door: its owner, seated already, drives off, the door is
    // blocked here) used to stand frozen by the car until the grace was over and then appeared inside.
    if (NowMs() - rp.enterTaskMs < kEnterGraceMs && EnteringCar(ped)) {
        CVector p = ped->GetPosition();
        if ((p - rp.enterPos).Length() > 0.05f || !rp.enterStillMs) rp.enterPos = p, rp.enterStillMs = NowMs();
        bool stuck = NowMs() - rp.enterStillMs > 1200 || nv->veh->MoveSpeed().Length2D() * 50.0f > 3.0f;
        if (!stuck) return;
        LOGD("Remote player %d: the enter animation stalled, into the seat", rp.id);
        Cmd(0x0792).I(rp.pedHandle).Run();  // CLEAR_CHAR_TASKS_IMMEDIATELY
    }
    rp.enterStillMs = 0;
    if (ped->InVehicle()) {
        CVector p = ped->GetPosition();
        Cmd(0x0362).I(rp.pedHandle).F(p.x).F(p.y).F(p.z).Run();  // WARP_CHAR_FROM_CAR_TO_COORD
    }
    // a player warped in as the driver of a car no player owned yet reports a car theft against FindPlayerPed, ours here
    // (CTaskSimpleCarSetPedInAsDriver, 0x64BBBA), and makes it a player's car: the local pad drove it until the next frame
    Field<uint8_t>(nv->veh, 0x42A) |= 2;  // CVehicle::bHasBeenOwnedByPlayer
    if (rp.seat < 0) {
        CPed* d = nv->veh->Driver();
        if (d && d != ped && d->IsPlayer()) return;  // our player: two drivers, Vehicles' ownership sorts it out
        // an NPC that its game pulled out (the jack went through there), while the copy's own jack here never got that
        // far: it sat at the wheel of a car the other player drives, the copy stood outside for good
        if (d && d != ped) {
            CVector p = d->GetPosition();
            Cmd(0x0362).I(GetPedRef(d)).F(p.x).F(p.y).F(p.z + 1.0f).Run();  // WARP_CHAR_FROM_CAR_TO_COORD
            LOGI("Player %d drives vehicle %04X in its game: the driver here gets out", rp.id, nv->netId);
        }
        QuietGroup quiet(rp.ped);  // its getting in calls nobody of ours
        Cmd(0x036A).I(rp.pedHandle).I(nv->handle).Run();  // WARP_CHAR_INTO_CAR
        if (nv->veh->Driver() == ped) nv->veh->SetStatus(3);  // STATUS_PHYSICS: its owner's state moves it
    } else {
        if (SeatOrAny(nv->veh, rp.seat) < 0 || nv->veh->Passengers()[rp.seat]) return;  // no such seat, or taken
        QuietGroup quiet(rp.ped);
        Cmd(0x0430).I(rp.pedHandle).I(nv->handle).I(rp.seat).Run();  // WARP_CHAR_INTO_CAR_AS_PASSENGER
    }
    rp.enterTaskMs = NowMs() - kEnterGraceMs + 1500;  // retry no more often than every 1.5 s
}

// A bike the host's mission made, or the one the host rides in it, is the mission's: in "OG Loc" OG Loc gets on behind
// CJ on the bike the script made for them only (TASK_ENTER_CAR_AS_PASSENGER on that bike, then it waits for both on
// it), and Freddy flees on the other. A partner behind the host (G) or on one of them left OG Loc no seat and the
// mission waiting for good. A partner gets a bike of its own instead (FollowHost).
namespace {
bool MissionBike(CVehicle* v) {
    if (!v || !v->IsBike() || Net().IsHost() || !HostMissionRecent(0)) return false;
    RemotePlayer* host = Players::Instance().Get(Net().HostId());
    if (host && host->ped && v->Driver() == host->ped) return true;
    NetVehicle* nv = Vehicles::Instance().Find(v);
    return nv && nv->script && proto::EntityMaker(nv->netId) == Net().HostId();
}
}  // namespace

// The nearest free seat by where one gets in to it: the door of that seat (CCarEnterExit::GetPositionToOpenCarDoor of
// ComputeTargetDoorToEnterAsPassenger), whoever's car it is. It used to be the nearest car's first free seat, and the
// partner's car over any other: the player walked round to the far door, or past a car to the partner's.
void Players::TryEnterAsPassenger() {
    if (!core::IsPlayerReady()) return;
    CPlayerPed* me = FindPlayerPed(0);
    // aiming at someone, G is the gang's key (recruiting a Grove member): CJ went for the parked car beside him instead
    if (me->InVehicle() || EnteringCar(me) || GetPad(0)->NewState.RightShoulder1 || me->TargetedObject()) return;
    CPoolHeader* pool = VehiclePool();
    if (!pool) return;
    CVector mp = me->GetPosition();
    CVehicle* best = nullptr;
    int bestSeat = -1;
    float bestDist = 1e9f;
    for (int i = 0; i < pool->size; ++i) {
        if (pool->byteMap[i] < 0) continue;
        auto* v = (CVehicle*)(pool->objects + (size_t)i * kVehiclePoolElemSize);
        if ((v->GetPosition() - mp).Length() > 12.0f || v->Status() == 5 /* wrecked */ || v->IsTrain()) continue;  // a train's copy is locked (Trains)
        if (MissionBike(v)) continue;
        for (int s = 0; s < v->MaxPassengers() && s < 8; ++s) {
            if (v->Passengers()[s]) continue;
            int door = Call<0x64F190, int>(v, s);  // CCarEnterExit::ComputeTargetDoorToEnterAsPassenger
            if (door < 0) continue;
            CVector at;
            Call<0x64E740, CVector*>(&at, v, door);  // CCarEnterExit::GetPositionToOpenCarDoor
            float d = (at - mp).Length();
            if (d < bestDist) {
                bestDist = d;
                best = v;
                bestSeat = s;
            }
        }
    }
    if (!best || bestDist > 8.0f) return;
    Cmd(0x05CA).I(GetPedRef(me)).I(GetVehicleRef(best)).I(10000).I(bestSeat).Run();  // TASK_ENTER_CAR_AS_PASSENGER
}

void Players::DetectCarTasks() {
    CPlayerPed* me = FindPlayerPed(0);
    void* task = nullptr;
    int type = ActivePrimaryTaskType(me, &task);
    bool passenger = type == TASK_ENTER_CAR_AS_PASSENGER || type == TASK_ENTER_CAR_AS_PASSENGER_TIMED;
    if ((type == TASK_ENTER_CAR_AS_DRIVER || passenger) && task) {
        auto* target = Field<CVehicle*>(task, 0x0C);  // CTaskComplexEnterCar::m_Car, ...AsPassengerTimed::m_TargetCar
        if (target && IsVehicleValid(target) && MissionBike(target)) {  // the mission's: not for us (MissionBike)
            Cmd(0x0792).I(GetPedRef(me)).Run();  // CLEAR_CHAR_TASKS_IMMEDIATELY: still on foot, by it
            static uint64_t toldMs = 0;
            if (NowMs() - toldMs > 10000) {
                toldMs = NowMs();
                ui::TheChat().AddSystem(TR("Этот мотоцикл нужен миссии. Когда хост поедет, рядом с вами появится свой.",
                                           "This bike is the mission's. When the host rides off, you get one of your own."));
                LOGI("Kept off a bike of the host's mission");
            }
            return;
        }
        if (target && target != m_enterReported && IsVehicleValid(target)) {
            m_enterReported = target;
            if (NetVehicle* nv = Vehicles::Instance().EnsureNetworked(target)) {
                LOGI("Our player gets into vehicle %04X (model %d, %s's game%s)%s", nv->netId, target->ModelIndex(),
                     Net().PeerName(proto::EntityMaker(nv->netId)).c_str(), nv->script ? ", a script's" : "",
                     passenger ? " as a passenger" : "");
                int seat = -1;
                if (type == TASK_ENTER_CAR_AS_PASSENGER_TIMED)  // the door the command chose for its seat (0: any)
                    seat = SeatOfTargetDoor(Field<int32_t>(task, 0x14));
                else if (passenger)  // m_TargetDoor once at the door, else m_TargetSeat (a door too)
                    seat = SeatOfTargetDoor(Field<int32_t>(task, 0x14) ? Field<int32_t>(task, 0x14) : Field<int32_t>(task, 0x1C));
                Writer w(proto::G_PLAYER_ENTER_CAR);
                w.U16(nv->netId);
                w.I8((int8_t)seat);
                Net().Send(proto::CH_RELIABLE, w);
            }
        }
    } else if (!me->InVehicle()) {
        m_enterReported = nullptr;
    }
    if (type == TASK_LEAVE_CAR && me->InVehicle()) {
        if (!m_exitReported) {
            m_exitReported = true;
            if (NetVehicle* nv = Vehicles::Instance().Find(me->Vehicle())) {
                Writer w(proto::G_PLAYER_EXIT_CAR);
                w.U16(nv->netId);
                Net().Send(proto::CH_RELIABLE, w);
            }
        }
    } else if (!me->InVehicle()) {
        m_exitReported = false;
    }
}

// The players near this one get its state at the full rate, the others twice a second: enough for their radar, and
// players spread over the map do not flood each other (the server relays each state to every player it goes to).
constexpr float kFullRateDist = 350.0f;
constexpr uint64_t kFarStateMs = 500;

proto::PlayerMask Players::StateTargets(const CVector& pos, int8_t area) {
    uint64_t now = NowMs();
    if (now - m_lastFarSend >= kFarStateMs) {
        m_lastFarSend = now;
        return Net().PeersMask();
    }
    return PlayersNear(pos, area, kFullRateDist);
}

void Players::SendVehicleState(CVehicle* v) {
    CPlayerPed* me = FindPlayerPed(0);
    NetVehicle* nv = Vehicles::Instance().EnsureNetworked(v);
    if (!nv) return;
    OnFootState common;
    FillCommon(common, me);
    uint64_t now = NowMs();
    // a button goes at once; a stick that moves (a gamepad's never rests) sent every frame: 33 ms at most, as SA-MP's rates
    bool padChanged = common.pad.buttons != m_lastSentPad.buttons || (!(common.pad == m_lastSentPad) && now - m_lastSend >= 33);
    bool driver = v->Driver() == me;
    // ours from the first frame at its wheel: past the limit below, the copy's next update put its owner's parked state
    // back on it (the engine off), and the car did not go till got into again (original #43)
    if (driver) Vehicles::Instance().TakeOwnership(*nv);
    // a passenger leaning out with a gun aims with the camera, which goes in this state: at the driver's rate then (10 a
    // second turned the copy's gun in steps, and its shots went where it had aimed a moment before)
    bool aiming = (common.pad.buttons & PadState::B_RS1) != 0;
    uint64_t interval = driver || aiming ? 33 : 100;
    if (!padChanged && now - m_lastSend < interval) return;
    m_lastSend = now;
    m_lastSentPad = common.pad;
    if (driver) {
        VehicleState vs = VehicleState::Capture(v);
        vs.time = (uint32_t)now;
        Writer w(proto::G_PLAYER_DRIVER);
        w.U16(nv->netId);
        vs.Write(w);
        WriteCommon(w, common);
        Net().SendTo(StateTargets(v->GetPosition(), me->AreaCode()), proto::CH_STATE, w);
    } else {
        int seat = PassengerSeatOf(v, me);
        Writer w(proto::G_PLAYER_PASSENGER);
        w.U16(nv->netId);
        w.I8((int8_t)std::max(seat, 0));
        w.U32((uint32_t)now);
        CVector p = v->GetPosition();
        w.Vec({p.x, p.y, p.z});
        WriteCommon(w, common);
        Net().SendTo(StateTargets(p, me->AreaCode()), proto::CH_STATE, w);
    }
}

void Players::OnPreProcess() {
    if (!Net().Online() || !core::IsPlayerReady()) return;
    for (auto& [id, rp] : m_remote) {
        if ((rp.ped && !IsPedValid(rp.ped)) || (!rp.ped && rp.pedHandle >= 0)) {  // the engine deleted it
            LOGW("Remote player %d ped vanished, respawning", id);
            rp.ped = nullptr;
            rp.pedHandle = -1;
            rp.blip = 0;
        }
        // states older than 5 s mean the player is loading / in the menu; a cutscene here keeps its ped in the scene
        // (its game loads the same cutscene and can be silent that long)
        bool active = rp.hasState && (NowMs() - rp.stateRecvMs < 5000 || (rp.ped && Ref<uint8_t>(0xB5F851)));
        if (!active) {
            if (rp.ped) DestroyRemote(rp);
            continue;
        }
        if (!rp.ped) SpawnRemote(rp);
        if (rp.ped) ApplyState(rp);
        if (rp.ped) KeepApart(rp);
        if (rp.ped) KeepWalkSet(rp);
    }
}

// The copy walks with its owner's body (CPlayerPed::GetDefaultPlayerMotionGroup on its stats, applied while it is
// processed), but CClothes::RequestMotionGroupAnims keeps only this game's player's walk set in memory: a fat or a
// muscular partner walked like a plain CJ on the other screen. Its set is asked for too, once a second.
void Players::KeepWalkSet(RemotePlayer& rp) {
    if (rp.stats.size() != kStatsBlockSize || NowMs() - rp.walkSetMs < 1000) return;
    rp.walkSetMs = NowMs();
    constexpr size_t kFloats = 0xB79380 - kStatsBlock;  // CStats::StatTypesFloat
    float fat = *(const float*)&rp.stats[kFloats + 21 * 4], muscle = *(const float*)&rp.stats[kFloats + 23 * 4];  // STAT_FAT, STAT_MUSCLE
    const char* block = fat > 500.0f && fat > muscle ? "fat" : muscle > 500.0f ? "muscular" : nullptr;
    if (int i = block ? Call<0x4D3990, int>(block) : -1; i >= 0) RequestModel(anim::kIfpBase + i, 2);  // CAnimManager::GetAnimationBlockIndex
}

// A script that puts this game's player on a spot (a changing room, a mission's) puts it inside the other player's
// ped when that one stands there: the two peds got stuck in each other and the player's next scripted task never
// started (the shop waits for it with the controls taken). Two players inside each other ignore each other's collision
// until they part (CPhysical::m_pEntityIgnoredCollision, the game clears it once they stop touching).
void Players::KeepApart(RemotePlayer& rp) {
    CPed* other = rp.ped;
    CPlayerPed* me = FindPlayerPed(0);
    CVector d = me ? other->GetPosition() - me->GetPosition() : CVector{};
    bool inside = me && !me->InVehicle() && !other->InVehicle() && d.x * d.x + d.y * d.y < 0.4f * 0.4f && std::fabs(d.z) < 1.5f;
    // Both on one scripted spot (the range's booth, a barber's chair, the bench press, a changing room, a casino seat):
    // each game puts its own player there, and the two bodies stood in each other. The other one is hidden after a
    // second of it, until they part (a cutscene shows the partner its own way, ScenePartner).
    rp.overlapMs = inside ? (rp.overlapMs ? rp.overlapMs : NowMs()) : 0;
    bool hide = ((inside && NowMs() - rp.overlapMs > 1000) || rp.goneInDoor) && !Ref<uint8_t>(0xB5F851);  // CCutsceneMgr::ms_running
    if (hide != rp.hiddenOnSpot) {
        rp.hiddenOnSpot = hide;
        if (hide) other->Flags() &= ~0x80u;  // CEntity::m_bIsVisible
        else other->Flags() |= 0x80u;
        LOGI("Remote player %d %s", rp.id, !hide ? "shown again" : rp.goneInDoor ? "gone in through the door" : "hidden on a shared spot");
    }
    if (!inside) return;
    Field<CEntity*>(me, 0x128) = other;
    Field<CEntity*>(other, 0x128) = me;
}

void Players::SendLocalState() {
    CPlayerPed* me = FindPlayerPed(0);
    if (!me) return;
    OnFootState st;
    st.time = (uint32_t)NowMs();
    st.pos = me->GetPosition();
    st.vel = me->MoveSpeed() * 50.0f;
    st.rotation = me->CurrentRotation();
    st.aimRotation = me->AimingRotation();
    st.health = ClampByte(me->Health());
    st.armour = ClampByte(me->Armour());
    CWeapon& w = me->ActiveWeapon();
    st.weapon = (uint8_t)w.type;
    st.ammo = (uint16_t)std::clamp(w.ammoTotal, 0, 65535);
    st.flags = (me->IsDucking() ? OnFootState::F_DUCKING : 0) | (me->IsInTheAir() ? OnFootState::F_IN_AIR : 0) |
               (HasJetpack(me) ? OnFootState::F_JETPACK : 0) |
               (me->Health() <= 0.0f ? OnFootState::F_DEAD : 0) |
               (me->InVehicle() && me->Vehicle() && me->Vehicle()->IsTrain() ? OnFootState::F_IN_TRAIN : 0) |
               (Field<int32_t>(me, 0x740) == kCellphone ? OnFootState::F_PHONE : 0) | MakesFlags() |  // CPed::m_nWeaponModelId
               (TaskWalks(me) ? OnFootState::F_TASK_WALK : 0);
    st.flags2 = Ref<int32_t>(0x96A7CC) != 0 ? OnFootState::F2_DOOR : 0;  // CEntryExitManager::ms_exitEnterState
    if (m_downed) st.flags |= OnFootState::F_DOWNED, st.downSeconds = m_downSeconds;
    if (m_reviving) st.flags |= OnFootState::F_REVIVING;
    if (CPhysical* to = me->AttachedTo(); to && to->IsVehicle()) {
        if (NetVehicle* nv = Vehicles::Instance().Find((CVehicle*)to)) {
            st.flags |= OnFootState::F_ATTACHED;
            st.attachVeh = nv->netId;
            st.attachOffset = Field<CVector>(me, 0x76C);  // CPed::m_vecTurretOffset
            st.attachA = Field<uint16_t>(me, 0x778);      // m_fTurretAngleA (the side it faces)
            st.attachB = Field<float>(me, 0x77C);         // m_fTurretAngleB (how far it turns, radians)
        }
    }
    if (!(st.flags & OnFootState::F_ATTACHED) && !me->InVehicle())
        if (CVehicle* v = StandingOnVehicle(me); v && IsVehicleValid(v) && !v->IsTrain())
            if (NetVehicle* nv = Vehicles::Instance().Find(v)) {
                st.flags |= OnFootState::F_ON_VEHICLE;
                st.onVeh = nv->netId;
                st.onLocal = ToLocal(v, st.pos);
            }
    st.area = me->AreaCode();
    st.shots = g_localShots;
    st.moveState = (uint8_t)me->MoveState();
    st.wanted = WantedLevel(me);
    CPad* pad = GetPad(0);
    if (pad->DisablePlayerControls == 0 && !game::MenuActive()) st.pad = PadState::From(pad->NewState);
    CameraState& c = st.cam;
    c.mode = cam::Mode();
    c.weaponMode = cam::WeaponMode();
    c.fov = cam::Fov();
    c.front = cam::Front();
    c.sourceOffset = cam::Source() - st.pos;
    c.up = cam::Up();
    c.orientation = cam::Orientation();
    c.alpha = cam::Alpha();
    c.beta = cam::Beta();
    c.lookPitch = me->PlayerData() ? me->LookPitch() : 0.0f;
    c.useMouse3rd = cam::UseMouse3rdPerson() != 0;
    c.chairX = cam::CrosshairMultX();
    c.chairY = cam::CrosshairMultY();

    uint64_t now = NowMs();
    // A jump or a crouch is a press, often a tap one frame long, and it went in one state only (on the unreliable channel):
    // with that state lost the other games never saw it. A tap now stays down in the states of the next 80 ms (a
    // longer press of those changes nothing there: they act on the press).
    constexpr uint16_t kTaps = PadState::B_SQUARE | PadState::B_SHOCKL;
    uint16_t down = st.pad.buttons & ~m_realButtons & kTaps;
    m_realButtons = st.pad.buttons;
    for (int b = 0; b < 16; ++b) {
        if (down & (1u << b)) m_stickyUntil[b] = now + 80;
        if (m_stickyUntil[b] > now) st.pad.buttons |= (uint16_t)(1u << b);
    }
    bool padChanged = st.pad.buttons != m_lastSentPad.buttons;  // a stick alone waits for the 30 Hz below (see SendVehicleState)
    if (!padChanged && now - m_lastSend < 33) return;  // ~30 Hz, immediately on a button
    m_lastSend = now;
    m_lastSentPad = st.pad;
    Writer wr(proto::G_PLAYER_ONFOOT);
    st.Write(wr);
    Net().SendTo(StateTargets(st.pos, st.area), proto::CH_STATE, wr);
}

void Players::OnPostProcess() {
    if (!Net().Online() || !core::IsPlayerReady()) return;
    CPlayerPed* me = FindPlayerPed(0);
    if (!me) return;
    // CBike::VehicleDamage calls CBike::DamageKnockOffRider (0x6B5A10) before its collision-proof test (0x6B8FAC): the
    // copy of the other player's bike, pushed by his states into kerbs and cars, threw our player off while he rode on.
    // A rider that is never knocked off (CPed+0x474 bits 27-28, SET_CHAR_CAN_BE_KNOCKED_OFF_BIKE) stops it for both:
    // his game decides his crash (ApplyState). What a script had set on our player comes back once we are off.
    CVehicle* bike = me->InVehicle() ? me->Vehicle() : nullptr;
    NetVehicle* bnv = bike && bike->IsBike() && bike->Driver() != me ? Vehicles::Instance().Find(bike) : nullptr;
    bool keepOn = bnv && Vehicles::Instance().IsMirror(*bnv);
    uint32_t& knock = me->PedFlags3();
    if (keepOn && m_knockOffSaved < 0) {
        m_knockOffSaved = (int8_t)((knock >> 27) & 3);
        knock = (knock & ~(3u << 27)) | (1u << 27);
    } else if (!keepOn && m_knockOffSaved >= 0) {
        knock = (knock & ~(3u << 27)) | ((uint32_t)m_knockOffSaved << 27);
        m_knockOffSaved = -1;
    }
    // our player's fall (CTaskComplexDie's animation, +0x10 group, +0x14 id) for its copies: the default group's only
    if (me->Health() > 0.0f) {
        m_deathSent = false;
    } else if (!m_deathSent && me->Intelligence()) {
        void* die = CallMethod<0x681740, void*>((uint8_t*)me->Intelligence() + 4, 217);  // FindActiveTaskByType: COMPLEX_DIE
        if (die && Field<int32_t>(die, 0x10) == 0) {
            Writer w(proto::G_PLAYER_DEATH);
            w.I16((int16_t)Field<int32_t>(die, 0x14));
            Net().Send(proto::CH_RELIABLE, w);
            LOGI("Died with animation %d", Field<int32_t>(die, 0x14));
        }
        m_deathSent = die != nullptr;
    }
    DetectCarTasks();
    KeepDriversGoing();
    KeepOurLeader(me);
    FollowHost();
    if (me->InVehicle() && me->Vehicle() && me->Vehicle()->Driver() == me) m_lastCar = GetVehicleRef(me->Vehicle());
    if (me->InVehicle() && me->Vehicle() && !me->Vehicle()->IsTrain()) SendVehicleState(me->Vehicle());
    else SendLocalState();  // a train is not networked: the other game seats this player in its copy (RideTrain)
}

// Where the story moves the host by script, the partner keeps up: joining after a new game's prologue (in a new game of
// its own) it comes to the host, and when the host gets on a bike in a mission or just after one, the partner gets a
// bike too (the prologue's ride home, the bikes of "Big Smoke").
void Players::FollowHost() {
    RemotePlayer* host = Get(Net().HostId());
    CPlayerPed* me = FindPlayerPed(0);
    if (Net().IsHost() || Ref<uint8_t>(0xB5F851) || !host || !host->ped || !host->hasState || me->InVehicle() ||
        NowMs() - m_followAt < 1000)  // CCutsceneMgr::ms_running: the scene holds the partner meanwhile
        return;
    CPed* hp = host->ped;
    CVector hpos = hp->GetPosition(), mpos = me->GetPosition();
    if (flow::IsFollower() && (hpos - mpos).Length() > 25.0f) {
        float h = hp->CurrentRotation();
        CVector p = hpos + CVector{std::cos(h) * 1.5f, std::sin(h) * 1.5f, 0};  // at the host's right hand
        int handle = GetPedRef(me);
        Cmd(0x04BB).I(hp->AreaCode()).Run();                      // SET_AREA_VISIBLE
        Cmd(0x0860).I(handle).I(hp->AreaCode()).Run();            // SET_CHAR_AREA_VISIBLE
        Cmd(0x00A1).I(handle).F(p.x).F(p.y).F(p.z - 1.0f).Run();  // SET_CHAR_COORDINATES
        Cmd(0x0173).I(handle).F(h * 57.29578f).Run();             // SET_CHAR_HEADING
        LightArea(hp->AreaCode(), p);
        m_followAt = NowMs();
        LOGI("Joined after the prologue: next to the host");
        return;
    }
    // No seat left for us in the host's vehicle: a bike, a forklift, Ryder's Picador with Ryder in it, Sweet's Greenwood
    // full of the gang, Smoke's bike with the host behind him. The partner gets one of its own to follow.
    CVehicle* bike = hp->InVehicle() ? hp->Vehicle() : nullptr;
    int model = bike ? bike->ModelIndex() : 0, type = bike ? bike->VehicleSubClass() : -1;
    bool land = type == VEHICLE_TYPE_AUTOMOBILE || type == VEHICLE_TYPE_MTRUCK || type == VEHICLE_TYPE_QUAD || (bike && bike->IsBike());
    bool full = bike && bike->Driver();
    for (int i = 0; bike && i < bike->MaxPassengers() && i < 8; ++i) full = full && bike->Passengers()[i];
    if (!bike || bike == m_bikeFor || !land || !full) return;
    if ((!flow::IsFollower() && !HostMissionRecent(20000)) || (hpos - mpos).Length() > 60.0f) return;
    // Once the host rides off on it. Made while the mission still put him on it, ours stood by its own, the same model
    // ("OG Loc": two PCJs of the mission, and one more by them): the host got on ours, and OG Loc, who gets on behind CJ on
    // the mission's bike only, waited by it for good.
    if (bike->MoveSpeed().Length() * 50.0f < 4.0f) return;
    m_bikeFor = bike;
    m_followAt = NowMs();
    if (!IsModelLoaded(model)) {
        RequestModel(model);
        LoadAllRequestedModels(false);
    }
    float h = me->CurrentRotation();
    CVector p = mpos + CVector{std::cos(h) * 1.3f, std::sin(h) * 1.3f, -0.5f};  // at the partner's right hand
    int car = -1;
    if (!HasRoom(VehiclePool(), kKeepVehicles, "vehicle")) return;
    Cmd(0x00A5).I(model).F(p.x).F(p.y).F(p.z).Out(&car).Run();  // CREATE_CAR
    if (car < 0) return;
    Cmd(0x0175).I(car).F(h * 57.29578f).Run();  // SET_CAR_HEADING
    Cmd(0x01C3).I(car).Run();                   // MARK_CAR_AS_NO_LONGER_NEEDED: an ordinary vehicle from now on
    ui::TheChat().AddSystem(bike->IsBike() ? TR("Рядом с вами мотоцикл. Садитесь и езжайте за хостом.",
                                                "There is a bike next to you. Get on and follow the host.")
                                           : TR("Рядом с вами машина. Садитесь и езжайте за хостом.",
                                                "There is a vehicle next to you. Get in and follow the host."));
    LOGI("The host is in a vehicle with no seat for us (model %d): one for the partner too", model);
}

// the point over rp's head the name tag hangs from (back buffer coordinates) and its fade with distance
bool Players::TagAnchor(RemotePlayer& rp, float& sx, float& sy, float& alpha) {
    CPlayerPed* me = FindPlayerPed(0);
    if (!rp.ped || !me || rp.ped->AreaCode() != me->AreaCode() || rp.hiddenOnSpot) return false;
    CVector head = rp.ped->GetPosition() + CVector{0, 0, 0.95f};
    float dist = (head - cam::Source()).Length();
    if (dist > 70.0f) return false;
    float depth;
    if (!WorldToScreen(head, sx, sy, depth)) return false;
    auto& g = ui::Gfx();
    // game screen space -> back buffer
    sx *= g.Width() / (float)std::max(1, ScreenWidth());
    sy *= g.Height() / (float)std::max(1, ScreenHeight());
    alpha = dist > 50.0f ? 1.0f - (dist - 50.0f) / 20.0f : 1.0f;
    return true;
}

void Players::DrawWeaponIcons() {
    auto& g = ui::Gfx();
    float kx = ScreenWidth() / std::max(1.0f, g.Width()), ky = ScreenHeight() / std::max(1.0f, g.Height());
    for (auto& [id, rp] : m_remote) {
        float sx, sy, alpha;
        if (!rp.hasState || !TagAnchor(rp, sx, sy, alpha)) continue;
        // right of the health bar (64 x 6 at sy - 6, see DrawNameTag), as tall as the bars
        float s = g.Scale(), h = 26 * s, w = h * 47.0f / 58.0f;
        float x = sx + 32 * s + 4 * s, y = sy - 3 * s - h / 2;
        DrawWeaponIcon(rp.state.weapon, x * kx, y * ky, w * kx, h * ky, (uint8_t)(alpha * 255));
        DrawWantedStars(rp.state.wanted, (sx - 32 * s - 4 * s) * kx, (sy - 3 * s) * ky, (uint8_t)(alpha * 255));
    }
}

void Players::DrawNameTag(RemotePlayer& rp) {
    float sx, sy, alpha;
    if (!TagAnchor(rp, sx, sy, alpha)) return;
    auto& g = ui::Gfx();
    float s = g.Scale();
    std::string name = Net().PeerName(rp.id);
    uint32_t nameColor = PlayerColor(rp.id);
    if (NowMs() - rp.heardMs < 300)  // talking right now
        name = Format("{FFDC50}((•)){%06X} ", (unsigned)(nameColor & 0xFFFFFF)) + name;
    float lh = g.LineHeight(ui::FONT_SMALL);
    if (rp.state.flags & OnFootState::F_DOWNED) {  // wounded: how long it can still be revived
        std::string line = Format(TR("РАНЕН · %d с", "WOUNDED · %d s"), (int)rp.state.downSeconds);
        g.Text(ui::FONT_SMALL, sx, sy - 2 * lh - 10 * s, line, ui::WithAlpha(0xFFFF5050, alpha), ui::TEXT_CENTER | ui::TEXT_OUTLINE);
    }
    g.Text(ui::FONT_SMALL, sx, sy - lh - 8 * s, name, ui::WithAlpha(nameColor, alpha), ui::TEXT_CENTER | ui::TEXT_OUTLINE);
    float bw = 64 * s, bh = 6 * s;
    float bx = sx - bw / 2, by = sy - 6 * s;
    float hp = std::clamp(rp.ped->Health() / 100.0f, 0.0f, 1.0f);
    g.Rect(bx - 1, by - 1, bw + 2, bh + 2, ui::WithAlpha(0xFF000000, alpha));
    g.Rect(bx, by, bw, bh, ui::WithAlpha(0xFF501414, alpha));
    g.Rect(bx, by, bw * hp, bh, ui::WithAlpha(0xFFDC3232, alpha));
    if (rp.ped->Armour() > 0) {
        float ar = std::clamp(rp.ped->Armour() / 100.0f, 0.0f, 1.0f);
        by -= bh + 3 * s;
        g.Rect(bx - 1, by - 1, bw + 2, bh + 2, ui::WithAlpha(0xFF000000, alpha));
        g.Rect(bx, by, bw, bh, ui::WithAlpha(0xFF3C3C3C, alpha));
        g.Rect(bx, by, bw * ar, bh, ui::WithAlpha(0xFFE6E6E6, alpha));
    }
}

void Players::OnRender() {
    if (!Net().Online() || !Cfg().nameTags) return;
    for (auto& [id, rp] : m_remote) DrawNameTag(rp);
}

bool Players::OnCommand(const std::string& cmd, const std::vector<std::string>& args) {
    if (cmd == "sync") {  // what the sync looks like from here: for a report of something out of step
        auto& chat = ui::TheChat();
        if (!Net().Online() || !core::IsPlayerReady()) return true;
        CPlayerPed* me = FindPlayerPed(0);
        int ownPeds = 0, peds = 0, ownCars = 0, cars = 0;
        Peds::Instance().Count(ownPeds, peds);
        Vehicles::Instance().Count(ownCars, cars);
        chat.AddSystem(Format(TR("Синхронизация: пинг %u мс; люди — свои %d, чужих копий %d; машины — свои %d, чужих копий %d.",
                                 "Sync: ping %u ms; people — own %d, others' copies %d; vehicles — own %d, others' copies %d."),
                              Net().Rtt(), ownPeds, peds, ownCars, cars));
        for (auto& [id, rp] : m_remote) {
            if (!rp.hasState) continue;
            uint64_t age = NowMs() - rp.stateRecvMs;
            float dist = (rp.state.pos - me->GetPosition()).Length();
            chat.AddSystem(Format(TR("  %s: задержка %d мс, состояние %llu мс назад, %.0f м, интерьер %d, %s",
                                     "  %s: delay %d ms, state %llu ms ago, %.0f m, interior %d, %s"),
                                  Net().PeerName(id).c_str(), (int)(Net().LatencyFrom(id) * 1000.0f), (unsigned long long)age, dist,
                                  rp.state.area,
                                  rp.mode == RemotePlayer::Mode::OnFoot ? TR("пешком", "on foot")
                                  : rp.mode == RemotePlayer::Mode::Driver ? TR("за рулём", "driving")
                                                                          : TR("пассажир", "a passenger")));
        }
        LOGI("/sync: rtt %u, peds %d own %d copies, vehicles %d own %d copies", Net().Rtt(), ownPeds, peds, ownCars, cars);
        return true;
    }
    if (cmd != "tp" && cmd != "tphere") return false;
    if (!core::IsPlayerReady()) return true;
    auto& chat = ui::TheChat();
    std::vector<RemotePlayer*> targets;
    std::string who = args.empty() ? "" : args[0];
    auto inGame = [&] {
        std::string names;
        for (auto& [id, rp] : m_remote)
            if (rp.hasState) names += (names.empty() ? "" : ", ") + Net().PeerName(id);
        return names;
    };
    if (who.empty()) {  // the only other player in the game
        for (auto& [id, rp] : m_remote)
            if (rp.hasState) targets.push_back(&rp);
        if (targets.size() > 1) {
            chat.AddError(Format(TR("Укажите, кого: /%s <ник>. В игре: %s.", "Say whom: /%s <nick>. In the game: %s."),
                                 cmd.c_str(), inGame().c_str()));
            return true;
        }
    } else if (cmd == "tphere" && (ToLower(who) == "all" || who == "*")) {
        if (!Net().IsHost()) {
            chat.AddError(TR("Собрать всех к себе может только хост.", "Only the host can bring everyone over."));
            return true;
        }
        for (auto& [id, rp] : m_remote)
            if (rp.hasState) targets.push_back(&rp);
    } else {
        uint8_t id = Net().FindPeer(who);
        if (id == 255) {
            chat.AddError(Format(TR("Нет игрока «%s» (или под этот ник подходят несколько).",
                                    "There is no player \"%s\" (or several fit the name)."), who.c_str()));
            return true;
        }
        RemotePlayer* rp = Get(id);
        if (!rp || !rp->hasState) {
            chat.AddError(Format(TR("%s ещё не в игре.", "%s is not in the game yet."), Net().PeerName(id).c_str()));
            return true;
        }
        targets.push_back(rp);
    }
    if (targets.empty()) {
        chat.AddError(TR("Других игроков в игре пока нет.", "No other player is in the game yet."));
        return true;
    }
    CPlayerPed* me = FindPlayerPed(0);
    // into a building only through its door: one teleported in skipped the game's entry (the interior's people, its
    // light, the door), and the two games saw that interior differently
    if (cmd == "tp" && targets[0]->state.area != 0) {
        ui::TheChat().AddError(TR("Нельзя переместиться к игроку, пока он в здании.",
                                  "You cannot go over to a player while they are inside a building."));
        return true;
    }
    if (cmd == "tphere" && me->AreaCode() != 0) {
        ui::TheChat().AddError(TR("Нельзя позвать игроков к себе, пока вы в здании.",
                                  "You cannot bring players over while you are inside a building."));
        return true;
    }
    if (cmd == "tp") {
        RemotePlayer* target = targets[0];
        CVector p = target->state.pos + CVector{1.0f, 0, 0};
        int handle = GetPedRef(me);
        Cmd(0x04BB).I(target->state.area).Run();            // SET_AREA_VISIBLE
        Cmd(0x0860).I(handle).I(target->state.area).Run();  // SET_CHAR_AREA_VISIBLE
        CVector was = me->GetPosition();
        Cmd(0x00A1).I(handle).F(p.x).F(p.y).F(p.z - 1.0f).Run();  // SET_CHAR_COORDINATES
        LightArea(target->state.area, p);
        UseStreetDoor(target->state.area, p, target->streetPos.x != 0.0f ? target->streetPos : was);
        chat.AddSystem(Format(TR("Вы переместились к %s.", "You went over to %s."), Net().PeerName(target->id).c_str()));
        return true;
    }
    // around this player, a step apart from each other
    CVector c = me->GetPosition();
    float n = (float)targets.size(), radius = targets.size() > 1 ? 1.5f + 0.2f * n : 1.0f;
    for (size_t i = 0; i < targets.size(); ++i) {
        float a = me->CurrentRotation() + 6.2831853f * (float)i / n;
        CVector p = c + CVector{std::cos(a) * radius, std::sin(a) * radius, 0};
        Writer w(proto::G_TELEPORT);
        w.U8(targets[i]->id);
        w.F32(p.x);
        w.F32(p.y);
        w.F32(p.z - 1.0f);
        w.I8(me->AreaCode());
        Net().SendTo(targets[i]->id, proto::CH_RELIABLE, w);
    }
    if (targets.size() == 1)
        chat.AddSystem(Format(TR("%s перемещён к вам.", "%s was brought over to you."), Net().PeerName(targets[0]->id).c_str()));
    else
        chat.AddSystem(Format(TR("Игроки перемещены к вам: %d.", "Players brought over to you: %d."), (int)targets.size()));
    return true;
}

void UseStreetDoor(int area, const CVector& inside, const CVector& from) {
    if (area == 0) return;
    CPlayerPed* me = FindPlayerPed(0);
    void* door = StreetDoorOf(area, inside, from);
    if (!me || !door) return;
    CVector at = DoorSpot(door);
    Cmd(0x08AD).I(GetPedRef(me)).F(at.x).F(at.y).F(3.0f).Run();  // SET_CHAR_HAS_USED_ENTRY_EXIT
    // it is in there all the same, whatever area the command took from the door
    int set = me->AreaCode(), visible = Ref<int32_t>(0xB72914);  // CGame::currArea
    if (set != area) me->AreaCode() = (int8_t)area;
    if (visible != area) Cmd(0x04BB).I(area).Run();  // SET_AREA_VISIBLE
    LOGI("Into area %d as through the door at %.1f,%.1f (%.8s; the door set area %d, visible %d)", area, at.x, at.y,
         (const char*)door, set, visible);
}

}  // namespace coop
