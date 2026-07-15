// src/exe/launcher/dxvk.cpp
//
// DXVK integration — implementation. See dxvk.hpp for the design.
//
// All functions run on the GameRunner worker thread (Layer 2). No
// threading, no mutexes — the only shared state is the
// `g_vulkan_checked` / `g_vulkan_available` pair in the anonymous
// namespace, which is only ever touched from the game runner worker.
//
// Vulkan loading strategy:
//   We do NOT link against libvulkan-1.dll at link time. Instead we
//   LoadLibraryA("vulkan-1.dll") at runtime and resolve
//   vkGetInstanceProcAddr via GetProcAddress. From there we can load
//   any other Vulkan function we need (vkCreateInstance, vkDestroyInstance)
//   via vkGetInstanceProcAddr(NULL, "vkXxx") — passing instance=NULL is
//   the official Khronos pattern for loading global functions.
//
//   We use the official Khronos <vulkan/vulkan.h> header (bundled with
//   SDL2's FetchContent at src/video/khronos/vulkan/vulkan.h) for the
//   real type definitions: VkInstance, VkInstanceCreateInfo,
//   VkApplicationInfo, VkResult, VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
//   VK_API_VERSION_1_0, etc. Defining VK_NO_PROTOTYPES before including
//   the header suppresses the function prototypes (so we don't need
//   libvulkan at link time) while keeping the PFN_vk* typedefs and
//   struct definitions available.

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

// VK_USE_PLATFORM_WIN32_KHR is required so vulkan.h includes <windows.h>
// before its own platform-specific block (vulkan_win32.h). Without it,
// the VkSurfaceKHR / VkWin32SurfaceCreateInfoKHR types aren't defined,
// but we don't actually use them — we just need the core types.
// We define it anyway because the header layout is cleaner that way
// and matches what a real Vulkan-using Windows app does.
//
// VK_NO_PROTOTYPES suppresses the `extern` function declarations in
// vulkan_core.h — we get the PFN_vk* typedefs but no symbols to link
// against. We load everything via vkGetInstanceProcAddr at runtime.
#define VK_USE_PLATFORM_WIN32_KHR 1
#define VK_NO_PROTOTYPES 1
#include <vulkan/vulkan.h>

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

// Try to create + immediately destroy a VkInstance. Returns true if
// the call succeeds (Vulkan loader + at least one ICD are functional).
//
// We do NOT pass any extensions or layers — we just want to confirm
// the loader can talk to an ICD. If vkCreateInstance succeeds with
// an empty create info, the system has working Vulkan.
bool try_create_vk_instance() {
    // 1. Load vulkan-1.dll. On Windows this is the Vulkan loader shipped
    //    with GPU drivers (NVIDIA, AMD, Intel) or the Windows-on-Arm
    //    system loader. On Wine it's the builtin winevulkan.dll wrapper.
    HMODULE vulkan = LoadLibraryA("vulkan-1.dll");
    if (!vulkan) {
        common::logger::info("dxvk: vulkan-1.dll not loadable (LoadLibraryA failed, "
                             "err={}): Vulkan not available", GetLastError());
        return false;
    }

    // 2. Get vkGetInstanceProcAddr — the bootstrap function. All other
    //    Vulkan functions are loaded through it.
    auto vkGetInstanceProcAddr = reinterpret_cast<PFN_vkGetInstanceProcAddr>(
        GetProcAddress(vulkan, "vkGetInstanceProcAddr"));
    if (!vkGetInstanceProcAddr) {
        common::logger::warn("dxvk: vulkan-1.dll loaded but missing "
                             "vkGetInstanceProcAddr export");
        FreeLibrary(vulkan);
        return false;
    }

    // 3. Load the two functions we need.
    //
    // The Khronos spec says vkGetInstanceProcAddr(NULL, "vkXxx") should
    // return function pointers for global (instance-level) functions.
    // In practice, not every loader implements this correctly:
    //   - Real Windows + GPU drivers: works.
    //   - Wine's builtin winevulkan.dll (which backs vulkan-1.dll on
    //     Wine): vkGetInstanceProcAddr(NULL, "vkCreateInstance") returns
    //     NULL, even though vkCreateInstance is a real export of the DLL.
    //
    // The robust pattern (used by SDL2, GLFW, and other cross-platform
    // Vulkan loaders) is to resolve the global functions directly via
    // GetProcAddress, since vkCreateInstance / vkDestroyInstance are
    // always exported by vulkan-1.dll as ordinary DLL symbols. We then
    // only need vkGetInstanceProcAddr for instance-scoped functions
    // (which we don't use here).
    //
    // Try vkGetInstanceProcAddr first (spec-compliant path), fall back
    // to GetProcAddress if it returns NULL (Wine path).
    PFN_vkCreateInstance vkCreateInstance = nullptr;
    PFN_vkDestroyInstance vkDestroyInstance = nullptr;

    if (vkGetInstanceProcAddr) {
        vkCreateInstance = reinterpret_cast<PFN_vkCreateInstance>(
            vkGetInstanceProcAddr(nullptr, "vkCreateInstance"));
        vkDestroyInstance = reinterpret_cast<PFN_vkDestroyInstance>(
            vkGetInstanceProcAddr(nullptr, "vkDestroyInstance"));
    }

    if (!vkCreateInstance) {
        vkCreateInstance = reinterpret_cast<PFN_vkCreateInstance>(
            GetProcAddress(vulkan, "vkCreateInstance"));
    }
    if (!vkDestroyInstance) {
        vkDestroyInstance = reinterpret_cast<PFN_vkDestroyInstance>(
            GetProcAddress(vulkan, "vkDestroyInstance"));
    }

    if (!vkCreateInstance || !vkDestroyInstance) {
        common::logger::warn("dxvk: vulkan-1.dll loaded but vkCreateInstance/"
                             "vkDestroyInstance not resolvable (tried both "
                             "vkGetInstanceProcAddr(NULL, ...) and GetProcAddress)");
        FreeLibrary(vulkan);
        return false;
    }

    // 4. Build a minimal VkApplicationInfo. We ask for Vulkan 1.0 —
    //    every ICD supports it, and that's all we need to probe
    //    availability.
    VkApplicationInfo app_info{};
    app_info.sType              = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    app_info.pNext              = nullptr;
    app_info.pApplicationName   = "ReCaster";
    app_info.applicationVersion = VK_MAKE_VERSION(0, 1, 0);
    app_info.pEngineName        = "ReCaster";
    app_info.engineVersion      = VK_MAKE_VERSION(0, 1, 0);
    app_info.apiVersion         = VK_API_VERSION_1_0;

    // 5. Build the VkInstanceCreateInfo. No layers, no extensions —
    //    just the application info. We're not going to render anything;
    //    we just want to confirm the loader can talk to an ICD.
    VkInstanceCreateInfo create_info{};
    create_info.sType                   = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    create_info.pNext                   = nullptr;
    create_info.flags                   = 0;
    create_info.pApplicationInfo        = &app_info;
    create_info.enabledLayerCount       = 0;
    create_info.ppEnabledLayerNames     = nullptr;
    create_info.enabledExtensionCount   = 0;
    create_info.ppEnabledExtensionNames = nullptr;

    // 6. Try to create the instance. VkResult is an int enum;
    //    VK_SUCCESS == 0.
    VkInstance instance = nullptr;
    const VkResult result = vkCreateInstance(&create_info, nullptr, &instance);

    bool ok = false;
    if (result == VK_SUCCESS && instance) {
        // Success — destroy the instance immediately. We don't keep it
        // around because we don't need a VkInstance for anything else;
        // DXVK creates its own when it loads.
        vkDestroyInstance(instance, nullptr);
        ok = true;
    } else {
        common::logger::warn("dxvk: vkCreateInstance returned VkResult={} "
                             "(VK_SUCCESS=0): Vulkan loader present but no "
                             "working ICD", static_cast<int>(result));
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
