// src/common/controller/mapping.cpp

#include "mapping.hpp"
#include "../ini.hpp"
#include "../logger.hpp"

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
            std::snprintf(buf, sizeof(buf), "Key 0x%02x", index);
            return buf;
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
    m.a     = {InputType::SdlButton, 2};   // X (green)
    m.b     = {InputType::SdlButton, 1};   // B (red)
    m.c     = {InputType::SdlButton, 3};   // Y (yellow)
    m.d     = {InputType::SdlButton, 0};   // A (blue)
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
    // air_dash_macro, device_index are preserved.
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
