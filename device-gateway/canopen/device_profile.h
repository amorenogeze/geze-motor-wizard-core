#pragma once

#include <cstdint>
#include <map>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

#include "../device_translator.h"

namespace wizard {

// Everything device-specific about a CANopen node, loaded from a JSON file
// (e.g. canopen/devices/solopico.json): which objects exist and where, how to
// configure the node, how to tell it is alive or running, and the telemetry.
// CanopenTranslator contains no object indexes, COB-IDs or scaling of its own.
// What a command does (run, stop, motion modes) is NOT here: that is the
// command set (commands/<device>_commands.json, commands/commands.h).

// How a 32-bit SDO/PDO payload is interpreted.
//   Q17 = signed fixed point value * 131072 (2^17), used by SOLO for floats.
enum class ValueType { U8, U16, U32, I16, I32, Q17 };

struct ObjectDef {
    std::string name;
    uint16_t index = 0;
    uint8_t subindex = 0;
    ValueType type = ValueType::U32;
};

// WriteStep (one write in a sequence) is defined in device_translator.h.

enum class TelemetrySource { Tpdo, SdoPoll };

struct TelemetryChannel {
    TelemetryKind kind = TelemetryKind::Position;
    TelemetrySource source = TelemetrySource::Tpdo;
    uint32_t cob_id = 0;       // Tpdo only
    std::string object;        // SdoPoll only
    uint32_t period_ms = 0;    // SdoPoll only
    ValueType type = ValueType::I32;
    double scale = 1.0;        // sent value = round(decoded * scale)
};

// A DeviceInfo field: read from an object, or a fixed value.
struct InfoField {
    std::optional<std::string> object;
    uint32_t value = 0;
};

struct DeviceProfile {
    std::string name;
    uint8_t node_id = 1;

    uint32_t sdo_timeout_ms = 2000;
    bool sdo_size_indicated = true;             // false: write cs 0x22 (SOLO style)
    std::optional<uint8_t> sdo_read_subindex;   // override subindex on reads

    uint32_t sync_period_ms = 0;                // 0 = never send SYNC
    bool telemetry_only_while_running = true;   // SYNC / polling only while RUN

    std::map<std::string, ObjectDef> objects;

    std::vector<WriteStep> configure;  // once each time the node (re)appears

    std::string status_object;         // read to answer "is it running?"
    double status_running_value = 1;
    std::string alive_object;          // read by the heartbeat

    InfoField vendor_id, product_code, revision, serial;

    std::vector<TelemetryChannel> telemetry;

    // Throws ProfileError if the name is unknown (cannot happen after a
    // successful load: every reference is checked while parsing).
    const ObjectDef& object(const std::string& name) const;
};

class ProfileError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

// Parse + validate. Throws ProfileError listing every problem found.
DeviceProfile parse_device_profile(const std::string& json_text);
DeviceProfile load_device_profile(const std::string& path);

// Value <-> raw 32-bit little-endian payload. encode_value throws
// ProfileError if the value does not fit the type.
uint32_t encode_value(ValueType type, double value);
double decode_value(ValueType type, uint32_t raw);
uint8_t value_size(ValueType type);  // bytes on the wire (1, 2 or 4)

}  // namespace wizard
