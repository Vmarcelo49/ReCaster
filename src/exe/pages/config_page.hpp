// src/exe/pages/config_page.hpp
//
// The "Game Config" page — player profile, match rules, network settings.
// Phase 3: placeholder. Phase 7 will add real InputText fields + Apply
// buttons that call config::save().

#pragma once

#include "../../common/config.hpp"

namespace caster::exe::pages::config_page {

void draw(const caster::common::config::Config& cfg);

} // namespace caster::exe::pages::config_page
