// src/common/version.hpp
//
// SINGLE SOURCE OF TRUTH for the project version (0.x.x). The whole
// project follows this one unified version — there is no separate
// "protocol version" or other version axis. Do not duplicate this value
// anywhere else.
//
// Used by:
//   - CMakeLists.txt (project VERSION, read at configure time; the build
//     fails if this header can't be parsed)
//   - the CLI help text and the startup log (exe)
//   - the netplay peer version exchange (session kLocalVersion — peers on
//     different app versions log a mismatch warning but proceed)
//
// Bump kAppVersion HERE when drafting a release — that is the only edit.
//
// Note: the target game's own version (MBAA.exe 1.07 Rev.1.4.0) is NOT a
// version we own; it is referenced only in user-facing strings.

#ifndef CASTER_VERSION_HPP
#define CASTER_VERSION_HPP

namespace caster::common::version {

inline constexpr const char* kAppVersion = "0.1.4";

}  // namespace caster::common::version

#endif  // CASTER_VERSION_HPP
