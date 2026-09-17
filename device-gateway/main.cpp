#include <atomic>
#include <chrono>
#include <cstring>
#include <iostream>
#include <memory>
#include <mutex>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/un.h>
#include <thread>
#include <unistd.h>

#include "canopen/canopen_translator.h"
#include "device_translator.h"
#include "message.h"
#include "unix_socket.h"

using namespace wizard;

namespace {

constexpr const char* kSocketPath = "/tmp/wizard-backend.sock";
constexpr const char* kDefaultCanInterface = "vcan0";
constexpr auto kHeartbeatInterval = std::chrono::milliseconds(500);
constexpr auto kReconnectInterval = std::chrono::seconds(2);

uint64_t now_us() {
    timeval tv{};
    gettimeofday(&tv, nullptr);
    return static_cast<uint64_t>(tv.tv_sec) * 1'000'000ULL + static_cast<uint64_t>(tv.tv_usec);
}

// True once wizard-engine has created its listening socket. Checked with
// stat() rather than a trial connect: the engine accepts a single client at a
// time, so probing by connecting would be accepted and immediately dropped,
// which the engine would see as a gateway that connected and vanished.
bool engine_socket_exists(const char* path) {
    struct stat st{};
    return ::stat(path, &st) == 0 && S_ISSOCK(st.st_mode);
}

// Reads the full Identity Object, replies to engine. Startup or on-demand.
void handle_device_info_request(DeviceTranslator& translator, UnixSocket& engine_sock,
                                std::mutex& send_mutex) {
    auto info = translator.read_device_info();
    if (!info) {
        std::cerr << "device info read failed\n";
        return;
    }
    DeviceInfoResponsePayload payload{info->vendor_id, info->product_code, info->revision,
                                      info->serial};
    std::lock_guard<std::mutex> lock(send_mutex);
    engine_sock.send(make_device_info_response(payload));
}

// Writes run/stop, reads back the resulting status, replies on the engine socket.
void handle_set_run_stop_command(DeviceTranslator& translator, UnixSocket& engine_sock,
                                 std::mutex& send_mutex, bool run) {
    translator.set_run_stop(run);
    auto status = translator.read_run_stop_status();
    if (!status) {
        std::cerr << "run/stop status read failed after command\n";
        return;
    }

    RunStopStatusPayload payload{now_us(), *status};
    std::lock_guard<std::mutex> lock(send_mutex);
    engine_sock.send(make_run_stop_status_event(payload));
}

// Converts one decoded telemetry sample into its corresponding socket event.
Message telemetry_sample_to_message(const TelemetrySample& sample) {
    switch (sample.kind) {
        case TelemetryKind::Position:
            return make_position_event({sample.timestamp_us, sample.value});
        case TelemetryKind::Velocity:
            return make_speed_event({sample.timestamp_us, sample.value});
        case TelemetryKind::Current:
            return make_current_event({sample.timestamp_us, static_cast<int16_t>(sample.value)});
    }
    return make_position_event({sample.timestamp_us, sample.value});  // unreachable
}

// Own thread. send_mutex protects engine_sock from the other threads.
void telemetry_loop(DeviceTranslator& translator, UnixSocket& engine_sock,
                    std::mutex& send_mutex, std::atomic<bool>& session_active) {
    while (session_active) {
        auto sample = translator.read_next_telemetry();
        if (!sample) {
            std::cerr << "telemetry read error, stopping telemetry loop\n";
            break;
        }
        std::lock_guard<std::mutex> lock(send_mutex);
        if (!engine_sock.send(telemetry_sample_to_message(*sample))) {
            std::cerr << "wizard-engine disconnected, stopping telemetry loop\n";
            break;
        }
    }
    session_active = false;
}

// Handles commands from engine, not just at startup. Only thread calling receive().
void command_loop(DeviceTranslator& translator, UnixSocket& engine_sock,
                  std::mutex& send_mutex, std::atomic<bool>& session_active) {
    while (session_active) {
        auto messages = engine_sock.receive();
        if (!messages) {
            std::cerr << "wizard-engine disconnected, stopping command loop\n";
            break;
        }
        for (const auto& msg : *messages) {
            if (msg.type == MessageType::DeviceInfoRequest) {
                handle_device_info_request(translator, engine_sock, send_mutex);
            } else if (msg.type == MessageType::SetRunStopCommand) {
                auto run = parse_set_run_stop_command(msg);
                if (run) handle_set_run_stop_command(translator, engine_sock, send_mutex, *run);
            }
        }
    }
    session_active = false;
}

// Probes the MCU every 500ms. Only sends McuStatusEvent if the state changes.
void heartbeat_loop(DeviceTranslator& translator, UnixSocket& engine_sock,
                    std::mutex& send_mutex, std::atomic<bool>& session_active) {
    bool last_known_alive = false;  // assume dead until proven otherwise

    while (session_active) {
        std::this_thread::sleep_for(kHeartbeatInterval);
        if (!session_active) break;

        const bool alive = translator.probe_alive();
        if (alive == last_known_alive) continue;

        last_known_alive = alive;

        std::lock_guard<std::mutex> lock(send_mutex);
        // The send result matters here: without checking it this loop never
        // noticed a dead engine, so main could not join the thread and the
        // process hung on shutdown instead of reconnecting.
        if (!engine_sock.send(make_mcu_status_event(alive))) {
            std::cerr << "wizard-engine disconnected, stopping heartbeat loop\n";
            break;
        }
    }
    session_active = false;
}

// One connected session with wizard-engine: connect, run until the engine goes
// away, then tear the threads down and return so the caller can reconnect.
// The CAN interface is deliberately left untouched — it belongs to the caller
// and survives engine restarts.
void run_session(DeviceTranslator& translator) {
    while (!engine_socket_exists(kSocketPath)) {
        std::cerr << "waiting for wizard-engine to create " << kSocketPath << "\n";
        std::this_thread::sleep_for(kReconnectInterval);
    }

    try {
        std::cout << "connecting to " << kSocketPath << "\n";
        UnixSocket engine_sock = connect_to(kSocketPath);
        std::cout << "connected to wizard-engine\n";

        std::mutex        send_mutex;
        std::atomic<bool> session_active{true};

        std::thread commands(command_loop, std::ref(translator), std::ref(engine_sock),
                             std::ref(send_mutex), std::ref(session_active));
        std::thread heartbeat(heartbeat_loop, std::ref(translator), std::ref(engine_sock),
                              std::ref(send_mutex), std::ref(session_active));

        telemetry_loop(translator, engine_sock, send_mutex, session_active);  // main thread

        commands.join();
        heartbeat.join();

        std::cout << "session with wizard-engine ended\n";

    } catch (const std::exception& e) {
        // connect_to throws when the engine is not listening yet, which is a
        // normal condition at boot rather than a fatal error.
        std::cerr << "connect failed: " << e.what() << "\n";
        std::this_thread::sleep_for(kReconnectInterval);
    }
}

}  // namespace

int main(int argc, char** argv) {
    const std::string can_iface = argc > 1 ? argv[1] : kDefaultCanInterface;

    std::cout << "opening CAN interface " << can_iface << "\n";
    std::unique_ptr<DeviceTranslator> translator = std::make_unique<CanopenTranslator>(can_iface);
    std::cout << "CAN interface ready\n";

    // Reconnect forever: wizard-engine restarting must not take the gateway
    // down with it, and must not force the CAN interface to be reopened.
    while (true) {
        run_session(*translator);
        std::this_thread::sleep_for(kReconnectInterval);
    }

    return 0;
}
