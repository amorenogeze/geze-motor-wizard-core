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
constexpr canid_t kTpdo1Base = 0x180;  // position
constexpr canid_t kTpdo2Base = 0x280;  // velocity
constexpr canid_t kTpdo3Base = 0x380;  // current
constexpr canid_t kSdoRequestBase = 0x600;   // client -> server (RSDO)
constexpr canid_t kSdoResponseBase = 0x580;  // server -> client (TSDO)

// SDO command specifiers (expedited transfer only, all our reads are UINT32).
constexpr uint8_t kScsInitiateUploadRequest = 0x40;
constexpr uint8_t kScsInitiateUploadResponseExpedited4Bytes = 0x43;
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
}

CanopenClient::~CanopenClient() {
    if (fd_ >= 0) close(fd_);
}

std::optional<uint32_t> CanopenClient::sdo_read_u32(uint16_t index, uint8_t subindex) {
    can_frame req{};
    req.can_id = kSdoRequestBase + kCanopenNodeId;
    req.can_dlc = 8;
    req.data[0] = kScsInitiateUploadRequest;
    req.data[1] = static_cast<uint8_t>(index & 0xFF);
    req.data[2] = static_cast<uint8_t>((index >> 8) & 0xFF);
    req.data[3] = subindex;
    // data[4..7] unused for upload requests.

    if (::write(fd_, &req, sizeof(req)) != sizeof(req)) return std::nullopt;

    // Timeout so a missing/malformed reply doesn't hang the gateway forever.
    timeval tv{2, 0};
    setsockopt(fd_, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    can_frame resp{};
    while (true) {
        ssize_t n = ::read(fd_, &resp, sizeof(resp));
        if (n < 0) return std::nullopt;  // timeout or error
        if (resp.can_id != kSdoResponseBase + kCanopenNodeId) continue;  // skip PDOs, etc.

        uint8_t cs = resp.data[0];
        if (cs == kScsAbort) return std::nullopt;
        if (cs != kScsInitiateUploadResponseExpedited4Bytes) return std::nullopt;
        return decode_u32_le(&resp.data[4]);
    }
}

std::optional<PdoSample> CanopenClient::read_next_pdo() {
    can_frame frame{};
    while (true) {
        ssize_t n = ::read(fd_, &frame, sizeof(frame));
        if (n < 0) return std::nullopt;

        uint64_t ts = now_us();
        canid_t base = frame.can_id - kCanopenNodeId;

        if (base == kTpdo1Base && frame.can_dlc >= 4) {
            return PdoSample{ts, PdoKind::Position, decode_i32_le(frame.data)};
        }
        if (base == kTpdo2Base && frame.can_dlc >= 4) {
            return PdoSample{ts, PdoKind::Velocity, decode_i32_le(frame.data)};
        }
        if (base == kTpdo3Base && frame.can_dlc >= 2) {
            return PdoSample{ts, PdoKind::Current, decode_i16_le_as_i32(frame.data)};
        }
        // Unrecognized frame (e.g. SDO traffic during startup) - skip and keep reading.
    }
}

}  // namespace wizard