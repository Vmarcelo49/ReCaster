// src/dll/game/character_tables.cpp
//
// Ported directly from CCCaster's netplay/CharacterSelect.cpp.

#include "character_tables.hpp"

namespace caster::dll {

uint8_t charaToSelector(uint8_t chara) {
    switch (chara) {
        // First row
        case 22: return  2; // Aoko
        case  7: return  3; // Tohno
        case 51: return  4; // Hime
        case 15: return  5; // Nanaya
        case 28: return  6; // Kouma
        // Second row
        case  8: return 10; // Miyako
        case  2: return 11; // Ciel
        case  0: return 12; // Sion
        case 30: return 13; // Ries
        case 11: return 14; // V.Sion
        case  9: return 15; // Wara
        case 31: return 16; // Roa
        // Third row
        case  4: return 19; // Maids
        case  3: return 20; // Akiha
        case  1: return 21; // Arc
        case 19: return 22; // P.Ciel
        case 12: return 23; // Warc
        case 13: return 24; // V.Akiha
        case 14: return 25; // M.Hisui
        // Fourth row
        case 29: return 28; // S.Akiha
        case 17: return 29; // Satsuki
        case 18: return 30; // Len
        case 33: return 31; // Ryougi
        case 23: return 32; // W.Len
        case 10: return 33; // Nero
        case 25: return 34; // NAC
        // Fifth row
        case 35: return 38; // KohaMech
        case  5: return 39; // Hisui
        case 20: return 40; // Neko
        case  6: return 41; // Kohaku
        case 34: return 42; // NekoMech
        // Last row
        case RANDOM_CHARACTER: return RANDOM_CHARA_SELECTOR;
    }
    return UNKNOWN_POSITION;
}

const char* getFullCharaName(uint8_t chara) {
    switch (chara) {
        case 22: return "Aozaki Aoko";
        case  7: return "Tohno Shiki";
        case 51: return "Archetype:Earth";
        case 15: return "Nanaya Shiki";
        case 28: return "Kishima Kouma";
        case  8: return "Miyako Arima";
        case  2: return "Ciel";
        case  0: return "Sion Eltnam Atlasia";
        case 30: return "Riesbyfe Strideberg";
        case 11: return "Sion TATARI";
        case  9: return "Warachia";
        case 31: return "Michael Roa Valdamjong";
        case  4: return "Hisui & Kohaku";
        case  3: return "Tohno Akiha";
        case  1: return "Arcuied Brunestud";
        case 19: return "Powered Ciel";
        case 12: return "Red Arcuied";
        case 13: return "Akiha Vermillion";
        case 14: return "Mech-Hisui";
        case 29: return "Seifuku Akiha";
        case 17: return "Yumizuka Satsuki";
        case 18: return "Len";
        case 33: return "Ryougi Shiki";
        case 23: return "White Len";
        case 10: return "Nero Chaos";
        case 25: return "Neko Arc Chaos";
        case 35: return "Koha & Mech";
        case  5: return "Hisui";
        case 20: return "Neko Arc";
        case  6: return "Kohaku";
        case 34: return "Neko & Mech";
        case RANDOM_CHARACTER: return "Random";
    }
    return "Unknown!";
}

} // namespace caster::dll
