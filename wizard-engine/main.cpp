#include <chrono>
#include <cstdlib>
#include <iomanip>
#include <iostream>
#include <mutex>
#include <optional>
#include <thread>

#include "db.h"
#include "message.h"
#include "unix_socket.h"

using namespace wizard;

namespace {

std::string get_default_db_path() {
    return kDefaultDbPath;
}

// Telemetry is high volume (~500 msg/sec while RUNNING), so per-message
// console logging is off unless WIZARD_VERBOSE is set.
bool g_verbose_telemetry = false;

// -----------------------------------------------------------------------
// Shared channel state
// -----------------------------------------------------------------------

struct UiCommandChannel {
    std::mutex                mutex;
    std::optional<UnixSocket> sock;
};

struct GatewayChannel {
    std::mutex  mutex;
    UnixSocket* sock = nullptr;
};

// The run this engine is currently writing telemetry into. The row itself is
// created by the UI backend; the engine only looks it up.
struct RunTracker {
    bool    run_open = false;
    int64_t run_id   = -1;
};

// -----------------------------------------------------------------------
// Logging
// -----------------------------------------------------------------------

void log_message(const Message& msg) {
    switch (msg.type) {
        case MessageType::PositionEvent: {
            if (!g_verbose_telemetry) break;
            auto p = parse_position_event(msg);
            if (p) std::cout << "[t=" << p->timestamp_us << "us] position=" << p->position << "\n";
            break;
        }
        case MessageType::VelocityEvent: {
            if (!g_verbose_telemetry) break;
            auto p = parse_speed_event(msg);
            if (p) std::cout << "[t=" << p->timestamp_us << "us] speed=" << p->speed<< "\n";
            break;
        }
        case MessageType::CurrentEvent: {
            if (!g_verbose_telemetry) break;
            auto p = parse_current_event(msg);
            if (p) std::cout << "[t=" << p->timestamp_us << "us] current=" << p->current << "\n";
            break;
        }
        case MessageType::DeviceInfoResponse: {
            auto p = parse_device_info_response(msg);
            if (p) {
                std::cout << std::hex << std::showbase
                          << "device info: vendor=" << p->vendor_id
                          << " product=" << p->product_code
                          << " revision=" << p->revision
                          << " serial=" << p->serial
                          << std::dec << std::noshowbase << "\n";
            }
            break;
        }
        case MessageType::RunStopStatusEvent: {
            auto p = parse_run_stop_status_event(msg);
            if (p)
                std::cout << "[t=" << p->timestamp_us << "us] run/stop: "
                          << (p->running ? "RUNNING" : "STOPPED") << "\n";
            break;
        }
        case MessageType::McuStatusEvent: {
            auto p = parse_mcu_status_event(msg);
            if (p) std::cout << "mcu status: " << (*p ? "ALIVE" : "DEAD") << "\n";
            break;
        }
        case MessageType::Pong:
            std::cout << "pong\n";
            break;
        default:
            std::cout << "unhandled message type 0x"
                      << std::hex << static_cast<int>(msg.type) << std::dec << "\n";
    }
}

bool is_command_reply(MessageType type) {
    return type == MessageType::RunStopStatusEvent ||
           type == MessageType::DeviceInfoResponse ||
           type == MessageType::McuStatusEvent;
}

// -----------------------------------------------------------------------
// Gateway loop — relays messages, writes telemetry to DB
// -----------------------------------------------------------------------

void gateway_loop(UnixSocket& gateway_sock, UiCommandChannel& cmd_channel,
                  Database& db, const DataTypeIds& dt_ids, RunTracker& tracker) {
    gateway_sock.send(make_device_info_request());

    while (true) {
        auto messages = gateway_sock.receive();
        if (!messages) {
            std::cout << "gateway disconnected\n";
            return;
        }

        for (const auto& msg : *messages) {
            log_message(msg);

            if (msg.type == MessageType::RunStopStatusEvent) {
                auto p = parse_run_stop_status_event(msg);
                if (p) {
                    if (p->running && !tracker.run_open) {
                        // The backend owns the Runs row; find the open one.
                        tracker.run_id = db.current_run_id();
                        if (tracker.run_id < 0) {
                            tracker.run_open = false;
                            std::cerr << "device running but no open run in DB, "
                                         "telemetry will be dropped\n";
                        } else {
                            tracker.run_open = true;
                            std::cout << "attached to run id=" << tracker.run_id << "\n";
                        }
                    } else if (!p->running && tracker.run_open) {
                        std::cout << "detached from run id=" << tracker.run_id << "\n";
                        tracker.run_open = false;
                        tracker.run_id   = -1;
                    }
                }

            } else if (tracker.run_open) {
                if (msg.type == MessageType::PositionEvent) {
                    auto p = parse_position_event(msg);
                    if (p) db.insert_data(tracker.run_id, dt_ids.position,
                                          p->timestamp_us, p->position);
                } else if (msg.type == MessageType::VelocityEvent) {
                    auto p = parse_speed_event(msg);
                    if (p) db.insert_data(tracker.run_id, dt_ids.speed,
                                          p->timestamp_us, p->speed);
                } else if (msg.type == MessageType::CurrentEvent) {
                    auto p = parse_current_event(msg);
                    if (p) db.insert_data(tracker.run_id, dt_ids.current,
                                          p->timestamp_us,
                                          static_cast<int32_t>(p->current));
                }
            }

            if (is_command_reply(msg.type)) {
                std::lock_guard<std::mutex> lock(cmd_channel.mutex);
                if (cmd_channel.sock) cmd_channel.sock->send(msg);
            }
        }
    }
}

// -----------------------------------------------------------------------
// Accept loops
// -----------------------------------------------------------------------

void gateway_accept_loop(GatewayChannel& gw_channel, UiCommandChannel& cmd_channel,
                         Database& db, const DataTypeIds& dt_ids) {
    while (true) {
        std::cout << "listening for gateway on " << kGatewaySocketPath << "\n";
        auto sock = listen_and_accept(kGatewaySocketPath);
        if (!sock) {
            std::cerr << "failed to listen on " << kGatewaySocketPath << "\n";
            return;
        }
        std::cout << "gateway connected\n";

        {
            std::lock_guard<std::mutex> lock(gw_channel.mutex);
            gw_channel.sock = &*sock;
        }

        RunTracker tracker;
        gateway_loop(*sock, cmd_channel, db, dt_ids, tracker);

        {
            std::lock_guard<std::mutex> lock(gw_channel.mutex);
            gw_channel.sock = nullptr;
        }
    }
}

void ui_command_loop(GatewayChannel& gw_channel, UiCommandChannel& cmd_channel) {
    while (true) {
        std::cout << "listening for UI on " << kUiCommandSocketPath << "\n";
        auto accepted = listen_and_accept(kUiCommandSocketPath);
        if (!accepted) {
            std::cerr << "failed to listen on " << kUiCommandSocketPath << "\n";
            return;
        }
        std::cout << "UI connected\n";

        {
            std::lock_guard<std::mutex> lock(cmd_channel.mutex);
            cmd_channel.sock = std::move(*accepted);
        }

        while (true) {
            auto messages = cmd_channel.sock->receive();
            if (!messages) {
                std::cout << "UI disconnected\n";
                break;
            }
            for (const auto& msg : *messages) {
                if (msg.type != MessageType::SetRunStopCommand &&
                    msg.type != MessageType::DeviceInfoRequest)
                    continue;
                std::lock_guard<std::mutex> lock(gw_channel.mutex);
                if (gw_channel.sock) gw_channel.sock->send(msg);
            }
        }

        {
            std::lock_guard<std::mutex> lock(cmd_channel.mutex);
            cmd_channel.sock = std::nullopt;
        }
    }
}

}  // namespace

// -----------------------------------------------------------------------
// main
// -----------------------------------------------------------------------

int main(int argc, char** argv) {
    g_verbose_telemetry = std::getenv("WIZARD_VERBOSE") != nullptr;

    const std::string db_path = argc > 1 ? argv[1] : get_default_db_path();
    std::cout << "opening database " << db_path << "\n";

    Database db(db_path);

    // The database is created and migrated by the UI backend. Wait for it
    // rather than failing, so a device that boots first recovers on its own.
    while (!db.validate()) {
        std::cerr << "idle: " << db.last_error() << "\n";
        std::this_thread::sleep_for(std::chrono::seconds(5));
    }

    DataTypeIds dt_ids = db.resolve_data_types();
    while (dt_ids.position < 0 || dt_ids.speed < 0 || dt_ids.current < 0) {
        std::cerr << "idle: Data_Type rows not seeded yet\n";
        std::this_thread::sleep_for(std::chrono::seconds(5));
        dt_ids = db.resolve_data_types();
    }

    std::cout << "db ready - position=" << dt_ids.position
              << " speeed=" << dt_ids.speed
              << " current="  << dt_ids.current << "\n";

    GatewayChannel   gw_channel;
    UiCommandChannel cmd_channel;

    std::thread ui_thread(ui_command_loop,
                          std::ref(gw_channel), std::ref(cmd_channel));

    gateway_accept_loop(gw_channel, cmd_channel, db, dt_ids);

    ui_thread.join();
    return 0;
}
