// src/dll/dll_main.cpp
//
// Entry point for hook.dll. On DLL_PROCESS_ATTACH:
//   - Spawn worker thread A: ENet listener on port 7500 (net_listener.cpp)
//   - Spawn worker thread B: own SDL2 + ImGui window (status panel)
//
// The GUI thread first tries to receive the launcher's IPC config_buffer
// message (so we know whether we're in training / netplay / spectator mode,
// what the delay/rollback/win_count are, who the peer is, etc.).
//
// Both threads stop on DLL_PROCESS_DETACH. Nothing heavy is done inside
// DllMain itself — it must return quickly and must NOT call LoadLibrary,
// CreateWindow, or anything that may acquire the loader lock.

#include "net_listener.hpp"
#include "ipc_receiver.hpp"
#include "gui_window.hpp"  // provided by caster_common's PUBLIC include dir
#include "ipc/config_buffer.hpp"
#include "logger.hpp"

#ifndef NOMINMAX
#  define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#  define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <objbase.h>  // CoInitializeEx, CoUninitialize

#include <imgui.h>
#include <SDL2/SDL.h>

#include <atomic>
#include <exception>
#include <thread>

#ifndef CASTER_NET_PORT
#  define CASTER_NET_PORT 7500
#endif

namespace {

// Stop flag shared between DllMain and the worker threads. Atomic so that
// writes from DllMain are visible to the workers without locking.
std::atomic<bool> g_running{false};

// Worker thread handles (kept alive across DllMain calls).
std::thread g_gui_thread;

// State reported back to the GUI thread for display.
struct GuiState {
    std::string last_error;
};
GuiState g_gui_state;

// How long to wait for the launcher's IPC config message before giving up
// and starting the UI with default/empty values. 5 s is plenty — the
// launcher sends the config immediately after we connect.
constexpr std::uint32_t kIpcReceiveTimeoutMs = 5000;

void gui_thread_fn() {
    // CoInitializeEx is needed because SDL2 on Win32 uses COM for some audio
    // and joystick paths. Even if we don't use those subsystems, initializing
    // COINIT_APARTMENTTHREADED is the safe choice for a UI thread.
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);

    // ---- 1. Receive the launcher's IPC config_buffer BEFORE we open the
    //         window — the config determines what the UI shows and (in
    //         later phases) what the netplay thread does. Non-fatal if
    //         it fails: we proceed with defaults so the user can still see
    //         the hook.dll status window.
    caster::dll::ipc_receiver::receive(kIpcReceiveTimeoutMs);

    // ---- 2. Open the SDL2+ImGui status window.
    try {
        caster::common::GuiWindow win("caster — hook.dll (payload)",
                                      480, 360);

        while (g_running.load()) {
            if (!win.pump_frame([&] {
                ImGui::SetNextWindowPos(ImVec2(0, 0));
                ImGui::SetNextWindowSize(ImVec2(480, 360));

                if (!ImGui::Begin("Netplay Placeholder", nullptr,
                                  ImGuiWindowFlags_NoResize |
                                  ImGuiWindowFlags_NoMove |
                                  ImGuiWindowFlags_NoCollapse)) {
                    ImGui::End();
                    return;
                }

                ImGui::TextDisabled("== hook.dll injected ==");
                ImGui::Text("Hello from inside the host process.");
                ImGui::Separator();

                // ---- IPC config display --------------------------------
                ImGui::TextDisabled("== Launcher IPC config ==");
                ImGui::TextWrapped("%s",
                    caster::dll::ipc_receiver::status_string().c_str());

                if (caster::dll::ipc_receiver::is_ready()) {
                    caster::common::ipc::config_buffer::Config cfg;
                    if (caster::dll::ipc_receiver::get_config(cfg)) {
                        ImGui::BulletText("Flags       : 0x%02x",
                                          cfg.flags);
                        ImGui::BulletText("Training    : %s",
                                          cfg.is_training() ? "yes" : "no");
                        ImGui::BulletText("Netplay     : %s",
                                          cfg.is_netplay() ? "yes" : "no");
                        ImGui::BulletText("Host        : %s",
                                          cfg.is_host() ? "yes" : "no");
                        ImGui::BulletText("Spectator   : %s",
                                          cfg.is_spectator() ? "yes" : "no");
                        ImGui::BulletText("Delay       : %d frames",
                                          cfg.delay);
                        ImGui::BulletText("Rollback    : %d frames",
                                          cfg.rollback);
                        ImGui::BulletText("Win count   : %d", cfg.win_count);
                        ImGui::BulletText("Host player : %d", cfg.host_player);
                        ImGui::BulletText("Peer port   : %d", cfg.peer_port);
                        ImGui::BulletText("Local UDP   : %d",
                                          cfg.local_udp_port);
                        ImGui::BulletText("Match seed  : 0x%08x",
                                          cfg.match_seed);
                        ImGui::BulletText("Peer addr   : %s",
                                          cfg.peer_addr.empty()
                                              ? "(none)"
                                              : cfg.peer_addr.c_str());
                    }
                }

                ImGui::Separator();
                ImGui::TextDisabled("== ENet listener ==");
                ImGui::Text("Bind 0.0.0.0:%d", CASTER_NET_PORT);
                bool up = caster::dll::net_listener_running();
                ImGui::Text("Status          : %s",
                            up ? "LISTENING" : "DOWN (bind failed?)");
                ImGui::Text("Accepted conns  : %llu",
                            static_cast<unsigned long long>(
                                caster::dll::net_listener_accepted_count()));

                ImGui::Separator();
                ImGui::TextDisabled("== Worker threads ==");
                ImGui::Text("Thread A (ENet) : running");
                ImGui::Text("Thread B (GUI)  : running (this thread)");
                ImGui::Text("Stop flag       : %s",
                            g_running.load() ? "true" : "false (detaching)");

                ImGui::End();
            })) {
                // pump_frame returned false → window was closed.
                break;
            }
        }
    } catch (const std::exception& e) {
        g_gui_state.last_error = std::string("GUI thread exception: ") + e.what();
        caster::common::logger::err("{}", g_gui_state.last_error);
    } catch (...) {
        g_gui_state.last_error = "GUI thread unknown exception";
        caster::common::logger::err("{}", g_gui_state.last_error);
    }

    caster::common::maybe_shutdown_sdl();
    CoUninitialize();
}

} // namespace

extern "C" BOOL WINAPI DllMain(HINSTANCE, DWORD reason, LPVOID) {
    switch (reason) {
        case DLL_PROCESS_ATTACH: {
            g_running.store(true);

            // Start ENet listener thread first (it's the cheaper of the two).
            caster::dll::start_net_listener(g_running);

            // Then spawn the GUI worker thread. SDL2 + ImGui live entirely on
            // this thread for the lifetime of the DLL.
            g_gui_thread = std::thread(gui_thread_fn);
            break;
        }
        case DLL_PROCESS_DETACH: {
            // Signal workers to stop, then join the GUI thread (the ENet
            // listener will be joined by stop_net_listener).
            g_running.store(false);

            if (g_gui_thread.joinable()) {
                g_gui_thread.join();
            }
            caster::dll::stop_net_listener();
            break;
        }
        case DLL_THREAD_ATTACH:
        case DLL_THREAD_DETACH:
        default:
            break;
    }
    return TRUE;
}
