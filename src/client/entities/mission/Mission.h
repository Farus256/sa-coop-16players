#pragma once
// The host's mission as the rest of the game sees it. The story feature (features/story/missions) runs the missions
// and answers these.
#include <cstdint>

namespace coop {

// partner: the host's mission runs, or ended less than graceMs ago
bool HostMissionRecent(uint64_t graceMs);

// host: its mission (or the mission's trigger) runs and a partner is there to see it
bool HostReplicating();

// the script (CRunningScript) whose commands run right now, or null
void* CurrentScript();

// the host's story mission script (not a side activity's mission, not a thread)
bool StoryMission(void* script);

// the mission script that runs in this game (m_bIsMission), or null: on the partner always its own side activity
void* RunningMission();

// partner: this player plays the host's rhythm contest (the host's game takes its pad; features/minigame answers)
bool PlayingHostMinigame();

}  // namespace coop
