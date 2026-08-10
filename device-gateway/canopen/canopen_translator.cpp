#include "canopen_translator.h"

namespace wizard {

namespace {
constexpr uint16_t kIdentityIndex = 0x1018;
constexpr uint16_t kControlIndex = 0x2000;
constexpr uint16_t kStatusIndex = 0x2001;

TelemetryKind to_telemetry_kind(PdoKind kind) {
    switch (kind) {
        case PdoKind::Position:
            return TelemetryKind::Position;
        case PdoKind::Velocity:
            return TelemetryKind::Velocity;
        case PdoKind::Current:
            return TelemetryKind::Current;
    }
    return TelemetryKind::Position;  // unreachable
}
}  // namespace

CanopenTranslator::CanopenTranslator(const std::string& iface) : client_(iface) {}

std::optional<DeviceInfo> CanopenTranslator::read_device_info() {
    auto vendor = client_.sdo_read_u32(kIdentityIndex, 0x01);
    if (!vendor) return std::nullopt;
    auto product = client_.sdo_read_u32(kIdentityIndex, 0x02);
    if (!product) return std::nullopt;
    auto revision = client_.sdo_read_u32(kIdentityIndex, 0x03);
    if (!revision) return std::nullopt;
    auto serial = client_.sdo_read_u32(kIdentityIndex, 0x04);
    if (!serial) return std::nullopt;

    return DeviceInfo{*vendor, *product, *revision, *serial};
}

bool CanopenTranslator::set_run_stop(bool run) {
    return client_.sdo_write_u8(kControlIndex, 0x00, run ? 1 : 0);
}

std::optional<bool> CanopenTranslator::read_run_stop_status() {
    auto status = client_.sdo_read_u8(kStatusIndex, 0x00);
    if (!status) return std::nullopt;
    return *status != 0;
}

std::optional<TelemetrySample> CanopenTranslator::read_next_telemetry() {
    auto sample = client_.read_next_pdo();
    if (!sample) return std::nullopt;
    return TelemetrySample{sample->timestamp_us, to_telemetry_kind(sample->kind), sample->value};
}

bool CanopenTranslator::probe_alive() {
    return client_.sdo_read_u8(kStatusIndex, 0x00).has_value();
}

}  // namespace wizard