// The attacks on territories ("capts"): up to two at once, each by another gang.
//
// The territory is the one under the buyer's map target (it need not stand there). The host keeps the attacks: who
// attacks what, for whom it is decided, the territory's owner after it. Each is fought in the game of a player near
// its territory, within 230 m (that game has the ground, the roads and the people there: a game far off had its cars
// and men stand frozen, waiting for the ground under them, and nothing happened): the host gives it to the game of the
// nearest player as one comes, and that game makes the cars and the men, fights it and says how it goes. When nobody
// comes within 2.5 minutes, or that game's player goes away (320 m for 25 s), or it is gone, the dice decide (by the
// men left on both sides, else by the gangs' weapons).
//
// The fight: the attackers' cars (each with its men) come from 150-220 m off, from the side of their base, and drive to
// a road in the territory; there their men get out and fight. The owners defend in two waves of men about the
// territory's middle. Taken: all the defenders down, someone of the attackers still there, and no player of another
// gang left alive in it (a player who stays must be killed); a free territory when held so for 20 s. Lost: nobody of
// the attackers there for 15 s (their players in it must be killed), or 7 minutes.
#include <algorithm>
#include <cmath>

#include "common/api/protocol/Protocol.h"
#include "common/lib/log/Log.h"
#include "common/lib/util/Util.h"
#include "entities/gang-mode/GangMode.h"
#include "entities/radar-icons/RadarIcons.h"
#include "entities/vehicle/Vehicles.h"
#include "features/gang-war/War.h"
#include "shared/api/net-session/Session.h"
#include "shared/game/Game.h"
#include "shared/game/Script.h"

namespace coop::gangwar::war {

namespace {
using namespace game;
using namespace gangmode;
using game::script::Cmd;

enum Outcome : uint8_t { GOING = 0, WON = 1, LOST = 2, LEFT = 3 };
constexpr float kRunnerRange = 230.0f;   // a player this near the territory: its game fights the attack
constexpr float kRunnerAway = 320.0f;    // ... and gives it up this far off
constexpr uint64_t kNobodyMs = 150000;   // nobody came: the dice
constexpr int kIconBase = 9810;  // 9810-9811 (9801 is the convoy's)

// ---------------------------------------------------------------- the host's
struct HostCapt {
    bool on = false;
    uint8_t att = NONE, def = NONE;
    int zone = -1;
    CVector mid;
    uint64_t startMs = 0, runMs = 0, reportMs = 0;
    uint8_t runner = NONE;  // whose game fights it
    uint8_t defLeft = 0, attLeft = 0;
    uint8_t captNo = 0;     // the gang's attacks bought before it
};
HostCapt g_hc[kMaxCapts];
CaptView g_view[kMaxCapts];
uint64_t g_viewSentMs = 0;

// ---------------------------------------------------------------- the fighting game's
struct Fight {
    bool on = false;
    uint8_t att = NONE, def = NONE;
    int zone = -1;
    CVector mid;
    uint64_t startMs = 0, arrivedMs = 0, emptySince = 0, holdSince = 0, farSince = 0, reportMs = 0;
    int wave = 0;  // defenders' waves made
    std::vector<int> cars;  // handles
    std::vector<CPed*> attackers, defenders;
    bool out = false;  // the attackers got out of their cars
    int attLeft = 0, defLeft = 0;
};
Fight g_fight[kMaxCapts];

void HostReport(uint8_t from, int slot, int zone, uint8_t defLeft, uint8_t attLeft, uint8_t outcome);

void Report(int slot, uint8_t outcome) {
    const Fight& f = g_fight[slot];
    uint8_t d = (uint8_t)std::clamp(f.defLeft, 0, 255), a = (uint8_t)std::clamp(f.attLeft, 0, 255);
    if (Net().IsHost()) return HostReport(Net().MyId(), slot, f.zone, d, a, outcome);
    int zone = f.zone;
    Send(OP_CAPT_REPORT, [&](Writer& w) {
        w.U8((uint8_t)slot), w.U16((uint16_t)zone), w.U8(d), w.U8(a), w.U8(outcome);
    }, Net().HostId());
}

void EndRun(int slot) {
    Fight& f = g_fight[slot];
    if (!f.on) return;
    for (CPed* p : f.attackers)
        if (IsPedValid(p)) Cmd(0x01C2).I(GetPedRef(p)).Run();  // MARK_CHAR_AS_NO_LONGER_NEEDED
    for (CPed* p : f.defenders)
        if (IsPedValid(p)) Cmd(0x01C2).I(GetPedRef(p)).Run();
    for (int c : f.cars)
        if (GetVehicle(c)) Cmd(0x01C3).I(c).Run();  // MARK_CAR_AS_NO_LONGER_NEEDED
    LOGI("Gang war: our game's fight for zone %d over", f.zone);
    f = Fight{};
}

void SpawnDefenders(Fight& f, int count) {
    if (f.def >= kGangs) return;
    for (int k = 0; k < count; ++k) {
        float a = Rand(0.0f, 6.2831853f), d = Rand(5.0f, 30.0f);
        CVector at = WalkNear(f.mid + CVector{std::cos(a) * d, std::sin(a) * d, 0.0f});
        int h = MakeMan(f.def, at, -1, -2);
        if (CPed* p = h >= 0 ? GetPed(h) : nullptr) f.defenders.push_back(p);
    }
    ++f.wave;
}

bool StartRun(int slot, uint8_t att, uint8_t def, int zone, const CVector& mid, uint8_t captNo) {
    EndRun(slot);
    Fight& f = g_fight[slot];
    f.on = true, f.att = att, f.def = def, f.zone = zone, f.startMs = f.reportMs = NowMs();
    f.mid = Ground(RoadNear(Ground(mid)));
    const GangInfo& gi = Info(att);
    CVector from = gi.base - f.mid;  // the cars: from the side of the attackers' base
    from.z = 0;
    float len = from.Length();
    CVector dir = len > 1.0f ? from * (1.0f / len) : CVector{1, 0, 0};
    bool bikes = CarOf(att) == MODEL_FREEWAY;
    int cars = att == GROVE && captNo < 4 ? 3 : att == VAGOS ? 5 : bikes ? 6 : 4;
    int per = bikes ? 2 : 4;
    int model = CarOf(att);
    if (!LoadModelNow(model)) model = 567;  // a Savanna then
    if (!LoadModelNow(model)) return false;
    for (int k = 0; k < cars; ++k) {
        float side = (float)(k - cars / 2) * 0.12f;
        CVector d{dir.x * std::cos(side) - dir.y * std::sin(side), dir.x * std::sin(side) + dir.y * std::cos(side), 0.0f};
        CVector at = RoadNear(f.mid + d * Rand(150.0f, 220.0f));
        int car = -1;
        Cmd(0x00A5).I(model).F(at.x).F(at.y).F(at.z + 1.0f).Out(&car).Run();  // CREATE_CAR
        if (car < 0) continue;
        CVector to = f.mid - at;
        Cmd(0x0175).I(car).F(std::atan2(-to.x, to.y) * 57.29578f).Run();  // SET_CAR_HEADING
        CVehicle* made = GetVehicle(car);
        if (made) Vehicles::Instance().EnsureNetworked(made);
        for (int s = -1; s < per - 1 && made && s < (int)made->MaxPassengers(); ++s) {
            int h = MakeMan(att, at, car, s);
            if (CPed* p = h >= 0 ? GetPed(h) : nullptr) f.attackers.push_back(p);
        }
        Cmd(0x00AD).I(car).F(bikes ? 30.0f : 24.0f).Run();                      // SET_CAR_CRUISE_SPEED
        Cmd(0x00AE).I(car).I(2).Run();                                           // SET_CAR_DRIVING_STYLE: round the cars
        Cmd(0x00A7).I(car).F(f.mid.x).F(f.mid.y).F(f.mid.z).Run();               // CAR_GOTO_COORDINATES
        f.cars.push_back(car);
    }
    if (f.cars.empty()) return false;
    SpawnDefenders(f, def == VAGOS ? 8 : 6);
    f.attLeft = (int)f.attackers.size(), f.defLeft = (int)f.defenders.size();
    LOGI("Gang war: our game fights the attack of %s on zone %d (%s) with %d cars", gi.name[1], zone, TerritoryName(zone).c_str(),
         (int)f.cars.size());
    return true;
}

bool InZone(const Fight& f, const CVector& p) { return TerritoryAt(p) == f.zone || (p - f.mid).Length2D() < 45.0f; }

void UpdateFight(int slot) {
    Fight& f = g_fight[slot];
    uint64_t now = NowMs();
    auto finish = [&](uint8_t outcome) {
        Report(slot, outcome);
        EndRun(slot);
    };
    if (now - f.startMs > 7 * kMin) return finish(LOST);
    // our player gone away (dead and back at its base, driven off): the dice, by the men left
    CPlayerPed* me = FindPlayerPed(0);
    bool away = !me || me->AreaCode() != 0 || (me->GetPosition() - f.mid).Length2D() > kRunnerAway;
    if (away) {
        if (!f.farSince) f.farSince = now;
        if (now - f.farSince > 25000) return finish(LEFT);
    } else {
        f.farSince = 0;
    }
    std::vector<PlayerPed> players = LivePlayers();
    uint8_t att = f.att, def = f.def;
    // the cars there (or long on their way): everyone out, into the fight
    if (!f.out) {
        bool there = now - f.startMs > 75000;
        for (int c : f.cars)
            if (CVehicle* v = GetVehicle(c); v && (v->GetPosition() - f.mid).Length2D() < 35.0f) there = true;
        if (there) {
            f.out = true;
            f.arrivedMs = now;
            for (CPed* p : f.attackers)
                if (Alive(p) && p->InVehicle()) Cmd(0x05CD).I(GetPedRef(p)).I(GetVehicleRef(p->Vehicle())).Run();  // TASK_LEAVE_CAR
        } else if (CarOf(att) == MODEL_FREEWAY) {  // the Aztecas shoot from their bikes on the way
            for (CPed* p : f.attackers) {
                if (!Alive(p) || !p->InVehicle()) continue;
                CPed* t = NearestOf(p->GetPosition(), 35.0f, [def](CPed* q) { return def < kGangs && OfGang(q, def); });
                if (t) Cmd(0x0713).I(GetPedRef(p)).I(GetPedRef(t)).I(-1).F(0.0f).F(0.0f).F(0.0f).F(40.0f).I(4).I(0).I(80).Run();  // TASK_DRIVE_BY
            }
        }
    }
    // who goes for whom: the nearest of the other side within 120 m (players among them); with none, to the middle.
    // The attackers go for every gang's man but their own in the territory: its owners' and other players alike.
    int attLeft = 0, defLeft = 0;
    for (CPed* p : f.attackers) {
        if (!Alive(p)) continue;
        ++attLeft;
        if (!f.out || p->InVehicle()) continue;
        CPed* t = NearestOf(p->GetPosition(), 120.0f, [&f, att, def](CPed* q) {
            if (std::find(f.defenders.begin(), f.defenders.end(), q) != f.defenders.end()) return true;
            uint8_t g = GangOfPed(q);
            return g < kGangs && g != att && (g == def || q->IsPlayer() || InZone(f, q->GetPosition()));
        });
        int h = GetPedRef(p);
        if (t) Cmd(0x05E2).I(h).I(GetPedRef(t)).Run();  // TASK_KILL_CHAR_ON_FOOT
        else if ((p->GetPosition() - f.mid).Length2D() > 8.0f)
            Cmd(0x05D3).I(h).F(f.mid.x + Rand(-5.0f, 5.0f)).F(f.mid.y + Rand(-5.0f, 5.0f)).F(f.mid.z).I(6).I(6000).Run();  // TASK_GO_STRAIGHT_TO_COORD
    }
    for (CPed* p : f.defenders) {
        if (!Alive(p)) continue;
        ++defLeft;
        CPed* t = NearestOf(p->GetPosition(), 120.0f, [att](CPed* q) { return OfGang(q, att); });
        if (t) Cmd(0x05E2).I(GetPedRef(p)).I(GetPedRef(t)).Run();
    }
    f.attLeft = attLeft, f.defLeft = defLeft;
    // the second wave when the first is mostly down
    if (def < kGangs && f.wave == 1 && f.out && defLeft <= 2) SpawnDefenders(f, def == VAGOS ? 8 : 6);
    bool attHere = false, enemyHere = false;
    for (CPed* p : f.attackers) attHere |= Alive(p) && f.out && InZone(f, p->GetPosition());
    for (const PlayerPed& pp : players) {
        if (!InZone(f, pp.ped->GetPosition()) || pp.gang >= kGangs) continue;
        if (pp.gang == att) attHere = true;  // an attacker player still standing there: to be killed for the defence
        else enemyHere = true;               // a player of another gang still standing there: to be killed to take it
    }
    if (!attHere && f.out) {
        if (!f.emptySince) f.emptySince = now;
        if (now - f.emptySince > 15000) return finish(LOST);
    } else {
        f.emptySince = 0;
    }
    bool defendersDone = def >= kGangs ? true : f.wave >= 2 && defLeft == 0;
    if (defendersDone && attHere && !enemyHere) {
        if (def < kGangs) return finish(WON);
        if (!f.holdSince) f.holdSince = now;
        if (now - f.holdSince > 20000) return finish(WON);
    } else {
        f.holdSince = 0;
    }
    if (now - f.reportMs > 2000) {
        f.reportMs = now;
        Report(slot, GOING);
    }
}

// ---------------------------------------------------------------- the host's, on
void SendViews() {
    g_viewSentMs = NowMs();
    for (int s = 0; s < kMaxCapts; ++s) {
        const HostCapt& c = g_hc[s];
        CaptView& v = g_view[s];
        v = CaptView{};
        if (c.on) {
            v.on = true, v.att = c.att, v.def = c.def, v.zone = c.zone, v.mid = c.mid, v.defLeft = c.defLeft, v.attLeft = c.attLeft;
            v.fighting = c.runner != NONE;
        }
        Send(OP_CAPT, [&](Writer& w) {
            w.U8((uint8_t)s), w.U8(v.on);
            if (!v.on) return;
            w.U8(v.att), w.U8(v.def), w.U16((uint16_t)v.zone), w.U8(v.defLeft), w.U8(v.attLeft), w.U8(v.fighting);
            w.Vec({v.mid.x, v.mid.y, v.mid.z});
        });
    }
}

void SendRun(int slot, uint8_t to, bool on) {
    const HostCapt& c = g_hc[slot];
    if (to == Net().MyId()) {
        if (!on) return EndRun(slot);
        if (!StartRun(slot, c.att, c.def, c.zone, c.mid, c.captNo)) {
            g_fight[slot] = Fight{};
            HostReport(Net().MyId(), slot, c.zone, 0, 0, LEFT);
        }
        return;
    }
    Send(OP_CAPT_RUN, [&](Writer& w) {
        w.U8((uint8_t)slot), w.U8(on);
        if (!on) return;
        w.U8(c.att), w.U8(c.def), w.U16((uint16_t)c.zone), w.Vec({c.mid.x, c.mid.y, c.mid.z}), w.U8(c.captNo);
    }, to);
}

// nobody fought it to its end: by the men left on both sides when it was fought, else by the gangs' weapons
bool Dice(const HostCapt& c) {
    if (c.def >= kGangs) return true;  // a free territory: theirs as they come
    float p = 0.5f + 0.1f * (float)(State(c.att).level - State(c.def).level) + (c.att == AZTECAS ? 0.05f : 0.0f);
    if (c.runner != NONE && c.attLeft + c.defLeft > 0) p = (float)c.attLeft / (float)(c.attLeft + c.defLeft);
    return Rand(0.0f, 1.0f) < std::clamp(p, 0.15f, 0.85f);
}

void HostEnd(int slot, bool won, bool told = true) {
    HostCapt c = g_hc[slot];
    if (!c.on) return;
    g_hc[slot] = HostCapt{};
    if (won) {
        SetOwner(c.zone, c.att);
        CountZones();
        if (told) SendEvent(EV_CAPT_WON, c.att, c.def, c.zone);
    } else {
        SetFlashing(c.zone, false);
        if (told) SendEvent(EV_CAPT_LOST, c.att, c.def, c.zone);
    }
    if (c.runner != NONE) SendRun(slot, c.runner, false);
    radar::RemoveIcon(kIconBase + slot);
    SendViews();
    LOGI("Gang war: the attack of %s on zone %d %s", Info(c.att).name[1], c.zone, won ? "took it" : "failed");
}

void HostReport(uint8_t from, int slot, int zone, uint8_t defLeft, uint8_t attLeft, uint8_t outcome) {
    if (slot < 0 || slot >= kMaxCapts) return;
    HostCapt& c = g_hc[slot];
    if (!c.on || c.runner != from || c.zone != zone) return;
    c.defLeft = defLeft, c.attLeft = attLeft, c.reportMs = NowMs();
    c.runner = outcome == GOING ? c.runner : (uint8_t)NONE;  // it ended there: nothing to stop
    if (outcome == WON) HostEnd(slot, true);
    else if (outcome == LOST) HostEnd(slot, false);
    else if (outcome == LEFT) {
        c.runner = from;  // the counts it had decide
        bool won = Dice(c);
        c.runner = NONE;
        HostEnd(slot, won);
    }
}
}  // namespace

const CaptView& View(int slot) { return g_view[std::clamp(slot, 0, kMaxCapts - 1)]; }

int ActiveCapts() {
    int n = 0;
    for (const HostCapt& c : g_hc) n += c.on;
    return Net().IsHost() ? n : (int)std::count_if(std::begin(g_view), std::end(g_view), [](const CaptView& v) { return v.on; });
}
bool Attacking(int gang) {
    for (const HostCapt& c : g_hc)
        if (c.on && c.att == gang) return true;
    return false;
}
bool ZoneInCapt(int zone) {
    for (const HostCapt& c : g_hc)
        if (c.on && c.zone == zone) return true;
    return false;
}

uint8_t HostStartCapt(uint8_t gang, int zone) {
    if (Attacking(gang)) return CAPT_ON;
    if (ZoneInCapt(zone)) return ZONE_ATTACKED;
    int slot = -1;
    for (int s = 0; s < kMaxCapts && slot < 0; ++s)
        if (!g_hc[s].on) slot = s;
    if (slot < 0) return TWO_CAPTS;
    HostCapt c;
    c.on = true, c.att = gang, c.def = OwnerOf(zone), c.zone = zone, c.startMs = NowMs();
    if (c.def < kGangs && !(State(c.def).flags & F_PLAYED)) c.def = NONE;
    c.mid = TerritoryMiddle(zone);
    c.captNo = State(gang).capts;
    g_hc[slot] = c;
    SetFlashing(zone, true);
    radar::SetIcon(kIconBase + slot, c.mid, 19 /* ENEMYATTACK */);
    SendEvent(EV_CAPT_START, gang, c.def, zone);
    SendViews();
    LOGI("Gang war: %s attack zone %d (%s), slot %d", Info(gang).name[1], zone, TerritoryName(zone).c_str(), slot);
    return 0;
}

void HostUpdateCapts() {
    uint64_t now = NowMs();
    std::vector<PlayerSpot> spots;
    bool haveSpots = false;
    for (int s = 0; s < kMaxCapts; ++s) {
        HostCapt& c = g_hc[s];
        if (!c.on) continue;
        if (c.runner == NONE) {
            if (!haveSpots) spots = PlayerSpots(), haveSpots = true;
            const PlayerSpot* best = nullptr;
            float bestD = kRunnerRange;
            for (const PlayerSpot& p : spots) {
                float d = (p.pos - c.mid).Length2D();
                if (d < bestD) bestD = d, best = &p;
            }
            if (best) {
                c.runner = best->id, c.runMs = c.reportMs = now;
                LOGI("Gang war: the attack on zone %d is fought in player %d's game", c.zone, c.runner);
                SendRun(s, c.runner, true);
                SendViews();
            } else if (now - c.startMs > kNobodyMs) {
                HostEnd(s, Dice(c));
            }
            continue;
        }
        bool present = c.runner == Net().MyId() || Net().Peers().count(c.runner);
        if (!present || now - c.reportMs > 30000) {  // that game is gone or silent
            bool won = Dice(c);
            c.runner = NONE;
            HostEnd(s, won);
        }
    }
    if (now - g_viewSentMs > 2000) SendViews();
}

void HostEndAllCapts() {
    for (int s = 0; s < kMaxCapts; ++s)
        if (g_hc[s].on) HostEnd(s, false, false);
}

void UpdateRuns() {
    static uint64_t last = 0;
    if (NowMs() - last < 1000) return;
    last = NowMs();
    for (int s = 0; s < kMaxCapts; ++s)
        if (g_fight[s].on) UpdateFight(s);
}

bool HandleCapt(uint8_t from, uint8_t op, Reader& r) {
    bool fromHost = from == Net().HostId();
    if (op == OP_CAPT) {
        uint8_t slot = r.U8(), on = r.U8();
        if (!r.Ok() || !fromHost || Net().IsHost() || slot >= kMaxCapts) return true;
        CaptView v;
        if (on) {
            v.on = true, v.att = r.U8(), v.def = r.U8(), v.zone = r.U16(), v.defLeft = r.U8(), v.attLeft = r.U8(), v.fighting = r.U8() != 0;
            Vec3 m = r.Vec();
            if (!r.Ok()) return true;
            v.mid = {m.x, m.y, m.z};
            radar::SetIcon(kIconBase + slot, v.mid, 19);
        } else {
            radar::RemoveIcon(kIconBase + slot);
        }
        g_view[slot] = v;
        return true;
    }
    if (op == OP_CAPT_RUN) {
        uint8_t slot = r.U8(), on = r.U8();
        if (!r.Ok() || !fromHost || slot >= kMaxCapts) return true;
        if (!on) {
            EndRun(slot);
            return true;
        }
        uint8_t att = r.U8(), def = r.U8();
        int zone = r.U16();
        Vec3 m = r.Vec();
        uint8_t captNo = r.U8();
        if (!r.Ok() || att >= kGangs) return true;
        if (!StartRun(slot, att, def, zone, {m.x, m.y, m.z}, captNo)) {  // no cars could be made here: the dice
            Report(slot, LEFT);
            EndRun(slot);
        }
        return true;
    }
    if (op == OP_CAPT_REPORT) {
        uint8_t slot = r.U8();
        int zone = r.U16();
        uint8_t d = r.U8(), a = r.U8(), outcome = r.U8();
        if (r.Ok() && Net().IsHost()) HostReport(from, slot, zone, d, a, outcome);
        return true;
    }
    return false;
}

void ResetCapts() {
    for (int s = 0; s < kMaxCapts; ++s) {
        g_hc[s] = HostCapt{};
        g_fight[s] = Fight{};
        g_view[s] = CaptView{};
        radar::RemoveIcon(kIconBase + s);
    }
}

}  // namespace coop::gangwar::war
