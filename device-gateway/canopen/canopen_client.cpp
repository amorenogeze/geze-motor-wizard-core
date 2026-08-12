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

// CiA301 COB-ID bases for the function codes we use in V1.
constexpr uint32_t kTpdo1Base = 0x180;  // position
constexpr uint32_t kTpdo2Base = 0x280;  // velocity
constexpr uint32_t kTpdo3Base = 0x380;  // current
constexpr uint32_t kSdoRequestBase = 0x600;   // client -> server (RSDO)
constexpr uint32_t kSdoResponseBase = 0x580;  // server -> client (TSDO)

// SDO command specifiers (expedited transfer only, all our reads are UINT32).
constexpr uint8_t kScsInitiateUploadRequest = 0x40;
constexpr uint8_t kScsInitiateUploadResponseExpedited4Bytes = 0x43;
constexpr uint8_t kScsInitiateUploadResponseExpedited1Byte = 0x4F;
constexpr uint8_t kScsInitiateDownloadRequest1Byte = 0x2F;
constexpr uint8_t kScsInitiateDownloadResponse = 0x60;
constexpr uint8_t kScsAbort = 0x80;

uint64_t now_us() {
    timeval tv{};
    gettimeofday(&tv, nullptr);
    return static_cast<uint64_t>(tv.tv_sec) * 1'000'000ULL + static_cast<uint64_t>(tv.tv_usec);
}

uint32_t decode_u32_le(const uint8_t* p) {
    return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) |
           (static_cast<uint32_t>(p[2]) << 16) | (static_cast<uint32_t>(p[3]) << 24);
}

int32_t decode_i32_le(const uint8_t* p) { return static_cast<int32_t>(decode_u32_le(p)); }

int32_t decode_i16_le_as_i32(const uint8_t* p) {
    uint16_t u = static_cast<uint16_t>(p[0]) | (static_cast<uint16_t>(p[1]) << 8);
    return static_cast<int32_t>(static_cast<int16_t>(u));
}

}  // namespace

CanopenClient::CanopenClient(const std::string& iface) {
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

void CanopenClient::reader_loop() {
    can_frame frame{};
    while (!stop_reader_.load()) {
        ssize_t n = ::read(fd_, &frame, sizeof(frame));
        if (n < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) continue;  // just a poll timeout
            break;  // real socket error, stop the reader
        }

        if (frame.can_id == kSdoResponseBase + kCanopenNodeId) {
            RawCanFrame raw{};
            raw.can_id = frame.can_id;
            std::memcpy(raw.data, frame.data, 8);
            sdo_response_queue_.push(raw);
            continue;
        }

        uint32_t base = frame.can_id - kCanopenNodeId;
        uint64_t ts = now_us();
        if (base == kTpdo1Base && frame.can_dlc >= 4) {
            pdo_queue_.push({ts, PdoKind::Position, decode_i32_le(frame.data)});
        } else if (base == kTpdo2Base && frame.can_dlc >= 4) {
            pdo_queue_.push({ts, PdoKind::Velocity, decode_i32_le(frame.data)});
        } else if (base == kTpdo3Base && frame.can_dlc >= 2) {
            pdo_queue_.push({ts, PdoKind::Current, decode_i16_le_as_i32(frame.data)});
        }
        // Unrecognized frame: ignore.
    }

    // Wake anyone blocked in read_next_pdo() or send_sdo_request_and_wait().
    pdo_queue_.close();
    sdo_response_queue_.close();
}

std::optional<PdoSample> CanopenClient::read_next_pdo() { return pdo_queue_.pop(); }

std::optional<CanopenClient::RawCanFrame> CanopenClient::send_sdo_request_and_wait(
    const RawCanFrame& request) {
    std::lock_guard<std::mutex> serialize(sdo_request_mutex_);

    // Discard a stale response left over from a previous timed-out request.
    sdo_response_queue_.clear();

    can_frame frame{};
    frame.can_id = request.can_id;
    frame.can_dlc = 8;
    std::memcpy(frame.data, request.data, 8);
    if (::write(fd_, &frame, sizeof(frame)) != sizeof(frame)) return std::nullopt;

    return sdo_response_queue_.pop_for(std::chrono::seconds(2));
}

std::optional<uint32_t> CanopenClient::sdo_read_u32(uint16_t index, uint8_t subindex) {
    RawCanFrame req{};
    req.can_id = kSdoRequestBase + kCanopenNodeId;
    req.data[0] = kScsInitiateUploadRequest;
    req.data[1] = static_cast<uint8_t>(index & 0xFF);
    req.data[2] = static_cast<uint8_t>((index >> 8) & 0xFF);
    req.data[3] = subindex;

    auto resp = send_sdo_request_and_wait(req);
    if (!resp) return std::nullopt;

    uint8_t cs = resp->data[0];
    if (cs == kScsAbort) return std::nullopt;
    if (cs != kScsInitiateUploadResponseExpedited4Bytes) return std::nullopt;
    return decode_u32_le(&resp->data[4]);
}

std::optional<uint8_t> CanopenClient::sdo_read_u8(uint16_t index, uint8_t subindex) {
    RawCanFrame req{};
    req.can_id = kSdoRequestBase + kCanopenNodeId;
    req.data[0] = kScsInitiateUploadRequest;
    req.data[1] = static_cast<uint8_t>(index & 0xFF);
    req.data[2] = static_cast<uint8_t>((index >> 8) & 0xFF);
    req.data[3] = subindex;

    auto resp = send_sdo_request_and_wait(req);
    if (!resp) return std::nullopt;

    uint8_t cs = resp->data[0];
    if (cs == kScsAbort) return std::nullopt;
    if (cs != kScsInitiateUploadResponseExpedited1Byte) return std::nullopt;
    return resp->data[4];
}

bool CanopenClient::sdo_write_u8(uint16_t index, uint8_t subindex, uint8_t value) {
    RawCanFrame req{};
    req.can_id = kSdoRequestBase + kCanopenNodeId;
    req.data[0] = kScsInitiateDownloadRequest1Byte;
    req.data[1] = static_cast<uint8_t>(index & 0xFF);
    req.data[2] = static_cast<uint8_t>((index >> 8) & 0xFF);
    req.data[3] = subindex;
    req.data[4] = value;

    auto resp = send_sdo_request_and_wait(req);
    if (!resp) return false;

    uint8_t cs = resp->data[0];
    if (cs == kScsAbort) return false;
    return cs == kScsInitiateDownloadResponse;
}

}  // namespace wizard