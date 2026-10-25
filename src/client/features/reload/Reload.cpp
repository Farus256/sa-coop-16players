// Reloading by hand: R with a gun whose clip is not full (and ammo left past it) starts the reload as the game's own one
// when the clip runs dry: the weapon's state "reloading" with the time it takes (CWeapon::Update fills the clip then,
// CWeapon::Reload), and the reload animation of the gun over the upper body (the player can walk and run meanwhile).
// The game's own reload goes on as before.
#include <windows.h>

#include <algorithm>

#include "common/lib/log/Log.h"
#include "common/lib/util/Util.h"
#include "shared/game/Game.h"
#include "shared/game/Script.h"
#include "shared/lib/core/Core.h"
#include "shared/ui/chat/Chat.h"

namespace coop {

namespace {
using namespace game;
using game::script::Cmd;

constexpr int WEAPONSTATE_READY = 0, WEAPONSTATE_FIRING = 1, WEAPONSTATE_RELOADING = 2;
constexpr int kReloadKey = 'R';

struct ReloadAnim {
    int weapon;
    const char* block;
    const char* anim;
    uint32_t ms;
};
// the guns with a clip, their reload's animation (the IFP the game loads for that gun) and how long it takes
const ReloadAnim kAnims[] = {
    {22, "COLT45", "colt45_reload", 1100}, {23, "SILENCED", "Silence_reload", 1100}, {24, "PYTHON", "python_reload", 1300},
    {26, nullptr, nullptr, 1300}, {27, "BUDDY", "buddy_reload", 1600},     {28, "UZI", "UZI_reload", 1300},
    {29, "UZI", "UZI_reload", 1400},       {30, "RIFLE", "RIFLE_load", 1700},       {31, "RIFLE", "RIFLE_load", 1700},
    {32, "TEC", "TEC_reload", 1300},
};

bool EnsureBlock(const char* block) {
    int idx = Call<0x4D3990, int>(block);  // CAnimManager::GetAnimationBlockIndex
    if (idx < 0) return false;
    uintptr_t b = 0xB5D4A0 + (uintptr_t)idx * 0x20;  // CAnimManager::ms_aAnimBlocks
    if (Ref<uint8_t>(b + 0x10)) return true;
    RequestModel(25575 + idx, 2);
    LoadAllRequestedModels(false);
    return Ref<uint8_t>(b + 0x10) != 0;
}

bool g_wasDown = false;

void TryReload() {
    CPlayerPed* me = FindPlayerPed(0);
    if (!me || me->Health() <= 0.0f || me->InVehicle() || GetPad(0)->DisablePlayerControls || cam::WideScreenOn()) return;
    CWeapon& w = me->ActiveWeapon();
    const ReloadAnim* a = nullptr;
    for (const ReloadAnim& r : kAnims)
        if (r.weapon == w.type) a = &r;
    if (!a || (w.state != WEAPONSTATE_READY && w.state != WEAPONSTATE_FIRING)) return;
    uint8_t skill = CallMethod<0x5E6580, uint8_t>(me);         // CPed::GetWeaponSkill
    void* info = Call<0x743C60, void*>(w.type, skill);        // CWeaponInfo::GetWeaponInfo
    int clip = info ? Field<int16_t>(info, 0x20) : 0;         // m_nAmmoClip
    if (clip <= 1 || w.ammoInClip >= clip || w.ammoTotal <= w.ammoInClip) return;  // full, or nothing to put in
    uint32_t ms = a->ms;
    if (PlayerInfos()[0]._padDD[0x14D - 0xDD]) ms /= 2;      // CPlayerInfo::m_bFastReload
    w.state = WEAPONSTATE_RELOADING;
    w.timeForNextShot = TimeMs() + ms;  // CWeapon::Update: the clip is filled once this time is up
    if (a->block && EnsureBlock(a->block))
        Cmd(0x0A1A).I(GetPedRef(me)).S(a->anim).S(a->block).F(4.0f).I(0).I(0).I(0).I(0).I((int)ms).Run();  // TASK_PLAY_ANIM_SECONDARY
    LOGD("Reload by hand: weapon %d, %d of %d in the clip", w.type, w.ammoInClip, clip);
}

class ReloadModule : public Module {
public:
    const char* Name() const override { return "Reload"; }
    void OnPostProcess() override {
        bool down = IsForeground() && (GetAsyncKeyState(kReloadKey) & 0x8000) != 0;
        bool pressed = down && !g_wasDown;
        g_wasDown = down;
        if (!pressed || !core::IsPlayerReady() || MenuActive() || ui::TheChat().IsOpen()) return;
        TryReload();
    }
};

ReloadModule g_module;
struct Reg {
    Reg() { core::RegisterModule(&g_module); }
} g_reg;
}  // namespace

}  // namespace coop
