#include <chrono>
#include <iostream>
#include <memory>
#include <mutex>
#include <sys/time.h>
#include <thread>

#include "canopen/canopen_translator.h"
#include "device_translator.h"
#include "message.h"
#include "unix_socket.h"

using namespace wizard;

namespace {
constexpr const char* kSocketPath = "/tmp/wizard-backend.sock";
constexpr const char* kDefaultCanInterface = "vcan0";
constexpr auto kHeartbeatInterval = std::chrono::milliseconds(500);

uint64_t now_us() {
    timeval tv{};
    gettimeofday(&tv, nullptr);
    return static_cast<uint64_t>(tv.tv_sec) * 1'000'000ULL + static_cast<uint64_t>(tv.tv_usec);
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
    if (!status) return;

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
            return make_velocity_event({sample.timestamp_us, sample.value});
        case TelemetryKind::Current:
            return make_current_event({sample.timestamp_us, static_cast<int16_t>(sample.value)});
    }
    return make_position_event({sample.timestamp_us, sample.value});  // unreachable
}

// Own thread. send_mutex protects engine_sock from the other threads.
void telemetry_loop(DeviceTranslator& translator, UnixSocket& engine_sock,
                     std::mutex& send_mutex) {
    while (true) {
        auto sample = translator.read_next_telemetry();
        if (!sample) {
            std::cerr << "telemetry read error, stopping telemetry loop\n";
            return;
        }
        std::lock_guard<std::mutex> lock(send_mutex);
        if (!engine_sock.send(telemetry_sample_to_message(*sample))) {
            std::cerr << "wizard-engine disconnected, stopping telemetry loop\n";
            return;
        }
    }
}

// Handles commands from engine, not just at startup. Only thread calling receive().
void command_loop(DeviceTranslator& translator, UnixSocket& engine_sock, std::mutex& send_mutex) {
    while (true) {
        auto messages = engine_sock.receive();
        if (!messages) {
            std::cerr << "wizard-engine disconnected, stopping command loop\n";
            return;
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
}

// Probes the MCU every 500ms. Only sends McuStatusEvent if the state changes.
void heartbeat_loop(DeviceTranslator& translator, UnixSocket& engine_sock, std::mutex& send_mutex) {
    bool last_known_alive = false;  // assume dead until proven otherwise
    while (true) {
        std::this_thread::sleep_for(kHeartbeatInterval);

        bool alive = translator.probe_alive();
        if (alive != last_known_alive) {
            last_known_alive = alive;
            std::lock_guard<std::mutex> lock(send_mutex);
            engine_sock.send(make_mcu_status_event(alive));
        }
    }
}

}  // namespace

int main(int argc, char** argv) {
    std::string can_iface = argc > 1 ? argv[1] : kDefaultCanInterface;

    std::cout << "connecting to " << kSocketPath << "\n";
    UnixSocket engine_sock = connect_to(kSocketPath);
    std::cout << "connected to wizard-engine\n";

    std::cout << "opening CAN interface " << can_iface << "\n";
    std::unique_ptr<DeviceTranslator> translator = std::make_unique<CanopenTranslator>(can_iface);
    std::cout << "CAN interface ready\n";

    std::mutex send_mutex;
    std::thread commands(command_loop, std::ref(*translator), std::ref(engine_sock),
                          std::ref(send_mutex));
    std::thread heartbeat(heartbeat_loop, std::ref(*translator), std::ref(engine_sock),
                          std::ref(send_mutex));
    telemetry_loop(*translator, engine_sock, send_mutex);  // runs on main thread
    commands.join();
    heartbeat.join();
    return 0;
}