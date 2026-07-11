// src/dll/input/air_dash_macro.cpp
//
// Implementação crua e simples do Air Dash Macro.
//
// Lógica:
//   - Se jump_countdown > 0: estamos na fase de jump. Emite jump_dir
//     sozinho (sem botões) e decrementa. Quando chegar a 0, emite o
//     dash (dash_dir + AB) neste MESMO frame e volta pra idle.
//   - Se jump_countdown == 0 e o input é 9AB/7AB: trigger! Seta
//     jump_dir, jump_countdown = jump_frames, emite jump_dir sozinho.
//   - Caso contrário: pass-through do input cru.
//
// Retrigger: como não há estado "Suppressed", se o jogador continuar
// segurando 9AB depois do dash, no frame seguinte o trigger dispara
// de novo (jump_countdown já está em 0 e o input ainda é 9AB). Isso
// produz o efeito "segura 9AB e fica dando air dash repetido".

#include "air_dash_macro.hpp"
#include "game/addresses.hpp"

namespace caster::dll {

namespace {

// CC_BUTTON_A | CC_BUTTON_B | CC_BUTTON_AB = 0x0070
// No formato combinado (buttons << 4) = 0x0700
constexpr std::uint16_t kAbButtonsCombined =
    static_cast<std::uint16_t>((CC_BUTTON_A | CC_BUTTON_B | CC_BUTTON_AB) << 4);

} // namespace

void AirDashMacro::reset() {
    jump_countdown_ = 0;
    jump_dir_       = 0;
}

AirDashMacro::StepResult AirDashMacro::step(std::uint16_t raw_input) {
    if (!enabled_) return {raw_input, false};

    const std::uint8_t  dir  = static_cast<std::uint8_t>(raw_input & 0x000F);
    const std::uint16_t btns = static_cast<std::uint16_t>((raw_input >> 4) & 0x0FFF);

    // has_ab: botão AB dedicado OU A+B simultâneos.
    const bool has_ab = (btns & CC_BUTTON_AB) != 0
                       || (btns & (CC_BUTTON_A | CC_BUTTON_B)) == (CC_BUTTON_A | CC_BUTTON_B);

    // Determinar direção de dash a partir da direção de jump.
    // 9 (up-forward) -> 6 (forward), 7 (up-back) -> 4 (back).
    const auto dash_dir_for = [](std::uint8_t jdir) -> std::uint8_t {
        if (jdir == 9) return 6;
        if (jdir == 7) return 4;
        return 0;
    };

    // Fase de jump ativa: emite jump_dir sozinho e decrementa.
    if (jump_countdown_ > 0) {
        --jump_countdown_;
        if (jump_countdown_ > 0) {
            // Ainda em jump: só jump_dir, sem botões.
            return {static_cast<std::uint16_t>(jump_dir_), false};
        }
        // Countdown chegou a 0 neste frame: emite o DASH agora.
        const std::uint8_t dash_dir = dash_dir_for(jump_dir_);
        jump_dir_ = 0;
        // Não limpamos nada além disso — se 9AB continuar no próximo
        // frame, o trigger lá embaixo dispara de novo (retrigger).
        const std::uint16_t dash_input =
            static_cast<std::uint16_t>(dash_dir | kAbButtonsCombined);
        return {dash_input, false};
    }

    // Idle: detecta 9AB / 7AB.
    //
    // other_buttons: qualquer botão que não seja A/B/AB. Se houver,
    // não disparamos (deixa o input passar cru) — evita conflito com
    // outras ações.
    const std::uint16_t other_buttons = static_cast<std::uint16_t>(
        btns & ~(CC_BUTTON_A | CC_BUTTON_B | CC_BUTTON_AB));
    if (has_ab && (dir == 9 || dir == 7) && other_buttons == 0) {
        // Trigger!
        jump_dir_       = dir;
        jump_countdown_ = jump_frames_;
        // Neste frame emitimos o jump_dir sozinho (primeiro frame da
        // fase de jump). O dash sai depois de jump_frames_ frames.
        return {static_cast<std::uint16_t>(dir), true};
    }

    // Pass-through.
    return {raw_input, false};
}

} // namespace caster::dll
