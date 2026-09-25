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
// run/stop sequences, telemetry mapping and scaling) comes from a
// DeviceProfile loaded from JSON (canopen/devices/<device>.json).
//
// Threads owned here:
//   pdo_thread_  : decodes TPDOs into TelemetrySamples (always running)
//   sync_thread_ : sends SYNC every profile.sync_period_ms (if > 0)
//   poll_thread_ : reads "sdo_poll" telemetry channels (if any)
// With telemetry_only_while_running, SYNC and polling pause while STOPPED,
// so no telemetry flows until RUN (same behaviour as the V1 simulator).
class CanopenTranslator : public DeviceTranslator {
public:
    CanopenTranslator(const std::string& iface, DeviceProfile profile);
    // For tests: use an already-constructed client.
    CanopenTranslator(std::unique_ptr<CanopenClient> client, DeviceProfile profile);
    ~CanopenTranslator() override;

    std::optional<DeviceInfo> read_device_info() override;
    bool set_run_stop(bool run) override;
    std::optional<bool> read_run_stop_status() override;
    std::optional<TelemetrySample> read_next_telemetry() override;

    // Reads profile.alive_object. The first successful probe after the node
    // (re)appears also applies profile.configure, so a power-cycled node is
    // reconfigured automatically.
    bool probe_alive() override;

    // Applies profile.configure. Public so tests (and later the engine's
    // CONFIGURATOR) can trigger it explicitly.
    bool configure();

    bool is_configured() const { return configured_; }
    bool is_running() const { return running_; }
    const DeviceProfile& profile() const { return profile_; }

private:
    void start_threads();
    bool configure_locked();
    bool run_steps(const std::vector<WriteStep>& steps, const char* sequence);
    bool write_step(const WriteStep& step);
    std::optional<double> read_object(const std::string& name);
    std::optional<uint32_t> read_object_raw(const ObjectDef& obj);
    std::optional<TelemetrySample> make_sample(const TelemetryChannel& ch, uint64_t ts, double decoded);
    bool telemetry_enabled() const;

    void pdo_loop();
    void sync_loop();
    void poll_loop();

    DeviceProfile profile_;
    std::unique_ptr<CanopenClient> client_;
    std::map<uint32_t, TelemetryChannel> tpdo_channels_;
    std::vector<TelemetryChannel> poll_channels_;

    ThreadSafeQueue<TelemetrySample> telemetry_;

    std::mutex sequence_mutex_;  // configure / run / stop never interleave
    std::atomic<bool> configured_{false};
    std::atomic<bool> running_{false};
    std::atomic<bool> stop_{false};

    std::thread pdo_thread_;
    std::thread sync_thread_;
    std::thread poll_thread_;
};

}  // namespace wizard
