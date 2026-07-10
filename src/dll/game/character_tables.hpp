// src/dll/game/character_tables.hpp
//
// Character ↔ selector mappings and name lookups for the MBAA roster.
// Ported directly from CCCaster's netplay/CharacterSelect.hpp.

#pragma once

#include <cstdint>

namespace caster::dll {

inline constexpr uint8_t RANDOM_CHARACTER      = 99;
inline constexpr uint8_t RANDOM_CHARA_SELECTOR = 49;
inline constexpr uint8_t UNKNOWN_POSITION      = 0xFF;

uint8_t    charaToSelector(uint8_t chara);
uint8_t    selectorToChara(uint8_t selector);
const char* getFullCharaName(uint8_t chara);
const char* getShortCharaName(uint8_t chara);

} // namespace caster::dll
