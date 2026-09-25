#include "canopen_client.h"

#include <linux/can.h>
#include <linux/can/raw.h>
#include <net/if.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>
#include <cerrno>
#include <cstring>
#include <stdexcept>

namespace wizard {

namespace {

// CiA301 COB-ID bases.
constexpr uint32_t kSyncCobId = 0x080;
constexpr uint32_t kSdoRequestBase = 0x600;   // client -> server (RSDO)
constexpr uint32_t kSdoResponseBase = 0x580;  // server -> client (TSDO)

// SDO command specifiers (expedited transfers only).
constexpr uint8_t kCcsInitiateUploadRequest = 0x40;
constexpr uint8_t kScsInitiateUploadMask = 0xE0;      // top 3 bits = scs
constexpr uint8_t kScsInitiateUpload = 0x40;          // scs = 2
constexpr uint8_t kSdoExpeditedBit = 0x02;            // e
constexpr uint8_t kSdoSizeIndicatedBit = 0x01;        // s
constexpr uint8_t kCcsInitiateDownloadNoSize = 0x22;  // expedited, size not indicated
constexpr uint8_t kScsInitiateDownloadResponse = 0x60;
constexpr uint8_t kSdoAbort = 0x80;

uint64_t now_us() {
    timeval tv{};
    gettimeofday(&tv, nullptr);
    return static_cast<uint64_t>(tv.tv_sec) * 1'000'000ULL + static_cast<uint64_t>(tv.tv_usec);
}

uint32_t decode_u32_le(const uint8_t* p) {
    return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) |
           (static_cast<uint32_t>(p[2]) << 16) | (static_cast<uint32_t>(p[3]) << 24);
}

// Expedited download with size: 0x23 (4 bytes), 0x27 (3), 0x2B (2), 0x2F (1).
uint8_t download_cs_with_size(uint8_t size) {
    return static_cast<uint8_t>(0x23 | ((4 - size) << 2));
}

}  // namespace

CanopenClient::CanopenClient(const std::string& iface, uint8_t node_id) : node_id_(node_id) {
    fd_ = ::socket(PF_CAN, SOCK_RAW, CAN_RAW);
    if (fd_ < 0) throw std::runtime_error("failed to create CAN socket");

    ifreq ifr{};
    std::strncpy(ifr.ifr_name, iface.c_str(), IFNAMSIZ - 1);
    if (::ioctl(fd_, SIOCGIFINDEX, &ifr) < 0) {
        close(fd_);
        throw std::runtime_error("unknown CAN interface: " + iface);
    }

    sockaddr_can addr{};
    addr.can_family = AF_CAN;
    addr.can_ifindex = ifr.ifr_ifindex;
    if (::bind(fd_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
        close(fd_);
        throw std::runtime_error("failed to bind CAN socket to " + iface);
    }
    start();
}

CanopenClient::CanopenClient(int fd, uint8_t node_id) : fd_(fd), node_id_(node_id) {
    if (fd_ < 0) throw std::runtime_error("invalid CAN fd");
    start();
}

void CanopenClient::start() {
    // Short timeout so the reader can periodically check stop_reader_.
    timeval tv{0, 200000};  // 200ms
    setsockopt(fd_, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    reader_thread_ = std::thread(&CanopenClient::reader_loop, this);
}

CanopenClient::~CanopenClient() {
    stop_reader_ = true;
    if (reader_thread_.joinable()) reader_thread_.join();
    if (fd_ >= 0) close(fd_);
}

void CanopenClient::set_tpdo_cob_ids(const std::set<uint32_t>& cob_ids) {
    std::lock_guard<std::mutex> lock(tpdo_mutex_);
    tpdo_cob_ids_ = cob_ids;
}

void CanopenClient::reader_loop() {
    can_frame frame{};
    while (!stop_reader_.load()) {
        ssize_t n = ::read(fd_, &frame, sizeof(frame));
        if (n < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) continue;  // poll timeout
            break;  // real socket error, stop the reader
        }
        if (n == 0) break;  // peer closed (socketpair in tests)
        if (n != static_cast<ssize_t>(sizeof(frame))) continue;

        const uint32_t id = frame.can_id & CAN_SFF_MASK;

        if (id == kSdoResponseBase + node_id_) {
            RawCanFrame raw{};
            raw.can_id = id;
            std::memcpy(raw.data, frame.data, 8);
            sdo_response_queue_.push(raw);
            continue;
        }

        bool wanted;
        {
            std::lock_guard<std::mutex> lock(tpdo_mutex_);
            wanted = tpdo_cob_ids_.count(id) != 0;
        }
        if (wanted) {
            PdoFrame pdo{};
            pdo.timestamp_us = now_us();
            pdo.cob_id = id;
            pdo.dlc = frame.can_dlc;
            std::memcpy(pdo.data, frame.data, 8);
            pdo_queue_.push(pdo);
        }
        // Unrecognized frame: ignore.
    }

    // Wake anyone blocked in read_next_pdo() or send_sdo_request_and_wait().
    pdo_queue_.close();
    sdo_response_queue_.close();
}

std::optional<PdoFrame> CanopenClient::read_next_pdo() { return pdo_queue_.pop(); }

std::optional<PdoFrame> CanopenClient::read_next_pdo(std::chrono::milliseconds timeout) {
    return pdo_queue_.pop_for(timeout);
}

bool CanopenClient::write_frame(uint32_t can_id, const uint8_t* data, uint8_t dlc) {
    can_frame frame{};
    frame.can_id = can_id;
    frame.can_dlc = dlc;
    if (dlc) std::memcpy(frame.data, data, dlc);
    std::lock_guard<std::mutex> lock(write_mutex_);
    return ::write(fd_, &frame, sizeof(frame)) == static_cast<ssize_t>(sizeof(frame));
}

bool CanopenClient::send_sync() { return write_frame(kSyncCobId, nullptr, 0); }

std::optional<CanopenClient::RawCanFrame> CanopenClient::send_sdo_request_and_wait(
    const RawCanFrame& request) {
    std::lock_guard<std::mutex> serialize(sdo_request_mutex_);

    // Discard a stale response left over from a previous timed-out request.
    sdo_response_queue_.clear();

    if (!write_frame(request.can_id, request.data, 8)) return std::nullopt;

    // Skip responses for another object (late answer to an earlier request).
    const auto deadline = std::chrono::steady_clock::now() + sdo_timeout_;
    while (true) {
        const auto left = deadline - std::chrono::steady_clock::now();
        if (left <= std::chrono::steady_clock::duration::zero()) return std::nullopt;
        auto resp = sdo_response_queue_.pop_for(left);
        if (!resp) return std::nullopt;
        if (resp->data[1] == request.data[1] && resp->data[2] == request.data[2]) return resp;
    }
}

SdoResult CanopenClient::sdo_read(uint16_t index, uint8_t subindex) {
    RawCanFrame req{};
    req.can_id = kSdoRequestBase + node_id_;
    req.data[0] = kCcsInitiateUploadRequest;
    req.data[1] = static_cast<uint8_t>(index & 0xFF);
    req.data[2] = static_cast<uint8_t>((index >> 8) & 0xFF);
    req.data[3] = subindex;

    SdoResult r;
    auto resp = send_sdo_request_and_wait(req);
    if (!resp) {
        r.timeout = true;
        return r;
    }

    const uint8_t cs = resp->data[0];
    if (cs == kSdoAbort) {
        r.abort_code = decode_u32_le(&resp->data[4]);
        return r;
    }
    if ((cs & kScsInitiateUploadMask) != kScsInitiateUpload || !(cs & kSdoExpeditedBit)) {
        return r;  // segmented or malformed: not supported
    }

    uint32_t value = decode_u32_le(&resp->data[4]);
    if (cs & kSdoSizeIndicatedBit) {
        const unsigned unused = (cs >> 2) & 0x03;  // n = bytes that do not hold data
        if (unused) value &= 0xFFFFFFFFu >> (8 * unused);
    }
    r.ok = true;
    r.value = value;
    return r;
}

SdoResult CanopenClient::sdo_write(uint16_t index, uint8_t subindex, uint32_t value, uint8_t size,
                                   bool size_indicated) {
    RawCanFrame req{};
    req.can_id = kSdoRequestBase + node_id_;
    req.data[0] = size_indicated ? download_cs_with_size(size) : kCcsInitiateDownloadNoSize;
    req.data[1] = static_cast<uint8_t>(index & 0xFF);
    req.data[2] = static_cast<uint8_t>((index >> 8) & 0xFF);
    req.data[3] = subindex;
    for (int i = 0; i < 4; ++i) req.data[4 + i] = static_cast<uint8_t>((value >> (8 * i)) & 0xFF);

    SdoResult r;
    auto resp = send_sdo_request_and_wait(req);
    if (!resp) {
        r.timeout = true;
        return r;
    }
    if (resp->data[0] == kSdoAbort) {
        r.abort_code = decode_u32_le(&resp->data[4]);
        return r;
    }
    r.ok = resp->data[0] == kScsInitiateDownloadResponse;
    return r;
}

std::optional<uint32_t> CanopenClient::sdo_read_u32(uint16_t index, uint8_t subindex) {
    auto r = sdo_read(index, subindex);
    if (!r.ok) return std::nullopt;
    return r.value;
}

std::optional<uint8_t> CanopenClient::sdo_read_u8(uint16_t index, uint8_t subindex) {
    auto r = sdo_read(index, subindex);
    if (!r.ok) return std::nullopt;
    return static_cast<uint8_t>(r.value & 0xFF);
}

bool CanopenClient::sdo_write_u8(uint16_t index, uint8_t subindex, uint8_t value) {
    return sdo_write(index, subindex, value, 1, true).ok;
}

}  // namespace wizard
