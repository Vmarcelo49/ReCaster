// src/common/win32/pe_parser.hpp
//
// Read the DOS+PE header from a remote process to extract the entry-point
// RVA. Used by the launcher for diagnostic logging — matches
// zzcaster's `launcher.zig` behavior of logging the entry point right
// after CreateProcessW(CREATE_SUSPENDED).

#pragma once

#include "process.hpp"

#include <cstdint>
#include <string>

namespace caster::common::win32::pe_parser {

struct PeInfo {
    bool          valid           = false;
    std::uint32_t image_base      = 0;     // typically 0x00400000 for x86
    std::uint32_t entry_point_rva = 0;     // relative virtual address
    std::uint32_t image_size      = 0;     // SizeOfImage from optional header
    std::string   error_message;
};

// Read the PE header of `proc` at the given `image_base`. The image base
// for a 32-bit exe is almost always 0x00400000, but we pass it in so this
// function works for any DLL or exe.
PeInfo read(process::ProcessHandle proc, std::uint32_t image_base);

} // namespace caster::common::win32::pe_parser
