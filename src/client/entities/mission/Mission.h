#pragma once
// The host's mission as the rest of the game sees it. The story feature (features/story/missions) runs the missions
// and answers these.
#include <cstdint>

namespace coop {

// partner: the host's mission runs, or ended less than graceMs ago
bool HostMissionRecent(uint64_t graceMs);

// a person of this game this game put on a car (ATTACH_CHAR_TO_CAR, at offset): its copy in the other games goes on the
// copy of that car the same way (false: the person or the car is not shared yet)
bool SharePersonOnCar(bool on, int ped, int car, float x, float y, float z);

// partner: it watches a scene of the host's mission on the engine (the host's camera)
bool SceneWatched();

// partner: on foot in a story mission's start point, no mission here or at the host's (the host's world after its
// progress loads there, and the host's start waits for it)
bool AtMissionMarker();

// host: its mission (or the mission's trigger) runs and a partner is there to see it
bool HostReplicating();

// the script (CRunningScript) whose commands run right now, or null
void* CurrentScript();

// the host's story mission script (not a side activity's mission, not a thread)
bool StoryMission(void* script);

// host: that script is its story mission's or a thread it started (AUDIOL, which says the lines of its scenes)
bool MissionThread(void* script);

// the mission script that runs in this game (m_bIsMission), or null: on the partner always its own side activity
void* RunningMission();

// partner: clothes the host's mission put on over this player's own look (the balaclava) are not its own to keep: its
// own keys go into clothes (a CPedClothesDesc), false when no mission clothes are on
bool MissionClothesUnder(void* clothes);

// partner: this object is the copy of one of the host's mission objects (the host's crate, not a prop of ours)
bool MissionObjectCopy(void* object);

// host: the net id of this object of the host's mission (0: not one); partner: the handle of its copy (-1: none here)
uint16_t MissionObjectNetId(void* object);
int MissionObjectCopyHandle(uint16_t netId);

// host: the other player holds the copy of this mission object in its game (a crate of "Home Invasion"); holder: its
// copy here, enterMs: when it last pressed its enter button holding it
bool PartnerHolds(void* object, void** holder = nullptr, uint64_t* enterMs = nullptr);

}  // namespace coop
