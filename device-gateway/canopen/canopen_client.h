#pragma once

#include <atomic>
#include <cstdint>
#include <mutex>
#include <optional>
#include <string>
#include <thread>

#include "thread_safe_queue.h"

namespace wizard {

// V1: single hardcoded node (see docs/v1-spec.md).
constexpr uint8_t kCanopenNodeId = 1;

enum class PdoKind { Position, Velocity, Current };

struct PdoSample {
    uint64_t timestamp_us;
    PdoKind kind;
    int32_t value;  // Current only uses the low 16 bits, sign-extended.
};

// CANopen client over SocketCAN (e.g. "vcan0", "can0"). V1: single node,
// hardcoded OD mapping.
//
// One background thread (reader_loop) reads every CAN frame and routes
// it into a queue: TPDOs -> pdo_queue_ (read_next_pdo), SDO responses ->
// sdo_response_queue_ (send_sdo_request_and_wait; only one SDO
// transaction in flight at a time, via sdo_request_mutex_). Avoids two
// callers racing reads on the same fd.
class CanopenClient {
public:
    // Opens/binds the CAN socket, starts the reader thread. Throws on failure.
    explicit CanopenClient(const std::string& iface);
    ~CanopenClient();

    CanopenClient(const CanopenClient&) = delete;
    CanopenClient& operator=(const CanopenClient&) = delete;

    // SDO upload of a UINT32. nullopt on timeout/abort/malformed response.
    std::optional<uint32_t> sdo_read_u32(uint16_t index, uint8_t subindex);

    // Same as sdo_read_u32, for UINT8 objects (e.g. run/stop status).
    std::optional<uint8_t> sdo_read_u8(uint16_t index, uint8_t subindex);

    // SDO download (write) of a UINT8. false on timeout/abort.
    bool sdo_write_u8(uint16_t index, uint8_t subindex, uint8_t value);

    // Blocks for the next TPDO. nullopt once the reader thread has stopped.
    std::optional<PdoSample> read_next_pdo();

private:
    void reader_loop();

    struct RawCanFrame {
        uint32_t can_id;
        uint8_t data[8];
    };
    // Sends an SDO request, waits (2s timeout) for the matching response
    // via sdo_response_queue_. Serialized by sdo_request_mutex_.
    std::optional<RawCanFrame> send_sdo_request_and_wait(const RawCanFrame& request);

    int fd_;
    std::thread reader_thread_;
    std::atomic<bool> stop_reader_{false};

    ThreadSafeQueue<PdoSample> pdo_queue_;
    ThreadSafeQueue<RawCanFrame> sdo_response_queue_;

    std::mutex sdo_request_mutex_;  // one SDO transaction in flight at a time
};

}  // namespace wizard