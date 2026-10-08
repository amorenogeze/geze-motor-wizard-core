#pragma once

#include <cstdint>
#include <optional>
#include <chrono>
#include <string>
#include <vector>

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

// One write in a sequence, by object NAME (the transport maps names to
// addresses). Used by the device profile (configure) and the command set
// (enable / stop / home / motion modes).
struct WriteStep {
    std::string object;
    double value = 0.0;
    bool verify = false;    // read back and compare after writing
    uint32_t wait_ms = 0;   // sleep after the write (e.g. motor identification)
    uint32_t ramp_ms = 0;   // >0: move from the current value to 'value' over this
                            // time in small steps (e.g. torque 0 -> 2 A -> 0)
    double ramp_rate = 0;   // >0: same, at this rate in object units per second
                            // (e.g. 0.5 = 0.5 A/s). Exclusive with ramp_ms.
    std::string ramp_start_object;  // optional: start the ramp from |this object|
                            // when it is smaller than the current value (e.g. the
                            // measured current when the reference is unreachable)
};

// Protocol-agnostic interface. main.cpp and CommandManager only depend on this.
class DeviceTranslator {
public:
    virtual ~DeviceTranslator() = default;

    // Blocking read, once at startup (see docs/v1-spec.md). Nullopt on failure.
    virtual std::optional<DeviceInfo> read_device_info() = 0;

    // Runs 'steps' to start the motor (configures first if needed). A ramp away
    // from zero gives way to request_stop() / stop(). On failure the stop
    // sequence runs, so the drive is never left half-started. True = running.
    virtual bool start(const std::vector<WriteStep>& steps) = 0;

    // Runs 'steps' without changing the running state (configures first if
    // needed): a live update of a running reference, or HOME while stopped.
    // Ramps away from zero give way to request_stop() / stop().
    virtual bool execute(const std::vector<WriteStep>& steps) = 0;

    // The stop sequence; every step is attempted even if one fails.
    virtual bool stop() = 0;

    // Non-blocking: makes a start / execute ramp in progress give way, so a
    // STOP that is queued behind it takes effect at once.
    virtual void request_stop() = 0;

    // True after a successful start(), until stop() or the node disappears.
    virtual bool is_running() const = 0;

    // Blocking read of the current run/stop status. Nullopt on failure.
    virtual std::optional<bool> read_run_stop_status() = 0;

    // Waits up to 'timeout' for the next telemetry sample, so the caller can
    // check other conditions in between (e.g. the engine went away while
    // STOPPED, when no telemetry flows). Sample: 'out' is filled. Closed:
    // unrecoverable transport error.
    virtual TelemetryWait wait_next_telemetry(std::chrono::milliseconds timeout, TelemetrySample& out) = 0;

    // Pong: just looking for answer no content.
    virtual bool probe_alive() = 0;
};

}  // namespace wizard
