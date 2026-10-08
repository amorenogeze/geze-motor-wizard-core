#pragma once

#include <atomic>
#include <map>
#include <memory>
#include <mutex>
#include <thread>

#include "canopen_client.h"
#include "device_profile.h"
#include "../device_translator.h"

namespace wizard {

// CANopen implementation of DeviceTranslator. Protocol mechanics live in
// CanopenClient; everything device-specific (objects, startup configuration,
// telemetry mapping and scaling) comes from a DeviceProfile loaded from JSON
// (canopen/devices/<device>.json). It executes the steps it is given (by the
// CommandManager) and knows one sequence of its own: 'stop_steps', run by
// stop() and after a failed start, so the drive is never left half-started.
//
// Threads owned here:
//   pdo_thread_  : decodes TPDOs into TelemetrySamples (always running)
//   sync_thread_ : sends SYNC every profile.sync_period_ms (if > 0)
//   poll_thread_ : reads "sdo_poll" telemetry channels (if any)
// With telemetry_only_while_running, SYNC and polling pause while STOPPED,
// so no telemetry flows until RUN (same behaviour as the V1 simulator).
class CanopenTranslator : public DeviceTranslator {
public:
    CanopenTranslator(const std::string& iface, DeviceProfile profile, std::vector<WriteStep> stop_steps);
    // For tests: use an already-constructed client.
    CanopenTranslator(std::unique_ptr<CanopenClient> client, DeviceProfile profile,
                      std::vector<WriteStep> stop_steps);
    ~CanopenTranslator() override;

    std::optional<DeviceInfo> read_device_info() override;
    bool start(const std::vector<WriteStep>& steps) override;
    bool execute(const std::vector<WriteStep>& steps) override;
    bool stop() override;
    void request_stop() override { stop_requested_ = true; }
    bool is_running() const override { return running_; }
    std::optional<bool> read_run_stop_status() override;
    TelemetryWait wait_next_telemetry(std::chrono::milliseconds timeout, TelemetrySample& out) override;

    // Reads profile.alive_object. The first successful probe after the node
    // (re)appears also applies profile.configure, so a power-cycled node is
    // reconfigured automatically.
    bool probe_alive() override;

    // Applies profile.configure. Public so tests (and later the engine's
    // CONFIGURATOR) can trigger it explicitly.
    bool configure();

    bool is_configured() const { return configured_; }
    const DeviceProfile& profile() const { return profile_; }

private:
    void start_threads();
    bool configure_locked();
    bool run_steps(const std::vector<WriteStep>& steps, const char* sequence);
    bool run_stop_steps(const char* sequence);
    bool write_step(const WriteStep& step);
    bool write_value(const ObjectDef& obj, double value);
    bool ramp_to(const ObjectDef& obj, const WriteStep& step);
    std::optional<double> read_object(const std::string& name);
    std::optional<uint32_t> read_object_raw(const ObjectDef& obj);
    std::optional<TelemetrySample> make_sample(const TelemetryChannel& ch, uint64_t ts, double decoded);
    bool telemetry_enabled() const;

    void pdo_loop();
    void sync_loop();
    void poll_loop();

    DeviceProfile profile_;
    std::vector<WriteStep> stop_steps_;
    std::unique_ptr<CanopenClient> client_;
    std::map<uint32_t, TelemetryChannel> tpdo_channels_;
    std::vector<TelemetryChannel> poll_channels_;

    ThreadSafeQueue<TelemetrySample> telemetry_;

    std::mutex sequence_mutex_;  // configure / run / stop never interleave
    std::atomic<bool> configured_{false};
    std::atomic<bool> running_{false};
    std::atomic<bool> stop_requested_{false};  // a STOP is waiting for sequence_mutex_
    std::atomic<bool> stop_{false};

    std::thread pdo_thread_;
    std::thread sync_thread_;
    std::thread poll_thread_;
};

}  // namespace wizard
