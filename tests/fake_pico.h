#pragma once

// Test helpers shared by the CanopenTranslator and CommandManager tests:
// a fake SOLO PICO over a socketpair (no SocketCAN / vcan needed) and the
// shipped device profile + command set, with or without ramps.
//
// The fake talks struct can_frame over the socketpair. It answers SDOs the way
// the PICO does (reads -> 0x42, writes -> 0x60, unknown objects -> abort) and
// sends synchronous TPDOs on SYNC.

#include <linux/can.h>
#include <sys/socket.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <cstring>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include "canopen/canopen_translator.h"
#include "canopen/device_profile.h"
#include "commands/commands.h"

namespace wizard::test {


inline constexpr uint32_t key(uint16_t index, uint8_t sub) { return (static_cast<uint32_t>(index) << 8) | sub; }

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
// here instead of hard-coding them, so tuning the JSON files doesn't break them.
inline WriteStep& find_step(std::vector<WriteStep>& seq, const std::string& object) {
    for (auto& s : seq)
        if (s.object == object) return s;
    throw std::runtime_error("sequence has no step '" + object + "'");
}

// Device profile + command set, as the gateway loads them. (Not "Setup": that name is taken by gtest.)
struct Config {
    DeviceProfile p;
    CommandSet cs;

    MotionModeDef& torque() { return cs.modes.at(MotionMode::Torque); }
    // Reference value (A) the legacy RUN drives the torque to.
    double run_torque() const {
        return cs.mode(cs.legacy_run.mode)->reference_value(cs.legacy_run.setpoint);
    }
};

// The shipped files, exactly as installed (ramps included).
inline Config shipped() {
    Config s;
    s.p = load_device_profile(std::string(WIZARD_SOURCE_DIR) + "/device-gateway/canopen/devices/solopico.json");
    s.p.sdo_timeout_ms = 100;  // keep "node silent" tests fast
    s.cs = load_command_set(std::string(WIZARD_SOURCE_DIR) + "/device-gateway/commands/solopico_commands.json", s.p);
    return s;
}

// Ramp tests use a fixed RUN torque target: they test the ramp mechanism, not
// the shipped torque, and the shipped 5.75 A would make each ramp take 11.5 s.
inline constexpr double kTestTorque = 2.0;  // A
inline Config ramp_setup() {
    Config s = shipped();
    s.cs.legacy_run.mode = MotionMode::Torque;
    s.cs.legacy_run.setpoint = static_cast<int32_t>(kTestTorque / s.torque().scale);  // 2000 mA
    return s;
}

// Same, without ramps, so tests that are not about ramps stay fast.
inline Config pico_setup() {
    Config s = shipped();
    for (auto* seq : {&s.p.configure, &s.cs.stop, &s.cs.enable})
        for (auto& step : *seq) {
            step.ramp_ms = 0;
            step.ramp_rate = 0;
            step.ramp_start_object.clear();
        }
    for (auto& [mode, def] : s.cs.modes) def.ramp_rate = 0;
    return s;
}

// CanopenTranslator plus run_stop(): what 0x40 RUN / STOP does (legacy_run /
// stop), without a CommandManager, for tests that are about the translator.
class TestTranslator : public CanopenTranslator {
public:
    TestTranslator(FakePico& pico, Config c)
        : CanopenTranslator(std::make_unique<CanopenClient>(pico.gateway_fd, c.p.node_id), c.p, c.cs.stop),
          cs_(std::move(c.cs)) {}

    bool run_stop(bool run) {
        if (!run) return stop();
        const LegacyRun& r = cs_.legacy_run;
        return start(cs_.start_steps(r.mode, r.setpoint, r.direction));
    }

private:
    CommandSet cs_;
};

inline std::unique_ptr<TestTranslator> make_translator(FakePico& pico, Config s = pico_setup()) {
    return std::make_unique<TestTranslator>(pico, std::move(s));
}

inline std::optional<SdoWrite> find_write(const std::vector<SdoWrite>& w, uint16_t index, uint8_t sub = 0) {
    for (const auto& x : w)
        if (x.index == index && x.sub == sub) return x;
    return std::nullopt;
}


// Values written to one object, in order, decoded as q17.
inline std::vector<double> q17_writes(FakePico& pico, uint16_t index) {
    std::vector<double> out;
    for (const auto& w : pico.writes())
        if (w.index == index) out.push_back(decode_value(ValueType::Q17, w.value));
    return out;
}

}  // namespace wizard::test
