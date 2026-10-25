#pragma once
// The story's music minigames played by two: the lowrider contest of "Cesar Vialpando" and the dance of "Life's a
// Beach". The partner plays beside the host in its own game (the same beat track, judged against its own playback), and
// its points go into the host's score.
#include <cstdint>

namespace coop::minigames {

// host: a command just ran (op, its script, where it was in the script; story: the host's story mission's, replicated)
void AfterCommand(void* script, uint16_t op, const uint8_t* ip, bool story);

// partner: its own dance runs (the host's counters must not go over its own score)
bool OwnDance();

// host: a minigame of its mission runs (its help is the partner's too: both play)
bool HostMinigame();

// host: a text of this script the partner shows its own way (the lowrider judge's "PERFECT", "Too late!")
bool PartnerShowsOwn(void* script);

}  // namespace coop::minigames
