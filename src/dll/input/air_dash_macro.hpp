// src/dll/input/air_dash_macro.hpp
//
// Air Dash Macro — versão crua e simples.
//
// Quando o jogador pressiona 9AB (ou 7AB):
//   - emite a direção de jump (9 ou 7) por `jump_frames` frames
//   - emite a direção de dash + AB (6 ou 4 + AB) por 1 frame
//
// Se o jogador continuar segurando 9AB (ou 7AB) depois do dash, o macro
// RETRIGGER: dispara a sequência de novo. Isso é o que diferencia do
// modelo anterior com "Suppressed" — agora não há lockout.
//
// Tudo é configurável via mapping.ini (air_dash_jump_frames).
// Defaults: jump_frames=5 (dá tempo do personagem ficar airborne).
//
// Formato do input (combined u16):
//   bits 0-3:   direção (numpad, 0=neutral, 9=UR, 7=UL, etc.)
//   bits 4-15:  bitmask de botões (CC_BUTTON_* << 4)

#pragma once

#include <cstdint>

namespace caster::dll {

class AirDashMacro {
public:
    // Default de jump frames. 6 foi o valor validado como consistente
    // no MBAACC via Wine no ReCaster. Ajustável via mapping.ini.
    static constexpr std::uint8_t kDefaultJumpFrames = 6;

    struct StepResult {
        std::uint16_t output;
        bool          triggered;  // true no frame em que a sequência começa
    };

    void setEnabled(bool enabled) { enabled_ = enabled; }
    bool isEnabled() const { return enabled_; }

    void setJumpFrames(std::uint8_t frames) {
        jump_frames_ = frames < 1 ? 1 : frames;
    }
    std::uint8_t jumpFrames() const { return jump_frames_; }

    void reset();

    StepResult step(std::uint16_t raw_input);

private:
    bool         enabled_      = false;
    std::uint8_t jump_frames_  = kDefaultJumpFrames;
    // Contador de frames restantes na fase de jump. 0 = idle.
    // Quando chega a 0 e o jogador ainda está com 9AB, emite o dash e
    // (se 9AB continuar) retrigger imediatamente na próxima chamada.
    std::uint8_t jump_countdown_ = 0;
    // Direção de jump gravada no trigger (9 ou 7). 0 quando idle.
    std::uint8_t jump_dir_       = 0;
};

} // namespace caster::dll
