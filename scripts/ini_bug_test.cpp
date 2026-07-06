// Mini test to isolate the INI section parsing bug.
#include "ini.hpp"
#include <cstdio>
#include <iostream>

int main() {
    caster::common::ini::Document doc;
    std::string text = R"([player]
display_name = Bob

[match]
versus_win_count = 3
)";
    std::cout << "=== INPUT ===\n" << text << "=== END ===\n\n";

    doc.parse(text);
    std::string dumped = doc.dump();
    std::cout << "=== DUMP ===\n" << dumped << "=== END ===\n\n";

    // Inspect section_order_ indirectly by what's in the doc.
    std::cout << "Sections present:\n";
    if (doc.get("player", "display_name")) {
        std::cout << "  [player].display_name = " << *doc.get("player", "display_name") << "\n";
    } else {
        std::cout << "  [player].display_name = MISSING\n";
    }
    if (doc.get("match", "versus_win_count")) {
        std::cout << "  [match].versus_win_count = " << *doc.get("match", "versus_win_count") << "\n";
    } else {
        std::cout << "  [match].versus_win_count = MISSING\n";
    }
    return 0;
}
