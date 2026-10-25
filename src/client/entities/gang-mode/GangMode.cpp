#include "entities/gang-mode/GangMode.h"

#include <algorithm>
#include <cstring>
#include <map>

#include "common/api/protocol/Protocol.h"
#include "common/lib/log/Log.h"
#include "common/lib/util/Util.h"
#include "shared/api/net-session/Session.h"
#include "shared/config/Config.h"
#include "shared/lib/core/Core.h"

namespace coop::gangmode {

namespace {
using namespace game;

// The entity's own ops of G_GANG; the feature's start at 10
enum : uint8_t { OP_ROSTER = 1, OP_PICK = 2, OP_STATE = 3, OP_TERRITORIES = 4 };

const GangInfo kGangTable[] = {
    {TRP("Grove Street Families", "Grove Street Families"), TRP("Грув-стрит", "Grove Street"), 0xFF35B845, 0x18, 0xA8, 0x28, 8, 1,
     {105, 106, 107}, "SWEET", 290, {2495.30f, -1687.20f, 13.52f}, 4},
    {TRP("Баллас", "Ballas"), TRP("Баллас", "Ballas"), 0xFFB45AE0, 0xA0, 0x28, 0xC8, 7, 0, {102, 103, 104}, "SMOKE", 291,
     {1985.60f, -1178.30f, 25.80f}, 3},
    {TRP("Лос-Сантос Вагос", "Los Santos Vagos"), TRP("Вагос", "Vagos"), 0xFFF0D23C, 0xF0, 0xD0, 0x18, 9, 2, {108, 109, 110},
     "RYDER", 292, {2522.70f, -1068.60f, 69.60f}, 4},
    {TRP("Варриос Лос Ацтекас", "Varrios Los Aztecas"), TRP("Ацтекас", "Aztecas"), 0xFF32D2DC, 0x18, 0xC0, 0xD0, 14, 7,
     {114, 115, 116}, "CESAR", 293, {1820.40f, -2040.70f, 13.55f}, 4},
};

bool g_on = false, g_started = false;
Settings g_rules;
GangState g_state[kGangs];
std::map<uint8_t, uint8_t> g_picks;  // player -> gang
std::vector<uint16_t> g_territories;
uint64_t g_endAtMs = 0;    // host: WIN_TIME
uint32_t g_secondsLeft = 0;  // partner: as the host said
uint64_t g_secondsAtMs = 0;
Handler g_handler;

constexpr uintptr_t kZoneInfos = 0xBA1DF0;  // CTheZones::ZoneInfoArray[380], 0x11 bytes
constexpr uintptr_t kNavZones = 0xBA3798, kNavZoneCount = 0xBA3794;  // CTheZones::NavigationZoneArray, 0x20 bytes
uint8_t* ZoneInfo(int i) { return (uint8_t*)(kZoneInfos + (uintptr_t)i * 0x11); }
int NavCount() { return std::clamp<int>(Ref<int16_t>(kNavZoneCount), 0, 380); }
struct Box {
    float x1, y1, z1, x2, y2, z2;
    int info;
};
Box NavZone(int i) {
    uintptr_t z = kNavZones + (uintptr_t)i * 0x20;
    return {(float)Ref<int16_t>(z + 0x10), (float)Ref<int16_t>(z + 0x12), (float)Ref<int16_t>(z + 0x14), (float)Ref<int16_t>(z + 0x16),
            (float)Ref<int16_t>(z + 0x18), (float)Ref<int16_t>(z + 0x1A), Ref<int16_t>(z + 0x1C)};
}
bool IsTerritory(int info) { return std::find(g_territories.begin(), g_territories.end(), (uint16_t)info) != g_territories.end(); }

void SendTo(int to, const Writer& w) {
    if (!Net().Online()) return;
    if (to < 0) Net().Send(proto::CH_RELIABLE, w);
    else Net().SendTo((uint8_t)to, proto::CH_RELIABLE, w);
}

void WriteRoster(Writer& w) {
    w.U8(g_on);
    w.U8(g_rules.rule), w.U8(g_rules.count), w.U8(g_rules.minutes);
    for (uint8_t a : g_rules.ai) w.U8(a);
    w.U8((uint8_t)g_picks.size());
    for (auto& [id, gang] : g_picks) w.U8(id), w.U8(gang);
}

void Handle(uint8_t from, Reader& r) {
    uint8_t op = r.U8();
    if (!r.Ok()) return;
    bool fromHost = from == Net().HostId();
    if (op == OP_ROSTER) {
        if (!fromHost || Net().IsHost()) return;
        bool on = r.U8() != 0;
        Settings s;
        s.rule = r.U8(), s.count = r.U8(), s.minutes = r.U8();
        for (uint8_t& a : s.ai) a = r.U8();
        std::map<uint8_t, uint8_t> picks;
        uint8_t n = r.U8();
        for (int k = 0; k < n; ++k) {
            uint8_t id = r.U8(), gang = r.U8();
            if (gang < kGangs) picks[id] = gang;
        }
        if (!r.Ok()) return;
        if (on != g_on) LOGI("Gang war mode %s (the host's)", on ? "on" : "off");
        g_on = on, g_rules = s, g_picks = picks;
    } else if (op == OP_PICK) {
        uint8_t gang = r.U8();
        if (!r.Ok() || !Net().IsHost() || !g_on) return;
        HostAssign(from, gang);
    } else if (op == OP_STATE) {
        if (!fromHost || Net().IsHost()) return;
        bool started = r.U8() != 0;
        uint32_t left = r.U32();
        GangState st[kGangs];
        for (GangState& g : st) {
            g.bank = r.I32(), g.level = r.U8(), g.flags = r.U8(), g.capts = r.U8(), g.zones = r.U16(), g.sold = r.U16();
        }
        if (!r.Ok()) return;
        g_started = started, g_secondsLeft = left, g_secondsAtMs = NowMs();
        std::copy(std::begin(st), std::end(st), std::begin(g_state));
    } else if (op == OP_TERRITORIES) {
        if (!fromHost || Net().IsHost()) return;
        uint16_t n = r.U16();
        std::vector<uint16_t> zones;
        for (int k = 0; k < n && k < 380; ++k) zones.push_back(r.U16());
        if (r.Ok()) g_territories = zones;
    } else if (g_handler) {
        g_handler(from, op, r);
    }
}

class GangModeModule : public Module {
public:
    const char* Name() const override { return "GangMode"; }
    void OnInstallHooks() override { Net().SetHandler(proto::G_GANG, &Handle); }
    void OnSessionStart() override {
        g_picks.clear();
        g_started = false;
        g_territories.clear();
        for (GangState& s : g_state) s = GangState{};
        g_on = Net().IsHost() && Cfg().gameMode == 1;
        if (g_on) {
            LOGI("Gang war mode: this server plays the gang war of Los Santos");
            SendRoster();
        }
    }
    void OnSessionEnd() override {
        g_on = g_started = false;
        g_picks.clear();
        g_territories.clear();
    }
    void OnPeerJoin(uint8_t id) override {
        if (!Net().IsHost() || !g_on) return;
        SendRoster();
        if (g_started) {
            Writer w(proto::G_GANG);
            w.U8(OP_TERRITORIES);
            w.U16((uint16_t)g_territories.size());
            for (uint16_t z : g_territories) w.U16(z);
            SendTo(id, w);
            SendState();
        }
    }
    void OnPeerLeave(uint8_t id) override {
        if (g_picks.erase(id) && Net().IsHost() && g_on) SendRoster();
    }
};

GangModeModule g_module;
struct Reg {
    Reg() { core::RegisterModule(&g_module); }
} g_reg;
}  // namespace

const GangInfo& Info(int gang) { return kGangTable[std::clamp(gang, 0, (int)kGangs - 1)]; }
std::string Name(int gang) { return gang < kGangs ? Info(gang).name[Cfg().english ? 1 : 0] : std::string(); }
std::string ShortName(int gang) { return gang < kGangs ? Info(gang).shortName[Cfg().english ? 1 : 0] : std::string(); }

bool On() { return g_on && Net().Online(); }
bool Started() { return On() && g_started; }
Settings& Rules() { return g_rules; }
const GangState& State(int gang) { return g_state[std::clamp(gang, 0, (int)kGangs - 1)]; }
GangState& HostState(int gang) { return g_state[std::clamp(gang, 0, (int)kGangs - 1)]; }

uint32_t SecondsLeft() {
    if (Net().IsHost()) return g_endAtMs && NowMs() < g_endAtMs ? (uint32_t)((g_endAtMs - NowMs()) / 1000) : 0;
    uint64_t gone = (NowMs() - g_secondsAtMs) / 1000;
    return g_secondsLeft > gone ? (uint32_t)(g_secondsLeft - gone) : 0;
}
void HostStart(uint32_t seconds) {
    g_started = true;
    g_endAtMs = seconds ? NowMs() + (uint64_t)seconds * 1000 : 0;
    Writer w(proto::G_GANG);
    w.U8(OP_TERRITORIES);
    w.U16((uint16_t)g_territories.size());
    for (uint16_t z : g_territories) w.U16(z);
    SendTo(-1, w);
    SendState();
}
void HostStop() {
    g_started = false;
    g_endAtMs = 0;
    SendState();
}

uint8_t GangOf(uint8_t player) {
    auto it = g_picks.find(player);
    return it == g_picks.end() ? (uint8_t)NONE : it->second;
}
uint8_t MyGang() { return GangOf(Net().MyId()); }
bool Leader(uint8_t player) {
    uint8_t gang = GangOf(player);
    if (gang == NONE) return false;
    for (auto& [id, g] : g_picks)
        if (g == gang) return id == player;  // the map is ordered: the first one of the gang
    return false;
}
bool Enemies(uint8_t a, uint8_t b) {
    if (!On()) return false;
    uint8_t ga = GangOf(a), gb = GangOf(b);
    return ga != NONE && gb != NONE && ga != gb;
}
int PlayersOf(int gang) {
    int n = 0;
    for (auto& [id, g] : g_picks) n += g == gang;
    return n;
}
void Pick(uint8_t gang) {
    if (!On() || gang >= kGangs) return;
    if (Net().IsHost()) {
        HostAssign(Net().MyId(), gang);
        return;
    }
    g_picks[Net().MyId()] = gang;  // shown at once; the host's roster confirms it
    Writer w(proto::G_GANG);
    w.U8(OP_PICK);
    w.U8(gang);
    SendTo(Net().HostId(), w);
}
void HostAssign(uint8_t player, uint8_t gang) {
    if (gang >= kGangs) return;
    g_picks[player] = gang;
    LOGI("Gang war: player %d plays %s", player, Info(gang).name[1]);
    SendRoster();
}
void SendRoster() {
    if (!Net().IsHost()) return;
    Writer w(proto::G_GANG);
    w.U8(OP_ROSTER);
    WriteRoster(w);
    SendTo(-1, w);
}
void SendState() {
    if (!Net().IsHost() || !g_on) return;
    Writer w(proto::G_GANG);
    w.U8(OP_STATE);
    w.U8(g_started);
    w.U32(SecondsLeft());
    for (const GangState& g : g_state) {
        w.I32(g.bank), w.U8(g.level), w.U8(g.flags), w.U8(g.capts), w.U16(g.zones), w.U16(g.sold);
    }
    SendTo(-1, w);
}

const std::vector<uint16_t>& Territories() { return g_territories; }
void SetTerritories(const std::vector<uint16_t>& zones) { g_territories = zones; }

uint8_t OwnerOf(int zoneInfo) {
    if (zoneInfo < 0 || zoneInfo >= 380) return NONE;
    const uint8_t* z = ZoneInfo(zoneInfo);
    uint8_t best = NONE, bestD = 0;
    for (int g = 0; g < kGangs; ++g)
        if (z[Info(g).density] > bestD) bestD = z[Info(g).density], best = (uint8_t)g;
    return best;
}
void SetOwner(int zoneInfo, uint8_t gang) {
    if (zoneInfo < 0 || zoneInfo >= 380) return;
    uint8_t* z = ZoneInfo(zoneInfo);
    int strength = 0;
    for (int g = 0; g < kGangs; ++g) strength = std::max<int>(strength, z[Info(g).density]), z[Info(g).density] = 0;
    strength = std::clamp(strength, 20, 45);
    if (gang < kGangs) {
        z[Info(gang).density] = (uint8_t)strength;
        z[0x0B] = Info(gang).r, z[0x0C] = Info(gang).g, z[0x0D] = Info(gang).b, z[0x0E] = 0x78;  // CZoneInfo::ZoneColour
    } else {
        z[0x0B] = z[0x0C] = z[0x0D] = z[0x0E] = 0;
    }
}

int TerritoryAt(const CVector& at) {
    int best = -1;
    float bestArea = 1e30f;
    for (int i = 0, n = NavCount(); i < n; ++i) {
        Box b = NavZone(i);
        if (at.x < b.x1 || at.x > b.x2 || at.y < b.y1 || at.y > b.y2 || at.z < b.z1 - 5.0f || at.z > b.z2 + 5.0f || !IsTerritory(b.info)) continue;
        float area = (b.x2 - b.x1) * (b.y2 - b.y1);
        if (area < bestArea) bestArea = area, best = b.info;
    }
    return best;
}

CVector TerritoryMiddle(int zoneInfo) {
    CVector sum{};
    int n = 0;
    for (int i = 0, c = NavCount(); i < c; ++i) {
        Box b = NavZone(i);
        if (b.info != zoneInfo) continue;
        sum = sum + CVector{(b.x1 + b.x2) * 0.5f, (b.y1 + b.y2) * 0.5f, 20.0f};
        ++n;
    }
    return n ? sum * (1.0f / (float)n) : CVector{};
}

std::string TerritoryName(int zoneInfo) {
    for (int i = 0, n = NavCount(); i < n; ++i) {
        uintptr_t z = kNavZones + (uintptr_t)i * 0x20;
        if (Ref<int16_t>(z + 0x1C) != zoneInfo) continue;
        char label[9] = {};
        memcpy(label, (const char*)z + 8, 8);  // m_szTextKey
        const char* text = CallMethod<0x6A0050, const char*>((void*)0xC1B340, label);  // CText::Get
        std::string s;
        for (const char* c = text; c && *c; ++c)
            if (*c >= 32 && *c <= 126) s += *c;
        return s.empty() ? std::string(label) : s;
    }
    return std::string();
}

void SetHandler(Handler h) { g_handler = std::move(h); }
void Send(uint8_t op, const std::function<void(Writer&)>& body, int to) {
    Writer w(proto::G_GANG);
    w.U8(op);
    if (body) body(w);
    SendTo(to, w);
}

}  // namespace coop::gangmode
