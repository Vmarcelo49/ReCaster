// src/common/controller/mapping.cpp

#include "mapping.hpp"
#include "../ini.hpp"
#include "../logger.hpp"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>
#include <string_view>

namespace caster::common::controller {

namespace {

// Hat direction codes (numpad layout). 5 = center (never bound).
//   1=DL  2=D  3=DR
//   4=L   5=?  6=R
//   7=UL  8=U  9=UR
struct HatDir {
    std::uint8_t code;
    const char* label;
};
constexpr HatDir kHatDirs[] = {
    {1, "DL"}, {2, "D"}, {3, "DR"},
    {4, "L"},  {5, "?"}, {6, "R"},
    {7, "UL"}, {8, "U"}, {9, "UR"},
};

const char* hat_dir_label(std::uint8_t dir) {
    for (const auto& d : kHatDirs) {
        if (d.code == dir) return d.label;
    }
    return "?";
}

// Map a Windows Virtual Key code to a human-readable label.
// The `index` field of an InputBinding with type=KeyboardKey stores a VK
// code (0x08..0xFE). We convert the common ones to friendly names; for
// anything unmapped, we fall back to "VK 0x%02X" so the user still sees
// something identifiable.
//
// This is a static table — no Win32 API calls (works the same on Wine
// and native Windows).
std::string keyboard_key_label(std::uint16_t vk) {
    // Letters A-Z (0x41-0x5A) → single uppercase letter.
    if (vk >= 0x41 && vk <= 0x5A) {
        return std::string(1, static_cast<char>('A' + (vk - 0x41)));
    }
    // Digits 0-9 (0x30-0x39) → single digit.
    if (vk >= 0x30 && vk <= 0x39) {
        return std::string(1, static_cast<char>('0' + (vk - 0x30)));
    }

    // Named keys — ordered by VK code for easy lookup.
    struct VkName { std::uint16_t code; const char* name; };
    static constexpr VkName kNames[] = {
        {0x08, "BACK"},
        {0x09, "TAB"},
        {0x0D, "ENTER"},
        {0x10, "SHIFT"},
        {0x11, "CTRL"},
        {0x12, "ALT"},
        {0x13, "PAUSE"},
        {0x14, "CAPS"},
        {0x1B, "ESC"},
        {0x20, "SPACE"},
        {0x21, "PGUP"},
        {0x22, "PGDN"},
        {0x23, "END"},
        {0x24, "HOME"},
        {0x25, "LEFT"},
        {0x26, "UP"},
        {0x27, "RIGHT"},
        {0x28, "DOWN"},
        {0x2C, "PRTSC"},
        {0x2D, "INS"},
        {0x2E, "DEL"},
        // F1-F24
        {0x70, "F1"},  {0x71, "F2"},  {0x72, "F3"},  {0x73, "F4"},
        {0x74, "F5"},  {0x75, "F6"},  {0x76, "F7"},  {0x77, "F8"},
        {0x78, "F9"},  {0x79, "F10"}, {0x7A, "F11"}, {0x7B, "F12"},
        {0x7C, "F13"}, {0x7D, "F14"}, {0x7E, "F15"}, {0x7F, "F16"},
        {0x80, "F17"}, {0x81, "F18"}, {0x82, "F19"}, {0x83, "F20"},
        {0x84, "F21"}, {0x85, "F22"}, {0x86, "F23"}, {0x87, "F24"},
        // Numpad
        {0x60, "NUM0"}, {0x61, "NUM1"}, {0x62, "NUM2"}, {0x63, "NUM3"},
        {0x64, "NUM4"}, {0x65, "NUM5"}, {0x66, "NUM6"}, {0x67, "NUM7"},
        {0x68, "NUM8"}, {0x69, "NUM9"},
        {0x6A, "NUM*"}, {0x6B, "NUM+"}, {0x6D, "NUM-"},
        {0x6E, "NUM."}, {0x6F, "NUM/"},
        // Modifier keys (left/right)
        {0xA0, "LSHIFT"}, {0xA1, "RSHIFT"},
        {0xA2, "LCTRL"},  {0xA3, "RCTRL"},
        {0xA4, "LALT"},   {0xA5, "RALT"},
        // Misc
        {0xBA, ";"},   {0xBB, "="},   {0xBC, ","},
        {0xBD, "-"},   {0xBE, "."},   {0xBF, "/"},
        {0xC0, "`"},   {0xDB, "["},   {0xDC, "\\"},
        {0xDD, "]"},   {0xDE, "'"},
    };
    for (const auto& n : kNames) {
        if (n.code == vk) return n.name;
    }

    // Fallback for anything unmapped.
    char buf[16];
    std::snprintf(buf, sizeof(buf), "VK 0x%02X", vk);
    return buf;
}

} // namespace

std::string InputBinding::label() const {
    char buf[32];
    switch (type) {
        case InputType::None:
            return "\xe2\x80\x94";  // em-dash UTF-8
        case InputType::SdlButton:
            std::snprintf(buf, sizeof(buf), "Btn %u", index);
            return buf;
        case InputType::SdlAxisPos:
            std::snprintf(buf, sizeof(buf), "Ax %u+", index);
            return buf;
        case InputType::SdlAxisNeg:
            std::snprintf(buf, sizeof(buf), "Ax %u-", index);
            return buf;
        case InputType::SdlHat: {
            const std::uint8_t hat_idx = hat_index(index);
            const std::uint8_t dir     = hat_direction(index);
            std::snprintf(buf, sizeof(buf), "Hat%u %s", hat_idx,
                          hat_dir_label(dir));
            return buf;
        }
        case InputType::KeyboardKey:
            return keyboard_key_label(index);
    }
    return "?";
}

std::string InputBinding::serialize() const {
    char buf[32];
    switch (type) {
        case InputType::None:        return "none";
        case InputType::SdlButton:
            std::snprintf(buf, sizeof(buf), "btn:%u", index);
            return buf;
        case InputType::SdlAxisPos:
            std::snprintf(buf, sizeof(buf), "axp:%u", index);
            return buf;
        case InputType::SdlAxisNeg:
            std::snprintf(buf, sizeof(buf), "axn:%u", index);
            return buf;
        case InputType::SdlHat: {
            const std::uint8_t hat_idx = hat_index(index);
            const std::uint8_t dir     = hat_direction(index);
            std::snprintf(buf, sizeof(buf), "hat:%u:%u", hat_idx, dir);
            return buf;
        }
        case InputType::KeyboardKey:
            std::snprintf(buf, sizeof(buf), "key:%u", index);
            return buf;
    }
    return "none";
}

InputBinding InputBinding::parse(const std::string& str) {
    InputBinding out;  // defaults to None

    if (str.empty() || str == "none") return out;

    auto colon = str.find(':');
    if (colon == std::string::npos) return out;

    std::string prefix = str.substr(0, colon);
    std::string rest   = str.substr(colon + 1);

    try {
        if (prefix == "btn") {
            const auto idx = static_cast<std::uint16_t>(std::stoul(rest));
            out.type  = InputType::SdlButton;
            out.index = idx;
        } else if (prefix == "axp") {
            const auto idx = static_cast<std::uint16_t>(std::stoul(rest));
            out.type  = InputType::SdlAxisPos;
            out.index = idx;
        } else if (prefix == "axn") {
            const auto idx = static_cast<std::uint16_t>(std::stoul(rest));
            out.type  = InputType::SdlAxisNeg;
            out.index = idx;
        } else if (prefix == "key") {
            const auto idx = static_cast<std::uint16_t>(std::stoul(rest));
            out.type  = InputType::KeyboardKey;
            out.index = idx;
        } else if (prefix == "hat") {
            // hat:<hat_idx>:<direction>
            auto colon2 = rest.find(':');
            if (colon2 == std::string::npos) return out;
            const std::uint8_t hat_idx = static_cast<std::uint8_t>(
                std::stoul(rest.substr(0, colon2)));
            const std::uint8_t dir = static_cast<std::uint8_t>(
                std::stoul(rest.substr(colon2 + 1)));
            out.type  = InputType::SdlHat;
            out.index = pack_hat(hat_idx, dir);
        }
        // Unknown prefix → return None.
    } catch (...) {
        // stoul threw — return None. Make sure `out` is reset to defaults
        // in case we set type before the throw (defensive; the code above
        // parses the number before setting type, but be safe).
        out = InputBinding{};
    }
    return out;
}

ControllerMapping ControllerMapping::default_xbox() {
    ControllerMapping m;
    m.a     = {InputType::SdlButton, 2};   // X (blue)
    m.b     = {InputType::SdlButton, 3};   // Y (yellow)
    m.c     = {InputType::SdlButton, 1};   // B (red)
    m.d     = {InputType::SdlButton, 0};   // A (green)
    m.e     = {InputType::SdlButton, 4};   // LB
    m.ab    = {InputType::SdlButton, 5};   // RB (A+B macro)
    m.start = {InputType::SdlButton, 7};   // Start
    m.fn1   = {InputType::SdlButton, 6};   // Back/Select
    m.fn2   = {InputType::SdlButton, 9};   // R-stick press
    m.up    = {InputType::SdlHat, InputBinding::pack_hat(0, 8)};
    m.down  = {InputType::SdlHat, InputBinding::pack_hat(0, 2)};
    m.left  = {InputType::SdlHat, InputBinding::pack_hat(0, 4)};
    m.right = {InputType::SdlHat, InputBinding::pack_hat(0, 6)};
    m.stick_x_axis    = 0;
    m.stick_y_axis    = 1;
    m.deadzone        = 8000;
    m.socd_mode       = SocdMode::LrNeutralize;
    m.device_index    = 0;
    m.air_dash_macro  = false;
    m.air_dash_jump_frames  = 6;
    m.air_dash_prep_frames  = 1;
    m.air_dash_pulse_frames = 1;
    return m;
}

ControllerMapping ControllerMapping::cleared_bindings() const {
    ControllerMapping out = *this;
    out.a     = {};
    out.b     = {};
    out.c     = {};
    out.d     = {};
    out.e     = {};
    out.ab    = {};
    out.start = {};
    out.fn1   = {};
    out.fn2   = {};
    out.up    = {};
    out.down  = {};
    out.left  = {};
    out.right = {};
    // stick_x_axis, stick_y_axis, deadzone, socd_mode,
    // air_dash_macro, air_dash_*_frames, device_index are preserved.
    return out;
}

InputBinding* ControllerMapping::binding(BindingTarget t) {
    switch (t) {
        case BindingTarget::A:     return &a;
        case BindingTarget::B:     return &b;
        case BindingTarget::C:     return &c;
        case BindingTarget::D:     return &d;
        case BindingTarget::E:     return &e;
        case BindingTarget::AB:    return &ab;
        case BindingTarget::Start: return &start;
        case BindingTarget::FN1:   return &fn1;
        case BindingTarget::FN2:   return &fn2;
        case BindingTarget::Up:    return &up;
        case BindingTarget::Down:  return &down;
        case BindingTarget::Left:  return &left;
        case BindingTarget::Right: return &right;
        case BindingTarget::None:  return nullptr;
    }
    return nullptr;
}

const InputBinding* ControllerMapping::binding(BindingTarget t) const {
    return const_cast<ControllerMapping*>(this)->binding(t);
}

namespace {

// Section name per player ("Player1" / "Player2").
void save_player(ini::Document& doc, const char* section,
                 const ControllerMapping& m) {
    doc.set(section, "device",         std::to_string(m.device_index));
    doc.set(section, "a",              m.a.serialize());
    doc.set(section, "b",              m.b.serialize());
    doc.set(section, "c",              m.c.serialize());
    doc.set(section, "d",              m.d.serialize());
    doc.set(section, "e",              m.e.serialize());
    doc.set(section, "ab",             m.ab.serialize());
    doc.set(section, "start",          m.start.serialize());
    doc.set(section, "fn1",            m.fn1.serialize());
    doc.set(section, "fn2",            m.fn2.serialize());
    doc.set(section, "up",             m.up.serialize());
    doc.set(section, "down",           m.down.serialize());
    doc.set(section, "left",           m.left.serialize());
    doc.set(section, "right",          m.right.serialize());
    doc.set(section, "stick_x",        std::to_string(m.stick_x_axis));
    doc.set(section, "stick_y",        std::to_string(m.stick_y_axis));
    doc.set(section, "deadzone",       std::to_string(m.deadzone));
    doc.set(section, "socd",           std::to_string(static_cast<int>(m.socd_mode)));
    doc.set(section, "air_dash_macro", m.air_dash_macro ? "1" : "0");
    doc.set(section, "air_dash_jump_frames",  std::to_string(m.air_dash_jump_frames));
    doc.set(section, "air_dash_prep_frames",  std::to_string(m.air_dash_prep_frames));
    doc.set(section, "air_dash_pulse_frames", std::to_string(m.air_dash_pulse_frames));
}

void load_player(const ini::Document& doc, const char* section,
                 ControllerMapping& m) {
    m.device_index = static_cast<int>(
        doc.getInt(section, "device", m.device_index));

    // Bindings — only overwrite if the key exists.
    if (auto* v = doc.get(section, "a"))     m.a     = InputBinding::parse(*v);
    if (auto* v = doc.get(section, "b"))     m.b     = InputBinding::parse(*v);
    if (auto* v = doc.get(section, "c"))     m.c     = InputBinding::parse(*v);
    if (auto* v = doc.get(section, "d"))     m.d     = InputBinding::parse(*v);
    if (auto* v = doc.get(section, "e"))     m.e     = InputBinding::parse(*v);
    if (auto* v = doc.get(section, "ab"))    m.ab    = InputBinding::parse(*v);
    if (auto* v = doc.get(section, "start")) m.start = InputBinding::parse(*v);
    if (auto* v = doc.get(section, "fn1"))   m.fn1   = InputBinding::parse(*v);
    if (auto* v = doc.get(section, "fn2"))   m.fn2   = InputBinding::parse(*v);
    if (auto* v = doc.get(section, "up"))    m.up    = InputBinding::parse(*v);
    if (auto* v = doc.get(section, "down"))  m.down  = InputBinding::parse(*v);
    if (auto* v = doc.get(section, "left"))  m.left  = InputBinding::parse(*v);
    if (auto* v = doc.get(section, "right")) m.right = InputBinding::parse(*v);

    m.stick_x_axis = static_cast<std::uint8_t>(
        doc.getInt(section, "stick_x", m.stick_x_axis));
    m.stick_y_axis = static_cast<std::uint8_t>(
        doc.getInt(section, "stick_y", m.stick_y_axis));
    m.deadzone = static_cast<std::uint32_t>(
        doc.getInt(section, "deadzone", m.deadzone));
    int socd = static_cast<int>(doc.getInt(section, "socd",
                                            static_cast<int>(m.socd_mode)));
    if (socd == 0) socd = 1;  // normalize invalid → L+R
    m.socd_mode = static_cast<SocdMode>(socd);
    m.air_dash_macro = doc.getBool(section, "air_dash_macro",
                                    m.air_dash_macro);
    m.air_dash_jump_frames  = static_cast<std::uint8_t>(
        std::clamp<long long>(
            doc.getInt(section, "air_dash_jump_frames",  m.air_dash_jump_frames),
            1, 30));
    m.air_dash_prep_frames  = static_cast<std::uint8_t>(
        std::clamp<long long>(
            doc.getInt(section, "air_dash_prep_frames",  m.air_dash_prep_frames),
            0, 30));
    m.air_dash_pulse_frames = static_cast<std::uint8_t>(
        std::clamp<long long>(
            doc.getInt(section, "air_dash_pulse_frames", m.air_dash_pulse_frames),
            1, 30));
}

} // namespace

bool save_mapping(const std::string& path,
                  const ControllerMapping& p1,
                  const ControllerMapping& p2) {
    // Make sure the parent directory exists.
    std::error_code ec;
    std::filesystem::path p(path);
    if (p.has_parent_path()) {
        std::filesystem::create_directories(p.parent_path(), ec);
        // Ignore ec — we'll fail at writeFile if we can't write.
    }

    ini::Document doc;
    save_player(doc, "Player1", p1);
    save_player(doc, "Player2", p2);

    if (!ini::writeFile(path, doc)) {
        logger::err("controller: failed to save mapping to {}", path);
        return false;
    }
    logger::info("controller: mapping saved to {}", path);
    return true;
}

bool load_mapping(const std::string& path,
                  ControllerMapping& p1,
                  ControllerMapping& p2) {
    ini::Document doc;
    if (!ini::loadFile(path, doc)) {
        logger::info("controller: mapping file not found at {}", path);
        return false;
    }
    load_player(doc, "Player1", p1);
    load_player(doc, "Player2", p2);
    logger::info("controller: mapping loaded from {}", path);
    return true;
}

} // namespace caster::common::controller
