#include <chrono>
#include <cstdlib>
#include <ctime>
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

bool g_verbose_telemetry = false;

// -----------------------------------------------------------------------
// Timestamped logging helpers
// -----------------------------------------------------------------------

std::string stamp() {
    using namespace std::chrono;
    const auto now  = system_clock::now();
    const auto secs = system_clock::to_time_t(now);
    const auto ms   = duration_cast<milliseconds>(now.time_since_epoch()).count() % 1000;

    std::tm tm_buf{};
    localtime_r(&secs, &tm_buf);

    char buf[16];
    strftime(buf, sizeof(buf), "%H:%M:%S", &tm_buf);

    std::ostringstream ss;
    ss << buf << '.' << std::setfill('0') << std::setw(3) << ms;
    return ss.str();
}

std::ostream& logline() { return std::cout << "[" << stamp() << "] "; }
std::ostream& errline() { return std::cerr << "[" << stamp() << "] "; }

std::string type_hex(MessageType t) {
    std::ostringstream ss;
    ss << "0x" << std::hex << static_cast<int>(t);
    return ss.str();
}

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

struct RunTracker {
    bool    run_open = false;
    int64_t run_id   = -1;
};

// Counters, so telemetry volume can be reported without logging every message.
struct Stats {
    uint64_t telemetry_received = 0;
    uint64_t rows_inserted      = 0;
    uint64_t replies_forwarded  = 0;
    uint64_t replies_dropped    = 0;
};

// -----------------------------------------------------------------------
// Logging
// -----------------------------------------------------------------------

void log_message(const Message& msg) {
    switch (msg.type) {
        case MessageType::PositionEvent: {
            if (!g_verbose_telemetry) break;
            auto p = parse_position_event(msg);
            if (p) logline() << "position=" << p->position << "\n";
            break;
        }
        case MessageType::SpeedEvent: {
            if (!g_verbose_telemetry) break;
            auto p = parse_speed_event(msg);
            if (p) logline() << "speed=" << p->speed << "\n";
            break;
        }
        case MessageType::CurrentEvent: {
            if (!g_verbose_telemetry) break;
            auto p = parse_current_event(msg);
            if (p) logline() << "current=" << p->current << "\n";
            break;
        }
        case MessageType::DeviceInfoResponse: {
            auto p = parse_device_info_response(msg);
            if (p) {
                logline() << std::hex << std::showbase
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
                logline() << "run/stop: " << (p->running ? "RUNNING" : "STOPPED")
                          << " (t=" << p->timestamp_us << "us)\n";
            break;
        }
        case MessageType::McuStatusEvent: {
            auto p = parse_mcu_status_event(msg);
            if (p) logline() << "mcu status: " << (*p ? "ALIVE" : "DEAD") << "\n";
            break;
        }
        case MessageType::Pong:
            logline() << "pong\n";
            break;
        default:
            logline() << "unhandled message type " << type_hex(msg.type) << "\n";
    }
}

bool is_command_reply(MessageType type) {
    return type == MessageType::RunStopStatusEvent ||
           type == MessageType::DeviceInfoResponse ||
           type == MessageType::McuStatusEvent;
}

bool is_telemetry(MessageType type) {
    return type == MessageType::PositionEvent ||
           type == MessageType::SpeedEvent ||
           type == MessageType::CurrentEvent;
}

// -----------------------------------------------------------------------
// Gateway loop
// -----------------------------------------------------------------------

void gateway_loop(UnixSocket& gateway_sock, UiCommandChannel& cmd_channel,
                  Database& db, const DataTypeIds& dt_ids, RunTracker& tracker,
                  Stats& stats) {
    logline() << "requesting device info\n";
    gateway_sock.send(make_device_info_request());

    while (true) {
        auto messages = gateway_sock.receive();
        if (!messages) {
            errline() << "gateway disconnected\n";
            return;
        }

        for (const auto& msg : *messages) {
            log_message(msg);

            if (is_telemetry(msg.type)) ++stats.telemetry_received;

            if (msg.type == MessageType::RunStopStatusEvent) {
                auto p = parse_run_stop_status_event(msg);
                if (p) {
                    if (p->running && !tracker.run_open) {
                        tracker.run_id = db.current_run_id();
                        if (tracker.run_id < 0) {
                            tracker.run_open = false;
                            errline() << "RUNNING but no open run in DB "
                                         "(no row in Runs with End_Time IS NULL) "
                                         "- telemetry will be dropped\n";
                        } else {
                            tracker.run_open = true;
                            logline() << "attached to run id=" << tracker.run_id << "\n";
                        }
                    } else if (p->running && tracker.run_open) {
                        logline() << "RUNNING repeated, already on run id="
                                  << tracker.run_id << "\n";
                    } else if (!p->running && tracker.run_open) {
                        logline() << "detached from run id=" << tracker.run_id
                                  << " after " << stats.rows_inserted << " rows\n";
                        tracker.run_open = false;
                        tracker.run_id   = -1;
                    } else {
                        logline() << "STOPPED with no run attached\n";
                    }
                } else {
                    errline() << "failed to parse RunStopStatusEvent\n";
                }

            } else if (tracker.run_open) {
                if (msg.type == MessageType::PositionEvent) {
                    auto p = parse_position_event(msg);
                    if (p) {
                        db.insert_data(tracker.run_id, dt_ids.position,
                                       p->timestamp_us, p->position);
                        ++stats.rows_inserted;
                    }
                } else if (msg.type == MessageType::SpeedEvent) {
                    auto p = parse_speed_event(msg);
                    if (p) {
                        db.insert_data(tracker.run_id, dt_ids.speed,
                                       p->timestamp_us, p->speed);
                        ++stats.rows_inserted;
                    }
                } else if (msg.type == MessageType::CurrentEvent) {
                    auto p = parse_current_event(msg);
                    if (p) {
                        db.insert_data(tracker.run_id, dt_ids.current,
                                       p->timestamp_us,
                                       static_cast<int32_t>(p->current));
                        ++stats.rows_inserted;
                    }
                }

                // Progress without per-message spam.
                if (stats.rows_inserted && stats.rows_inserted % 500 == 0)
                    logline() << stats.rows_inserted << " rows written to run id="
                              << tracker.run_id << "\n";

            } else if (is_telemetry(msg.type)) {
                // Telemetry arriving with no run attached is the silent
                // data-loss case, so say it once per hundred.
                if (stats.telemetry_received % 100 == 1)
                    errline() << "telemetry with no run attached, dropping ("
                              << stats.telemetry_received << " so far)\n";
            }

            // --- Forward command replies to the UI ---
            if (is_command_reply(msg.type)) {
                std::lock_guard<std::mutex> lock(cmd_channel.mutex);
                if (cmd_channel.sock) {
                    const bool ok = cmd_channel.sock->send(msg);
                    ++stats.replies_forwarded;
                    logline() << "forwarded " << type_hex(msg.type)
                              << " to UI" << (ok ? "" : " (SEND FAILED)") << "\n";
                } else {
                    ++stats.replies_dropped;
                    errline() << "dropped " << type_hex(msg.type)
                              << ": no UI connected (" << stats.replies_dropped
                              << " dropped so far)\n";
                }
            }
        }
    }
}

// -----------------------------------------------------------------------
// Accept loops
// -----------------------------------------------------------------------

void gateway_accept_loop(GatewayChannel& gw_channel, UiCommandChannel& cmd_channel,
                         Database& db, const DataTypeIds& dt_ids, Stats& stats) {
    while (true) {
        logline() << "listening for gateway on " << kGatewaySocketPath << "\n";
        auto sock = listen_and_accept(kGatewaySocketPath);
        if (!sock) {
            errline() << "failed to listen on " << kGatewaySocketPath << "\n";
            return;
        }
        logline() << "gateway connected\n";

        {
            std::lock_guard<std::mutex> lock(gw_channel.mutex);
            gw_channel.sock = &*sock;
        }

        RunTracker tracker;
        gateway_loop(*sock, cmd_channel, db, dt_ids, tracker, stats);

        {
            std::lock_guard<std::mutex> lock(gw_channel.mutex);
            gw_channel.sock = nullptr;
        }

        logline() << "gateway session ended: " << stats.rows_inserted
                  << " rows, " << stats.replies_forwarded << " replies forwarded, "
                  << stats.replies_dropped << " dropped\n";
    }
}

void ui_command_loop(GatewayChannel& gw_channel, UiCommandChannel& cmd_channel) {
    while (true) {
        logline() << "listening for UI on " << kUiCommandSocketPath << "\n";
        auto accepted = listen_and_accept(kUiCommandSocketPath);
        if (!accepted) {
            errline() << "failed to listen on " << kUiCommandSocketPath << "\n";
            return;
        }
        logline() << "UI connected\n";

        {
            std::lock_guard<std::mutex> lock(cmd_channel.mutex);
            cmd_channel.sock = std::move(*accepted);
        }

        while (true) {
            auto messages = cmd_channel.sock->receive();
            if (!messages) {
                errline() << "UI disconnected\n";
                break;
            }
            for (const auto& msg : *messages) {
                logline() << "UI sent " << type_hex(msg.type) << "\n";

                if (msg.type != MessageType::SetRunStopCommand &&
                    msg.type != MessageType::DeviceInfoRequest) {
                    errline() << "ignoring unexpected UI message "
                              << type_hex(msg.type) << "\n";
                    continue;
                }

                std::lock_guard<std::mutex> lock(gw_channel.mutex);
                if (gw_channel.sock) {
                    gw_channel.sock->send(msg);
                    logline() << "forwarded " << type_hex(msg.type) << " to gateway\n";
                } else {
                    errline() << "dropped " << type_hex(msg.type)
                              << ": no gateway connected\n";
                }
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
    logline() << "opening database " << db_path
              << (g_verbose_telemetry ? " (verbose telemetry ON)" : "") << "\n";

    Database db(db_path);

    DataTypeIds dt_ids;
    while (true) {
        if (db.validate()) {
            dt_ids = db.resolve_data_types();
            if (dt_ids.position >= 0 && dt_ids.speed >= 0 && dt_ids.current >= 0)
                break;
            errline() << "idle: Data_Type incomplete - position="
                      << dt_ids.position << " speed=" << dt_ids.speed
                      << " current=" << dt_ids.current << " (-1 = name not found)\n";
        } else {
            errline() << "idle: " << db.last_error() << "\n";
        }
        std::this_thread::sleep_for(std::chrono::seconds(5));
    }

    logline() << "db ready - position=" << dt_ids.position
              << " speed=" << dt_ids.speed
              << " current=" << dt_ids.current << "\n";

    logline() << "open runs at startup: run id=" << db.current_run_id() << "\n";

    GatewayChannel   gw_channel;
    UiCommandChannel cmd_channel;
    Stats            stats;

    std::thread ui_thread(ui_command_loop,
                          std::ref(gw_channel), std::ref(cmd_channel));

    gateway_accept_loop(gw_channel, cmd_channel, db, dt_ids, stats);

    ui_thread.join();
    return 0;
}
