// The story's races raced as a team. A race runs in the host's game only (CPRACE: "High Stakes, Low-Rider" and the
// other street races): each racer, the host's CJ among them, goes on to its next checkpoint once LOCATE_CHAR_IN_CAR_3D
// finds it there, and the race is won when CJ's last one comes before the others'. The partners see the host's
// checkpoints and blips already (Missions), but they counted for nothing. Now CJ's checkpoint counts as reached when a
// partner drives through it too: the team goes on with whoever is ahead, and a partner first over the line wins the race
// for the host. When the race starts, each partner who is not driving gets a car of the host's model next to it.
#include <cctype>
#include <cmath>
#include <cstring>

#include "common/api/protocol/Protocol.h"
#include "common/lib/log/Log.h"
#include "common/lib/util/Util.h"
#include "entities/player/Players.h"
#include "shared/api/net-session/Session.h"
#include "shared/config/Config.h"
#include "shared/game/Game.h"
#include "shared/game/Script.h"
#include "shared/lib/core/Core.h"
#include "shared/lib/memory/Memory.h"
#include "shared/ui/chat/Chat.h"

namespace coop {

namespace {
using namespace game;
using game::script::Cmd;

enum : uint8_t { OP_START = 1, OP_OVER = 2 };
constexpr const char* kRaceScript = "cprace";
constexpr uint64_t kStartWaitMs = 15000;  // how long the start waits for the host to sit in its car

bool ScriptIs(void* s, const char* lowerName) {
    char name[9]{};
    memcpy(name, (const char*)s + 0x08, 8);  // CRunningScript::m_szName
    for (char& c : name) c = (char)std::tolower((unsigned char)c);
    return strcmp(name, lowerName) == 0;
}

bool RaceRunning() {
    for (void* s = Ref<void*>(0xA8B42C); s; s = Field<void*>(s, 0x00))  // CTheScripts::pActiveScripts
        if (ScriptIs(s, kRaceScript)) return true;
    return false;
}

// ---------------------------------------------------------------- host
enum class Host { Idle, Waiting, Started };
Host g_host = Host::Idle;
uint64_t g_raceSinceMs = 0;
CVector g_lastPoint{};  // the checkpoint a partner reached last (logged once each)

// A partner driving within the box CJ's checkpoint is looked for in; 255: none
uint8_t TeammateAt(const CVector& c, const CVector& r) {
    for (auto& [id, peer] : Net().Peers()) {
        RemotePlayer* rp = Players::Instance().Get(id);
        if (!rp || !rp->hasState || rp->mode != RemotePlayer::Mode::Driver || NowMs() - rp->stateRecvMs > 1000) continue;
        const CVector& p = rp->state.pos;
        if (std::fabs(p.x - c.x) <= r.x && std::fabs(p.y - c.y) <= r.y && std::fabs(p.z - c.z) <= r.z) return id;
    }
    return 255;
}

// CRunningScript::ProcessCommands200To299 (LOCATE_CHAR_IN_CAR_3D among them). The race asks whether CJ is in his car
// at his checkpoint; with a partner there, the answer is yes. The answer goes in again over what the command had
// left (CRunningScript::UpdateCompareFlag puts it together with the IF's other conditions).
using Commands200Fn = int8_t(__fastcall*)(void* script, void* edx, int command);
Commands200Fn g_origCommands200 = nullptr;
int8_t __fastcall HookCommands200(void* script, void* edx, int command) {
    if (command != 0x0100 || g_host == Host::Idle || !ScriptIs(script, kRaceScript)) return g_origCommands200(script, edx, command);
    uint8_t cond = Field<uint8_t>(script, 0xC5);    // m_bCondResult
    uint16_t logic = Field<uint16_t>(script, 0xD0); // m_nLogicalOp
    int8_t result = g_origCommands200(script, edx, command);
    const int32_t* param = (const int32_t*)0xA43C78;  // CTheScripts::ScriptParams: the command's, as it read them
    CPlayerPed* me = FindPlayerPed(0);
    if (!me || param[0] != GetPedRef(me)) return result;
    const float* f = (const float*)param;
    CVector at{f[1], f[2], f[3]}, box{f[4], f[5], f[6]};
    uint8_t who = TeammateAt(at, box);
    if (who == 255) return result;
    Field<uint8_t>(script, 0xC5) = cond;
    Field<uint16_t>(script, 0xD0) = logic;
    CallMethod<0x4859D0>(script, true);  // CRunningScript::UpdateCompareFlag
    if ((at - g_lastPoint).Length() > 1.0f) {
        g_lastPoint = at;
        LOGI("Race: player %d reached the checkpoint at %.0f, %.0f for the team", who, at.x, at.y);
    }
    return result;
}

void Send(uint8_t op, uint16_t value = 0) {
    Writer w(proto::G_RACE);
    w.U8(op);
    w.U16(value);
    Net().Send(proto::CH_RELIABLE, w);
}

void HostFrame() {
    bool running = RaceRunning();
    if (!running) {
        if (g_host == Host::Started) {
            Send(OP_OVER);
            LOGI("Race over");
        }
        g_host = Host::Idle;
        return;
    }
    if (g_host == Host::Idle) {
        g_host = Host::Waiting;
        g_raceSinceMs = NowMs();
        g_lastPoint = {};
    }
    if (g_host != Host::Waiting) return;
    // the race seats CJ in his car before its start: that car's model for the partners
    CPlayerPed* me = FindPlayerPed(0);
    CVehicle* car = me && me->InVehicle() ? me->Vehicle() : nullptr;
    if (!car && NowMs() - g_raceSinceMs < kStartWaitMs) return;
    g_host = Host::Started;
    int model = car ? car->ModelIndex() : 0;
    Send(OP_START, (uint16_t)model);
    ui::TheChat().AddSystem(TR("Командная гонка: контрольную точку берёт тот из вас, кто доедет первым.",
                               "Team race: a checkpoint counts for whichever of you drives through it first."));
    LOGI("Race started (the host's car: model %d)", model);
}

// ---------------------------------------------------------------- partner
// a car of the host's for this player, behind it (the racers stand side by side at the start)
void GiveCar(int model) {
    CPlayerPed* me = FindPlayerPed(0);
    if (!me || model <= 0 || !IsVehicleModel(model)) return;
    if (me->InVehicle() && me->Vehicle() && me->Vehicle()->Driver() == me) return;  // it drives one already
    if (!HasRoom(VehiclePool(), kKeepVehicles, "vehicle")) return;
    if (!IsModelLoaded(model)) {
        RequestModel(model);
        LoadAllRequestedModels(false);
    }
    CEntity* at = me->InVehicle() && me->Vehicle() ? (CEntity*)me->Vehicle() : (CEntity*)me;
    float h = at->GetHeading();
    CVector p = at->GetPosition() + CVector{std::sin(h) * 7.0f, -std::cos(h) * 7.0f, 0.5f};
    int car = -1;
    Cmd(0x00A5).I(model).F(p.x).F(p.y).F(p.z).Out(&car).Run();  // CREATE_CAR
    if (car < 0) return;
    Cmd(0x0175).I(car).F(h * 57.29578f).Run();  // SET_CAR_HEADING
    Cmd(0x01C3).I(car).Run();                   // MARK_CAR_AS_NO_LONGER_NEEDED: an ordinary car from now on
    ui::TheChat().AddSystem(TR("Позади вас машина для гонки. Садитесь: точки хоста считаются и за вас.",
                               "There is a car behind you for the race. Get in: the host's checkpoints count for you too."));
    LOGI("Race: a car (model %d) for this player", model);
}

void Handle(uint8_t from, Reader& r) {
    uint8_t op = r.U8();
    uint16_t value = r.U16();
    if (!r.Ok() || Net().IsHost() || from != Net().HostId()) return;
    if (op == OP_START) {
        LOGI("Race of the host's mission started");
        GiveCar(value);
    } else if (op == OP_OVER) {
        LOGI("Race of the host's mission over");
    }
}

class RaceModule : public Module {
public:
    const char* Name() const override { return "Race"; }
    void OnInstallHooks() override {
        // CRunningScript::CommandHandlerTable[2]: commands 200-299
        g_origCommands200 = (Commands200Fn)mem::SwapVTable((void**)0x8A6168, 2, (void*)&HookCommands200);
        Net().SetHandler(proto::G_RACE, &Handle);
    }
    void OnSessionEnd() override { g_host = Host::Idle; }
    void OnGameShutdown() override { g_host = Host::Idle; }
    void OnPostProcess() override {
        if (Net().Online() && Net().IsHost() && core::IsPlayerReady()) HostFrame();
    }
};

RaceModule g_module;
struct Reg {
    Reg() { core::RegisterModule(&g_module); }
} g_reg;
}  // namespace

}  // namespace coop
