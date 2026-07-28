#include <iostream>
#include <iomanip>

#include "message.h"
#include "unix_socket.h"

using namespace wizard;

namespace {

constexpr const char* kSocketPath = "/tmp/wizard-backend.sock";

// Prints one received message in a human-readable form.
void log_message(const Message& msg) {
    switch (msg.type) {
        case MessageType::PositionEvent: {
            auto p = parse_position_event(msg);
            if (p) std::cout << "[t=" << p->timestamp_us << "us] position=" << p->position << "\n";
            break;
        }
        case MessageType::VelocityEvent: {
            auto p = parse_velocity_event(msg);
            if (p) std::cout << "[t=" << p->timestamp_us << "us] velocity=" << p->velocity << "\n";
            break;
        }
        case MessageType::CurrentEvent: {
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
        case MessageType::Pong:
            std::cout << "pong\n";
            break;
        default:
            std::cout << "unhandled message type\n";
    }
}

}  // namespace

int main() {
    std::cout << "listening on " << kSocketPath << "\n";
    auto server = listen_and_accept(kSocketPath);
    if (!server) {
        std::cerr << "failed to listen on " << kSocketPath << "\n";
        return 1;
    }
    std::cout << "gateway connected\n";

    // device info is requested once at startup, no refresh.
    server->send(make_device_info_request());

    while (true) {
        auto messages = server->receive();
        if (!messages) {
            std::cout << "gateway disconnected\n";
            break;
        }
        for (const auto& msg : *messages) log_message(msg);
    }
    return 0;
}