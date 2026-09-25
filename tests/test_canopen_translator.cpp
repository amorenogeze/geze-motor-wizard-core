// CanopenClient + CanopenTranslator against a fake SOLO PICO.
//
// The fake talks struct can_frame over a socketpair, so no SocketCAN/vcan is
// needed. It answers SDOs the way the PICO does (reads -> 0x42, writes ->
// 0x60, unknown objects -> abort) and sends synchronous TPDOs on SYNC.
#include <gtest/gtest.h>

#include <linux/can.h>
#include <sys/socket.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <cstring>
#include <map>
#include <mutex>
#include <set>
#include <thread>
#include <vector>

#include "canopen/canopen_translator.h"
#include "canopen/device_profile.h"

using namespace wizard;
using namespace std::chrono_literals;

namespace {

constexpr uint32_t key(uint16_t index, uint8_t sub) { return (static_cast<uint32_t>(index) << 8) | sub; }

struct SdoWrite {
    uint8_t cs;
    uint16_t index;
    uint8_t sub;
    uint32_t value;
};

class FakePico {
public:
    FakePico() {
        int fds[2];
        if (::socketpair(AF_UNIX, SOCK_SEQPACKET, 0, fds) != 0) throw std::runtime_error("socketpair");
        gateway_fd = fds[0];
        fd_ = fds[1];
        timeval tv{0, 50000};
        setsockopt(fd_, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

        set(0x303A, 0, 0x0000B020);  // firmware
        set(0x303B, 0, 0x00000123);  // hardware
        set(0x3037, 0, 1000);        // position counts
        set(0x3036, 0, 1500);        // speed rpm
        set(0x3032, 0, encode_value(ValueType::Q17, 0.75));  // Im = 0.75 A
        thread_ = std::thread([this] { loop(); });
    }
    ~FakePico() {
        stop_ = true;
        thread_.join();
        ::close(fd_);
    }

    void set(uint16_t index, uint8_t sub, uint32_t v) {
        std::lock_guard<std::mutex> l(m_);
        od_[key(index, sub)] = v;
    }
    uint32_t get(uint16_t index, uint8_t sub = 0) {
        std::lock_guard<std::mutex> l(m_);
        return od_[key(index, sub)];
    }
    std::vector<SdoWrite> writes() {
        std::lock_guard<std::mutex> l(m_);
        return writes_;
    }
    void clear_writes() {
        std::lock_guard<std::mutex> l(m_);
        writes_.clear();
    }

    int gateway_fd = -1;                   // handed to CanopenClient
    std::atomic<bool> silent{false};       // node powered off
    std::atomic<bool> size_indicated_replies{false};
    std::atomic<int> syncs{0};
    std::set<uint16_t> reject_writes;      // answer these writes with an abort
    std::set<uint16_t> ignore_writes;      // accept, but keep the old value

private:
    void send(uint32_t id, const uint8_t* data, uint8_t dlc) {
        can_frame f{};
        f.can_id = id;
        f.can_dlc = dlc;
        std::memcpy(f.data, data, dlc);
        (void)!::write(fd_, &f, sizeof f);
    }
    static void put_le(uint8_t* p, uint32_t v) {
        for (int i = 0; i < 4; ++i) p[i] = static_cast<uint8_t>(v >> (8 * i));
    }

    void loop() {
        while (!stop_) {
            can_frame f{};
            if (::read(fd_, &f, sizeof f) != static_cast<ssize_t>(sizeof f)) continue;
            if (silent) continue;

            if (f.can_id == 0x080) {  // SYNC: send enabled TPDOs
                ++syncs;
                std::lock_guard<std::mutex> l(m_);
                const std::pair<uint16_t, uint16_t> map[] = {{0x1814, 0x3037}, {0x1815, 0x3036}};
                for (auto [pdo, source] : map) {
                    const uint32_t cfg = od_[key(pdo, 1)];
                    if (!(cfg & 0x80000000u)) continue;
                    uint8_t d[4];
                    put_le(d, od_[key(source, 0)]);
                    send(cfg & 0x7FF, d, 4);
                }
                continue;
            }
            if (f.can_id != 0x601) continue;

            const uint8_t cs = f.data[0];
            const uint16_t index = static_cast<uint16_t>(f.data[1] | (f.data[2] << 8));
            const uint8_t sub = f.data[3];
            uint32_t value = 0;
            for (int i = 0; i < 4; ++i) value |= static_cast<uint32_t>(f.data[4 + i]) << (8 * i);

            uint8_t r[8] = {0, f.data[1], f.data[2], sub, 0, 0, 0, 0};
            std::lock_guard<std::mutex> l(m_);
            const bool known = od_.count(key(index, sub)) || index == 0x1814 || index == 0x1815 ||
                               (index >= 0x3001 && index <= 0x3040);
            if (cs == 0x40) {
                if (!od_.count(key(index, sub))) {
                    r[0] = 0x80;
                    put_le(&r[4], 0x06020000);  // object does not exist
                } else {
                    r[0] = size_indicated_replies ? 0x43 : 0x42;
                    put_le(&r[4], od_[key(index, sub)]);
                }
            } else if ((cs & 0xE0) == 0x20) {
                writes_.push_back({cs, index, sub, value});
                if (!known || reject_writes.count(index)) {
                    r[0] = 0x80;
                    put_le(&r[4], 0x06090030);  // value range exceeded
                } else {
                    if (!ignore_writes.count(index)) od_[key(index, sub)] = value;
                    else od_.emplace(key(index, sub), 1);
                    r[0] = 0x60;
                }
            } else {
                continue;
            }
            send(0x581, r, 8);
        }
    }

    int fd_ = -1;
    std::atomic<bool> stop_{false};
    std::thread thread_;
    std::mutex m_;
    std::map<uint32_t, uint32_t> od_;
    std::vector<SdoWrite> writes_;
};

DeviceProfile pico_profile() {
    DeviceProfile p = load_device_profile(std::string(WIZARD_SOURCE_DIR) +
                                          "/device-gateway/canopen/devices/solopico.json");
    p.sdo_timeout_ms = 100;  // keep "node silent" tests fast
    return p;
}

std::unique_ptr<CanopenTranslator> make_translator(FakePico& pico, DeviceProfile p = pico_profile()) {
    return std::make_unique<CanopenTranslator>(std::make_unique<CanopenClient>(pico.gateway_fd, p.node_id),
                                               p);
}

std::optional<SdoWrite> find_write(const std::vector<SdoWrite>& w, uint16_t index, uint8_t sub = 0) {
    for (const auto& x : w)
        if (x.index == index && x.sub == sub) return x;
    return std::nullopt;
}

}  // namespace

TEST(CanopenClient, AcceptsSoloAndStandardUploadResponses) {
    FakePico pico;
    CanopenClient client(pico.gateway_fd, 1);
    auto r = client.sdo_read(0x303A, 0);  // PICO style 0x42
    ASSERT_TRUE(r.ok);
    EXPECT_EQ(r.value, 0x0000B020u);

    pico.size_indicated_replies = true;  // standard 0x43
    EXPECT_EQ(client.sdo_read_u32(0x303B, 0).value_or(0), 0x123u);

    auto missing = client.sdo_read(0x2000, 0);
    EXPECT_FALSE(missing.ok);
    EXPECT_EQ(missing.abort_code, 0x06020000u);
}

TEST(CanopenClient, WriteCommandSpecifiers) {
    FakePico pico;
    CanopenClient client(pico.gateway_fd, 1);
    EXPECT_TRUE(client.sdo_write(0x3008, 0, 1, 4, false).ok);
    EXPECT_TRUE(client.sdo_write(0x3009, 0, 20, 1, true).ok);
    auto w = pico.writes();
    ASSERT_EQ(w.size(), 2u);
    EXPECT_EQ(w[0].cs, 0x22);  // SOLO: size not indicated
    EXPECT_EQ(w[1].cs, 0x2F);  // standard 1-byte
}

TEST(CanopenTranslator, ConfigureAppliesProfileInOrder) {
    FakePico pico;
    auto t = make_translator(pico);
    ASSERT_TRUE(t->configure());

    auto w = pico.writes();
    ASSERT_FALSE(w.empty());
    EXPECT_EQ(w.front().index, 0x3002);  // command_mode first
    for (const auto& x : w) EXPECT_EQ(x.cs, 0x22);
    EXPECT_EQ(pico.get(0x3015), 0u);                                        // motor type DC
    EXPECT_EQ(pico.get(0x3003), encode_value(ValueType::Q17, 2.0));         // current limit 2 A
    EXPECT_EQ(pico.get(0x3016), 1u);                                        // torque mode
    EXPECT_EQ(pico.get(0x1814, 1), 0x80000281u);                            // position TPDO
    EXPECT_EQ(pico.get(0x1815, 2), 1u);                                     // speed TPDO every SYNC
    EXPECT_FALSE(find_write(w, 0x3007));                                    // identification skipped
    EXPECT_TRUE(t->is_configured());
}

TEST(CanopenTranslator, VerifyMismatchStopsConfigure) {
    FakePico pico;
    pico.set(0x3015, 0, 1);            // PICO still BLDC...
    pico.ignore_writes.insert(0x3015);  // ...and silently keeps it
    auto t = make_translator(pico);
    EXPECT_FALSE(t->configure());
    EXPECT_FALSE(t->is_configured());
    EXPECT_FALSE(find_write(pico.writes(), 0x3016));  // later steps not executed
}

TEST(CanopenTranslator, RunStopAndStatus) {
    FakePico pico;
    auto t = make_translator(pico);
    ASSERT_TRUE(t->set_run_stop(true));  // configures on first use
    EXPECT_EQ(pico.get(0x3008), 1u);
    EXPECT_EQ(pico.get(0x3004), encode_value(ValueType::Q17, 0.5));
    EXPECT_EQ(t->read_run_stop_status().value_or(false), true);

    ASSERT_TRUE(t->set_run_stop(false));
    EXPECT_EQ(pico.get(0x3004), 0u);
    EXPECT_EQ(pico.get(0x3008), 0u);
    EXPECT_EQ(t->read_run_stop_status().value_or(true), false);
}

TEST(CanopenTranslator, FailedRunFallsBackToStop) {
    FakePico pico;
    auto t = make_translator(pico);
    ASSERT_TRUE(t->configure());
    pico.reject_writes.insert(0x300C);  // motor_direction rejected mid-run
    EXPECT_FALSE(t->set_run_stop(true));
    EXPECT_FALSE(t->is_running());
    EXPECT_EQ(pico.get(0x3008), 0u);  // drive disabled again by the stop sequence
}

TEST(CanopenTranslator, TelemetryOnlyWhileRunning) {
    FakePico pico;
    auto t = make_translator(pico);
    ASSERT_TRUE(t->configure());

    std::this_thread::sleep_for(100ms);
    EXPECT_EQ(pico.syncs.load(), 0);  // STOPPED: no SYNC on the bus

    ASSERT_TRUE(t->set_run_stop(true));
    bool pos = false, spd = false, cur = false;
    for (int i = 0; i < 200 && !(pos && spd && cur); ++i) {
        auto s = t->read_next_telemetry();
        ASSERT_TRUE(s);
        if (s->kind == TelemetryKind::Position) { pos = true; EXPECT_EQ(s->value, 1000); }
        if (s->kind == TelemetryKind::Velocity) { spd = true; EXPECT_EQ(s->value, 1500); }
        if (s->kind == TelemetryKind::Current)  { cur = true; EXPECT_EQ(s->value, 750); }  // 0.75 A -> mA
    }
    EXPECT_TRUE(pos && spd && cur);

    ASSERT_TRUE(t->set_run_stop(false));
    std::this_thread::sleep_for(50ms);
    const int after_stop = pico.syncs.load();
    std::this_thread::sleep_for(100ms);
    EXPECT_EQ(pico.syncs.load(), after_stop);
}

TEST(CanopenTranslator, ProbeReconfiguresAfterPowerCycle) {
    FakePico pico;
    auto t = make_translator(pico);
    EXPECT_TRUE(t->probe_alive());  // first contact configures
    EXPECT_TRUE(t->is_configured());

    pico.silent = true;
    EXPECT_FALSE(t->probe_alive());
    EXPECT_FALSE(t->is_configured());

    pico.silent = false;
    pico.clear_writes();
    pico.set(0x3015, 0, 1);  // came back with factory motor type
    EXPECT_TRUE(t->probe_alive());
    EXPECT_TRUE(t->is_configured());
    EXPECT_EQ(pico.get(0x3015), 0u);
    EXPECT_TRUE(find_write(pico.writes(), 0x3002));
}

TEST(CanopenTranslator, DeviceInfoFromProfile) {
    FakePico pico;
    auto t = make_translator(pico);
    auto info = t->read_device_info();
    ASSERT_TRUE(info);
    EXPECT_EQ(info->vendor_id, 0u);
    EXPECT_EQ(info->product_code, 0x123u);
    EXPECT_EQ(info->revision, 0x0000B020u);
    EXPECT_EQ(info->serial, 0u);
}
