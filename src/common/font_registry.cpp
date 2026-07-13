// src/common/font_registry.cpp

#include "font_registry.hpp"

namespace caster::common::font_registry {

namespace {
ImFont* g_body    = nullptr;
ImFont* g_body_sm = nullptr;
ImFont* g_body_lg = nullptr;
ImFont* g_mono    = nullptr;
} // namespace

void set(ImFont* body, ImFont* body_sm, ImFont* body_lg, ImFont* mono) {
    g_body    = body;
    g_body_sm = body_sm;
    g_body_lg = body_lg;
    g_mono    = mono;
}

ImFont* body()        { return g_body; }
ImFont* body_sm()     { return g_body_sm; }
ImFont* body_lg()     { return g_body_lg; }
ImFont* mono()        { return g_mono; }

} // namespace caster::common::font_registry
