#include "engine_session.h"

#include <sys/stat.h>
#include <sys/time.h>

#include <chrono>
#include <iostream>
#include <thread>

namespace wizard {

namespace {

constexpr auto kHeartbeatInterval = std::chrono::milliseconds(500);
constexpr auto kReconnectInterval = std::chrono::seconds(2);
constexpr auto kPollInterval = std::chrono::milliseconds(200);

uint64_t now_us() {
    timeval tv{};
    gettimeofday(&tv, nullptr);
    return static_cast<uint64_t>(tv.tv_sec) * 1'000'000ULL + static_cast<uint64_t>(tv.tv_usec);
}

// True once wizard-engine has created its listening socket. Checked with
// stat() rather than a trial connect: the engine accepts a single client at a
// time, so probing by connecting would be accepted and immediately dropped,
// which the engine would see as a gateway that connected and vanished.
bool engine_socket_exists(const std::string& path) {
    struct stat st{};
    return ::stat(path.c_str(), &st) == 0 && S_ISSOCK(st.st_mode);
}

Message to_message(const TelemetrySample& s) {
    switch (s.kind) {
        case TelemetryKind::Position: return make_position_event({s.timestamp_us, s.value});
        case TelemetryKind::Velocity: return make_speed_event({s.timestamp_us, s.value});
        case TelemetryKind::Current: return make_current_event({s.timestamp_us, static_cast<int16_t>(s.value)});
    }
    return make_position_event({s.timestamp_us, s.value});  // unreachable
}

}  // namespace

EngineSession::EngineSession(DeviceTranslator& translator, const CommandSet& commands, UnixSocket& engine_sock)
    : translator_(translator), commands_(commands), sock_(engine_sock), manager_(translator, commands) {}

void EngineSession::run() {
    std::thread reader(&EngineSession::reader_loop, this);
    std::thread worker(&EngineSession::worker_loop, this);
    std::thread heartbeat(&EngineSession::heartbeat_loop, this);

    telemetry_loop();  // caller's thread, until the session ends

    reader.join();
    // Drop commands nobody will get a reply for; a ramp in progress gives way.
    queue_.clear();
    translator_.request_stop();
    queue_.close();
    worker.join();
    heartbeat.join();

    // Nobody can send STOP any more: never leave the motor driven without a
    // controller attached.
    std::cout << "session with wizard-engine ended, stopping the motor\n";
    if (!translator_.stop()) std::cerr << "stop after session end failed\n";
}

bool EngineSession::send(const Message& msg) {
    std::lock_guard<std::mutex> lock(send_mutex_);
    return sock_.send(msg);
}

void EngineSession::send_device_info() {
    auto info = translator_.read_device_info();
    if (!info) {
        std::cerr << "device info read failed\n";
        return;
    }
    send(make_device_info_response({info->vendor_id, info->product_code, info->revision, info->serial}));
}

// Every run/stop or motion command gets this reply: the status read back from
// the device, or the gateway's own view if that read fails, so the UI and the
// engine's run tracking always get an answer.
void EngineSession::send_run_stop_status() {
    bool running = translator_.is_running();
    if (auto status = translator_.read_run_stop_status()) {
        running = *status;
    } else {
        std::cerr << "run/stop status read failed after command, reporting " << (running ? "RUNNING" : "STOPPED")
                  << " (gateway state)\n";
    }
    send(make_run_stop_status_event({now_us(), running}));
}

void EngineSession::telemetry_loop() {
    while (active_) {
        // Timed wait: while STOPPED no telemetry arrives, and a blocking read
        // would never notice that the other loops ended the session.
        TelemetrySample sample{};
        const auto r = translator_.wait_next_telemetry(kPollInterval, sample);
        if (r == TelemetryWait::Timeout) continue;
        if (r == TelemetryWait::Closed) {
            std::cerr << "telemetry read error, ending the session\n";
            break;
        }
        if (!send(to_message(sample))) {
            std::cerr << "wizard-engine disconnected (telemetry)\n";
            break;
        }
    }
    active_ = false;
}

void EngineSession::reader_loop() {
    while (active_) {
        auto messages = sock_.receive();
        if (!messages) {
            std::cerr << "wizard-engine disconnected (commands)\n";
            break;
        }
        for (const auto& msg : *messages) {
            std::optional<SetMotionPayload> cmd;
            switch (msg.type) {
                case MessageType::DeviceInfoRequest:
                    send_device_info();
                    continue;
                case MessageType::SetRunStopCommand:
                    if (auto run = parse_set_run_stop_command(msg)) cmd = commands_.run_stop_command(*run);
                    break;
                case MessageType::SetMotionCommand:
                    cmd = parse_set_motion_command(msg);
                    break;
                default:
                    continue;  // not for the gateway
            }
            if (!cmd) {
                // Malformed: still answer, so the UI does not wait for a reply.
                std::cerr << "malformed command 0x" << std::hex << static_cast<int>(msg.type) << std::dec << " ("
                          << msg.payload.size() << " bytes), answering with the current status\n";
                send_run_stop_status();
                continue;
            }
            if (cmd->mode == MotionMode::Stop) translator_.request_stop();
            queue_.push(*cmd);
        }
    }
    active_ = false;
}

void EngineSession::worker_loop() {
    while (true) {
        auto cmd = queue_.pop_for(kPollInterval);
        if (!cmd) {
            if (queue_.is_closed()) break;
            continue;
        }
        manager_.handle(*cmd);
        send_run_stop_status();
    }
}

void EngineSession::heartbeat_loop() {
    bool last_known_alive = false;  // assume dead until proven otherwise
    while (active_) {
        std::this_thread::sleep_for(kHeartbeatInterval);
        if (!active_) break;
        const bool alive = translator_.probe_alive();
        if (alive == last_known_alive) continue;
        last_known_alive = alive;
        // The send result matters: without it this loop never noticed a dead
        // engine, so the session could not end and the process hung.
        if (!send(make_mcu_status_event(alive))) {
            std::cerr << "wizard-engine disconnected (heartbeat)\n";
            break;
        }
    }
    active_ = false;
}

void run_engine_session(DeviceTranslator& translator, const CommandSet& commands, const std::string& socket_path) {
    while (!engine_socket_exists(socket_path)) {
        std::cerr << "waiting for wizard-engine to create " << socket_path << "\n";
        std::this_thread::sleep_for(kReconnectInterval);
    }
    try {
        std::cout << "connecting to " << socket_path << "\n";
        UnixSocket sock = connect_to(socket_path);
        std::cout << "connected to wizard-engine\n";
        EngineSession(translator, commands, sock).run();
        std::this_thread::sleep_for(kReconnectInterval);
    } catch (const std::exception& e) {
        // connect_to throws when the engine is not listening yet: normal at boot.
        std::cerr << "connect failed: " << e.what() << "\n";
        std::this_thread::sleep_for(kReconnectInterval);
    }
}

}  // namespace wizard
