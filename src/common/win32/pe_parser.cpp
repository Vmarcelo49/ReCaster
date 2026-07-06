// src/common/win32/pe_parser.cpp

#include "pe_parser.hpp"
#include "memory.hpp"
#include "../logger.hpp"

#include <cstring>

namespace caster::common::win32::pe_parser {

namespace {

#pragma pack(push, 1)
struct DosHeader {
    std::uint16_t e_magic;      // "MZ"
    std::uint16_t e_cblp;
    std::uint16_t e_cp;
    std::uint16_t e_crlc;
    std::uint16_t e_cparhdr;
    std::uint16_t e_minalloc;
    std::uint16_t e_maxalloc;
    std::uint16_t e_ss;
    std::uint16_t e_sp;
    std::uint16_t e_csum;
    std::uint16_t e_ip;
    std::uint16_t e_cs;
    std::uint16_t e_lfarlc;
    std::uint16_t e_ovno;
    std::uint16_t e_res[4];
    std::uint16_t e_oemid;
    std::uint16_t e_oeminfo;
    std::uint16_t e_res2[10];
    std::uint32_t e_lfanew;     // offset to PE header
};
static_assert(sizeof(DosHeader) == 64);

struct PeSignatureAndCoff {
    std::uint32_t signature;    // "PE\0\0"
    std::uint16_t machine;
    std::uint16_t number_of_sections;
    std::uint32_t time_date_stamp;
    std::uint32_t pointer_to_symbol_table;
    std::uint32_t number_of_symbols;
    std::uint16_t size_of_optional_header;
    std::uint16_t characteristics;
};
static_assert(sizeof(PeSignatureAndCoff) == 24);

// PE32 Optional Header (we only need the first ~24 bytes to get to
// AddressOfEntryPoint). The full struct is much larger.
struct OptionalHeaderStart {
    std::uint16_t magic;                   // 0x10b = PE32, 0x20b = PE32+
    std::uint8_t  major_linker_version;
    std::uint8_t  minor_linker_version;
    std::uint32_t size_of_code;
    std::uint32_t size_of_initialized_data;
    std::uint32_t size_of_uninitialized_data;
    std::uint32_t address_of_entry_point;  // RVA
    std::uint32_t base_of_code;
    // (more fields follow — we don't read past here)
};
static_assert(sizeof(OptionalHeaderStart) == 24);
#pragma pack(pop)

} // namespace

PeInfo read(process::ProcessHandle proc, std::uint32_t image_base) {
    PeInfo info;
    info.image_base = image_base;

    // 1. Read the DOS header (64 bytes) at image_base.
    DosHeader dos{};
    if (!memory::read_remote(proc, image_base, &dos, sizeof(dos))) {
        info.error_message = "PE: failed to read DOS header at 0x" +
                             std::to_string(image_base);
        return info;
    }
    if (dos.e_magic != 0x5A4D) {  // 'MZ' little-endian
        info.error_message = "PE: bad MZ magic (got 0x" +
                             std::to_string(dos.e_magic) + ")";
        return info;
    }

    // 2. Read PE signature + COFF header at e_lfanew offset.
    PeSignatureAndCoff pe{};
    std::uint32_t pe_offset = image_base + dos.e_lfanew;
    if (!memory::read_remote(proc, pe_offset, &pe, sizeof(pe))) {
        info.error_message = "PE: failed to read PE header at 0x" +
                             std::to_string(pe_offset);
        return info;
    }
    if (pe.signature != 0x00004550) {  // 'PE\0\0' little-endian
        info.error_message = "PE: bad PE signature (got 0x" +
                             std::to_string(pe.signature) + ")";
        return info;
    }
    if (pe.machine != 0x014c) {  // IMAGE_FILE_MACHINE_I386
        logger::warn("PE: unexpected machine=0x{:04x} (expected 0x014c i386)",
                     pe.machine);
    }

    // 3. Read the start of the optional header to get AddressOfEntryPoint.
    OptionalHeaderStart opt{};
    std::uint32_t opt_offset = pe_offset + sizeof(pe);
    if (!memory::read_remote(proc, opt_offset, &opt, sizeof(opt))) {
        info.error_message = "PE: failed to read optional header at 0x" +
                             std::to_string(opt_offset);
        return info;
    }
    if (opt.magic != 0x10b) {
        logger::warn("PE: not PE32 (magic=0x{:04x}, expected 0x010b)", opt.magic);
    }

    info.entry_point_rva = opt.address_of_entry_point;
    info.valid           = true;

    // SizeOfImage is at offset 56 inside the optional header (PE32). We
    // already read 24 bytes (OptionalHeaderStart), so we need to read 36
    // more bytes to reach SizeOfImage at the end. Layout of those 36 bytes:
    //   offset 24: BaseOfData (u32)
    //   offset 28: ImageBase  (u32)
    //   offset 32: SectionAlignment (u32)
    //   offset 36: FileAlignment (u32)
    //   offset 40: MajorOperatingSystemVersion (u16)
    //   offset 42: MinorOperatingSystemVersion (u16)
    //   offset 44: MajorImageVersion (u16)
    //   offset 46: MinorImageVersion (u16)
    //   offset 48: MajorSubsystemVersion (u16)
    //   offset 50: MinorSubsystemVersion (u16)
    //   offset 52: Win32VersionValue (u32)
    //   offset 56: SizeOfImage (u32)  ← what we want
#pragma pack(push, 1)
    struct OptMid {
        std::uint32_t base_of_data;
        std::uint32_t image_base_full;
        std::uint32_t section_alignment;
        std::uint32_t file_alignment;
        std::uint16_t os_ver_major;
        std::uint16_t os_ver_minor;
        std::uint16_t img_ver_major;
        std::uint16_t img_ver_minor;
        std::uint16_t sub_ver_major;
        std::uint16_t sub_ver_minor;
        std::uint32_t win32_version_value;
        std::uint32_t size_of_image;
    };
    static_assert(sizeof(OptMid) == 36);
#pragma pack(pop)

    OptMid mid{};
    if (memory::read_remote(proc, opt_offset + sizeof(opt),
                            &mid, sizeof(mid))) {
        info.image_size = mid.size_of_image;
    }

    return info;
}

} // namespace caster::common::win32::pe_parser
