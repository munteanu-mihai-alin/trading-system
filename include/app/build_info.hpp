#pragma once

#include <string>

// Build provenance baked into the executable at compile time.
//
// Why the binary carries this instead of a sidecar file: bin/binary.json
// described the binary from the outside, so the two could drift -- and
// did. bin/hft_app was overwritten by an ad-hoc build whose md5 matched
// no staged version, and with no manifest next to it GET /binaries
// reported branch=None. Provenance compiled INTO the image cannot be
// separated from the image it describes.
//
// Values come from CMake (-DHFT_BRANCH / -DHFT_COMMIT / -DHFT_VERSION).
// CI passes the authoritative values from the GitHub context; a local
// build falls back to `git`; if neither is available the value is
// "unknown" rather than silently empty.
//
// Query from the command line -- see src/app/main.cpp:
//   hft_app --version            -> 2026.09.12
//   hft_app --branch             -> chronos2-mr-pred-exit
//   hft_app --branch --commit    -> branch=... / commit=... (labeled)
// Any of these flags prints and exits WITHOUT starting a trading
// session.

#ifndef HFT_BRANCH
#define HFT_BRANCH "unknown"
#endif

#ifndef HFT_COMMIT
#define HFT_COMMIT "unknown"
#endif

#ifndef HFT_VERSION
#define HFT_VERSION "unknown"
#endif

namespace hft::build_info {

inline std::string branch() {
  return HFT_BRANCH;
}

inline std::string commit() {
  return HFT_COMMIT;
}

inline std::string version() {
  return HFT_VERSION;
}

}  // namespace hft::build_info
