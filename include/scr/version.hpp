#pragma once

#include <cstdint>

namespace scr {

// Library version. The same values are exported by CMake as ServiceClassRegistry_VERSION.
inline constexpr int kVersionMajor = 1;
inline constexpr int kVersionMinor = 0;
inline constexpr int kVersionPatch = 0;
inline constexpr const char* kVersionString = "1.0.0";
inline constexpr const char* kProductName = "Service Class Registry";
inline constexpr const char* kCopyrightLine = "Copyright 2026 Summon Software Labs.";

// On-disk / on-wire format versions. Bumping any of these is a breaking change
// that must be accompanied by an explicit migration decision.
inline constexpr std::uint16_t kFrameFormatVersion = 1;
inline constexpr std::uint16_t kStoreFormatVersion = 1;
inline constexpr std::uint8_t kCanonicalTextVersion = 1;

}  // namespace scr
