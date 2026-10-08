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
#include <stdexcept>
#include <string>
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
    std::atomic<int> reply_delay_ms{0};    // slow node: delay every SDO answer

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
            if (reply_delay_ms) std::this_thread::sleep_for(std::chrono::milliseconds(reply_delay_ms.load()));

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

// The step that writes 'object' in a sequence. Tests take expected values from
// here instead of hard-coding them, so tuning solopico.json doesn't break them.
WriteStep& find_step(std::vector<WriteStep>& seq, const std::string& object) {
    for (auto& s : seq)
        if (s.object == object) return s;
    throw std::runtime_error("profile sequence has no step '" + object + "'");
}

// The shipped profile, exactly as installed (ramps included).
DeviceProfile shipped_profile() {
    DeviceProfile p = load_device_profile(std::string(WIZARD_SOURCE_DIR) +
                                          "/device-gateway/canopen/devices/solopico.json");
    p.sdo_timeout_ms = 100;  // keep "node silent" tests fast
    return p;
}

// Ramp tests use a fixed RUN torque target: they test the ramp mechanism, not
// the shipped torque, and the shipped 5.75 A would make each ramp take 11.5 s.
constexpr double kTestTorque = 2.0;  // A
DeviceProfile ramp_profile() {
    DeviceProfile p = shipped_profile();
    find_step(p.run, "torque_reference").value = kTestTorque;
    return p;
}

// Same, without ramps, so tests that are not about ramps stay fast.
DeviceProfile pico_profile() {
    DeviceProfile p = shipped_profile();
    for (auto* seq : {&p.configure, &p.run, &p.stop})
        for (auto& step : *seq) {
            step.ramp_ms = 0;
            step.ramp_rate = 0;
            step.ramp_start_object.clear();
        }
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
    DeviceProfile p = pico_profile();
    auto t = make_translator(pico, p);
    ASSERT_TRUE(t->configure());

    auto w = pico.writes();
    ASSERT_FALSE(w.empty());
    EXPECT_EQ(w.front().index, 0x3002);  // command_mode first
    for (const auto& x : w) EXPECT_EQ(x.cs, 0x22);
    EXPECT_EQ(pico.get(0x3015), 0u);                                        // motor type DC
    EXPECT_EQ(pico.get(0x3003),                                             // current limit from profile
              encode_value(ValueType::Q17, find_step(p.configure, "current_limit").value));
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
    DeviceProfile p = pico_profile();
    auto t = make_translator(pico, p);
    ASSERT_TRUE(t->set_run_stop(true));  // configures on first use
    EXPECT_EQ(pico.get(0x3008), 1u);
    EXPECT_EQ(pico.get(0x3004), encode_value(ValueType::Q17, find_step(p.run, "torque_reference").value));
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

// Values written to one object, in order, decoded as q17.
std::vector<double> q17_writes(FakePico& pico, uint16_t index) {
    std::vector<double> out;
    for (const auto& w : pico.writes())
        if (w.index == index) out.push_back(decode_value(ValueType::Q17, w.value));
    return out;
}

TEST(CanopenTranslator, TorqueRampsUpAtRateOnRun) {
    FakePico pico;
    DeviceProfile p = ramp_profile();                          // 0 -> 2.0 A at the profile's ramp_rate
    const double rate = find_step(p.run, "torque_reference").ramp_rate;
    ASSERT_GT(rate, 0.0);
    auto t = make_translator(pico, p);
    ASSERT_TRUE(t->configure());

    pico.clear_writes();
    const auto t0 = std::chrono::steady_clock::now();
    ASSERT_TRUE(t->set_run_stop(true));
    const auto took = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    const double expected = kTestTorque / rate;                // 2.0 A / 0.5 A/s = 4 s
    EXPECT_GE(took, expected * 0.85);
    EXPECT_LT(took, expected * 1.5);
    auto up = q17_writes(pico, 0x3004);
    ASSERT_GT(up.size(), static_cast<size_t>(expected / 0.02 / 2));  // at least half of the 20 ms steps
    for (size_t i = 1; i < up.size(); ++i) EXPECT_GE(up[i], up[i - 1]);
    for (size_t i = 1; i < up.size(); ++i) EXPECT_LE(up[i] - up[i - 1], rate * 0.04 + 1e-6);  // <= 2 steps, no jumps
    EXPECT_DOUBLE_EQ(up.back(), kTestTorque);
    t->set_run_stop(false);
}

TEST(CanopenTranslator, RampDownStartsFromMeasuredCurrent) {
    FakePico pico;  // dc_motor_current (0x3032) reads 0.75 A
    auto t = make_translator(pico, shipped_profile());
    ASSERT_TRUE(t->configure());
    pico.set(0x3004, 0, encode_value(ValueType::Q17, 2.0));  // reference 2.0 A, only 0.75 A flows
    pico.set(0x3008, 0, 1);

    pico.clear_writes();
    const auto t0 = std::chrono::steady_clock::now();
    ASSERT_TRUE(t->set_run_stop(false));
    const auto took = std::chrono::steady_clock::now() - t0;
    auto down = q17_writes(pico, 0x3004);
    ASSERT_GT(down.size(), 20u);
    EXPECT_LE(down.front(), 0.75);                             // starts at the measured 0.75 A, not 2.0
    EXPECT_GT(down.front(), 0.70);
    for (size_t i = 1; i < down.size(); ++i) EXPECT_LE(down[i], down[i - 1]);
    EXPECT_DOUBLE_EQ(down.back(), 0.0);
    EXPECT_GE(took, 1200ms);                                   // 0.75 A at 0.5 A/s = 1.5 s
    EXPECT_LT(took, 3s);
    EXPECT_EQ(pico.get(0x3008), 0u);
}

TEST(CanopenTranslator, RampKeepsItsDurationWithASlowNode) {
    FakePico pico;
    DeviceProfile p = pico_profile();
    find_step(p.run, "torque_reference").ramp_ms = 1000;
    find_step(p.run, "torque_reference").value = kTestTorque;
    auto t = make_translator(pico, p);
    ASSERT_TRUE(t->configure());
    pico.reply_delay_ms = 60;                                  // every SDO answer takes 60 ms
    pico.clear_writes();
    const auto t0 = std::chrono::steady_clock::now();
    ASSERT_TRUE(t->set_run_stop(true));
    const auto took = std::chrono::steady_clock::now() - t0;
    // run = drive_enable + direction + ramp (1 s) + final write: ~1.3 s.
    // Step-counted ramp: 50 steps x (20 + 60) ms = 4 s for the ramp alone.
    EXPECT_LT(took, 1800ms);
    EXPECT_GE(took, 1000ms);
    auto up = q17_writes(pico, 0x3004);
    EXPECT_GT(up.size(), 5u);                                  // still a ramp, just coarser
    EXPECT_DOUBLE_EQ(up.back(), kTestTorque);
    pico.reply_delay_ms = 0;
    t->set_run_stop(false);
}

TEST(CanopenTranslator, FixedTimeRampStillSupported) {
    FakePico pico;
    DeviceProfile p = pico_profile();
    find_step(p.run, "torque_reference").ramp_ms = 400;
    find_step(p.run, "torque_reference").value = kTestTorque;
    auto t = make_translator(pico, p);
    ASSERT_TRUE(t->configure());
    pico.clear_writes();
    const auto t0 = std::chrono::steady_clock::now();
    ASSERT_TRUE(t->set_run_stop(true));
    EXPECT_GE(std::chrono::steady_clock::now() - t0, 300ms);
    EXPECT_GT(q17_writes(pico, 0x3004).size(), 10u);
    EXPECT_DOUBLE_EQ(q17_writes(pico, 0x3004).back(), kTestTorque);
    t->set_run_stop(false);
}

TEST(CanopenTranslator, StopInterruptsRampUp) {
    FakePico pico;
    auto t = make_translator(pico, ramp_profile());
    ASSERT_TRUE(t->configure());

    std::atomic<bool> run_result{true};
    std::thread runner([&] { run_result = t->set_run_stop(true); });
    std::this_thread::sleep_for(500ms);                        // ramp-up in progress (~0.25 A)
    const auto t0 = std::chrono::steady_clock::now();
    t->set_run_stop(false);
    runner.join();

    EXPECT_FALSE(run_result);                                  // RUN gave way
    EXPECT_FALSE(t->is_running());
    EXPECT_EQ(pico.get(0x3004), 0u);
    EXPECT_EQ(pico.get(0x3008), 0u);
    auto up = q17_writes(pico, 0x3004);
    double peak = 0;
    for (double v : up) peak = std::max(peak, v);
    EXPECT_LT(peak, 1.0);                                      // never reached the 2 A target
    EXPECT_LT(std::chrono::steady_clock::now() - t0, 4s);
}

TEST(CanopenTranslator, RampDownFailureStillDisablesDrive) {
    FakePico pico;
    auto t = make_translator(pico, ramp_profile());
    ASSERT_TRUE(t->set_run_stop(true));
    pico.reject_writes.insert(0x3004);                         // every torque write fails
    EXPECT_FALSE(t->set_run_stop(false));
    EXPECT_EQ(pico.get(0x3008), 0u);                           // drive disable still sent
    EXPECT_FALSE(t->is_running());
}

TEST(CanopenTranslator, StopNeverRunsConfigure) {
    FakePico pico;
    auto t = make_translator(pico);
    pico.set(0x3008, 0, 1);  // drive left enabled, gateway never configured
    pico.set(0x3004, 0, encode_value(ValueType::Q17, 0.5));
    pico.reject_writes.insert(0x3015);  // configure would fail here
    ASSERT_FALSE(t->is_configured());

    EXPECT_TRUE(t->set_run_stop(false));
    EXPECT_FALSE(find_write(pico.writes(), 0x3002));  // no configure step written
    EXPECT_EQ(pico.get(0x3004), 0u);
    EXPECT_EQ(pico.get(0x3008), 0u);
}

TEST(CanopenTranslator, StopContinuesAfterFailedStep) {
    FakePico pico;
    auto t = make_translator(pico);
    ASSERT_TRUE(t->set_run_stop(true));
    pico.reject_writes.insert(0x3004);  // "torque = 0" rejected...

    EXPECT_FALSE(t->set_run_stop(false));
    EXPECT_EQ(pico.get(0x3008), 0u);  // ...drive is still disabled
    EXPECT_FALSE(t->is_running());
}

TEST(CanopenTranslator, WaitNextTelemetryTimesOutWhileStopped) {
    FakePico pico;
    auto t = make_translator(pico);
    ASSERT_TRUE(t->configure());

    TelemetrySample s{};
    const auto start = std::chrono::steady_clock::now();
    EXPECT_EQ(t->wait_next_telemetry(100ms, s), TelemetryWait::Timeout);
    EXPECT_LT(std::chrono::steady_clock::now() - start, 1s);  // returned, did not block

    ASSERT_TRUE(t->set_run_stop(true));
    TelemetryWait r = TelemetryWait::Timeout;
    for (int i = 0; i < 20 && r != TelemetryWait::Sample; ++i) r = t->wait_next_telemetry(100ms, s);
    EXPECT_EQ(r, TelemetryWait::Sample);
    ASSERT_TRUE(t->set_run_stop(false));
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
