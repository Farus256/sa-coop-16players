#pragma once
// The gang war's parts inside its slice: GangWar.cpp (the players in the war, the money, the purchases, the convoy, the
// screen and the menu), Capt.cpp (the attacks on territories), Base.cpp (the bases: their people and their doors).
#include <cstdint>
#include <functional>
#include <vector>

#include "common/lib/buffer/Buffer.h"
#include "shared/game/Game.h"

namespace coop::gangwar::war {

// the feature's ops of G_GANG (entities/gang-mode has 1-4)
enum : uint8_t {
    OP_BUY = 10, OP_EVENT = 11, OP_GIFT = 12, OP_CAPT = 13, OP_CONVOY = 14,
    OP_CAPT_RUN = 15,     // host -> the game that fights an attack: begin it there (or stop it)
    OP_CAPT_REPORT = 16,  // that game -> host: how it goes, how it ended
    OP_GUARD = 18,        // host -> everyone: whose game keeps a base's people
};
enum Item : uint8_t { IT_CAPT = 1, IT_ARMOUR, IT_BRIBE, IT_PRODUCTION, IT_QUALITY, IT_EMMET, kItems };
enum Event : uint8_t {
    EV_BEGIN = 1, EV_INCOME, EV_BOUGHT, EV_CAPT_START, EV_CAPT_WON, EV_CAPT_LOST, EV_BRIBE, EV_LEVEL, EV_CONVOY,
    EV_CONVOY_TAKEN, EV_CONVOY_GONE, EV_OUT, EV_WIN, EV_DENIED, EV_EMMET
};
enum Denied : uint8_t { NO_MONEY = 1, NO_ZONE, OWN_ZONE, CAPT_ON, HAVE_IT, NOT_HERE, MAX_LEVEL, NO_MARK, TWO_CAPTS, ZONE_ATTACKED };

constexpr uint64_t kMin = 60 * 1000;
constexpr int PED_TYPE_COP = 6, PED_TYPE_MISSION1 = 24;
constexpr int MODEL_COP = 280, MODEL_ARMY = 287, MODEL_BARRACKS = 433, MODEL_FREEWAY = 463;
constexpr float kBaseRadius = 25.0f;

int RandInt(int a, int b);
float Rand(float a, float b);

struct Fighter {
    int health, accuracy, armour;
};
Fighter FighterOf(int gang);
int MainGun(int gang);
int CarOf(int gang);
bool GunReady(int weapon);

game::CVector Ground(game::CVector p);
game::CVector RoadNear(const game::CVector& p);
game::CVector WalkNear(const game::CVector& p);
void Teleport(int8_t area, const game::CVector& to, float heading);

// the players in the world outside: their peds (this game's and the copies here)
struct PlayerPed {
    game::CPed* ped;
    uint8_t id;
    uint8_t gang;
};
std::vector<PlayerPed> LivePlayers();
// ... and where every player is by its states (a copy far off may have no ped here)
struct PlayerSpot {
    uint8_t id;
    uint8_t gang;
    game::CVector pos;
};
std::vector<PlayerSpot> PlayerSpots();

bool Alive(game::CPed* p);
game::CPed* NearestOf(const game::CVector& at, float within, const std::function<bool(game::CPed*)>& want);
uint8_t GangOfPed(game::CPed* p);  // a player's gang, a gang member's, else gangmode::NONE
bool OfGang(game::CPed* p, int gang);
// a man of the gang (seat: -2 on foot, -1 the car's driver, else its passenger seat), shared with every game; -1: none
// calm: the street's own ways (it reacts as its gang's people do), not a fighter's that only does what it is told
int MakeMan(int gang, const game::CVector& at, int car, int seat, bool calm = false);

void SendEvent(uint8_t ev, uint8_t a = 255, uint8_t b = 255, int zone = -1, int32_t value = 0, int to = -1);
void CountZones();  // host
bool HostRunning();  // host: the war is on and not won yet

// ---- Capt.cpp: up to two attacks at once, by two different gangs. The host keeps them; each is fought in the game
// of a player near its territory (that game has the ground, the roads and the people there), decided by the dice when
// nobody comes.
constexpr int kMaxCapts = 2;
struct CaptView {
    bool on = false;
    uint8_t att = 255, def = 255, defLeft = 0, attLeft = 0;
    bool fighting = false;  // a game fights it (else its cars are on the way)
    int zone = -1;
    game::CVector mid;
};
const CaptView& View(int slot);
int ActiveCapts();
bool Attacking(int gang);       // host: that gang has an attack on
bool ZoneInCapt(int zone);      // host
uint8_t HostStartCapt(uint8_t gang, int zone);  // host: 0 begun, else why not (Denied)
void HostUpdateCapts();
void HostEndAllCapts();
void UpdateRuns();              // every game: the attacks it fights
bool HandleCapt(uint8_t from, uint8_t op, Reader& r);
void ResetCapts();

// ---- Base.cpp: each base's people (5-10 of the gang's men walking about it, kept by the game of the player nearest
// it) and its door (a house's door near the base, into the house, with the gang's marker)
void HostUpdateGuards();
void UpdateGuards();
bool HandleGuards(uint8_t from, uint8_t op, Reader& r);
void UpdateBaseDoors();   // every game: through the base's door, in and out
void PlaceBaseMarkers();  // every game, before the frame's process: the doors' markers
void ShutStreetDoors();   // the city's other doors shut (no markers)
bool BaseDoorAt(int gang, game::CVector& at);  // the base's door outside (found once the world is up)
void ResetBase();

}  // namespace coop::gangwar::war
