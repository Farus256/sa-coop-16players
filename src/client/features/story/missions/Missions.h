#pragma once
#include <cstdint>
#include <string>

namespace coop {

// a mission marker waits for everybody: "Ждём напарника" for the host, "<host> ждёт вас..." for the partner, the
// partner's call from one for the host (its keys in hint); empty otherwise
std::string MissionWaitText(std::string& hint);

// someone wants to skip the cutscene: the line for the box ("Tester1 хочет пропустить катсцену") and the keys to
// answer (empty when this player said yes already); empty when no vote is on
std::string CutsceneSkipText(std::string& hint);
// Esc: this player wants to watch on. False when there was nothing to answer (the key goes on to the game).
bool CutsceneSkipDecline();
// what the skip button does in a cutscene (debug bot)
void CutsceneSkipPress();

// a side activity's call to play it together: the line for the box ("Lesha зовёт вас на совместное задание в такси",
// "Позвать Lesha на совместное задание в такси?") and the keys to answer; empty when there is nothing to say
std::string SideCallText(std::string& hint);
// Y (yes) or N: answers the call (a side activity's, or the partner's from a mission's start point), or makes it. False
// when there was nothing to answer (the key goes on to the game).
bool SideCallKey(bool yes);

}  // namespace coop
