// src/dll/asm_hacks.cpp
// Ported from CCCaster DllAsmHacks.cpp. Stripped: palette, trial, SFX, screenshot, saveReplay.

#include "asm_hacks.hpp"
#include "frame_rate.hpp"

#include <cstring>

namespace caster::dll::asm_hacks {

uint32_t currentMenuIndex = 0;
uint32_t menuConfirmState = 0;
uint32_t roundStartCounter = 0;
uint8_t  enableEscapeToExit = 1;

// ---- memwrite helper ----
static int memwrite(void* dst, const void* src, size_t len) {
    DWORD old, tmp;
    if (!VirtualProtect(dst, len, PAGE_READWRITE, &old))
        return GetLastError();
    memcpy(dst, src, len);
    if (!VirtualProtect(dst, len, old, &tmp))
        return GetLastError();
    return 0;
}

int Asm::write() const {
    backup.resize(bytes.size());
    memcpy(backup.data(), addr, backup.size());
    return memwrite(addr, bytes.data(), bytes.size());
}

int Asm::revert() const {
    return memwrite(addr, backup.data(), backup.size());
}

// ---- Present hook trampoline ----
void presentFuncCaller() {
    frame_rate::limitFPS();
}

__attribute__((naked, noinline)) void _naked_presentFuncCaller() {
    __asm__ __volatile__(
        "push %esp; push %ebp; push %edi; push %esi;"
        "push %edx; push %ecx; push %ebx; push %eax; push %ebp;"
        "mov %esp, %ebp;"
    );
    presentFuncCaller();
    __asm__ __volatile__(
        "pop %ebp; pop %eax; pop %ebx; pop %ecx; pop %edx;"
        "pop %esi; pop %edi; pop %ebp; pop %esp;"
        // Epilogue from 0x004bdd9d
        ".byte 0x5E;" // pop esi
        ".byte 0x5B;" // pop ebx
        ".byte 0x8B; .byte 0xe5;" // mov esp,ebp
        ".byte 0x5D;" // pop ebp
        ".byte 0xC2; .byte 0x04; .byte 0x00;" // ret 4
    );
}

// ---- ASM patch lists ----

const AsmList hookMainLoop = {
    { (void*)MM_HOOK_CALL1_ADDR, {
        0xE8, INLINE_DWORD((unsigned)&callback - MM_HOOK_CALL1_ADDR - 5),
        0xE9, INLINE_DWORD(MM_HOOK_CALL2_ADDR - MM_HOOK_CALL1_ADDR - 10)
    }},
    { (void*)MM_HOOK_CALL2_ADDR, {
        0x6A, 0x01, 0x6A, 0x00, 0x6A, 0x00,
        0xE9, INLINE_DWORD(CC_LOOP_START_ADDR - MM_HOOK_CALL2_ADDR - 5)
    }},
    { (void*)CC_LOOP_START_ADDR, {
        0xE9, INLINE_DWORD(MM_HOOK_CALL1_ADDR - CC_LOOP_START_ADDR - 5),
        0x90
    }},
};

const AsmList hijackControls = {
    { (void*)0x41F098, INLINE_NOP_TWO },
    { (void*)0x41F0A0, INLINE_NOP_THREE },
    { (void*)0x4A024E, INLINE_NOP_TWO },
    { (void*)0x4A027F, INLINE_NOP_THREE },
    { (void*)0x4A0291, INLINE_NOP_THREE },
    { (void*)0x4A02A2, INLINE_NOP_THREE },
    { (void*)0x4A02B4, INLINE_NOP_THREE },
    { (void*)0x4A02E9, INLINE_NOP_TWO },
    { (void*)0x4A02F2, INLINE_NOP_THREE },
    { (void*)0x54D2C0, {
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
    }},
};

const AsmList hijackMenu = {
    { (void*)0x4294D1, {
        0x8B, 0x7E, 0x40,
        0x89, 0x3D, INLINE_DWORD((unsigned)&currentMenuIndex),
        0xE9, 0xF1, 0x04, 0x00, 0x00
    }},
    { (void*)0x429817, {
        0x85, 0xC9,
        0xE9, 0xB3, 0xFC, 0xFF, 0xFF
    }},
    { (void*)0x4299CB, {
        0xE9, 0x47, 0xFE, 0xFF, 0xFF
    }},
    { (void*)0x428F52, {
        0x53, 0x51, 0x52,
        0x8D, 0x5C, 0x24, 0x30,
        0xB9, INLINE_DWORD((unsigned)&menuConfirmState),
        0xEB, 0x04
    }},
    { (void*)0x428F64, {
        0x83, 0x39, 0x01,
        0x8B, 0x13,
        0x89, 0x11,
        0x7F, 0x5B,
        0xEB, 0x55
    }},
    { (void*)0x428F7A, {
        0x5A, 0x59, 0x5B,
        0x90,
        0xEB, 0x0D
    }},
    { (void*)0x428FC4, {
        0x89, 0x03,
        0xEB, 0xB2,
        0x89, 0x01,
        0xEB, 0xAE
    }},
    { (void*)0x428F82, {
        0x81, 0x3C, 0x24, 0xF5, 0x99, 0x42, 0x00,
        0x75, 0x02,
        0xEB, 0xC5,
        0xC2, 0x04, 0x00
    }},
};

const AsmList detectRoundStart = {
    { (void*)0x440D16, {
        0xB9, INLINE_DWORD((unsigned)&roundStartCounter),
        0xE9, 0xE2, 0x02, 0x00, 0x00
    }},
    { (void*)0x441002, {
        0x8B, 0x31,
        0x46,
        0x89, 0x31,
        0x5E,
        0x59,
        0xC3
    }},
    { (void*)0x440CC5, {
        0xEB, 0x4F
    }},
};

const AsmList enableDisabledStages = {
    { (void*)0x54CEBC, INLINE_DWORD_FF }, { (void*)0x54CEC0, INLINE_DWORD_FF },
    { (void*)0x54CEC4, INLINE_DWORD_FF }, { (void*)0x54CFA8, INLINE_DWORD_FF },
    { (void*)0x54CFAC, INLINE_DWORD_FF }, { (void*)0x54CFB0, INLINE_DWORD_FF },
    { (void*)0x54CF68, INLINE_DWORD_FF }, { (void*)0x54CF6C, INLINE_DWORD_FF },
    { (void*)0x54CF70, INLINE_DWORD_FF }, { (void*)0x54CF74, INLINE_DWORD_FF },
    { (void*)0x54CF78, INLINE_DWORD_FF }, { (void*)0x54CF7C, INLINE_DWORD_FF },
    { (void*)0x54CF80, INLINE_DWORD_FF }, { (void*)0x54CF84, INLINE_DWORD_FF },
    { (void*)0x54CF88, INLINE_DWORD_FF }, { (void*)0x54CF8C, INLINE_DWORD_FF },
    { (void*)0x54CF90, INLINE_DWORD_FF }, { (void*)0x54CF94, INLINE_DWORD_FF },
    { (void*)0x54CF98, INLINE_DWORD_FF }, { (void*)0x54CF9C, INLINE_DWORD_FF },
    { (void*)0x54CFA0, INLINE_DWORD_FF }, { (void*)0x54CFA4, INLINE_DWORD_FF },
    // Fix Ryougi stage music
    { (void*)0x7695F6, { 0x35, 0x00, 0x00, 0x00 } },
    { (void*)0x7695EC, { 0xAA, 0xCC, 0x1E, 0x40 } },
};

const Asm multiWindow = { (void*)MULTIPLE_MELTY, { 0xEB } };

const Asm hijackEscapeKey = { (void*)0x4A0070, {
    0x80, 0x3D, INLINE_DWORD((unsigned)&enableEscapeToExit), 0x00,
    0xA0, INLINE_DWORD(0x5544F1),
    0x75, 0x03,
    0x66, 0x31, 0xC0,
    0x24, 0x80,
    0x33, 0xC9,
    0x3C, 0x80,
    0x0F, 0x94, 0xC1,
    0x8B, 0xC1,
    0xC3
}};

const Asm disableFpsLimit = { (void*)CC_PERF_FREQ_ADDR, {
    INLINE_DWORD(1), INLINE_DWORD(0)
}};

const Asm disableFpsCounter = { (void*)0x41FD43, INLINE_NOP_THREE };

const AsmList hookPresentCaller = {
    PATCHJUMP(0x004bdd4a, _naked_presentFuncCaller),
    PATCHJUMP(0x004bdd6c, _naked_presentFuncCaller),
    PATCHJUMP(0x004bdd9d, _naked_presentFuncCaller),
};

const Asm disableTrainingMusicReset = { (void*)0x472C6D, { 0xEB, 0x05 } };

const Asm fixBossStageSuperFlashOverlay = { (void*)0x53B3C8,
    { 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF } };

} // namespace caster::dll::asm_hacks
