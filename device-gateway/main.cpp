#include <iostream>

#include "canopen/canopen_client.h"
#include "message.h"
#include "unix_socket.h"

using namespace wizard;

namespace {
constexpr const char* kSocketPath = "/tmp/wizard-backend.sock";
constexpr const char* kDefaultCanInterface = "vcan0";

// Identity Object (0x1018) subindices, read once at startup (see spec).
constexpr uint16_t kIdentityIndex = 0x1018;

// Reads all 4 Identity Object fields and replies on the engine socket.
// Returns false if any SDO read fails.
bool handle_device_info_request(CanopenClient& can, UnixSocket& engine_sock) {
    auto vendor = can.sdo_read_u32(kIdentityIndex, 0x01);
    auto product = can.sdo_read_u32(kIdentityIndex, 0x02);
    auto revision = can.sdo_read_u32(kIdentityIndex, 0x03);
    auto serial = can.sdo_read_u32(kIdentityIndex, 0x04);

    if (!vendor || !product || !revision || !serial) {
        std::cerr << "device info SDO read failed\n";
        return false;
    }

    DeviceInfoResponsePayload payload{*vendor, *product, *revision, *serial};
    return engine_sock.send(make_device_info_response(payload));
}

// Converts one decoded PDO sample into its corresponding socket event.
Message pdo_sample_to_message(const PdoSample& sample) {
    switch (sample.kind) {
        case PdoKind::Position:
            return make_position_event({sample.timestamp_us, sample.value});
        case PdoKind::Velocity:
            return make_velocity_event({sample.timestamp_us, sample.value});
        case PdoKind::Current:
            return make_current_event({sample.timestamp_us, static_cast<int16_t>(sample.value)});
    }
    return make_position_event({sample.timestamp_us, sample.value});  // unreachable
}

}  // namespace

int main(int argc, char** argv) {
    std::string can_iface = argc > 1 ? argv[1] : kDefaultCanInterface;

    std::cout << "connecting to " << kSocketPath << "\n";
    UnixSocket engine_sock = connect_to(kSocketPath);
    std::cout << "connected to wizard-engine\n";

    std::cout << "opening CAN interface " << can_iface << "\n";
    CanopenClient can(can_iface);
    std::cout << "CAN interface ready\n";

    // Version1: block until engine's single DeviceInfoRequest arrives, handle it,
    // then move on to the PDO forwarding loop. No further commands expected.
    while (true) {
        auto messages = engine_sock.receive();
        if (!messages) {
            std::cerr << "wizard-engine disconnected before requesting device info\n";
            return 1;
        }
        bool handled = false;
        for (const auto& msg : *messages) {
            if (msg.type == MessageType::DeviceInfoRequest) {
                handled = handle_device_info_request(can, engine_sock);
                break;
            }
        }
        if (handled) break;
    }

    std::cout << "forwarding telemetry\n";
    while (true) {
        auto sample = can.read_next_pdo();
        if (!sample) {
            std::cerr << "CAN read error\n";
            return 1;
        }
        if (!engine_sock.send(pdo_sample_to_message(*sample))) {
            std::cerr << "wizard-engine disconnected\n";
            return 1;
        }
    }
}