#include <cstdlib>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <mutex>
#include <optional>
#include <thread>
#include <unistd.h>

#include "db.h"
#include "message.h"
#include "unix_socket.h"

using namespace wizard;

namespace {

// Returns the DB path next to the binary, falling back to the compiled-in
// default if /proc/self/exe is not readable (e.g. non-Linux host).
std::string get_default_db_path() {
    char buf[4096];
    ssize_t len = readlink("/proc/self/exe", buf, sizeof(buf) - 1);
    if (len < 0) return kDefaultDbPath;
    buf[len] = '\0';
    return (std::filesystem::path(buf).parent_path() / "door_tuning_wizard.db").string();
}

// Controls per-telemetry-message console logging. Off by default —
// telemetry is high volume (~500 msg/sec while RUNNING). Enable with
// WIZARD_VERBOSE=1 at runtime. Connection/error logging is always on.
bool g_verbose_telemetry = false;

// -----------------------------------------------------------------------
// Shared channel state
// -----------------------------------------------------------------------

// Command channel: UI -> engine (SetRunStopCommand, DeviceInfoRequest)
//                  engine -> UI (RunStopStatusEvent, DeviceInfoResponse,
//                                McuStatusEvent)
struct UiCommandChannel {
    std::mutex            mutex;
    std::optional<UnixSocket> sock;
};

// Points to the currently active gateway socket. nullptr if disconnected.
struct GatewayChannel {
    std::mutex  mutex;
    UnixSocket* sock = nullptr;
};

// Tracks the currently open run/session. Only touched from gateway_loop
// (single thread) — no locking needed.
struct RunTracker {
    bool    run_open    = false;
    int64_t session_id  = -1;
    int64_t run_id      = -1;
    int64_t device_id   = -1;
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
            auto p = parse_velocity_event(msg);
            if (p) std::cout << "[t=" << p->timestamp_us << "us] velocity=" << p->velocity << "\n";
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
                   Database& db, DataTypeIds& dt_ids, RunTracker& tracker) {
    // Request device info once per (re)connection.
    gateway_sock.send(make_device_info_request());

    while (true) {
        auto messages = gateway_sock.receive();
        if (!messages) {
            std::cout << "gateway disconnected\n";
            return;
        }

        for (const auto& msg : *messages) {
            log_message(msg);

            // --- Device Info — register/find device in DB ---
            if (msg.type == MessageType::DeviceInfoResponse) {
                auto p = parse_device_info_response(msg);
                if (p && tracker.device_id < 0) {
                    std::string serial = std::to_string(p->serial);
                    tracker.device_id = db.ensure_device(serial);
                    std::cout << "device registered, db id=" << tracker.device_id << "\n";

                    // Open session now that we know the device.
                    if (tracker.session_id < 0) {
                        tracker.session_id = db.open_session();
                        std::cout << "session opened, id=" << tracker.session_id << "\n";
                    }
                }

            // --- Run/Stop state machine ---
            } else if (msg.type == MessageType::RunStopStatusEvent) {
                auto p = parse_run_stop_status_event(msg);
                if (p) {
                    if (p->running && !tracker.run_open && tracker.session_id >= 0) {
                        tracker.run_id  = db.open_run(tracker.session_id, tracker.device_id);
                        tracker.run_open = true;
                        std::cout << "run opened, id=" << tracker.run_id << "\n";
                    } else if (!p->running && tracker.run_open) {
                        db.close_run(tracker.run_id);
                        tracker.run_open = false;
                        std::cout << "run closed, id=" << tracker.run_id << "\n";
                    }
                }

            // --- Telemetry — write to DB only while a run is open ---
	    } else if (tracker.run_open) {
		    if (msg.type == MessageType::PositionEvent) {
		        auto p = parse_position_event(msg);
		        if (p) db.insert_data(tracker.run_id, 1,  // position
		                               p->timestamp_us, p->position);
		    } else if (msg.type == MessageType::VelocityEvent) {
		        auto p = parse_velocity_event(msg);
		        if (p) db.insert_data(tracker.run_id, 2,  // velocity
		                               p->timestamp_us, p->velocity);
		    } else if (msg.type == MessageType::CurrentEvent) {
		        auto p = parse_current_event(msg);
			if (p) db.insert_data(tracker.run_id, dt_ids.current,
                       p->timestamp_us, static_cast<int32_t>(p->current));
		    }
		}

            // --- Forward command replies to UI ---
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
                          Database& db, DataTypeIds& dt_ids) {
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

        // Fresh tracker per gateway connection — each reconnect starts a
        // new session/run context.
        RunTracker tracker;
        gateway_loop(*sock, cmd_channel, db, dt_ids, tracker);

        // If a run was still open when gateway disconnected, close it.
        if (tracker.run_open)  db.close_run(tracker.run_id);
        if (tracker.session_id >= 0) db.close_session(tracker.session_id);

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

    std::string db_path = argc > 1 ? argv[1] : get_default_db_path();

    std::cout << "opening database " << db_path << "\n";
    Database db(db_path);
    db.connect();
    DataTypeIds dt_ids = db.seed_static_data();
    std::cout << "db ready — position=" << dt_ids.position
              << " velocity=" << dt_ids.velocity
              << " current=" << dt_ids.current << "\n";

    GatewayChannel  gw_channel;
    UiCommandChannel cmd_channel;

    std::thread ui_thread(ui_command_loop,
                          std::ref(gw_channel), std::ref(cmd_channel));

    gateway_accept_loop(gw_channel, cmd_channel, db, dt_ids);  // runs forever

    ui_thread.join();
    return 0;
}
