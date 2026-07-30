#pragma once

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <optional>
#include <queue>
#include <string>
#include <thread>

namespace wizard {

// V1: single hardcoded node (see docs/v1-spec.md).
constexpr uint8_t kCanopenNodeId = 1;

enum class PdoKind { Position, Velocity, Current };

struct PdoSample {
    uint64_t timestamp_us;
    PdoKind kind;
    int32_t value;  // Current only uses the low 16 bits, sign-extended.
};

// CANopen client over a SocketCAN interface (e.g. "vcan0", "can0").
// V1 scope: single node, hardcoded OD mapping.
//
// Internally owns a single background thread that reads every CAN frame
// and dispatches it: TPDOs go to an internal queue (consumed by
// read_next_pdo), SDO responses wake up whichever SDO call is waiting for
// one (there's at most one in-flight SDO transaction at a time - see
// sdo_request_mutex_). This avoids two callers racing reads on the same
// socket fd (see docs/v1-spec.md, note on the single-reader-thread design).
class CanopenClient {
public:
    // Opens and binds a raw CAN socket on 'iface', starts the reader
    // thread. Throws on failure to open/bind.
    explicit CanopenClient(const std::string& iface);
    ~CanopenClient();

    CanopenClient(const CanopenClient&) = delete;
    CanopenClient& operator=(const CanopenClient&) = delete;

    // SDO expedited upload of a UINT32 at index:subindex. Blocks with a
    // timeout. Returns nullopt on timeout, SDO abort, or malformed response.
    std::optional<uint32_t> sdo_read_u32(uint16_t index, uint8_t subindex);

    // SDO expedited upload of a UINT8 at index:subindex. Same semantics
    // as sdo_read_u32, for single-byte objects (e.g. run/stop status).
    std::optional<uint8_t> sdo_read_u8(uint16_t index, uint8_t subindex);

    // SDO expedited download (write) of a UINT8 at index:subindex. Blocks
    // with a timeout. Returns false on timeout, SDO abort, or unexpected
    // response.
    bool sdo_write_u8(uint16_t index, uint8_t subindex, uint8_t value);

    // Blocks until the next TPDO is available from the internal queue.
    // Returns nullopt only once the reader thread has stopped (socket
    // closed/error) and the queue is drained.
    std::optional<PdoSample> read_next_pdo();

private:
    void reader_loop();
    void push_pdo(const PdoSample& sample);

    struct RawCanFrame {
        uint32_t can_id;
        uint8_t data[8];
    };
    // Sends an SDO request and blocks (with a 2s timeout) for the matching
    // response frame, delivered by reader_loop. Serializes concurrent SDO
    // calls via sdo_request_mutex_ (only one in-flight transaction at a time).
    std::optional<RawCanFrame> send_sdo_request_and_wait(const RawCanFrame& request);

    int fd_;
    std::thread reader_thread_;
    std::atomic<bool> stop_reader_{false};
    std::atomic<bool> reader_stopped_{false};

    std::mutex pdo_mutex_;
    std::condition_variable pdo_cv_;
    std::queue<PdoSample> pdo_queue_;

    std::mutex sdo_mutex_;
    std::condition_variable sdo_cv_;
    bool sdo_response_ready_ = false;
    RawCanFrame sdo_response_{};

    std::mutex sdo_request_mutex_;  // one SDO transaction in flight at a time
};

}  // namespace wizard