#pragma once

#include <memory>

#include "canopen_client.h"
#include "../device_translator.h"

namespace wizard {

// CANopen implementation of DeviceTranslator. Hardcoded OD: Identity,
// 0x2000 Control, 0x2001 Status, TPDO1-3 (see docs/v1-spec.md §8.1).
class CanopenTranslator : public DeviceTranslator {
public:
    explicit CanopenTranslator(const std::string& iface);

    std::optional<DeviceInfo> read_device_info() override;
    bool set_run_stop(bool run) override;
    std::optional<bool> read_run_stop_status() override;
    std::optional<TelemetrySample> read_next_telemetry() override;
    bool probe_alive() override;

private:
    CanopenClient client_;
};

}  // namespace wizard