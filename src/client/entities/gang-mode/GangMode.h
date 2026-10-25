#pragma once
// The gang war of Los Santos: a game mode the host picks when it creates the server, instead of the story. Everyone
// starts a new game (its prologue skipped) and plays one of the four gangs of the city, Grove Street, the Ballas, the
// Vagos and the Aztecas, each picked in the lobby; a gang nobody plays is left out or played by the computer, as the
// host sets it there. The gangs earn money, buy attacks on the others' territories (four cars of their men) and more,
// and win as the host set it: all the territories, a number of them, or the most when the time is up.
//
// This is the mode's shared part: whether it is on, who plays which gang, the host's settings, the gangs' state as the
// host keeps it (money, weapons, upgrades), the territories, and the mode's messages (G_GANG). The war itself is
// features/gang-war.
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "common/lib/buffer/Buffer.h"
#include "shared/game/Game.h"

namespace coop::gangmode {

enum Gang : uint8_t { GROVE = 0, BALLAS, VAGOS, AZTECAS, kGangs, NONE = 255 };
enum WinRule : uint8_t { WIN_ALL = 0, WIN_COUNT = 1, WIN_TIME = 2 };

struct Settings {
    uint8_t rule = WIN_ALL;
    uint8_t count = 30;    // WIN_COUNT: this many territories
    uint8_t minutes = 45;  // WIN_TIME: the most territories after this long
    uint8_t ai[kGangs] = {1, 1, 1, 1};  // a gang nobody plays: 1 the computer plays it, 0 it is left out
};

// what the host keeps of each gang
enum GangFlags : uint8_t {
    F_PRODUCTION = 1,  // the goods are made faster
    F_QUALITY = 2,     // and sell for more
    F_EMMET = 4,       // Grove Street took its weapons upgrade at Emmet's
    F_OUT = 8,         // the gang is out of the war (no territory left)
    F_PLAYED = 16,     // in the war (a player's or the computer's)
    F_AI = 32,         // the computer plays it
};
struct GangState {
    int32_t bank = 0;
    uint8_t level = 1;  // its weapons, 1-4
    uint8_t flags = 0;
    uint8_t capts = 0;  // attacks bought so far
    uint16_t zones = 0; // territories held
    uint16_t sold = 0;  // batches of goods sold
};

struct GangInfo {
    const char* name[2];       // Russian, English
    const char* shortName[2];
    uint32_t argb;             // its colour on our screens
    uint8_t r, g, b;           // and on the radar's territories
    uint8_t pedType;           // PED_TYPE_GANG*
    uint8_t density;           // its index in a zone info's gang densities
    uint16_t models[3];        // its men
    const char* leader;        // the leader's special character
    uint16_t leaderModel;      // the special slot it is loaded into (290 + slot - 1)
    game::CVector base;        // where its players come into the war and back after a death
    uint8_t maxLevel;          // the weapons it can reach
};
const GangInfo& Info(int gang);
std::string Name(int gang);       // in the player's language
std::string ShortName(int gang);

bool On();       // this session plays the gang war
bool Started();  // and the war is on (the host's world is up)
Settings& Rules();                 // the host changes them in the lobby (then SendRoster)
const GangState& State(int gang);
GangState& HostState(int gang);    // host
uint32_t SecondsLeft();            // WIN_TIME: till the end (0: no limit or over)
void HostStart(uint32_t seconds);  // host: the war begins (state sent from now on)
void HostStop();

uint8_t GangOf(uint8_t player);    // NONE: not picked yet
uint8_t MyGang();
bool Leader(uint8_t player);       // the first of its gang (the lowest player number)
bool Enemies(uint8_t a, uint8_t b);  // two players of different gangs (they hurt each other)
int PlayersOf(int gang);
void Pick(uint8_t gang);           // this player's choice in the lobby
void HostAssign(uint8_t player, uint8_t gang);  // host
void SendRoster();                 // host: the picks and the settings to everyone
void SendState();                  // host

// the territories: zone infos of Los Santos with a gang in them as the new game has it (the host's list, sent)
const std::vector<uint16_t>& Territories();
void SetTerritories(const std::vector<uint16_t>& zones);  // host
uint8_t OwnerOf(int zoneInfo);                 // by its gangs' densities: the strongest of the four, or NONE
void SetOwner(int zoneInfo, uint8_t gang);     // host: its densities and its colour (Zones takes it to the others)
int TerritoryAt(const game::CVector& at);      // the territory there (a zone info of the list), -1: none
game::CVector TerritoryMiddle(int zoneInfo);   // the middle of its navigation zones
std::string TerritoryName(int zoneInfo);       // its district's name as the map shows it

// the feature's ops of G_GANG (see GangMode.cpp for the entity's own)
using Handler = std::function<void(uint8_t from, uint8_t op, Reader& r)>;
void SetHandler(Handler h);
void Send(uint8_t op, const std::function<void(Writer&)>& body, int to = -1);  // to: a player, -1 all

}  // namespace coop::gangmode
