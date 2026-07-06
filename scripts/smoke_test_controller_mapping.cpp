// Smoke test for controller_mapping: serialize → deserialize round-trip.
#include "controller/mapping.hpp"
#include "logger.hpp"

#include <cstdio>
#include <fstream>
#include <iterator>
#include <filesystem>
#include <string>

static int failures = 0;
void check(bool ok, const char* label) {
    std::printf("%s %s\n", ok ? "  OK   " : "  FAIL ", label);
    if (!ok) ++failures;
}

int main() {
    namespace cm = caster::common::controller;
    namespace lg = caster::common::logger;
    lg::init({}, true);

    // 1. InputBinding serialize/parse round-trip
    {
        struct Case {
            cm::InputBinding b;
            const char* expected_serialized;
            const char* expected_label;
        };
        Case cases[] = {
            {{cm::InputType::None,        0},   "none",      "\xe2\x80\x94"},
            {{cm::InputType::SdlButton,   2},   "btn:2",     "Btn 2"},
            {{cm::InputType::SdlAxisPos,  4},   "axp:4",     "Ax 4+"},
            {{cm::InputType::SdlAxisNeg,  1},   "axn:1",     "Ax 1-"},
            {{cm::InputType::SdlHat,      cm::InputBinding::pack_hat(0, 8)}, "hat:0:8", "Hat0 U"},
            {{cm::InputType::SdlHat,      cm::InputBinding::pack_hat(1, 2)}, "hat:1:2", "Hat1 D"},
            {{cm::InputType::KeyboardKey, 0x20}, "key:32",   "Key 0x20"},
        };
        for (const auto& c : cases) {
            std::string s = c.b.serialize();
            check(s == c.expected_serialized,
                  (std::string("serialize ") + c.expected_serialized).c_str());
            std::string lbl = c.b.label();
            check(lbl == c.expected_label,
                  (std::string("label ") + c.expected_label).c_str());
            cm::InputBinding parsed = cm::InputBinding::parse(s);
            check(parsed == c.b,
                  (std::string("parse round-trip ") + c.expected_serialized).c_str());
        }
    }

    // 2. Parse invalid strings → None
    {
        cm::InputBinding b = cm::InputBinding::parse("garbage");
        check(b.type == cm::InputType::None, "parse garbage → None");
        b = cm::InputBinding::parse("");
        check(b.type == cm::InputType::None, "parse empty → None");
        b = cm::InputBinding::parse("btn:abc");
        check(b.type == cm::InputType::None, "parse btn:abc → None");
    }

    // 3. default_xbox() produces expected defaults
    {
        cm::ControllerMapping m = cm::ControllerMapping::default_xbox();
        check(m.a.type == cm::InputType::SdlButton && m.a.index == 2,
              "default_xbox: a = btn:2");
        check(m.b.type == cm::InputType::SdlButton && m.b.index == 1,
              "default_xbox: b = btn:1");
        check(m.up.type == cm::InputType::SdlHat &&
              cm::InputBinding::hat_direction(m.up.index) == 8,
              "default_xbox: up = hat dir 8");
        check(m.deadzone == 8000, "default_xbox: deadzone = 8000");
        check(m.socd_mode == cm::SocdMode::LrNeutralize,
              "default_xbox: socd = LrNeutralize");
        check(m.device_index == 0, "default_xbox: device_index = 0");
        check(!m.air_dash_macro, "default_xbox: air_dash_macro = false");
    }

    // 4. cleared_bindings() preserves config but clears actions
    {
        cm::ControllerMapping m = cm::ControllerMapping::default_xbox();
        m.deadzone = 12000;
        m.socd_mode = cm::SocdMode::BothNeutralize;
        m.device_index = 2;
        m.air_dash_macro = true;
        cm::ControllerMapping cleared = m.cleared_bindings();
        check(cleared.a.type == cm::InputType::None, "cleared: a = None");
        check(cleared.up.type == cm::InputType::None, "cleared: up = None");
        check(cleared.deadzone == 12000, "cleared: deadzone preserved");
        check(cleared.socd_mode == cm::SocdMode::BothNeutralize,
              "cleared: socd preserved");
        check(cleared.device_index == 2, "cleared: device_index preserved");
        check(cleared.air_dash_macro, "cleared: air_dash_macro preserved");
    }

    // 5. binding() accessor
    {
        cm::ControllerMapping m;
        cm::InputBinding* p = m.binding(cm::BindingTarget::A);
        check(p != nullptr, "binding(A) returns ptr");
        check(p == &m.a, "binding(A) returns &a");
        check(m.binding(cm::BindingTarget::None) == nullptr,
              "binding(None) returns nullptr");
    }

    // 6. Save/load round-trip
    {
        cm::ControllerMapping p1 = cm::ControllerMapping::default_xbox();
        cm::ControllerMapping p2 = cm::ControllerMapping::default_xbox();
        p1.device_index = 0;
        p1.deadzone = 6000;
        p2.device_index = -1;  // keyboard
        p2.b = {cm::InputType::KeyboardKey, 0x20};
        p2.air_dash_macro = true;

        auto path = std::filesystem::temp_directory_path() / "test-mapping.ini";
        std::filesystem::remove(path);

        check(cm::save_mapping(path.string(), p1, p2),
              "save_mapping succeeded");

        cm::ControllerMapping loaded_p1, loaded_p2;
        // Start from defaults so we know the load actually overwrites.
        loaded_p1 = cm::ControllerMapping::default_xbox();
        loaded_p2 = cm::ControllerMapping::default_xbox();
        check(cm::load_mapping(path.string(), loaded_p1, loaded_p2),
              "load_mapping succeeded");

        check(loaded_p1.device_index == 0, "loaded p1 device_index = 0");
        check(loaded_p1.deadzone == 6000, "loaded p1 deadzone = 6000");
        check(loaded_p2.device_index == -1, "loaded p2 device_index = -1");
        check(loaded_p2.b.type == cm::InputType::KeyboardKey &&
              loaded_p2.b.index == 0x20,
              "loaded p2 b = key:32");
        check(loaded_p2.air_dash_macro, "loaded p2 air_dash_macro = true");
    }

    // 7. INI format is human-readable
    {
        cm::ControllerMapping p1 = cm::ControllerMapping::default_xbox();
        cm::ControllerMapping p2;
        auto path = std::filesystem::temp_directory_path() / "test-mapping-format.ini";
        std::filesystem::remove(path);
        cm::save_mapping(path.string(), p1, p2);

        std::ifstream f(path);
        std::string content((std::istreambuf_iterator<char>(f)),
                             std::istreambuf_iterator<char>());
        check(content.find("[Player1]") != std::string::npos,
              "INI has [Player1] section");
        check(content.find("[Player2]") != std::string::npos,
              "INI has [Player2] section");
        check(content.find("a=btn:2") != std::string::npos,
              "INI has a=btn:2 (default Xbox)");
        check(content.find("up=hat:0:8") != std::string::npos,
              "INI has up=hat:0:8");
        check(content.find("device=0") != std::string::npos,
              "INI has device=0");
        check(content.find("deadzone=8000") != std::string::npos,
              "INI has deadzone=8000");
        check(content.find("socd=1") != std::string::npos,
              "INI has socd=1");
    }

    lg::shutdown();
    std::printf("\n");
    if (failures == 0) {
        std::printf("ALL CONTROLLER MAPPING TESTS PASSED\n");
        return 0;
    } else {
        std::printf("%d TESTS FAILED\n", failures);
        return 1;
    }
}
