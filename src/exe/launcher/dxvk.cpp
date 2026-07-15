// src/exe/launcher/dxvk.cpp
//
// DXVK integration — implementation. See dxvk.hpp for the design.
//
// All functions run on the GameRunner worker thread (Layer 2). No
// threading, no mutexes — the only shared state is the
// `g_vulkan_checked` / `g_vulkan_available` pair in the anonymous
// namespace, which is only ever touched from the game runner worker.

#include "dxvk.hpp"
#include "../../common/logger.hpp"
#include "../../common/win32/env.hpp"

#ifndef NOMINMAX
#  define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#  define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <filesystem>
#include <fstream>

namespace fs = std::filesystem;

namespace caster::exe::launcher::dxvk {

namespace {

// Cached result of is_vulkan_available(). The check involves a
// LoadLibraryA + vkCreateInstance round-trip (~10-50ms), so we cache
// it for the lifetime of the launcher process. Multiple game launches
// in the same session only pay the cost once.
//
// Both variables are only ever read/written from the game runner worker
// thread — no atomic needed. (If we ever get a second caller, e.g. the
// config page wanting to show "Vulkan: detected" in the UI, we'll need
// to make these atomic. Until then, YAGNI.)
bool g_vulkan_checked    = false;
bool g_vulkan_available  = false;

// Minimal Vulkan types we need to call vkCreateInstance without pulling
// in the full vulkan.h header. We're only testing whether the loader
// + ICD are functional — we don't need the actual VkInstance.
//
// These typedefs match the official vulkan.h signatures.
using PFN_vkCreateInstance = std::uint64_t (*)(
    const void* /*pCreateInfo*/,
    const void* /*pAllocator*/,
    void* /*pInstance*/);
using PFN_vkDestroyInstance = void (*)(
    void* /*instance*/,
    const void* /*pAllocator*/);

// VkInstanceCreateInfo — minimal layout for the no-op instance creation.
// Field layout matches vulkan.h exactly (we only fill the first 4 fields;
// the rest are pointers we leave null).
struct VkInstanceCreateInfo {
    std::uint32_t sType;
    const void*   pNext;
    std::uint32_t flags;
    const void*   pApplicationInfo;
    std::uint32_t enabledLayerCount;
    const char* const* ppEnabledLayerNames;
    std::uint32_t enabledExtensionCount;
    const char* const* ppEnabledExtensionNames;
};

// VkApplicationInfo — minimal layout.
struct VkApplicationInfo {
    std::uint32_t sType;
    const void*   pNext;
    const char*   pApplicationName;
    std::uint32_t applicationVersion;
    const char*   pEngineName;
    std::uint32_t engineVersion;
    std::uint32_t apiVersion;
};

constexpr std::uint32_t VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO = 1;
constexpr std::uint32_t VK_STRUCTURE_TYPE_APPLICATION_INFO     = 0;
constexpr std::uint32_t VK_API_VERSION_1_0 = (1u << 22) | (0u << 12) | 0u;  // variant=1 major=1 minor=0 patch=0

// Try to create + immediately destroy a VkInstance. Returns true if
// the call succeeds (Vulkan loader + at least one ICD are functional).
bool try_create_vk_instance() {
    HMODULE vulkan = LoadLibraryA("vulkan-1.dll");
    if (!vulkan) return false;

    auto vkCreateInstance = reinterpret_cast<PFN_vkCreateInstance>(
        GetProcAddress(vulkan, "vkCreateInstance"));
    auto vkDestroyInstance = reinterpret_cast<PFN_vkDestroyInstance>(
        GetProcAddress(vulkan, "vkDestroyInstance"));
    if (!vkCreateInstance || !vkDestroyInstance) {
        FreeLibrary(vulkan);
        return false;
    }

    VkApplicationInfo app_info{};
    app_info.sType           = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    app_info.pApplicationName = "ReCaster";
    app_info.applicationVersion = 0;
    app_info.pEngineName     = "ReCaster";
    app_info.engineVersion   = 0;
    app_info.apiVersion      = VK_API_VERSION_1_0;

    VkInstanceCreateInfo create_info{};
    create_info.sType              = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    create_info.pApplicationInfo   = &app_info;
    // No layers, no extensions — we just want to confirm the loader
    // can talk to an ICD.
    create_info.enabledLayerCount     = 0;
    create_info.ppEnabledLayerNames   = nullptr;
    create_info.enabledExtensionCount = 0;
    create_info.ppEnabledExtensionNames = nullptr;

    void* instance = nullptr;
    // vkCreateInstance returns VkResult (int). 0 = VK_SUCCESS.
    const std::int64_t result = reinterpret_cast<std::int64_t(*)(
        const void*, const void*, void*)>(vkCreateInstance)(
        &create_info, nullptr, &instance);

    bool ok = false;
    if (result == 0 && instance) {
        // Success — destroy the instance immediately.
        reinterpret_cast<void(*)(void*, const void*)>(vkDestroyInstance)(
            instance, nullptr);
        ok = true;
    }

    FreeLibrary(vulkan);
    return ok;
}

} // namespace

// ============================================================================
// Public API
// ============================================================================

bool is_vulkan_available() {
    if (g_vulkan_checked) return g_vulkan_available;

    common::logger::info("dxvk: checking Vulkan availability...");
    g_vulkan_available = try_create_vk_instance();
    g_vulkan_checked   = true;

    if (g_vulkan_available) {
        common::logger::info("dxvk: Vulkan available — DXVK will be used");
    } else {
        common::logger::info("dxvk: Vulkan NOT available — DXVK disabled "
                             "(falling back to native D3D9)");
    }
    return g_vulkan_available;
}

bool deploy(const std::string& bundled_dll_path,
            const std::string& game_dir,
            std::string& error) {
    // Validate source.
    if (!fs::exists(bundled_dll_path)) {
        error = "bundled d3d9.dll not found at " + bundled_dll_path +
                " — install may be incomplete (expected d3d9.dll next to caster.exe)";
        common::logger::err("dxvk: {}", error);
        return false;
    }

    const fs::path dest = fs::path(game_dir) / "d3d9.dll";

    // Skip the copy if the destination already exists with the same size.
    // This avoids unnecessary disk writes + antivirus triggers on every
    // launch. If the sizes differ, we overwrite (likely a DXVK upgrade).
    std::error_code ec;
    if (fs::exists(dest, ec)) {
        const auto src_size = fs::file_size(bundled_dll_path, ec);
        const auto dst_size = fs::file_size(dest, ec);
        if (src_size == dst_size) {
            common::logger::info("dxvk: d3d9.dll already deployed at {} "
                                 "(size match, skipping copy)", dest.string());
            return true;
        }
        common::logger::info("dxvk: existing d3d9.dll at {} has different size "
                             "({} vs {}), overwriting",
                             dest.string(), dst_size, src_size);
    }

    // Copy. fs::copy_file with overwrite = true.
    if (!fs::copy_file(bundled_dll_path, dest,
                       fs::copy_options::overwrite_existing, ec)) {
        error = "failed to copy " + bundled_dll_path + " -> " + dest.string() +
                " (" + ec.message() + ")";
        common::logger::err("dxvk: {}", error);
        return false;
    }

    common::logger::info("dxvk: deployed d3d9.dll ({} bytes) to {}",
                         fs::file_size(dest, ec), dest.string());
    return true;
}

void set_env_vars(const std::string& game_dir) {
    // DXVK_HUD=0 — disable the DXVK built-in HUD (we have our own overlay
    // via D3D9 vtable swap, which is more informative and toggleable
    // with hotkey 3).
    common::win32::env::set("DXVK_HUD", "0");

    // DXVK_STATE_CACHE=1 — persist the shader cache to disk. The first
    // run compiles all shaders (causing "Compiling shaders..." hitches);
    // subsequent runs load them from disk and are smooth.
    common::win32::env::set("DXVK_STATE_CACHE", "1");

    // DXVK_STATE_CACHE_PATH — where to put the cache. Default is
    // %LOCALAPPDATA%\dxvk-state-cache, but we override to <game_dir>
    // so the cache travels with the game (portable install friendly,
    // and lets users nuke it by deleting the game folder).
    common::win32::env::set("DXVK_STATE_CACHE_PATH", game_dir);

    // DXVK_FRAME_RATE=60 — driver-level frame limiter, far more precise
    // than our QPC-based limiter in frame_limiter.cpp. The hook.dll
    // detects this env var and disables its own limiter to avoid double-
    // limiting (which would cause hitching).
    common::win32::env::set("DXVK_FRAME_RATE", "60");

    // DXVK_MAX_FRAME_LATENCY=1 — minimum input lag. Default is 2; lowering
    // to 1 trades a tiny amount of throughput for noticeably snappier
    // inputs, which matters for a fighting game.
    common::win32::env::set("DXVK_MAX_FRAME_LATENCY", "1");

    common::logger::info("dxvk: env vars set (HUD=0, STATE_CACHE=1, "
                         "STATE_CACHE_PATH='{}', FRAME_RATE=60, MAX_FRAME_LATENCY=1)",
                         game_dir);
}

bool cleanup(const std::string& game_dir) {
    bool removed_any = false;
    std::error_code ec;

    // Remove the deployed d3d9.dll.
    const fs::path dll = fs::path(game_dir) / "d3d9.dll";
    if (fs::remove(dll, ec)) {
        common::logger::info("dxvk: removed {}", dll.string());
        removed_any = true;
    }

    // Remove the state cache directory. DXVK creates files named
    // <game>.dxvk-cache and a subdirectory <game>.dxvk-state-cache/
    // under DXVK_STATE_CACHE_PATH. We remove both patterns.
    // (For MBAACC the cache file is "MBAA.dxvk-cache".)
    for (const auto& entry : fs::directory_iterator(game_dir, ec)) {
        const auto path = entry.path();
        const auto name = path.filename().string();
        if (name.ends_with(".dxvk-cache") ||
            name.ends_with(".dxvk-state-cache")) {
            if (fs::is_directory(path, ec)) {
                fs::remove_all(path, ec);
            } else {
                fs::remove(path, ec);
            }
            common::logger::info("dxvk: removed cache {}", path.string());
            removed_any = true;
        }
    }

    return removed_any;
}

} // namespace caster::exe::launcher::dxvk
