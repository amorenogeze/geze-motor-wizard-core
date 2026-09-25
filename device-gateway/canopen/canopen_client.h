#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <thread>

#include "thread_safe_queue.h"

namespace wizard {

// A received TPDO, undecoded: the translator knows (from the device
// profile) what each COB-ID carries.
struct PdoFrame {
    uint64_t timestamp_us;
    uint32_t cob_id;
    uint8_t dlc;
    uint8_t data[8];
};

// Result of an SDO transfer: the value, or why it failed.
struct SdoResult {
    bool ok = false;
    uint32_t value = 0;        // reads: raw value, little-endian decoded
    uint32_t abort_code = 0;   // non-zero if the node answered with an SDO abort
    bool timeout = false;
};

// CANopen client over SocketCAN (e.g. "vcan0", "can0"). Single node.
//
// One background thread (reader_loop) reads every CAN frame and routes
// it into a queue: TPDOs whose COB-ID was registered -> pdo_queue_
// (read_next_pdo), SDO responses -> sdo_response_queue_
// (send_sdo_request_and_wait; only one SDO transaction in flight at a time,
// via sdo_request_mutex_). Avoids two callers racing reads on the same fd.
class CanopenClient {
public:
    // Opens/binds the CAN socket, starts the reader thread. Throws on failure.
    CanopenClient(const std::string& iface, uint8_t node_id);

    // Adopts an already-open fd that carries struct can_frame records
    // (tests use one end of a socketpair). Takes ownership of fd.
    CanopenClient(int fd, uint8_t node_id);

    ~CanopenClient();

    CanopenClient(const CanopenClient&) = delete;
    CanopenClient& operator=(const CanopenClient&) = delete;

    void set_sdo_timeout(std::chrono::milliseconds timeout) { sdo_timeout_ = timeout; }

    // Only TPDOs with these COB-IDs are queued; everything else is ignored.
    // Call before telemetry is expected (thread-safe).
    void set_tpdo_cob_ids(const std::set<uint32_t>& cob_ids);

    // SDO upload (read), expedited. Accepts every expedited response form
    // (0x42 without size, 0x43/0x47/0x4B/0x4F with size).
    SdoResult sdo_read(uint16_t index, uint8_t subindex);

    // SDO download (write), expedited, of 'size' bytes (1, 2 or 4).
    // size_indicated=true  -> cs 0x2F/0x2B/0x23 (standard, size in the frame)
    // size_indicated=false -> cs 0x22 (size not indicated; what SOLO uses)
    SdoResult sdo_write(uint16_t index, uint8_t subindex, uint32_t value, uint8_t size,
                        bool size_indicated);

    // Kept for existing callers: thin wrappers over sdo_read / sdo_write.
    std::optional<uint32_t> sdo_read_u32(uint16_t index, uint8_t subindex);
    std::optional<uint8_t> sdo_read_u8(uint16_t index, uint8_t subindex);
    bool sdo_write_u8(uint16_t index, uint8_t subindex, uint8_t value);

    // CANopen SYNC (COB-ID 0x080, no data). Nodes whose TPDOs are
    // synchronous answer with their TPDOs.
    bool send_sync();

    // Blocks for the next registered TPDO. nullopt once the reader thread
    // has stopped (or, for the timed version, on timeout).
    std::optional<PdoFrame> read_next_pdo();
    std::optional<PdoFrame> read_next_pdo(std::chrono::milliseconds timeout);

    // True until the reader thread hit an unrecoverable socket error.
    bool is_open() const { return !pdo_queue_.is_closed(); }

private:
    void start();
    void reader_loop();

    struct RawCanFrame {
        uint32_t can_id;
        uint8_t data[8];
    };
    bool write_frame(uint32_t can_id, const uint8_t* data, uint8_t dlc);
    // Sends an SDO request, waits (sdo_timeout_) for the response with the
    // same object index via sdo_response_queue_. Serialized by sdo_request_mutex_.
    std::optional<RawCanFrame> send_sdo_request_and_wait(const RawCanFrame& request);

    int fd_ = -1;
    uint8_t node_id_;
    std::chrono::milliseconds sdo_timeout_{2000};
    std::thread reader_thread_;
    std::atomic<bool> stop_reader_{false};

    std::mutex tpdo_mutex_;
    std::set<uint32_t> tpdo_cob_ids_;

    ThreadSafeQueue<PdoFrame> pdo_queue_;
    ThreadSafeQueue<RawCanFrame> sdo_response_queue_;

    std::mutex sdo_request_mutex_;  // one SDO transaction in flight at a time
    std::mutex write_mutex_;        // SDO requests and SYNC come from different threads
};

}  // namespace wizard
