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
    SpeedEvent = 0x22,
    CurrentEvent = 0x23,

    // Device Info command (SDO), engine -> gateway -> engine.
    DeviceInfoRequest = 0x30,
    DeviceInfoResponse = 0x31,

    // Run/stop: UI -> engine -> gateway (command), gateway -> engine -> UI (status).
    SetRunStopCommand = 0x40,
    RunStopStatusEvent = 0x41,

    // Motion: UI -> engine -> gateway. Torque / speed / position / stop / home
    // with a setpoint. Answered with RunStopStatusEvent (0x41), like 0x40.
    SetMotionCommand = 0x43,

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

struct SpeedEventPayload {
    uint64_t timestamp_us;
    int32_t speed;
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

// SetMotionCommand (0x43), 6-byte payload: [mode u8][setpoint i32 LE][direction u8].
enum class MotionMode : uint8_t {
    Stop = 0,
    Torque = 1,    // setpoint in mA
    Speed = 2,     // setpoint in rpm (magnitude, direction picks the sign)
    Position = 3,  // setpoint in encoder counts, absolute, 0 = home; direction ignored
    Home = 4,      // current position becomes 0; refused while running
};

enum class MotionDirection : uint8_t {
    Cw = 0,
    Ccw = 1,
};

struct SetMotionPayload {
    MotionMode mode;
    int32_t setpoint;  // 0 for Stop / Home
    MotionDirection direction;
};

constexpr size_t kSetMotionPayloadSize = 6;

Message make_position_event(const PositionEventPayload& p);
Message make_speed_event(const SpeedEventPayload& p);
Message make_current_event(const CurrentEventPayload& p);
Message make_device_info_request();
Message make_device_info_response(const DeviceInfoResponsePayload& p);
Message make_set_run_stop_command(bool run);
Message make_run_stop_status_event(const RunStopStatusPayload& p);
Message make_mcu_status_event(bool responding);
Message make_set_motion_command(const SetMotionPayload& p);

// Return nullopt if the Message payload doesn't have the expected size
// for that type (corrupted frame or unexpected type).
std::optional<PositionEventPayload> parse_position_event(const Message& m);
std::optional<SpeedEventPayload> parse_speed_event(const Message& m);
std::optional<CurrentEventPayload> parse_current_event(const Message& m);
std::optional<DeviceInfoResponsePayload> parse_device_info_response(const Message& m);
std::optional<bool> parse_set_run_stop_command(const Message& m);
std::optional<RunStopStatusPayload> parse_run_stop_status_event(const Message& m);
std::optional<bool> parse_mcu_status_event(const Message& m);
// Also nullopt for an unknown mode (> 4) or direction (> 1).
std::optional<SetMotionPayload> parse_set_motion_command(const Message& m);

}  // namespace wizard
