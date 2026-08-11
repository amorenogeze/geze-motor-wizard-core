#include <iostream>
#include <iomanip>
#include <cstdlib>
#include <mutex>
#include <optional>
#include <thread>

#include "message.h"
#include "unix_socket.h"

using namespace wizard;

namespace {

constexpr const char* kGatewaySocketPath = "/tmp/wizard-backend.sock";
constexpr const char* kUiSocketPath = "/tmp/wizard-ui.sock";

// Holds the UI's UnixSocket once connected, shared between gateway_loop
// (forwards events to it) and ui_loop (owns the connection, receives
// commands from it).
struct UiChannel {
    std::mutex mutex;
    std::optional<UnixSocket> sock;
};

// Points to whichever UnixSocket is the currently active gateway
// connection (owned by main()'s reconnect loop). Nullptr if no gateway
// is connected right now. Lets ui_loop forward commands even across
// gateway reconnects.
struct GatewayChannel {
    std::mutex mutex;
    UnixSocket* sock = nullptr;
};

// Controls per-telemetry-message logging in log_message() below. Off by
// default - telemetry can be very chatty (~500 messages/sec combined
// while RUNNING). Enable with WIZARD_VERBOSE=1. Connection/error
// logging stays on regardless of this flag.
bool g_verbose_telemetry = false;

// Prints one received message in a human-readable form.
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
            if (p) {
                std::cout << "[t=" << p->timestamp_us << "us] run/stop status: "
                          << (p->running ? "RUNNING" : "STOPPED") << "\n";
            }
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
            std::cout << "unhandled message type\n";
    }
}

// Reads telemetry/device-info/status/heartbeat from device-gateway, logs
// everything, and forwards it to the UI. No inference happens here -
// device-gateway already decided what McuStatusEvent/RunStopStatusEvent
// mean; engine just relays. Returns when the gateway disconnects (caller
// decides whether to reconnect).
void gateway_loop(UnixSocket& gateway_sock, UiChannel& ui_channel) {
    // Requested once per (re)connection - no refresh beyond that unless
    // the UI asks for it on demand.
    gateway_sock.send(make_device_info_request());

    while (true) {
        auto messages = gateway_sock.receive();
        if (!messages) {
            std::cout << "gateway disconnected\n";
            return;
        }
        for (const auto& msg : *messages) {
            log_message(msg);

            bool forward_to_ui = msg.type == MessageType::RunStopStatusEvent ||
                                  msg.type == MessageType::DeviceInfoResponse ||
                                  msg.type == MessageType::McuStatusEvent ||
                                  msg.type == MessageType::PositionEvent ||
                                  msg.type == MessageType::VelocityEvent ||
                                  msg.type == MessageType::CurrentEvent;
            if (forward_to_ui) {
                std::lock_guard<std::mutex> lock(ui_channel.mutex);
                if (ui_channel.sock) ui_channel.sock->send(msg);
            }
        }
    }
}

// Accepts the (single) gateway connection over and over, forever: when
// one disconnects, goes back to listening for the next one.
void gateway_accept_loop(GatewayChannel& gateway_channel, UiChannel& ui_channel) {
    while (true) {
        std::cout << "listening on " << kGatewaySocketPath << "\n";
        auto sock = listen_and_accept(kGatewaySocketPath);
        if (!sock) {
            std::cerr << "failed to listen on " << kGatewaySocketPath << "\n";
            return;
        }
        std::cout << "gateway connected\n";

        {
            std::lock_guard<std::mutex> lock(gateway_channel.mutex);
            gateway_channel.sock = &*sock;
        }

        gateway_loop(*sock, ui_channel);  // blocks until gateway disconnects

        {
            std::lock_guard<std::mutex> lock(gateway_channel.mutex);
            gateway_channel.sock = nullptr;
        }
        // loop back and accept the next gateway connection
    }
}

// Waits for the UI to connect, stores the connection in 'ui_channel', then
// forwards every SetRunStopCommand/DeviceInfoRequest it sends towards
// whichever gateway is currently connected (if any). Runs forever on its
// own thread.
void ui_loop(GatewayChannel& gateway_channel, UiChannel& ui_channel) {
    std::cout << "listening for UI on " << kUiSocketPath << "\n";
    auto accepted = listen_and_accept(kUiSocketPath);
    if (!accepted) {
        std::cerr << "failed to listen on " << kUiSocketPath << "\n";
        return;
    }
    std::cout << "UI connected\n";

    {
        std::lock_guard<std::mutex> lock(ui_channel.mutex);
        ui_channel.sock = std::move(*accepted);
    }

    while (true) {
        auto messages = ui_channel.sock->receive();
        if (!messages) {
            std::cout << "UI disconnected\n";
            return;
        }
        for (const auto& msg : *messages) {
            if (msg.type != MessageType::SetRunStopCommand &&
                msg.type != MessageType::DeviceInfoRequest) {
                continue;
            }
            std::lock_guard<std::mutex> lock(gateway_channel.mutex);
            if (gateway_channel.sock) gateway_channel.sock->send(msg);
        }
    }
}

}  // namespace

int main() {
    g_verbose_telemetry = std::getenv("WIZARD_VERBOSE") != nullptr;
    
    GatewayChannel gateway_channel;
    UiChannel ui_channel;

    std::thread ui_thread(ui_loop, std::ref(gateway_channel), std::ref(ui_channel));

    gateway_accept_loop(gateway_channel, ui_channel);  // runs on main thread, forever

    ui_thread.join();
    return 0;
}