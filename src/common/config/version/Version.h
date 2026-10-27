#pragma once

#define COOP_NAME "SA:COOP"
#define COOP_VERSION_STR "0.6.1"  // tools/build.py gives the version resource its numbers from this

#ifndef RC_INVOKED  // the resource compiler reads only the macros above (VersionInfo.rc)
#include <cstdint>

namespace coop {
// Bumped on any incompatible change of the network protocol. The multi-player fork counts from 100: a game or a server
// of the original two-player mod (protocol 20 and earlier) is told that the versions differ instead of joining.
constexpr uint32_t kProtocolVersion = 122;
constexpr uint16_t kDefaultPort = 7788;
// Size of gta_sa.exe 1.0 US (HOODLUM) — the only supported executable.
constexpr uint32_t kSupportedExeSize = 14383616;
}  // namespace coop
#endif
