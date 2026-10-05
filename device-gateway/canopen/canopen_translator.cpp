#include "canopen_translator.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <iomanip>
#include <iostream>
#include <limits>
#include <sstream>

namespace wizard {

namespace {

using Clock = std::chrono::steady_clock;

std::string describe(const ObjectDef& o) {
    std::ostringstream ss;
    ss << o.name << " (0x" << std::hex << std::uppercase << std::setw(4) << std::setfill('0') << o.index
       << ":" << std::setw(2) << static_cast<int>(o.subindex) << ")";
    return ss.str();
}

std::string describe(const SdoResult& r) {
    if (r.timeout) return "timeout";
    if (r.abort_code) {
        std::ostringstream ss;
        ss << "SDO abort 0x" << std::hex << std::setw(8) << std::setfill('0') << r.abort_code;
        return ss.str();
    }
    return "unexpected response";
}

uint32_t le32(const uint8_t* p) {
    return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) |
           (static_cast<uint32_t>(p[2]) << 16) | (static_cast<uint32_t>(p[3]) << 24);
}

// Two values are "the same setting" if they encode to the same raw value.
bool same_value(ValueType type, double a, double b) {
    try {
        return encode_value(type, a) == encode_value(type, b);
    } catch (const ProfileError&) {
        return false;
    }
}

}  // namespace

CanopenTranslator::CanopenTranslator(const std::string& iface, DeviceProfile profile)
    : CanopenTranslator(std::make_unique<CanopenClient>(iface, profile.node_id), profile) {}

CanopenTranslator::CanopenTranslator(std::unique_ptr<CanopenClient> client, DeviceProfile profile)
    : profile_(std::move(profile)), client_(std::move(client)) {
    client_->set_sdo_timeout(std::chrono::milliseconds(profile_.sdo_timeout_ms));

    std::set<uint32_t> cob_ids;
    for (const auto& ch : profile_.telemetry) {
        if (ch.source == TelemetrySource::Tpdo) {
            tpdo_channels_[ch.cob_id] = ch;
            cob_ids.insert(ch.cob_id);
        } else {
            poll_channels_.push_back(ch);
        }
    }
    client_->set_tpdo_cob_ids(cob_ids);

    std::cout << "device profile '" << profile_.name << "': node " << static_cast<int>(profile_.node_id)
              << ", " << tpdo_channels_.size() << " TPDO + " << poll_channels_.size()
              << " polled telemetry channel(s), SYNC "
              << (profile_.sync_period_ms ? std::to_string(profile_.sync_period_ms) + " ms" : "off") << "\n";
    start_threads();
}

CanopenTranslator::~CanopenTranslator() {
    stop_ = true;
    if (sync_thread_.joinable()) sync_thread_.join();
    if (poll_thread_.joinable()) poll_thread_.join();
    if (pdo_thread_.joinable()) pdo_thread_.join();
    telemetry_.close();
}

void CanopenTranslator::start_threads() {
    pdo_thread_ = std::thread(&CanopenTranslator::pdo_loop, this);
    if (profile_.sync_period_ms > 0) sync_thread_ = std::thread(&CanopenTranslator::sync_loop, this);
    if (!poll_channels_.empty()) poll_thread_ = std::thread(&CanopenTranslator::poll_loop, this);
}

// ---------------------------------------------------------------- SDO helpers

std::optional<uint32_t> CanopenTranslator::read_object_raw(const ObjectDef& obj) {
    const uint8_t sub = profile_.sdo_read_subindex.value_or(obj.subindex);
    auto r = client_->sdo_read(obj.index, sub);
    if (!r.ok) {
        std::cerr << "read " << describe(obj) << " failed: " << describe(r) << "\n";
        return std::nullopt;
    }
    return r.value;
}

std::optional<double> CanopenTranslator::read_object(const std::string& name) {
    const ObjectDef& obj = profile_.object(name);
    auto raw = read_object_raw(obj);
    if (!raw) return std::nullopt;
    return decode_value(obj.type, *raw);
}

bool CanopenTranslator::write_step(const WriteStep& step) {
    const ObjectDef& obj = profile_.object(step.object);
    const uint32_t raw = encode_value(obj.type, step.value);  // validated at load time

    auto r = client_->sdo_write(obj.index, obj.subindex, raw, value_size(obj.type), profile_.sdo_size_indicated);
    if (!r.ok) {
        std::cerr << "write " << describe(obj) << " = " << step.value << " failed: " << describe(r) << "\n";
        return false;
    }
    if (step.verify) {
        auto back = read_object(step.object);
        if (!back) return false;
        if (!same_value(obj.type, *back, step.value)) {
            std::cerr << "verify " << describe(obj) << ": wrote " << step.value << ", read back " << *back
                      << "\n";
            return false;
        }
    }
    if (step.wait_ms) std::this_thread::sleep_for(std::chrono::milliseconds(step.wait_ms));
    return true;
}

bool CanopenTranslator::run_steps(const std::vector<WriteStep>& steps, const char* sequence) {
    for (const auto& step : steps) {
        if (stop_) return false;
        if (!write_step(step)) {
            std::cerr << sequence << " sequence aborted at '" << step.object << "'\n";
            return false;
        }
    }
    return true;
}

// Like run_steps, but never gives up early: a failed "torque = 0" must not
// prevent "drive disable". Used for every stop sequence.
bool CanopenTranslator::run_stop_steps(const char* sequence) {
    bool ok = true;
    for (const auto& step : profile_.stop) {
        if (!write_step(step)) {
            std::cerr << sequence << ": step '" << step.object << "' failed, continuing\n";
            ok = false;
        }
    }
    return ok;
}

// ---------------------------------------------------------------- configure / run / stop

bool CanopenTranslator::configure() {
    std::lock_guard<std::mutex> lock(sequence_mutex_);
    return configure_locked();
}

bool CanopenTranslator::configure_locked() {
    running_ = false;
    std::cout << "configuring " << profile_.name << " (" << profile_.configure.size() << " steps)\n";
    const bool ok = run_steps(profile_.configure, "configure");
    configured_ = ok;
    std::cout << "configure " << (ok ? "done" : "FAILED") << "\n";
    return ok;
}

bool CanopenTranslator::set_run_stop(bool run) {
    std::lock_guard<std::mutex> lock(sequence_mutex_);

    // STOP never depends on configure: it must be attempted even if the node
    // was never configured or configure is failing (e.g. a rejected step).
    if (!run) {
        running_ = false;  // pause SYNC/polling first
        return run_stop_steps("stop");
    }

    if (!configured_ && !configure_locked()) return false;

    if (run_steps(profile_.run, "run")) {
        running_ = true;
        return true;
    }
    // Half-applied RUN: bring the device back to a known stopped state.
    run_stop_steps("stop (after failed run)");
    running_ = false;
    return false;
}

std::optional<bool> CanopenTranslator::read_run_stop_status() {
    const ObjectDef& obj = profile_.object(profile_.status_object);
    auto value = read_object(profile_.status_object);
    if (!value) return std::nullopt;
    return same_value(obj.type, *value, profile_.status_running_value);
}

std::optional<DeviceInfo> CanopenTranslator::read_device_info() {
    auto field = [&](const InfoField& f) -> std::optional<uint32_t> {
        if (!f.object) return f.value;
        return read_object_raw(profile_.object(*f.object));
    };
    auto vendor = field(profile_.vendor_id);
    if (!vendor) return std::nullopt;
    auto product = field(profile_.product_code);
    if (!product) return std::nullopt;
    auto revision = field(profile_.revision);
    if (!revision) return std::nullopt;
    auto serial = field(profile_.serial);
    if (!serial) return std::nullopt;
    return DeviceInfo{*vendor, *product, *revision, *serial};
}

bool CanopenTranslator::probe_alive() {
    const ObjectDef& obj = profile_.object(profile_.alive_object);
    const uint8_t sub = profile_.sdo_read_subindex.value_or(obj.subindex);
    const bool alive = client_->sdo_read(obj.index, sub).ok;  // silent: polled every 500 ms

    if (!alive) {
        if (configured_) std::cerr << profile_.name << " not answering, will reconfigure when it returns\n";
        configured_ = false;
        running_ = false;
        return false;
    }
    if (!configured_) configure();
    return true;
}

// ---------------------------------------------------------------- telemetry

bool CanopenTranslator::telemetry_enabled() const {
    return running_ || !profile_.telemetry_only_while_running;
}

std::optional<TelemetrySample> CanopenTranslator::make_sample(const TelemetryChannel& ch, uint64_t ts,
                                                              double decoded) {
    double v = std::round(decoded * ch.scale);
    // CurrentEvent carries an int16 on the socket.
    const double lo = ch.kind == TelemetryKind::Current ? std::numeric_limits<int16_t>::min()
                                                        : std::numeric_limits<int32_t>::min();
    const double hi = ch.kind == TelemetryKind::Current ? std::numeric_limits<int16_t>::max()
                                                        : std::numeric_limits<int32_t>::max();
    v = std::clamp(v, lo, hi);
    return TelemetrySample{ts, ch.kind, static_cast<int32_t>(v)};
}

std::optional<TelemetrySample> CanopenTranslator::read_next_telemetry() { return telemetry_.pop(); }

TelemetryWait CanopenTranslator::wait_next_telemetry(std::chrono::milliseconds timeout,
                                                     TelemetrySample& out) {
    if (auto s = telemetry_.pop_for(timeout)) {
        out = *s;
        return TelemetryWait::Sample;
    }
    return telemetry_.is_closed() ? TelemetryWait::Closed : TelemetryWait::Timeout;
}

void CanopenTranslator::pdo_loop() {
    while (!stop_) {
        auto frame = client_->read_next_pdo(std::chrono::milliseconds(200));
        if (!frame) {
            if (!client_->is_open()) break;  // CAN socket died: unrecoverable
            continue;
        }
        auto it = tpdo_channels_.find(frame->cob_id);
        if (it == tpdo_channels_.end() || !telemetry_enabled()) continue;
        const TelemetryChannel& ch = it->second;
        if (frame->dlc < value_size(ch.type)) continue;

        uint8_t bytes[4] = {0, 0, 0, 0};
        std::copy(frame->data, frame->data + std::min<uint8_t>(frame->dlc, 4), bytes);
        if (auto s = make_sample(ch, frame->timestamp_us, decode_value(ch.type, le32(bytes)))) telemetry_.push(*s);
    }
    telemetry_.close();  // read_next_telemetry -> nullopt, gateway reports the error
}

void CanopenTranslator::sync_loop() {
    const auto period = std::chrono::milliseconds(profile_.sync_period_ms);
    auto next = Clock::now();
    while (!stop_) {
        next += period;
        std::this_thread::sleep_until(next);
        if (Clock::now() > next + 10 * period) next = Clock::now();  // fell far behind: no burst
        if (telemetry_enabled()) client_->send_sync();
    }
}

void CanopenTranslator::poll_loop() {
    std::vector<Clock::time_point> due(poll_channels_.size(), Clock::now());
    while (!stop_) {
        const auto earliest = *std::min_element(due.begin(), due.end());
        std::this_thread::sleep_until(std::min(earliest, Clock::now() + std::chrono::milliseconds(100)));
        if (stop_) break;

        const auto now = Clock::now();
        for (size_t i = 0; i < poll_channels_.size(); ++i) {
            if (now < due[i]) continue;
            const TelemetryChannel& ch = poll_channels_[i];
            due[i] = now + std::chrono::milliseconds(ch.period_ms);
            if (!telemetry_enabled()) continue;

            const ObjectDef& obj = profile_.object(ch.object);
            const uint8_t sub = profile_.sdo_read_subindex.value_or(obj.subindex);
            auto r = client_->sdo_read(obj.index, sub);
            if (!r.ok) continue;  // heartbeat reports a dead node; don't spam here
            const uint64_t ts = static_cast<uint64_t>(
                std::chrono::duration_cast<std::chrono::microseconds>(
                    std::chrono::system_clock::now().time_since_epoch()).count());
            if (auto s = make_sample(ch, ts, decode_value(obj.type, r.value))) telemetry_.push(*s);
        }
    }
}

}  // namespace wizard
