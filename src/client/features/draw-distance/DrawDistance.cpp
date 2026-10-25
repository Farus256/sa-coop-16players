// Draw distance. The game's far clip and fog come from the time cycle (CTimeCycle::m_CurrentColours, set for the
// camera's place every frame by CTimeCycle::Update), and the range where models show in detail instead of their low
// LODs is the menu's draw distance (CRenderer::ms_lodDistScale, 0.925..1.8 on the slider). Both are stretched by the
// factors of coop.ini ([game] draw_distance, lod_distance; /draw in the chat changes them while playing).
//
// A longer view puts more entities in the frame, and the renderer's lists have fixed room: ms_aVisibleEntityPtrs[1000]
// and ms_aVisibleLodPtrs[1000] (CRenderer::AddEntityToRenderList checks nothing: past them lie the lists' counters), and
// the LOD render list in the 16 KB PC_Scratch (CRenderer::SetupBigBuildingVisibility writes it itself, unchecked; past
// the block lie the platform's globals). 0.3.1 stretched the view by the full factors everywhere and left out of the
// frame what did not fit. In a forest or over the city that was a part of the frame, chosen anew in every frame by the
// order the sectors were scanned: trees and buildings blinked as the camera turned. A LOD whose child was left out
// counted it as drawn, so neither the LOD nor the child showed (a hole in the map), and the LOD list overflowed into the
// globals after the scratch block: the game crashed. Now the stretch shrinks as soon as the lists fill up (to the game's
// own view when it must) and grows back slowly when there is room again, so nothing has to be left out; the guard
// stays as the last resort. The streaming memory is raised as far as the process's address space allows.
#include <windows.h>

#include <algorithm>
#include <cmath>
#include <cstdlib>

#include "common/lib/ini/Ini.h"
#include "common/lib/log/Log.h"
#include "common/lib/util/Util.h"
#include "shared/config/Config.h"
#include "shared/game/Game.h"
#include "shared/lib/core/Core.h"
#include "shared/lib/memory/Memory.h"
#include "shared/ui/chat/Chat.h"

namespace coop {

namespace {
using namespace game;

constexpr uintptr_t kFarClip = 0xB7C4F0;          // CTimeCycle::m_CurrentColours.m_fFarClip
constexpr uintptr_t kFogStart = 0xB7C4F4;         // CTimeCycle::m_CurrentColours.m_fFogStart
constexpr uintptr_t kLodDistScale = 0x8CD800;     // CRenderer::ms_lodDistScale
constexpr uintptr_t kMenuDrawDistance = addr::MenuManager + 0x40;  // CMenuManager::m_fDrawDistance
constexpr uintptr_t kVisibleLods = 0xB76840;      // CRenderer::ms_nNoOfVisibleLods
constexpr uintptr_t kVisibleEntities = 0xB76844;  // CRenderer::ms_nNoOfVisibleEntities
constexpr uintptr_t kLodRenderList = 0xB745D0;    // CRenderer::ms_pLodRenderList, entries of 8 bytes
constexpr uintptr_t kScratch = 0xC8E0C8;          // PC_Scratch, 16 KB
constexpr uintptr_t kLodRenderListBase = kScratch + 24;  // CRenderer::GetLodRenderListBase
constexpr uintptr_t kLodRenderListEnd = kScratch + 16384;
constexpr uintptr_t kStreamingMemory = 0x8A5A80;  // CStreaming::ms_memoryAvailable
constexpr int kVisibleMax = 1000;
// the game's own LOD list ends where its unused "don't render" list begins (+0x2000): kept to that
constexpr int kLodListRoom = (0x2000 - 24) / 8;
// The far plane at most this. The depth buffer's step grows with the square of the distance (24 bits from a near plane
// of 0.3 m: some 0.2 m at 1 km, nearly a metre at 2 km), and far off the road over the ground, a wall's paint over the
// wall and an LOD over its building shimmered as the camera moved: the game's own fog hides that from 1-1.5 km on.
constexpr float kMaxFarClip = 3000.0f;  // 0.4.0: farther (was 2400); the fog thickens over the last part
// The models' detail range at most this (the menu's slider ends at 1.8): beyond, the streamer cannot keep the HD models
// in, they swap with their LODs back and forth, and the two overlap while they cross-fade (their textures shimmer).
// 0.5.2: 2.0 (0.4.0's 2.2 kept the streamer swapping models with their LODs over the city at a view of 2 and more)
constexpr float kMaxLodScale = 2.0f;

// The stretch moves in steps of a tenth and seldom. 0.4.0's went down a little whenever the lists were 72% full and up
// a little whenever they were under 60%, every frame: at a view of 2 and more over the city the lists stood about
// there, the far plane and the models' range went back and forth all the time, and the distance flickered (things at
// its edge came and went, buildings swapped with their LODs). Now: a tenth back at once when the lists are nearly
// full (and nothing more for a few frames, till the frame shows it), a tenth out only after 3 s of room, never sooner
// than a while after the last step back (15 s, doubled with each step back soon after, up to 2 minutes), and the view
// grows into the new tenth smoothly over a second.
float g_adapt = 1.0f;          // the share of the configured stretch in use now (0: the game's own view)
float g_shown = 1.0f;          // ... as the frame has it (follows g_adapt: at once down, smoothly up)
uint32_t g_droppedFrame = 0;   // entities the guard left out since the last frame
uint32_t g_dropped = 0;        // ... since the last log line
float g_minAdapt = 1.0f;       // the lowest share since the last log line
int g_settle = 0;              // frames to wait after a step back
uint64_t g_holdUntilMs = 0, g_roomSinceMs = 0, g_lastBackMs = 0;
uint64_t g_holdMs = 15000;

// How full the renderer's lists were in the frame drawn last (they keep their counts until the next one is built)
float LastFill() {
    float ents = (float)Ref<int32_t>(kVisibleEntities) / kVisibleMax;
    float lods = (float)Ref<int32_t>(kVisibleLods) / kVisibleMax;
    uintptr_t at = Ref<uintptr_t>(kLodRenderList);
    float lodList = at > kLodRenderListBase && at <= kLodRenderListEnd ? (float)((at - kLodRenderListBase) / 8) / kLodListRoom : 0.0f;
    return std::max({ents, lods, lodList});
}

void Adapt() {
    float fill = LastFill();
    uint64_t now = NowMs();
    if (g_settle > 0) --g_settle;
    if ((g_droppedFrame || fill > 0.85f) && (g_settle == 0 || g_droppedFrame) && g_adapt > 0.0f) {
        g_adapt = std::max(0.0f, std::round(g_adapt * 10.0f) / 10.0f - 0.1f);
        g_settle = 6;
        g_holdMs = now - g_lastBackMs < 60000 ? std::min<uint64_t>(g_holdMs * 2, 120000) : 15000;
        g_holdUntilMs = now + g_holdMs;
        g_lastBackMs = now;
        g_roomSinceMs = 0;
    } else if (fill < 0.6f && g_adapt < 1.0f && now >= g_holdUntilMs) {
        if (!g_roomSinceMs) g_roomSinceMs = now;
        if (now - g_roomSinceMs > (g_lastBackMs ? 3000u : 800u)) {  // a world just loaded: out sooner till the first step back
            g_adapt = std::min(1.0f, std::round(g_adapt * 10.0f) / 10.0f + 0.1f);
            g_roomSinceMs = 0;
            g_holdUntilMs = now + 3000;
        }
    } else if (fill >= 0.6f) {
        g_roomSinceMs = 0;
    }
    g_shown = g_adapt < g_shown ? g_adapt : std::min(g_adapt, g_shown + 0.0035f);  // ~a second per tenth at 30 fps
    g_minAdapt = std::min(g_minAdapt, g_adapt);
    g_droppedFrame = 0;
}

using VoidFn = void(__cdecl*)();
VoidFn g_origTimeCycleUpdate = nullptr;
void __cdecl HookTimeCycleUpdate() {
    g_origTimeCycleUpdate();
    Adapt();
    const auto& cfg = Cfg();
    float k = 1.0f + (cfg.drawDistance - 1.0f) * g_shown;
    if (std::fabs(k - 1.0f) > 0.01f) {
        float& farClip = Ref<float>(kFarClip);  // ("far" is a macro of windows.h)
        float& fog = Ref<float>(kFogStart);
        if (farClip > 0.0f && farClip < 100000.0f) {
            float stretched = std::min(farClip * k, std::max(farClip, kMaxFarClip));
            // The fog starts where the game has it, a little farther at most, and thickens over the longer view: the
            // distance is seen, its shimmer (the depth buffer's) is under the fog. 0.3.1 moved the fog out with the
            // plane and showed it bare.
            fog = std::max(fog, std::min(fog * 1.25f, stretched * 0.6f));
            farClip = stretched;
        }
    }
    float menu = Ref<float>(kMenuDrawDistance);
    if (menu > 0.1f && menu < 10.0f) {
        float l = 1.0f + (cfg.lodDistance - 1.0f) * g_shown;
        Ref<float>(kLodDistScale) = std::min(menu * l, std::max(menu, kMaxLodScale));
    }
}

using AddToRenderListFn = void(__cdecl*)(CEntity*, float);
AddToRenderListFn g_origAddEntity = nullptr, g_origAddLod = nullptr;

void __cdecl HookAddEntityToRenderList(CEntity* entity, float distance) {
    // which of the two lists it would go to is decided inside (a LOD with children, else an entity): either full, it waits
    if (Ref<int32_t>(kVisibleEntities) >= kVisibleMax - 1 || Ref<int32_t>(kVisibleLods) >= kVisibleMax - 1) {
        ++g_droppedFrame, ++g_dropped;
        return;
    }
    g_origAddEntity(entity, distance);
}

void __cdecl HookAddToLodRenderList(CEntity* entity, float distance) {
    if (Ref<uintptr_t>(kLodRenderList) + 8 > kLodRenderListEnd) {
        ++g_droppedFrame, ++g_dropped;
        return;
    }
    g_origAddLod(entity, distance);
}

// The game is a 32-bit program: 2 GB of address space, 4 GB with the large-address flag (a patched gta_sa.exe). Models
// kept for a long view go into that space with everything else, and a build with HD textures ran out of it.
uint32_t StreamingMemoryCap() {
    auto* base = (uint8_t*)GetModuleHandleW(nullptr);
    auto* nt = (IMAGE_NT_HEADERS*)(base + ((IMAGE_DOS_HEADER*)base)->e_lfanew);
    bool laa = (nt->FileHeader.Characteristics & IMAGE_FILE_LARGE_ADDRESS_AWARE) != 0;
    uint32_t cap = laa ? 1024u : 160u;
    MEMORYSTATUSEX ms{};
    ms.dwLength = sizeof ms;
    // what is still free of the address space: a quarter of it at most goes to more models
    if (GlobalMemoryStatusEx(&ms)) cap = std::min<uint32_t>(cap, (uint32_t)(ms.ullAvailVirtual / 4 / (1024 * 1024)));
    return cap;
}

void RaiseStreamingMemory() {
    uint32_t& have = Ref<uint32_t>(kStreamingMemory);
    uint32_t wantMb = std::min<uint32_t>((uint32_t)Cfg().streamingMemoryMb, StreamingMemoryCap());
    uint32_t want = wantMb * 1024u * 1024u;
    if (want && have < want) {
        LOGI("Streaming memory %u MB -> %u MB (draw distance)", have >> 20, want >> 20);
        have = want;
    }
}

class DrawDistance : public Module {
public:
    const char* Name() const override { return "DrawDistance"; }
    void OnInstallHooks() override {
        mem::HookFn(0x561760, &HookTimeCycleUpdate, &g_origTimeCycleUpdate, "CTimeCycle::Update");
        mem::HookFn(0x5534B0, &HookAddEntityToRenderList, &g_origAddEntity, "CRenderer::AddEntityToRenderList");
        mem::HookFn(0x553710, &HookAddToLodRenderList, &g_origAddLod, "CRenderer::AddToLodRenderList");
        LOGI("Draw distance x%.2f, model detail x%.2f", Cfg().drawDistance, Cfg().lodDistance);
    }
    void OnGameStarted() override {
        g_adapt = g_shown = 0.0f;  // a world just loaded: its view grows as the lists show room
        g_holdUntilMs = g_roomSinceMs = g_lastBackMs = 0, g_holdMs = 15000, g_settle = 0;
        RaiseStreamingMemory();
    }
    void OnPreProcess() override {
        static uint64_t last = 0;
        if (NowMs() - last < 5000) return;
        last = NowMs();
        RaiseStreamingMemory();  // CStreaming::Init and stream.ini set it on their own
        if (g_dropped || g_minAdapt < 0.99f)
            LOGD("Draw distance: stretch at %.0f%% (lowest %.0f%%), %u entities left out of full render lists in the last 5 s",
                 g_adapt * 100.0f, g_minAdapt * 100.0f, g_dropped);
        g_dropped = 0;
        g_minAdapt = g_adapt;
    }
    bool OnCommand(const std::string& cmd, const std::vector<std::string>& args) override {
        if (cmd != "draw") return false;
        auto& cfg = Cfg();
        if (args.empty()) {
            ui::TheChat().AddSystem(Format(TR("Дальность прорисовки: %.1f (детализация %.1f), сейчас используется %.0f%%. /draw 1.0–4.0",
                                              "Draw distance: %.1f (model detail %.1f), %.0f%% in use now. /draw 1.0-4.0"),
                                           cfg.drawDistance, cfg.lodDistance, g_adapt * 100.0f));
            return true;
        }
        float v = (float)std::atof(args[0].c_str());
        if (v < 0.5f || v > 4.0f) {
            ui::TheChat().AddError(TR("Укажите число от 0.5 до 4.0, например /draw 2.5", "Give a number from 0.5 to 4.0, e.g. /draw 2.5"));
            return true;
        }
        cfg.drawDistance = v;
        if (args.size() > 1) {
            float l = (float)std::atof(args[1].c_str());
            if (l >= 0.5f && l <= 3.0f) cfg.lodDistance = l;
        }
        Ini ini;
        ini.Load(cfg.iniPath);
        ini.Set("game", "draw_distance", Format("%.1f", cfg.drawDistance));
        ini.Set("game", "lod_distance", Format("%.2f", cfg.lodDistance));
        ini.Save(cfg.iniPath);
        ui::TheChat().AddSystem(Format(TR("Дальность прорисовки: %.1f (детализация %.1f).", "Draw distance: %.1f (model detail %.1f)."),
                                       cfg.drawDistance, cfg.lodDistance));
        return true;
    }
};

DrawDistance g_drawDistance;
struct Reg {
    Reg() { core::RegisterModule(&g_drawDistance); }
} g_reg;
}  // namespace

}  // namespace coop
