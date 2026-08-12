#pragma once

#include <cstdint>
#include <cstring>
#include <optional>
#include <vector>

namespace wizard {

// Binary frame: [1 byte type][2 bytes length LE][payload of 'length' bytes]
// Deliberately simple for V1 (see docs/v1-spec.md, section 3).
enum class MessageType : uint8_t {
    Ping = 0x01,
    Pong = 0x02,

    // Telemetry events: one per PDO received, gateway -> engine.
    PositionEvent = 0x21,
    VelocityEvent = 0x22,
    CurrentEvent = 0x23,

    // Device Info command (SDO), engine -> gateway -> engine.
    DeviceInfoRequest = 0x30,
    DeviceInfoResponse = 0x31,

    // Run/stop: UI -> engine -> gateway (command), gateway -> engine -> UI (status).
    SetRunStopCommand = 0x40,
    RunStopStatusEvent = 0x41,

    // MCU status, only engine -> UI (device-gateway decides it and
    // sends it; engine just relays).
    McuStatusEvent = 0x51,
};

struct Message {
    MessageType type{};
    std::vector<uint8_t> payload;

    bool operator==(const Message& other) const {
        return type == other.type && payload == other.payload;
    }
};

constexpr size_t kHeaderSize = 3;  // 1 byte type + 2 bytes length

// Serializes a Message into the binary frame format, ready to write to the socket.
std::vector<uint8_t> encode_frame(const Message& msg);

class MessageParser {
public:
    // Appends bytes just read from the socket into the internal buffer.
    void feed(const uint8_t* data, size_t len);

    // Attempts to extract one complete message from the internal buffer.
    // Returns nullopt if there isn't a complete frame yet (normal on a
    // stream socket: data can arrive split across calls).
    std::optional<Message> try_parse();

private:
    std::vector<uint8_t> buffer_;
};

// --- Typed payloads for the V1 messages (docs/v1-spec.md, section 3) ---

struct PositionEventPayload {
    uint64_t timestamp_us;
    int32_t position;
};

struct VelocityEventPayload {
    uint64_t timestamp_us;
    int32_t velocity;
};

struct CurrentEventPayload {
    uint64_t timestamp_us;
    int16_t current;
};

struct DeviceInfoResponsePayload {
    uint32_t vendor_id;
    uint32_t product_code;
    uint32_t revision;
    uint32_t serial;
};

struct RunStopStatusPayload {
    uint64_t timestamp_us;
    bool running;
};

Message make_position_event(const PositionEventPayload& p);
Message make_velocity_event(const VelocityEventPayload& p);
Message make_current_event(const CurrentEventPayload& p);
Message make_device_info_request();
Message make_device_info_response(const DeviceInfoResponsePayload& p);
Message make_set_run_stop_command(bool run);
Message make_run_stop_status_event(const RunStopStatusPayload& p);
Message make_mcu_status_event(bool responding);

// Return nullopt if the Message payload doesn't have the expected size
// for that type (corrupted frame or unexpected type).
std::optional<PositionEventPayload> parse_position_event(const Message& m);
std::optional<VelocityEventPayload> parse_velocity_event(const Message& m);
std::optional<CurrentEventPayload> parse_current_event(const Message& m);
std::optional<DeviceInfoResponsePayload> parse_device_info_response(const Message& m);
std::optional<bool> parse_set_run_stop_command(const Message& m);
std::optional<RunStopStatusPayload> parse_run_stop_status_event(const Message& m);
std::optional<bool> parse_mcu_status_event(const Message& m);

}  // namespace wizard