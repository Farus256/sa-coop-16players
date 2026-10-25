#pragma once
#include <cstdint>
#include <map>
#include <set>
#include <string>

#include "common/api/protocol/Protocol.h"
#include "common/lib/buffer/Buffer.h"
#include "shared/game/Game.h"
#include "shared/lib/core/Core.h"

namespace coop {

struct PedSyncState {
    uint32_t time = 0;
    game::CVector pos, vel;
    float heading = 0;
    uint8_t moveState = 1;
    uint8_t health = 100, armour = 0, weapon = 0;
    uint16_t flags = 0;  // F_*
    uint16_t vehNetId = 0;
    int8_t seat = -1;  // F_IN_VEHICLE: current seat, F_ENTERING: target seat (-1 driver)
    int8_t area = 0;
    game::CVector aim;  // F_AIMING: gun target relative to pos
    uint8_t aimKind = 0;  // F_AIMING: 0 the point above, 1 player aimId, 2 ped netId aimId
    uint16_t aimId = 0;
    // action animation (anything but locomotion): IFP block + index inside it, 0xFFFF = none
    uint16_t animBlock = 0xFFFF, animIdx = 0, animFlags = 0;
    float animTime = 0;
    uint8_t walkGroup = 0xFF;  // CPed::m_nAnimGroup: its walk set (a script's SET_ANIM_GROUP_FOR_CHAR: drunk, old, ...)

    enum : uint16_t {
        F_DEAD = 1,
        F_DUCKING = 2,
        F_IN_VEHICLE = 4,
        F_AIMING = 8,
        F_FIRING = 16,
        F_ENTERING = 32,  // walking to / opening vehicle vehNetId
        F_LEAVING = 64,
        F_ON_VEHICLE = 128,  // standing on vehicle vehNetId (a car's roof, a truck's back) at onLocal in its own frame
        F_NO_DRAG = 256,     // its script keeps it at the wheel (SET_CHAR_CANT_BE_DRAGGED_OUT)
    };
    game::CVector onLocal;
};

// NPC known to the players. owner == my id: simulated here and announced; otherwise a mirror (a record without a ped
// while this game's player is far from it).
struct NetPed {
    uint16_t netId = 0;
    game::CPed* ped = nullptr;  // mirror: nullptr until it could be spawned (model streaming, vehicle missing)
    int handle = -1;
    uint8_t owner = 0;
    uint16_t model = 0;
    uint8_t pedType = 4;
    std::string special;  // model name for special character slots 290-299
    // owner side
    uint64_t lastSendMs = 0;
    int lastClip = -1;  // shots are detected by the clip emptying
    uint8_t lastWeapon = 0;
    uint64_t lastShotMs = 0;
    // mirror side
    bool hasState = false;
    PedSyncState state;
    uint64_t recvMs = 0;
    uint64_t createdMs = 0;  // when its owner announced it (a new passer-by waits out of this player's sight)
    uint64_t lastTaskMs = 0;
    game::CVector lastTaskTarget;
    uint8_t lastTaskMove = 0;
    bool deadApplied = false;
    uint64_t corpseMs = 0;   // its body last moved to where its owner's lies
    uint64_t lastHitMs = 0;  // our player hit it (sent to the owner): its loot falls here if it dies of it
    uint64_t askedMs = 0;    // we asked its owner for it (a Grove member to recruit, a hooker by our car)
    uint8_t hitWeapon = 0, hitPiece = 0, hitDir = 0;  // that hit: it dies of the same one here
    bool dying = false;                               // its death goes through the game's damage (Damage lets it pass)
    uint64_t lastRamMs = 0;  // our player's car hit it (sent once per contact)
    uint8_t gunMode = 0;  // 0 none, 1 aiming, 2 shooting
    int gunTargetHandle = -1;  // ped aimed at, -1: gunTarget point
    game::CVector gunTarget;
    uint64_t gunTaskMs = 0;
    uint64_t weaponMs = 0;     // the last weapon change: its own timer, the walk tasks' held a walking mirror's gun back
    uint16_t enterVeh = 0;
    uint64_t enterTaskMs = 0, leaveTaskMs = 0;
    uint64_t duckMs = 0;  // its crouch last set as the owner's
    bool hanging = false;  // its owner is in the air there (a rope, a fall): the copy is held where it is
    uint16_t animBlock = 0xFFFF, animIdx = 0;  // action animation replayed on the mirror
    uint16_t baseBlock = 0xFFFF, baseIdx = 0;  // the full-body one under it (a fight stance under punches)
    float animTime = 0;                         // owner's time of it in the latest state (restarts go back)
    bool animFull = false;                      // it is not partial: locomotion tasks must wait
    uint64_t fadeMs = 0;                        // spawned here at: it fades in over half a second
};

class Peds : public Module {
public:
    const char* Name() const override { return "Peds"; }
    void OnInstallHooks() override;
    void OnSessionEnd() override;
    void OnPeerLeave(uint8_t id) override;
    void OnGameStarted() override;
    void OnGameShutdown() override;
    void OnPreProcess() override;
    void OnPostProcess() override;

    static Peds& Instance();
    NetPed* Find(game::CPed* ped);
    game::CPed* FindOwned(uint16_t netId);  // our live ped announced under netId
    int HandleOf(uint16_t netId);            // script handle of the live ped (ours or a mirror), -1 if none
    // special character slots (models 290-299) are filled by name at run time
    void RememberSpecialModel(int model, const char* name);
    std::string SpecialName(int model) const;  // what the slot was filled with here, "" if nothing
    // the model here of the special character `name`: its slot, or a free one asked for it, from the top (scripts and
    // cutscenes fill them from 290); -1 while it loads or with no slot free
    int SpecialModelFor(const std::string& name);
    void SpecialWorn(int model);  // a copy wears a special model we asked for: it is kept by that from now on
    // a script here fills that slot with another character: the copies wearing it are made again elsewhere (the game
    // drops the model from under script people, and a copy is one)
    void RespawnCopiesOf(int model);
    // a player (re)started its game: announce our peds to it again
    void Reannounce(uint8_t to);
    // our ped goes to player `to` (it takes the mirror over as its own) and ours is deleted
    void GiveAway(uint16_t netId, uint8_t to);
    // this game would drop the ped, the other player is nearer: it goes to that player's game (false: not ours to give)
    // noGround: this game has no ground under it any more (it would fall through the map), so its own view does not count
    bool QueueGiveAway(game::CPed* ped, uint8_t to, bool noGround = false);
    // a kind of ped the other game can take over (a mirror is a civilian there: no cops, medics or firemen)
    static bool CanGiveAwayType(const NetPed& np);
    // A ped every player's game makes for itself (an interior's shopkeeper, customers and residents): it stays this
    // game's own and is never announced, so that each player deals with a ped of his own game.
    void KeepLocal(game::CPed* ped);
    // such a ped, or one a script of this game made outside a mission (a shop's, a restaurant's, a barber's staff:
    // those scripts run in every game)
    bool IsLocalOnly(game::CPed* ped);
    // A ped a feature of the mod made with a script command (CREATE_CHAR: an arena's enemies, a tank battle's
    // soldiers) for every player to see: announced although no mission made it (IsLocalOnly took such a ped for an
    // ambient script's, which every game runs itself, and the others never saw it).
    void Share(game::CPed* ped);
    // The recruit key (G) by a Grove Street member another player's game made (here its copy, a civilian the game
    // will not recruit): that game is asked to hand it over, and once it is this game's own it joins this player's
    // gang. True when such a ped was there to ask for.
    bool AskToRecruit();
    // the gang war: one of this player's own gang's men of this game in front (locked on to, else within 3 m) joins it
    bool RecruitLocal();
    // This game's player went into an interior (or out of one): the game drops its random people now
    // (CPopulation::RemoveAllRandomPeds), and its cars are left behind. Those near another player in the place left go
    // to that player's game instead, with whoever sits in them, so they do not vanish in front of it.
    void HandOverLeftBehind();
    // /sync: how many people this game shares (its own) and shows for the others (their copies made here)
    void Count(int& own, int& copies) const;
    static bool CanGiveAwayType(const NetPed& np, uint8_t to);  // a cop to the game of a wanted player, too
    // a script that runs in each game for its own player made this ped (a shop's seller, the range's shooters, a
    // casino's dealers, an airport's guard): it stays this game's own (sideJob: a side job's, kept local only indoors)
    void MarkLocal(game::CPed* ped, bool sideJob);
    // a copy of the other game's ped sits on this seat of the car in its owner's game, or goes for it (seat -1: the
    // driver's), while the copy here is not there yet: Ryder sat beside the host there, his copy still walked here, and
    // our player took his seat ("Ryder sank into the ground")
    bool SeatClaimed(game::CVehicle* v, int seat);

private:
    void ScanOwned();
    void AskForTheirs();
    void Announce(NetPed& np, proto::PlayerMask to = proto::kAllPlayers);
    void SendState(NetPed& np);
    proto::PlayerMask StateTargets(NetPed& np);
    bool WantMirror(const NetPed& np) const;  // whether this game keeps a copy of another game's ped now
    void Unspawn(NetPed& np);
    void HandleCreate(uint8_t from, Reader& r);
    void HandleRemove(uint8_t from, Reader& r);
    void HandleSync(uint8_t from, Reader& r);
    void HandleOwner(uint8_t from, Reader& r);
    void HandleAsk(uint8_t from, Reader& r);
    void Recruit(NetPed& np);
    void ApplyMirror(NetPed& np);
    bool EvictRandomCopy();
    game::CPed* SpawnMirror(NetPed& np);
    void Forget(uint16_t netId, bool deletePed);
    void Adopt(NetPed& np);
    void DropMirrorsOf(uint8_t id, bool keep);  // keep: this game's own people now (it left), else deleted (it restarted)
    PedSyncState Capture(NetPed& np);

    std::map<uint16_t, NetPed> m_byId;
    std::map<game::CPed*, uint16_t> m_byPtr;
    uint16_t m_nextId = 1;
    std::map<int, std::string> m_specialNames;
    std::set<int> m_ourSpecials;  // special slots SpecialModelFor asked for (no script or cutscene holds them)
    std::map<game::CPed*, uint8_t> m_giveAway;  // to hand over after the population code (ped -> player)
    std::set<int> m_localOnly;                  // handles of KeepLocal's peds
    std::set<int> m_shared;                     // handles of Share's peds
    std::map<uint16_t, uint64_t> m_recruitAsks; // peds asked for to recruit, and when
    std::map<int, bool> m_local;                // MarkLocal: script handle -> made by a side job
    uint64_t m_lastScan = 0, m_lastStatsMs = 0;
    bool m_wasReady = false;
    int m_mirrorShots = 0;  // diagnostics: shots fired by mirrors since the last stats line
};

// the interior a ped is in, by its area or, for the interiors' own people (they keep area 0), by a player near it
int8_t AreaOf(game::CPed* ped);

}  // namespace coop
