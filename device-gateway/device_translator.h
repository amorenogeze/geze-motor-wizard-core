#pragma once

#include <cstdint>
#include <optional>

namespace wizard {

// Generic telemetry sample, independent of the underlying protocol
// (CANopen PDO, Modbus register poll, etc).
enum class TelemetryKind { Position, Velocity, Current };

struct TelemetrySample {
    uint64_t timestamp_us;
    TelemetryKind kind;
    int32_t value;  // Current only uses the low 16 bits, sign-extended.
};

struct DeviceInfo {
    uint32_t vendor_id;
    uint32_t product_code;
    uint32_t revision;
    uint32_t serial;
};

// Protocol-agnostic device access. device-gateway/main.cpp depends only on
// this interface, never on a specific protocol client directly.
class DeviceTranslator {
public:
    virtual ~DeviceTranslator() = default;

    // Blocking read, once at startup (see docs/v1-spec.md). Nullopt on failure.
    virtual std::optional<DeviceInfo> read_device_info() = 0;

    // Writes the run/stop command. Returns false on failure.
    virtual bool set_run_stop(bool run) = 0;

    // Blocking read of the current run/stop status. Nullopt on failure.
    virtual std::optional<bool> read_run_stop_status() = 0;

    // Blocks until the next telemetry sample is available. Nullopt only on
    // unrecoverable transport error.
    virtual std::optional<TelemetrySample> read_next_telemetry() = 0;

    // Lightweight liveness probe: true if the device responded to a
    // minimal request. Ignores the actual value read - only cares whether
    // the device answered at all (see docs/v1-spec.md, heartbeat design).
    virtual bool probe_alive() = 0;
};

}  // namespace wizard