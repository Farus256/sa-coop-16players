#pragma once
// SA:COOP network protocol. See docs/PROTOCOL.md.
//
// Every ENet packet starts with a one-byte message id.
//  * ids < 64  — service messages, handled by the server itself;
//  * ids >= 64 — game messages: the client sends [id][payload], the server relays them to the other
//    players as [id][u8 senderId][payload]. The server never parses game payloads.
//  * C_RELAY_TO wraps a game message for some of the players only: [C_RELAY_TO][u16 mask][id][payload]. The
//    others never get it (a car's state goes to the players near that car, not across the whole map).
#include <cstdint>
#include <string>
#include <vector>

#include "common/lib/buffer/Buffer.h"

namespace coop::proto {

// The protocol's limit: a player id is 0..15, a set of players is a 16-bit mask, a network id of a car or a ped
// carries its owner in its top 4 bits. A server takes kDefaultMaxPlayers unless it is told otherwise (max_players).
constexpr int kMaxPlayers = 16;
constexpr int kDefaultMaxPlayers = 4;
constexpr uint8_t kServerId = 255;
constexpr size_t kMaxChatLen = 160;
constexpr size_t kWorldChunk = 16 * 1024;

using PlayerMask = uint16_t;
constexpr PlayerMask kAllPlayers = 0xFFFF;
inline PlayerMask Bit(uint8_t id) { return id < kMaxPlayers ? (PlayerMask)(1u << id) : 0; }
// Ids of the vehicles and peds a player's game announces: its number in the top 4 bits, 1..4095 below (0 stands for
// "none" in the messages, so no id is 0)
inline uint16_t EntityId(uint8_t owner, uint16_t serial) {
    return (uint16_t)(((owner & 0x0F) << 12) | (serial % 4095 + 1));
}
inline uint8_t EntityMaker(uint16_t id) { return (uint8_t)(id >> 12); }  // whose game made it (its id says)

enum Channel : uint8_t {
    CH_RELIABLE = 0,  // ordered, reliable: events
    CH_STATE = 1,     // unreliable sequenced: positions/states
    CH_VOICE = 2,     // unreliable: voice frames
    CH_BULK = 3,      // ordered, reliable: file transfers (save game)
    CH_COUNT = 4,
};

enum MsgId : uint8_t {
    // ---- client -> server ----
    C_HELLO = 1,
    C_CHAT = 2,              // str text (may be a /command)
    C_PROFILE_SAVE = 3,      // bytes profile (opaque for the server)
    C_WORLD_BEGIN = 4,       // host uploads its save: u32 size, u32 crc, str label
    C_WORLD_CHUNK = 5,       // u32 offset, bytes
    C_WORLD_END = 6,
    C_WORLD_REQUEST = 7,     // ask the server to send the current world save
    C_STATUS = 8,            // u8 ClientStatus
    C_WORLD_KEEP = 9,        // host: a fresh snapshot is impossible now (mission), send the stored world
    C_WORLD_NEWGAME = 10,    // host: it plays a new game's prologue (no save of it can exist yet)
    C_WORLD_FOLLOW = 11,     // host: the prologue is over but cannot be saved yet: partners follow in a new game
    C_RELAY_TO = 12,         // u16 mask, then a game message ([id >= 64][payload]): relayed to the players of the mask

    // ---- server -> client ----
    S_WELCOME = 20,          // see Welcome
    S_REJECT = 21,           // str reason, str reasonEn (the same in English; empty when there is nothing to translate)
    S_PLAYER_JOIN = 22,      // u8 id, str name, u8 isHost
    S_PLAYER_LEAVE = 23,     // u8 id, u8 reason
    S_CHAT = 24,             // u8 fromId (255 = server), u32 rgba, str text, str textEn (the server's own texts)
    S_PROFILE = 25,          // bytes profile (empty when the player is new)
    S_WORLD_BEGIN = 26,      // u32 size, u32 crc, str label
    S_WORLD_CHUNK = 27,      // u32 offset, bytes
    S_WORLD_END = 28,
    S_WORLD_NONE = 29,       // the host has not uploaded a save yet
    S_HOST_CHANGED = 30,     // u8 newHostId
    S_PINGS = 31,            // u8 count, {u8 id, u16 ping}
    S_KICKED = 32,           // str reason, str reasonEn
    S_WORLD_WANTED = 33,     // to the host: a partner needs the world, please upload a fresh snapshot
    S_ADDRESS = 34,          // where friends connect to: str external ip:port, str lan ip:port, u8 ADDRESS_*, str UPnP error
    S_WORLD_NEWGAME = 35,    // to a partner: start a new game and join the host where the prologue left it
    S_WORLD_WAIT = 36,       // to a partner: the host plays a new game's prologue, keep waiting

    // ---- relayed game messages (client <-> client) ----
    G_FIRST = 64,
    G_PLAYER_ONFOOT = 64,     // unreliable
    G_PLAYER_DRIVER = 65,     // unreliable
    G_PLAYER_PASSENGER = 66,  // unreliable
    G_PLAYER_AIM = 67,        // unreliable
    G_PLAYER_SPAWN = 68,      // reliable: (re)spawn, model, interior
    G_PLAYER_DEATH = 69,      // reliable: i16 the default group's animation the sender's player fell with (its copies die so)
    G_PLAYER_LOOK = 70,       // reliable: clothes, fat, muscle, u16 model, the special character's name (290-299),
                              // u8 fighting style, u8 its moves
    G_PLAYER_STATS = 71,      // reliable: stats affecting the remote ped (skills, stamina, fight style)
    G_PLAYER_WEAPONS = 72,    // reliable: weapon inventory
    G_BULLET = 73,            // visual bullet replay
    G_DAMAGE = 74,            // damage event addressed to the owner of the victim
    G_EXPLOSION = 75,         // reliable: a blast went off (creator kind and id, type, position...), see Fire.cpp
    G_PROJECTILE = 76,        // reliable: a grenade, a rocket... was thrown (creator, weapon, position, speed, target)
    G_PLAYER_ENTER_CAR = 77,
    G_PLAYER_EXIT_CAR = 78,
    G_PLAYER_ANIM = 79,       // reliable: one-shot animations (eating, drinking, gestures)

    G_VEH_CREATE = 80,
    G_VEH_REMOVE = 81,
    G_VEH_SYNC = 82,          // unreliable
    G_VEH_OWNER = 83,         // ownership change request/announce
    G_VEH_STATUS = 84,        // reliable: damage, colours, mods, locks, siren
    G_VEH_HANDOVER = 85,      // reliable: u16 netId, u8 to: the owner gives its car away (its peds go before it)
    G_VEH_SCRIPT_FREE = 86,   // reliable: u16 netId: a script of this game let go of its car (a mirror here)
    G_PED_CREATE = 90,
    G_PED_REMOVE = 91,
    G_PED_SYNC = 92,          // unreliable
    G_PED_OWNER = 93,         // reliable: u16 netId, u8 to: the owner gives its ped away (the mirror becomes its own)
    G_PED_STATUS = 94,        // reliable: death, weapon, task hints
    G_RESYNC = 95,            // reliable: "my game (re)started", the others announce their vehicles and peds again
    G_OBJ_CREATE = 100,       // reliable: an object of the host's mission
    G_OBJ_REMOVE = 101,       // reliable: it is gone (or broke)
    G_OBJ_SYNC = 102,         // unreliable: it moved
    G_PICKUP_EVENT = 103,     // reliable: a pickup was collected
    G_PICKUP_CREATE = 104,    // reliable: a pickup appeared (loot, the host's mission)
    G_OBJ_DAMAGE = 105,       // reliable: the partner hit a mirror object, its owner applies it
    G_FIRE = 106,             // reliable: a fire started / went out
    G_ZONE = 107,             // reliable: gang densities of a territory
    G_CHEAT = 108,            // reliable: a cheat typed (u8 id, u8 on, str code, u8 a switch, u8 the others apply it)
    G_ENEX = 109,             // reliable: entrances the host's scripts opened or closed

    G_WORLD_STATE = 110,      // time, weather, gravity... (host -> all)
    G_WANTED = 111,

    G_MISSION_START = 120,
    G_MISSION_END = 121,
    G_TEXT = 122,             // subtitles / help / big messages
    G_BLIP = 123,             // add/update/remove radar blip
    G_MARKER = 124,           // 3d markers, checkpoints, script spheres
    G_ONSCREEN = 125,         // timers and counters
    G_FADE = 126,
    G_CUTSCENE = 127,         // load/start/finish/skip
    G_SCRIPT_VARS = 128,      // story progress (script global variables)
    G_MISSION_REWARD = 129,
    G_TELEPORT = 130,         // teleport request/command
    G_GAME_EVENT = 131,       // misc one-shot events (audio, widescreen, camera shake)
    G_MISSION_WAIT = 132,     // the host stands in a mission marker and waits for the partner
    G_WAYPOINT = 133,         // reliable: the player's map waypoint (set or cleared)
    G_PROPERTY = 134,         // reliable: the player bought a property (its index in main.scm)
    G_HOST_SAVED = 135,       // reliable: the host saved its game, the partners save their profiles
    G_MISSION_WEAPON = 136,   // reliable: u8 op (1 give, 2 take), u8 weapon, i32 ammo (CJ's count after the change):
                              // the host's mission armed its CJ
    G_CHECKPOINTS = 137,      // reliable: the host's script checkpoints and spheres (u8 n, records)
    G_WORLD_OBJ = 138,        // a map object broke or moved (u16 model, origin, stage, flags, weapon[, pose, speeds]);
                              // reliable for changes, unreliable while it moves
    G_GARAGE = 139,           // reliable: u8 op, u8 garage: the partner's cars into the host's safe house store
                              // (u8 n, u16 net ids), the host has them (same), bring the stored ones out
    G_VEH_CLEAR = 140,        // reliable: u8 n, u16 net ids: the host's mission cleared an area (CLEAR_AREA), the
                              // owners delete these empty cars of theirs there
    G_WATER_CANNON = 141,     // unreliable: u16 vehicle, the jet's offset from it and speed (a fire truck shoots water)
    G_TRAIN = 142,            // a train of the sender's game (u8 op, u16 id; state: kind, track, direction, rails, speed,
                              // pos), unreliable; gone: reliable
    G_MISSION_AUDIO = 143,    // reliable, host -> partner: u8 op, u8 slot of the mission's speech (load: i32 sample;
                              // attach: u8 kind, u16 net id; position: vec3; play; clear)
    G_GARAGE_DOOR = 144,      // reliable: u8 garage, u8 door of a shop garage (0 shut, 1 open, 2 none: the sender's
                              // door follows ours)
    G_PROP = 145,             // reliable: u8 op, u16 id of an object the sender's player or ped holds or carries
                              // (state: model, holder, how it hangs, look, own animation; gone)
    G_PED_SPEECH = 146,       // reliable: u8 player/ped, u16 net id, the line it starts (context, bank, sound, delay,
                              // volume, forced)
    G_SEAT_CALL = 147,        // reliable, host -> all: u8 kind (1 the host's mission put CJ in a car, 2 stood him on
                              // one, 3 took him off), u16 vehicle; u8 n, {u8 player, i8 seat (-1: stand on it), vec3
                              // place, u16 turret angle, f32 turret range}: where each partner near goes
    G_REVIVE = 148,           // reliable, to one player: u8 op (1 started, 2 stopped, 3 done): the sender revives it
    G_SCRIPT_CAM = 149,       // reliable, host -> all: u8 op and its data: the host's mission's script took the camera
                              // (a fixed place, a point, an entity, a move, a zoom) or gave it back
    G_LOBBY = 150,            // reliable: u8 op, u8 value: a newcomer asks (1), the host says whether the game is on
                              // (2), a partner is ready or not (3)
    G_RADIO = 151,            // reliable: u16 vehicle, i8 station: the radio its driver plays (its passengers tune to it)
    // 153: free (the toy car race of a test build)
    G_TANK_SHOT = 155,        // reliable: u16 vehicle, vec3 muzzle, vec3 velocity: a Rhino fired a shell (seen here,
                              // its blast comes as an explosion of the shooter's game)
    G_TANK_HIT = 156,         // reliable, to the owner: u16 vehicle, f32 damage, vec3 from: a shell hit its tank
    G_TANK_BATTLE = 157,      // reliable: u8 op and its data: the host's tank battle (start, a wave, the end)
    G_PED_ASK = 158,          // reliable, to the owner: u16 ped, u8 why (1 to recruit it): hand it over
    G_ARENA = 159,            // reliable: u8 op and its data: the host's ground battle (/arena: start, a wave, left, the boss, the end)
    G_KNIFE = 160,            // reliable, to the victim: u16 ped: a knifeman of the sender's game killed you from behind
    G_CHEAT_RULE = 161,       // reliable, from the host: u8 1 when cheat codes do not work in its game
    // the original mod's 0.3.0 (its own numbers 147-156 were this fork's already)
    G_MISSION_POLICY = 162,   // reliable, host -> partner: the mission's police rules (u8 max wanted, u8 back-off bits,
                              // f32 wanted multiplier)
    G_VEH_SCRIPT = 163,       // reliable, host -> the driver's game: the host's mission moved, turned or froze a car that
                              // the other player drives (u16 car, u8 its owner, u16 command, its three parameters)
    G_INTERIOR_POP = 164,     // reliable, host -> partner: i8 area, vec3 at: its game made that interior's people
    G_MAP_EDIT = 165,         // reliable, host -> partner: u16 command, vec3, f32 radius, i32 model, i32 state or model: a
                              // map edit of the host's mission (an object's collision or visibility, a building's model)
    G_PHOTO = 166,            // reliable, partner -> host: u8 n, u16 peds, u8 m, u16 mission objects: the host's own that
                              // the partner's camera took (their copies there), for the mission's photo checks
    G_IPL = 167,              // reliable, host -> partner: u8 op (0 in, 1 out, 2 out when far), str name: a part of the
                              // map the host's mission brings in or takes out (REQUEST_IPL / REMOVE_IPL...)
    G_HELI_SHOT = 168,        // reliable: u16 helicopter, vec3 gun from it, vec3 aimed at, u8 damage: a round of a police
                              // helicopter of the sender's game
    G_CONTACTS = 169,         // reliable, host -> partner: u8 n, the story's mission start points of the host's radar
                              // (vec3, u8 sprite, u8 flags, u8 display, u32 colour, u16 size, i16 entrance)
    G_GANG_WAR = 170,         // reliable, host -> partner: u8 fought: a gang war of the host's is being fought (sent on
                              // a change and every 3 s while it is)
    G_SIDE = 171,             // reliable: u8 op (1 offer, 2 accept, 3 decline, 4 end, 5 leave), u8 to (a player, 255 all),
                              // str name (the side activity's mission script), i32 money (end: what it paid)
    G_MONEY = 172,            // reliable, to one player: u8 op (1 a payment, 2 taken, 3 refused), u32 id, i32 amount (/pay)
    G_MINIGAME = 173,         // u8 op: the host's rhythm contest (1 offered to all, 2 a player claims it, 3 who plays it:
                              // u8 player, 255 the host; 4 over), reliable; 5 its sprites to the one who plays it (u8 n,
                              // {u8 slot, str name}), reliable; 6 this frame's drawing (u8 n, {u8 type, u8 before fade,
                              // i16 sprite, 4 f32 corners in screen fractions, f32 angle, u32 colour}), unreliable
    G_RACE = 174,             // reliable, host -> partners: u8 op (1 a race of the host's mission started: u16 the host's car
                              // model; 2 it is over)

    G_VOICE = 200,            // unreliable: voice frame
    G_LAST = 255,
};

inline bool IsRelayed(uint8_t id) { return id >= G_FIRST; }

enum ClientStatus : uint8_t {
    STATUS_MENU = 0,
    STATUS_LOADING = 1,
    STATUS_INGAME = 2,
    STATUS_PAUSED = 3,
};

enum LeaveReason : uint8_t { LEAVE_QUIT = 0, LEAVE_TIMEOUT = 1, LEAVE_KICKED = 2 };

enum AddressFlags : uint8_t { ADDRESS_DONE = 1, ADDRESS_UPNP = 2, ADDRESS_CGNAT = 4 };

// The data of an ENet disconnect, so the other side knows why the connection ended (0: it timed out)
enum DisconnectCode : uint32_t {
    DISCONNECT_TIMEOUT = 0,
    DISCONNECT_QUIT = 1,         // the client left
    DISCONNECT_SERVER_STOP = 2,  // the server shut down (the host closed the game or the server)
    DISCONNECT_KICKED = 3,
    DISCONNECT_REJECTED = 4,     // refused at the handshake (S_REJECT says why)
};

struct Hello {
    uint32_t protocol = 0;
    std::string version;
    std::string name;
    std::string password;
    bool wantsHost = false;
    uint32_t exeSize = 0;

    void Write(Writer& w) const {
        w.U32(protocol);
        w.Str(version, 32);
        w.Str(name, 64);
        w.Str(password, 64);
        w.Bool(wantsHost);
        w.U32(exeSize);
    }
    bool Read(Reader& r) {
        protocol = r.U32();
        version = r.Str(32);
        name = r.Str(64);
        password = r.Str(64);
        wantsHost = r.Bool();
        exeSize = r.U32();
        return r.Ok();
    }
};

struct PlayerEntry {
    uint8_t id = 0;
    std::string name;
    bool isHost = false;
};

struct Welcome {
    uint8_t yourId = 0;
    uint8_t hostId = 0;
    std::string serverName;
    std::string motd, motdEn;
    bool worldAvailable = false;
    bool friendlyFire = false;  // the players can hurt each other (the host chose it creating the game)
    uint8_t maxPlayers = kDefaultMaxPlayers;  // the server's places
    std::vector<PlayerEntry> players;  // already connected players (excluding you)

    void Write(Writer& w) const {
        w.U8(yourId);
        w.U8(hostId);
        w.Str(serverName, 64);
        w.Str(motd, 512);
        w.Bool(worldAvailable);
        w.U8((uint8_t)players.size());
        for (auto& p : players) {
            w.U8(p.id);
            w.Str(p.name, 64);
            w.Bool(p.isHost);
        }
        w.Str(motdEn, 512);
        w.Bool(friendlyFire);
        w.U8(maxPlayers);
    }
    bool Read(Reader& r) {
        yourId = r.U8();
        hostId = r.U8();
        serverName = r.Str(64);
        motd = r.Str(512);
        worldAvailable = r.Bool();
        uint8_t n = r.U8();
        players.clear();
        for (uint8_t i = 0; i < n && r.Ok(); ++i) {
            PlayerEntry p;
            p.id = r.U8();
            p.name = r.Str(64);
            p.isHost = r.Bool();
            players.push_back(p);
        }
        motdEn = r.Str(512);
        friendlyFire = r.Bool();
        maxPlayers = r.U8();
        return r.Ok();
    }
};

}  // namespace coop::proto
