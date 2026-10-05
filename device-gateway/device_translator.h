#pragma once

#include <cstdint>
#include <optional>
#include <chrono>

namespace wizard {

// Generic, protocol-independent (CANopen, Modbus, etc).
enum class TelemetryKind { Position, Velocity, Current };

struct TelemetrySample {
    uint64_t timestamp_us;
    TelemetryKind kind;
    int32_t value;  // Current only uses the low 16 bits, sign-extended.
};

enum class TelemetryWait { Sample, Timeout, Closed };

struct DeviceInfo {
    uint32_t vendor_id;
    uint32_t product_code;
    uint32_t revision;
    uint32_t serial;
};

// Protocol-agnostic interface. main.cpp only depends on this.
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

    // Like read_next_telemetry, but gives up after 'timeout' so the caller
    // can check other conditions (e.g. the engine went away while STOPPED,
    // when no telemetry flows). Sample: 'out' is filled. Closed: transport
    // error, same meaning as nullopt above.
    virtual TelemetryWait wait_next_telemetry(std::chrono::milliseconds timeout, 
            TelemetrySample& out) = 0;

    // Pong: just looking for answer no content.
    virtual bool probe_alive() = 0;
};

}  // namespace wizard
