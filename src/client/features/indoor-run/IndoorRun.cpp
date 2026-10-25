// Running in houses and other interiors: the game lets CJ only jog there (no sprint) for two reasons, the floor's
// surface (surfinfo.dat: carpets, a house's floors are "can't sprint on") and the player's sprint switched off
// (DISABLE_PLAYER_SPRINT, CPlayerPedData::m_bPlayerSprintDisabled). In an interior the surface never stops the sprint
// (SurfaceInfos_c::CantSprintOn answers no for this game's player there), and the switch is put back on (goggles,
// which switch it off themselves, are left alone).
#include "common/lib/util/Util.h"
#include "shared/game/Game.h"
#include "shared/game/Script.h"
#include "shared/lib/core/Core.h"
#include "shared/lib/memory/Memory.h"

namespace coop {

namespace {
using namespace game;
using game::script::Cmd;

bool Indoors() {
    CPlayerPed* me = core::IsPlayerReady() ? FindPlayerPed(0) : nullptr;
    return me && me->AreaCode() != 0 && !me->InVehicle();
}

using CantSprintFn = bool(__fastcall*)(void*, void*, uint32_t);
CantSprintFn g_orig = nullptr;
bool __fastcall HookCantSprintOn(void* infos, void* edx, uint32_t surface) {
    if (Indoors()) return false;
    return g_orig(infos, edx, surface);
}

class IndoorRunModule : public Module {
public:
    const char* Name() const override { return "IndoorRun"; }
    void OnInstallHooks() override { mem::HookFn(0x55E870, &HookCantSprintOn, &g_orig, "SurfaceInfos_c::CantSprintOn"); }
    void OnPostProcess() override {
        static uint64_t ms = 0;
        if (NowMs() - ms < 500 || !Indoors()) return;
        ms = NowMs();
        int w = FindPlayerPed(0)->ActiveWeapon().type;
        if (w == 44 || w == 45) return;  // night vision, thermal goggles: they keep it off themselves
        Cmd(0x06AF).I(0).I(0).Run();     // DISABLE_PLAYER_SPRINT off
    }
};

IndoorRunModule g_module;
struct Reg {
    Reg() { core::RegisterModule(&g_module); }
} g_reg;
}  // namespace

}  // namespace coop
