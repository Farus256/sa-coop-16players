// The host's rhythm contest played by another player. The contest runs in the host's game only: its script (BDISPLY)
// draws the arrows coming in time with the music, and LOWGAME reads the right stick of pad 0
// (GET_POSITION_OF_ANALOGUE_STICKS) and compares its direction and time with the beat's (GET_BEAT_PROXIMITY). When the
// contest starts, every partner is told it can take it with Y; the first one plays it. From then on, the host's pad 0
// is that player's pad (right after CPad::UpdatePads in CGame::Process, before the scripts read it), and that player's
// game draws what the host's script draws (its sprites and rectangles, with the same sprites loaded). Nobody within
// 10 s: the host plays it as before. The mission, its score and its result stay the host's.
#include "features/minigame/Minigame.h"

#include <windows.h>

#include <algorithm>
#include <cctype>
#include <cstring>
#include <string>
#include <vector>

#include "common/api/protocol/Protocol.h"
#include "common/lib/log/Log.h"
#include "common/lib/util/Util.h"
#include "entities/mission/Mission.h"
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

enum : uint8_t { OP_OFFER = 1, OP_CLAIM = 2, OP_WHO = 3, OP_OVER = 4, OP_SPRITES = 5, OP_FRAME = 6 };
constexpr uint8_t kHost = 255;            // OP_WHO: the host plays it
constexpr uint64_t kOfferMs = 10000;      // how long the contest waits for a player to take it
constexpr uint64_t kFrameEveryMs = 33;    // the drawing goes 30 times a second
constexpr const char* kContestScript = "bdisply";  // the beat display of the lowrider contest and the dances
constexpr const char* kContestTxd = "ld_beat";      // its sprites' dictionary (LOAD_TEXTURE_DICTIONARY in BDISPLY)

// CTheScripts::IntroRectangles: what scripts draw this frame (CTheScripts::Process empties it, CHud::DrawScriptText
// draws it). 128 of 0x3C bytes: +0 type (3 a rectangle, 4 a sprite, 5 a turned sprite; 1 and 2 are text windows),
// +4 before the fade, +6 sprite (CTheScripts::ScriptSprites), +8 left, +C bottom, +10 right, +14 top in screen pixels,
// +18 angle, +1C colour
constexpr uintptr_t kRects = 0xA92D68, kRectCount = 0xA44B5C;  // ..., CTheScripts::NumberOfIntroRectanglesThisFrame
constexpr int kRectSize = 0x3C, kMaxRects = 128, kMaxSent = 40;
constexpr uintptr_t kSprites = 0xA94B68;  // CTheScripts::ScriptSprites: CSprite2d (RwTexture*) each; LOAD_SPRITE n is [n-1]
constexpr int kMaxSprites = 128;
// CTheScripts::UseTextCommands: CTheScripts::Process empties the list only while it is set (1: this once, 2: on). A
// script's drawing sets it; nothing here draws during the contest, so the arrows of every frame stayed in the list and
// the partner saw a smear of them until the 128 places were full.
constexpr uintptr_t kUseTextCommands = 0xA44B67;

struct Rect {
    uint8_t type = 0, beforeFade = 0;
    int16_t sprite = 0;
    float corner[4]{};  // left, bottom, right, top as fractions of the screen
    float angle = 0;
    uint32_t colour = 0;
};

// ---------------------------------------------------------------- host
enum class Host { Idle, Offered, Given };
Host g_host = Host::Idle;
uint8_t g_player = kHost;  // who plays it
uint64_t g_offerMs = 0, g_frameMs = 0;

using UpdatePadsFn = void(__cdecl*)();
UpdatePadsFn g_origUpdatePads = nullptr;

bool ScriptIs(void* s, const char* lowerName) {
    char name[9]{};
    memcpy(name, (const char*)s + 0x08, 8);  // CRunningScript::m_szName
    for (char& c : name) c = (char)std::tolower((unsigned char)c);
    return strcmp(name, lowerName) == 0;
}

bool ContestRunning() {
    for (void* s = Ref<void*>(0xA8B42C); s; s = Field<void*>(s, 0x00))  // CTheScripts::pActiveScripts
        if (ScriptIs(s, kContestScript)) return true;
    return false;
}

bool Delegated() { return g_host == Host::Given && g_player != kHost; }

// The judging stays in the host's game: LOWGAME takes the first frame the stick points somewhere within 200 ms of a beat
// and scores how far that beat is (GET_BEAT_PROXIMITY: within 35 ms the best, 100 a weak hit, 200 off time). The player
// sees the arrows a network trip late and its stick comes back another trip later, so each push came here that long
// after its beat (the first test: off time or missed throughout). While another player plays, the judging scripts get
// the beats as they were that long ago; BDISPLY (the arrows that player sees) keeps the beats as they are.
uint32_t g_judgeShiftMs = 0;

uint32_t JudgeDelayMs() {
    const PeerInfo* p = Net().Peer(g_player);
    uint32_t trip = Net().Rtt() + (p ? p->ping : Net().Rtt());  // host -> player (the frame) -> host (its stick)
    return std::min<uint32_t>(trip + (uint32_t)kFrameEveryMs, 400);  // + the wait for the next frame and stick sent
}

using Commands1900Fn = int8_t(__fastcall*)(void* script, void* edx, int command);
Commands1900Fn g_origCommands1900 = nullptr;
int8_t __fastcall HookCommands1900(void* script, void* edx, int command) {  // CRunningScript::ProcessCommands1900To1999
    if (command != 0x07B1 || !Delegated() || ScriptIs(script, kContestScript)) return g_origCommands1900(script, edx, command);
    g_judgeShiftMs = JudgeDelayMs();  // GET_BEAT_PROXIMITY of a judging script
    int8_t result = g_origCommands1900(script, edx, command);
    g_judgeShiftMs = 0;
    return result;
}

// the beat track's play time the beat info is worked out from (AEAudioHardware's GetBeatInfo, from its stream thread)
using PlayTimeFn = int(__fastcall*)(void* thread, void* edx);
PlayTimeFn g_origPlayTime = nullptr;
int __fastcall HookBeatPlayTime(void* thread, void* edx) {
    int ms = g_origPlayTime(thread, edx);
    return ms < 0 || !g_judgeShiftMs ? ms : std::max(0, ms - (int)g_judgeShiftMs);
}

void ContestKeys();

// right after the game read the pads: the host's pad 0 is the player's who plays the contest
void __cdecl HookUpdatePads() {
    g_origUpdatePads();
    ContestKeys();
    if (!Delegated() || !Net().Online()) return;
    RemotePlayer* rp = Players::Instance().Get(g_player);
    if (!rp || !rp->hasState || NowMs() - rp->stateRecvMs > 1000) return;
    rp->state.pad.To(GetPad(0)->NewState);
}

void Send(uint8_t op, uint8_t value = 0) {
    Writer w(proto::G_MINIGAME);
    w.U8(op);
    w.U8(value);
    Net().Send(proto::CH_RELIABLE, w);
}

void SendSprites(uint8_t to) {
    Writer w(proto::G_MINIGAME);
    w.U8(OP_SPRITES);
    std::vector<std::pair<uint8_t, std::string>> sprites;
    for (int i = 0; i < kMaxSprites; ++i)
        if (void* tex = Ref<void*>(kSprites + (uintptr_t)i * 4))
            sprites.push_back({(uint8_t)(i + 1), std::string((const char*)tex + 0x10, strnlen((const char*)tex + 0x10, 32))});  // RwTexture::name
    w.U8((uint8_t)sprites.size());
    for (auto& [slot, name] : sprites) {
        w.U8(slot);
        w.Str(name, 32);
    }
    Net().SendTo(to, proto::CH_RELIABLE, w);
    LOGI("Rhythm contest: %zu sprites to player %d", sprites.size(), to);
}

void Give(uint8_t player) {
    g_host = Host::Given;
    g_player = player;
    Send(OP_WHO, player);
    if (player == kHost) {
        ui::TheChat().AddSystem(TR("Никто не взял конкурс: играете вы.", "Nobody took the contest: you play it."));
        return;
    }
    SendSprites(player);
    ui::TheChat().AddSystem(Format(TR("Конкурс играет %s. Ваш пад сейчас его.", "%s plays the contest. Your pad is theirs for now."),
                                   Net().PeerName(player).c_str()));
    LOGI("Rhythm contest: player %d plays it (judged %u ms back)", player, JudgeDelayMs());
}

void SendFrame() {
    Writer w(proto::G_MINIGAME);
    w.U8(OP_FRAME);
    float sw = (float)std::max(1, ScreenWidth()), sh = (float)std::max(1, ScreenHeight());
    std::vector<uintptr_t> rects;
    for (int i = 0; i < kMaxRects && (int)rects.size() < kMaxSent; ++i) {
        uintptr_t r = kRects + (uintptr_t)i * kRectSize;
        int type = Ref<int32_t>(r);
        if (type >= 3 && type <= 5) rects.push_back(r);  // text windows stay: their text is the host's game's
    }
    w.U8((uint8_t)rects.size());
    for (uintptr_t r : rects) {
        w.U8((uint8_t)Ref<int32_t>(r));
        w.U8(Ref<uint8_t>(r + 0x04));
        w.I16(Ref<int16_t>(r + 0x06));
        w.F32(Ref<float>(r + 0x08) / sw);
        w.F32(Ref<float>(r + 0x0C) / sh);
        w.F32(Ref<float>(r + 0x10) / sw);
        w.F32(Ref<float>(r + 0x14) / sh);
        w.F32(Ref<float>(r + 0x18));
        w.U32(Ref<uint32_t>(r + 0x1C));
    }
    Net().SendTo(g_player, proto::CH_STATE, w);
}

void HostFrame() {
    bool running = ContestRunning();
    uint64_t now = NowMs();
    if (!running) {
        if (g_host != Host::Idle) {
            Send(OP_OVER);
            LOGI("Rhythm contest over");
        }
        g_host = Host::Idle;
        g_player = kHost;
        return;
    }
    if (g_host == Host::Idle) {
        g_host = Host::Offered;
        g_offerMs = now;
        Send(OP_OFFER);
        ui::TheChat().AddSystem(TR("Конкурс начался. Другой игрок может взять его клавишей Y (10 с).",
                                   "The contest started. Another player can take it with Y (10 s)."));
        LOGI("Rhythm contest offered");
    } else if (g_host == Host::Offered && now - g_offerMs > kOfferMs) {
        Give(kHost);
    } else if (g_host == Host::Given && g_player != kHost && now - g_frameMs >= kFrameEveryMs) {
        g_frameMs = now;
        SendFrame();
    }
}

// ---------------------------------------------------------------- partner
bool g_offered = false, g_playing = false, g_txdLoaded = false;
std::vector<Rect> g_rects;
uint64_t g_rectsMs = 0;

// The partner's numpad as the right stick. The game makes it the right stick only for a car's driver (the hydraulics):
// a passenger's pushed nothing, and the host's game saw no push in the whole contest (all "late"). Read here as the
// game reads it, with Num Lock on or off; a gamepad's stick stays as it is.
void ContestKeys() {
    if (!g_playing || !IsForeground() || ui::TheChat().IsOpen() || MenuActive()) return;
    auto down = [](int a, int b) { return (GetAsyncKeyState(a) & 0x8000) || (GetAsyncKeyState(b) & 0x8000); };
    int x = 0, y = 0;
    if (down(VK_NUMPAD4, VK_LEFT) || down(VK_NUMPAD7, VK_HOME) || down(VK_NUMPAD1, VK_END)) x -= 128;
    if (down(VK_NUMPAD6, VK_RIGHT) || down(VK_NUMPAD9, VK_PRIOR) || down(VK_NUMPAD3, VK_NEXT)) x += 128;
    if (down(VK_NUMPAD8, VK_UP) || down(VK_NUMPAD7, VK_HOME) || down(VK_NUMPAD9, VK_PRIOR)) y -= 128;
    if (down(VK_NUMPAD2, VK_DOWN) || down(VK_NUMPAD1, VK_END) || down(VK_NUMPAD3, VK_NEXT)) y += 128;
    if (!x && !y) return;
    CControllerState& s = GetPad(0)->NewState;
    s.RightStickX = (int16_t)x;
    s.RightStickY = (int16_t)y;
}

void StopPlaying() {
    if (g_txdLoaded) Cmd(0x0391).Run();  // REMOVE_TEXTURE_DICTIONARY: the sprites go with it
    g_txdLoaded = g_playing = false;
    g_rects.clear();
    if (!Ref<uint8_t>(kUseTextCommands)) Ref<uint8_t>(kUseTextCommands) = 1;  // what was drawn last goes next frame
}

void LoadSprites(Reader& r) {
    uint8_t n = r.U8();
    Cmd(0x0390).S8(kContestTxd).Run();  // LOAD_TEXTURE_DICTIONARY
    g_txdLoaded = true;
    for (uint8_t i = 0; i < n && r.Ok(); ++i) {
        uint8_t slot = r.U8();
        std::string name = r.Str(32);
        if (r.Ok() && slot >= 1 && slot <= kMaxSprites && !name.empty()) Cmd(0x038F).I(slot).S(name.c_str()).Run();  // LOAD_SPRITE
    }
    LOGI("Rhythm contest: %d sprites loaded", n);
}

void ReadFrame(Reader& r) {
    uint8_t n = r.U8();
    std::vector<Rect> rects;
    for (uint8_t i = 0; i < n && i < kMaxSent; ++i) {
        Rect x;
        x.type = r.U8();
        x.beforeFade = r.U8();
        x.sprite = r.I16();
        for (float& c : x.corner) c = r.F32();
        x.angle = r.F32();
        x.colour = r.U32();
        rects.push_back(x);
    }
    if (!r.Ok()) return;
    g_rects = std::move(rects);
    g_rectsMs = NowMs();
}

// after this game's scripts filled this frame's list: the host's contest's drawing goes in after it
void DrawFrame() {
    if (g_rects.empty() || NowMs() - g_rectsMs > 500) return;
    if (!Ref<uint8_t>(kUseTextCommands)) Ref<uint8_t>(kUseTextCommands) = 1;  // this frame's goes next frame
    uint16_t& count = Ref<uint16_t>(kRectCount);
    float sw = (float)ScreenWidth(), sh = (float)ScreenHeight();
    for (const Rect& x : g_rects) {
        if (count >= kMaxRects) break;
        if (x.sprite >= kMaxSprites || (x.type != 3 && (x.sprite < 0 || !Ref<void*>(kSprites + (uintptr_t)x.sprite * 4)))) continue;
        uintptr_t r = kRects + (uintptr_t)count * kRectSize;
        memset((void*)r, 0, kRectSize);
        Ref<int32_t>(r) = x.type;
        Ref<uint8_t>(r + 0x04) = x.beforeFade;
        Ref<int16_t>(r + 0x06) = x.sprite;
        Ref<float>(r + 0x08) = x.corner[0] * sw;
        Ref<float>(r + 0x0C) = x.corner[1] * sh;
        Ref<float>(r + 0x10) = x.corner[2] * sw;
        Ref<float>(r + 0x14) = x.corner[3] * sh;
        Ref<float>(r + 0x18) = x.angle;
        Ref<uint32_t>(r + 0x1C) = x.colour;
        ++count;
    }
}

void Handle(uint8_t from, Reader& r) {
    uint8_t op = r.U8();
    if (!r.Ok()) return;
    auto& chat = ui::TheChat();
    if (Net().IsHost()) {
        if (op == OP_CLAIM && g_host == Host::Offered) Give(from);
        return;
    }
    if (from != Net().HostId()) return;
    if (op == OP_OFFER) {
        g_offered = true;
        chat.AddSystem(TR("Конкурс в миссии хоста: нажмите Y в течение 10 с, чтобы играть его самому.",
                          "The contest of the host's mission: press Y within 10 s to play it yourself."));
    } else if (op == OP_WHO) {
        uint8_t who = r.U8();
        g_offered = false;
        if (!r.Ok()) return;
        if (who == Net().MyId()) {
            g_playing = true;
            chat.AddSystem(TR("Конкурс ваш: правый стик (на клавиатуре — цифровой блок) в сторону стрелок, в такт.",
                              "The contest is yours: the right stick (the numpad on a keyboard) the way the arrows show, in time."));
            LOGI("Rhythm contest: this player plays it");
        } else {
            if (g_playing) StopPlaying();
            if (who != kHost)
                chat.AddSystem(Format(TR("Конкурс играет %s.", "%s plays the contest."), Net().PeerName(who).c_str()));
        }
    } else if (op == OP_OVER) {
        g_offered = false;
        if (g_playing) LOGI("Rhythm contest over");
        StopPlaying();
    } else if (op == OP_SPRITES && g_playing) {
        LoadSprites(r);
    } else if (op == OP_FRAME && g_playing) {
        ReadFrame(r);
    }
}

void Reset() {
    if (g_playing) StopPlaying();
    g_offered = g_playing = false;
    g_host = Host::Idle;
    g_player = kHost;
}

class MinigameModule : public Module {
public:
    const char* Name() const override { return "Minigame"; }
    void OnInstallHooks() override {
        mem::RedirectCallFn(0x53BEE6, &HookUpdatePads, &g_origUpdatePads);  // CGame::Process -> CPad::UpdatePads
        // CRunningScript::CommandHandlerTable[19]: commands 1900-1999 (GET_BEAT_PROXIMITY among them)
        g_origCommands1900 = (Commands1900Fn)mem::SwapVTable((void**)0x8A6168, 19, (void*)&HookCommands1900);
        mem::RedirectCallFn(0x4D8FEF, &HookBeatPlayTime, &g_origPlayTime);  // GetBeatInfo -> the track's play time
        Net().SetHandler(proto::G_MINIGAME, &Handle);
    }
    void OnSessionEnd() override { Reset(); }
    void OnGameShutdown() override { Reset(); }
    void OnPeerLeave(uint8_t id) override {
        if (Net().IsHost() && g_host == Host::Given && g_player == id) {  // it left mid-contest: the host plays on
            g_player = kHost;
            Send(OP_WHO, kHost);
            ui::TheChat().AddSystem(TR("Игрок, игравший конкурс, вышел: играете вы.", "The player of the contest left: you play it."));
        }
    }
    void OnPostProcess() override {
        if (!Net().Online() || !core::IsPlayerReady()) return;
        if (Net().IsHost()) HostFrame();
        else if (g_playing) DrawFrame();
    }
};

MinigameModule g_module;
struct Reg {
    Reg() { core::RegisterModule(&g_module); }
} g_reg;
}  // namespace

bool PlayingHostMinigame() { return g_playing; }

namespace minigame {

bool KeyY() {
    if (!g_offered || Net().IsHost() || !Net().Online()) return false;
    g_offered = false;
    Writer w(proto::G_MINIGAME);
    w.U8(OP_CLAIM);
    w.U8(0);
    Net().SendTo(Net().HostId(), proto::CH_RELIABLE, w);
    ui::TheChat().AddSystem(TR("Вы попросили конкурс у хоста...", "You asked the host for the contest..."));
    return true;
}

}  // namespace minigame

}  // namespace coop
