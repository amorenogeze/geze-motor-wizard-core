#pragma once

#include <cstdint>
#include <optional>
#include <string>

namespace wizard {

// V1: single hardcoded node (see docs/v1-spec.md).
constexpr uint8_t kCanopenNodeId = 1;

enum class PdoKind { Position, Velocity, Current };

struct PdoSample {
    uint64_t timestamp_us;
    PdoKind kind;
    int32_t value;  // Current only uses the low 16 bits, sign-extended.
};

// Blocking CANopen client over a SocketCAN interface (e.g. "vcan0", "can0").
// V1 scope: single node, hardcoded OD mapping.
class CanopenClient {
public:
    // Opens and binds a raw CAN socket on 'iface'. Throws on failure.
    explicit CanopenClient(const std::string& iface);
    ~CanopenClient();

    CanopenClient(const CanopenClient&) = delete;
    CanopenClient& operator=(const CanopenClient&) = delete;

    // SDO expedited upload of a UINT32 at index:subindex. Blocks with a
    // timeout. Returns nullopt on timeout, SDO abort, or malformed response.
    std::optional<uint32_t> sdo_read_u32(uint16_t index, uint8_t subindex);

    // Blocks until the next recognized TPDO arrives and decodes it.
    // Unrecognized frames are skipped internally. Returns nullopt only on
    // socket error.
    std::optional<PdoSample> read_next_pdo();

private:
    int fd_;
};

}  // namespace wizard