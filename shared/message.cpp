#include "message.h"

namespace wizard {

namespace {

void put_u16le(std::vector<uint8_t>& out, uint16_t v) {
    out.push_back(static_cast<uint8_t>(v & 0xFF));
    out.push_back(static_cast<uint8_t>((v >> 8) & 0xFF));
}

void put_u64le(std::vector<uint8_t>& out, uint64_t v) {
    for (int i = 0; i < 8; ++i) out.push_back(static_cast<uint8_t>((v >> (8 * i)) & 0xFF));
}

void put_u32le(std::vector<uint8_t>& out, uint32_t v) {
    for (int i = 0; i < 4; ++i) out.push_back(static_cast<uint8_t>((v >> (8 * i)) & 0xFF));
}

void put_i32le(std::vector<uint8_t>& out, int32_t v) { put_u32le(out, static_cast<uint32_t>(v)); }

void put_i16le(std::vector<uint8_t>& out, int16_t v) {
    uint16_t u = static_cast<uint16_t>(v);
    out.push_back(static_cast<uint8_t>(u & 0xFF));
    out.push_back(static_cast<uint8_t>((u >> 8) & 0xFF));
}

uint64_t get_u64le(const uint8_t* p) {
    uint64_t v = 0;
    for (int i = 0; i < 8; ++i) v |= static_cast<uint64_t>(p[i]) << (8 * i);
    return v;
}

uint32_t get_u32le(const uint8_t* p) {
    uint32_t v = 0;
    for (int i = 0; i < 4; ++i) v |= static_cast<uint32_t>(p[i]) << (8 * i);
    return v;
}

int32_t get_i32le(const uint8_t* p) { return static_cast<int32_t>(get_u32le(p)); }

int16_t get_i16le(const uint8_t* p) {
    uint16_t v = static_cast<uint16_t>(p[0]) | (static_cast<uint16_t>(p[1]) << 8);
    return static_cast<int16_t>(v);
}

}  // namespace

std::vector<uint8_t> encode_frame(const Message& msg) {
    std::vector<uint8_t> out;
    out.reserve(kHeaderSize + msg.payload.size());
    out.push_back(static_cast<uint8_t>(msg.type));
    put_u16le(out, static_cast<uint16_t>(msg.payload.size()));
    out.insert(out.end(), msg.payload.begin(), msg.payload.end());
    return out;
}

void MessageParser::feed(const uint8_t* data, size_t len) {
    buffer_.insert(buffer_.end(), data, data + len);
}

std::optional<Message> MessageParser::try_parse() {
    if (buffer_.size() < kHeaderSize) return std::nullopt;

    uint8_t type_byte = buffer_[0];
    uint16_t length = static_cast<uint16_t>(buffer_[1]) | (static_cast<uint16_t>(buffer_[2]) << 8);

    if (buffer_.size() < kHeaderSize + length) return std::nullopt;  // frame incompleto todavía

    Message msg;
    msg.type = static_cast<MessageType>(type_byte);
    msg.payload.assign(buffer_.begin() + kHeaderSize, buffer_.begin() + kHeaderSize + length);

    buffer_.erase(buffer_.begin(), buffer_.begin() + kHeaderSize + length);
    return msg;
}

Message make_position_event(const PositionEventPayload& p) {
    Message m;
    m.type = MessageType::PositionEvent;
    put_u64le(m.payload, p.timestamp_us);
    put_i32le(m.payload, p.position);
    return m;
}

Message make_velocity_event(const VelocityEventPayload& p) {
    Message m;
    m.type = MessageType::VelocityEvent;
    put_u64le(m.payload, p.timestamp_us);
    put_i32le(m.payload, p.velocity);
    return m;
}

Message make_current_event(const CurrentEventPayload& p) {
    Message m;
    m.type = MessageType::CurrentEvent;
    put_u64le(m.payload, p.timestamp_us);
    put_i16le(m.payload, p.current);
    return m;
}

Message make_device_info_request() {
    Message m;
    m.type = MessageType::DeviceInfoRequest;
    return m;  // payload vacío, ver spec 3.2
}

Message make_device_info_response(const DeviceInfoResponsePayload& p) {
    Message m;
    m.type = MessageType::DeviceInfoResponse;
    put_u32le(m.payload, p.vendor_id);
    put_u32le(m.payload, p.product_code);
    put_u32le(m.payload, p.revision);
    put_u32le(m.payload, p.serial);
    return m;
}

Message make_set_run_stop_command(bool run) {
    Message m;
    m.type = MessageType::SetRunStopCommand;
    m.payload.push_back(run ? 1 : 0);
    return m;
}

Message make_run_stop_status_event(const RunStopStatusPayload& p) {
    Message m;
    m.type = MessageType::RunStopStatusEvent;
    put_u64le(m.payload, p.timestamp_us);
    m.payload.push_back(p.running ? 1 : 0);
    return m;
}

Message make_mcu_status_event(bool responding) {
    Message m;
    m.type = MessageType::McuStatusEvent;
    m.payload.push_back(responding ? 1 : 0);
    return m;
}

std::optional<PositionEventPayload> parse_position_event(const Message& m) {
    if (m.type != MessageType::PositionEvent || m.payload.size() != 12) return std::nullopt;
    return PositionEventPayload{get_u64le(&m.payload[0]), get_i32le(&m.payload[8])};
}

std::optional<VelocityEventPayload> parse_velocity_event(const Message& m) {
    if (m.type != MessageType::VelocityEvent || m.payload.size() != 12) return std::nullopt;
    return VelocityEventPayload{get_u64le(&m.payload[0]), get_i32le(&m.payload[8])};
}

std::optional<CurrentEventPayload> parse_current_event(const Message& m) {
    if (m.type != MessageType::CurrentEvent || m.payload.size() != 10) return std::nullopt;
    return CurrentEventPayload{get_u64le(&m.payload[0]), get_i16le(&m.payload[8])};
}

std::optional<DeviceInfoResponsePayload> parse_device_info_response(const Message& m) {
    if (m.type != MessageType::DeviceInfoResponse || m.payload.size() != 16) return std::nullopt;
    return DeviceInfoResponsePayload{get_u32le(&m.payload[0]), get_u32le(&m.payload[4]),
                                      get_u32le(&m.payload[8]), get_u32le(&m.payload[12])};
}

std::optional<bool> parse_set_run_stop_command(const Message& m) {
    if (m.type != MessageType::SetRunStopCommand || m.payload.size() != 1) return std::nullopt;
    return m.payload[0] != 0;
}

std::optional<RunStopStatusPayload> parse_run_stop_status_event(const Message& m) {
    if (m.type != MessageType::RunStopStatusEvent || m.payload.size() != 9) return std::nullopt;
    return RunStopStatusPayload{get_u64le(&m.payload[0]), m.payload[8] != 0};
}

std::optional<bool> parse_mcu_status_event(const Message& m) {
    if (m.type != MessageType::McuStatusEvent || m.payload.size() != 1) return std::nullopt;
    return m.payload[0] != 0;
}

}  // namespace wizard