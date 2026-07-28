#pragma once

#include <cstdint>
#include <cstring>
#include <optional>
#include <vector>

namespace wizard {

// Frame binario: [1 byte type][2 bytes length LE][payload de 'length' bytes]
// Simple a propósito para V1 (ver docs/v1-spec.md, sección 3).
enum class MessageType : uint8_t {
    Ping = 0x01,
    Pong = 0x02,

    // Eventos de telemetría: uno por PDO recibido, gateway -> engine.
    PositionEvent = 0x21,
    VelocityEvent = 0x22,
    CurrentEvent = 0x23,

    // Comando Device Info (SDO), engine -> gateway -> engine.
    DeviceInfoRequest = 0x30,
    DeviceInfoResponse = 0x31,
};

struct Message {
    MessageType type{};
    std::vector<uint8_t> payload;

    bool operator==(const Message& other) const {
        return type == other.type && payload == other.payload;
    }
};

constexpr size_t kHeaderSize = 3;  // 1 byte type + 2 bytes length

// Serializa un Message al formato de frame binario, listo para escribir al socket.
std::vector<uint8_t> encode_frame(const Message& msg);

class MessageParser {
public:
    // Añade bytes recién leídos del socket al buffer interno.
    void feed(const uint8_t* data, size_t len);

    // Intenta extraer un mensaje completo del buffer interno.
    // Devuelve nullopt si no hay un frame completo todavía (normal en un
    // socket de stream: los datos pueden llegar partidos entre llamadas).
    std::optional<Message> try_parse();

private:
    std::vector<uint8_t> buffer_;
};

// --- Payloads tipados para los mensajes de V1 (docs/v1-spec.md, sección 3) ---

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

Message make_position_event(const PositionEventPayload& p);
Message make_velocity_event(const VelocityEventPayload& p);
Message make_current_event(const CurrentEventPayload& p);
Message make_device_info_request();
Message make_device_info_response(const DeviceInfoResponsePayload& p);

// Devuelven nullopt si el payload del Message no tiene el tamaño esperado
// para ese tipo (frame corrupto o tipo inesperado).
std::optional<PositionEventPayload> parse_position_event(const Message& m);
std::optional<VelocityEventPayload> parse_velocity_event(const Message& m);
std::optional<CurrentEventPayload> parse_current_event(const Message& m);
std::optional<DeviceInfoResponsePayload> parse_device_info_response(const Message& m);

}  // namespace wizard