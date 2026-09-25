#include "device_profile.h"

#include <cmath>
#include <fstream>
#include <limits>
#include <set>
#include <sstream>

#include <nlohmann/json.hpp>

namespace wizard {

namespace {

using nlohmann::json;

constexpr double kQ17One = 131072.0;  // 2^17

// Collects every problem so one run of the gateway reports them all.
class Checker {
public:
    void error(const std::string& where, const std::string& what) {
        errors_.push_back(where + ": " + what);
    }
    bool ok() const { return errors_.empty(); }
    std::string report() const {
        std::ostringstream ss;
        ss << errors_.size() << " problem(s) in device profile:";
        for (const auto& e : errors_) ss << "\n  - " << e;
        return ss.str();
    }

private:
    std::vector<std::string> errors_;
};

// Keys starting with '_' are comments. Any other unknown key is an error so
// that a typo ("perod_ms") does not silently fall back to a default.
void check_keys(const json& obj, const std::set<std::string>& allowed, const std::string& where,
                Checker& c) {
    for (auto it = obj.begin(); it != obj.end(); ++it) {
        const std::string& key = it.key();
        if (!key.empty() && key[0] == '_') continue;
        if (!allowed.count(key)) c.error(where, "unknown key '" + key + "'");
    }
}

// Integers may be written as JSON numbers or as "0x..." strings.
std::optional<uint64_t> to_uint(const json& v) {
    if (v.is_number_unsigned()) return v.get<uint64_t>();
    if (v.is_number_integer() && v.get<int64_t>() >= 0) return static_cast<uint64_t>(v.get<int64_t>());
    if (v.is_string()) {
        const std::string s = v.get<std::string>();
        try {
            size_t used = 0;
            const uint64_t n = std::stoull(s, &used, 0);  // base 0: accepts 0x..
            if (used == s.size()) return n;
        } catch (...) {
        }
    }
    return std::nullopt;
}

std::optional<uint64_t> get_uint(const json& obj, const std::string& key, const std::string& where,
                                 Checker& c, uint64_t max, bool required = true) {
    if (!obj.contains(key)) {
        if (required) c.error(where, "missing '" + key + "'");
        return std::nullopt;
    }
    auto n = to_uint(obj[key]);
    if (!n || *n > max) {
        c.error(where, "'" + key + "' must be an integer 0.." + std::to_string(max));
        return std::nullopt;
    }
    return n;
}

bool get_bool(const json& obj, const std::string& key, bool fallback, const std::string& where, Checker& c) {
    if (!obj.contains(key)) return fallback;
    if (!obj[key].is_boolean()) {
        c.error(where, "'" + key + "' must be true or false");
        return fallback;
    }
    return obj[key].get<bool>();
}

std::optional<ValueType> to_type(const json& v) {
    if (!v.is_string()) return std::nullopt;
    const std::string s = v.get<std::string>();
    if (s == "u8") return ValueType::U8;
    if (s == "u16") return ValueType::U16;
    if (s == "u32") return ValueType::U32;
    if (s == "i16") return ValueType::I16;
    if (s == "i32") return ValueType::I32;
    if (s == "q17") return ValueType::Q17;
    return std::nullopt;
}

std::optional<TelemetryKind> to_kind(const std::string& s) {
    if (s == "position") return TelemetryKind::Position;
    if (s == "speed" || s == "velocity") return TelemetryKind::Velocity;
    if (s == "current") return TelemetryKind::Current;
    return std::nullopt;
}

std::vector<WriteStep> parse_steps(const json& root, const std::string& key,
                                   const DeviceProfile& p, Checker& c) {
    std::vector<WriteStep> steps;
    if (!root.contains(key)) {
        c.error(key, "missing");
        return steps;
    }
    if (!root[key].is_array()) {
        c.error(key, "must be an array of steps");
        return steps;
    }
    for (size_t i = 0; i < root[key].size(); ++i) {
        const json& s = root[key][i];
        const std::string where = key + "[" + std::to_string(i) + "]";
        if (!s.is_object()) {
            c.error(where, "must be an object");
            continue;
        }
        check_keys(s, {"object", "value", "verify", "wait_ms", "skip"}, where, c);
        if (get_bool(s, "skip", false, where, c)) continue;  // kept in the file, not executed

        WriteStep step;
        if (!s.contains("object") || !s["object"].is_string()) {
            c.error(where, "missing 'object'");
            continue;
        }
        step.object = s["object"].get<std::string>();
        if (!p.objects.count(step.object)) {
            c.error(where, "unknown object '" + step.object + "'");
            continue;
        }
        if (s.contains("value") && s["value"].is_number()) {
            step.value = s["value"].get<double>();
        } else if (auto n = s.contains("value") ? to_uint(s["value"]) : std::nullopt) {
            step.value = static_cast<double>(*n);  // "0x80000281" style
        } else {
            c.error(where, "'value' must be a number or a \"0x...\" string");
            continue;
        }
        try {
            encode_value(p.objects.at(step.object).type, step.value);
        } catch (const ProfileError& e) {
            c.error(where, e.what());
        }
        step.verify = get_bool(s, "verify", false, where, c);
        if (auto w = get_uint(s, "wait_ms", where, c, 60000, false)) step.wait_ms = static_cast<uint32_t>(*w);
        steps.push_back(step);
    }
    return steps;
}

std::string get_object_ref(const json& obj, const std::string& key, const std::string& where,
                           const DeviceProfile& p, Checker& c) {
    if (!obj.contains(key) || !obj[key].is_string()) {
        c.error(where, "missing '" + key + "' (object name)");
        return {};
    }
    std::string name = obj[key].get<std::string>();
    if (!p.objects.count(name)) c.error(where, "unknown object '" + name + "'");
    return name;
}

InfoField parse_info_field(const json& info, const std::string& key, const DeviceProfile& p,
                           Checker& c) {
    InfoField f;
    const std::string where = "device_info." + key;
    if (!info.contains(key) || !info[key].is_object()) {
        c.error(where, "must be {\"object\": name} or {\"value\": n}");
        return f;
    }
    const json& v = info[key];
    check_keys(v, {"object", "value"}, where, c);
    if (v.contains("object")) {
        f.object = get_object_ref(v, "object", where, p, c);
    } else if (auto n = get_uint(v, "value", where, c, 0xFFFFFFFFull)) {
        f.value = static_cast<uint32_t>(*n);
    }
    return f;
}

}  // namespace

// --------------------------------------------------------------- values

uint8_t value_size(ValueType type) {
    switch (type) {
        case ValueType::U8: return 1;
        case ValueType::U16:
        case ValueType::I16: return 2;
        default: return 4;
    }
}

uint32_t encode_value(ValueType type, double value) {
    auto out_of_range = [&](const char* t) {
        std::ostringstream ss;
        ss << "value " << value << " does not fit type " << t;
        return ProfileError(ss.str());
    };
    const bool integral = std::floor(value) == value;
    switch (type) {
        case ValueType::U8:
            if (!integral || value < 0 || value > 0xFF) throw out_of_range("u8");
            return static_cast<uint32_t>(value);
        case ValueType::U16:
            if (!integral || value < 0 || value > 0xFFFF) throw out_of_range("u16");
            return static_cast<uint32_t>(value);
        case ValueType::U32:
            if (!integral || value < 0 || value > 4294967295.0) throw out_of_range("u32");
            return static_cast<uint32_t>(value);
        case ValueType::I16:
            if (!integral || value < -32768 || value > 32767) throw out_of_range("i16");
            return static_cast<uint32_t>(static_cast<uint16_t>(static_cast<int16_t>(value)));
        case ValueType::I32:
            if (!integral || value < -2147483648.0 || value > 2147483647.0) throw out_of_range("i32");
            return static_cast<uint32_t>(static_cast<int32_t>(value));
        case ValueType::Q17: {
            const double raw = std::round(value * kQ17One);
            if (raw < -2147483648.0 || raw > 2147483647.0) throw out_of_range("q17");
            return static_cast<uint32_t>(static_cast<int32_t>(raw));
        }
    }
    throw ProfileError("unknown value type");
}

double decode_value(ValueType type, uint32_t raw) {
    switch (type) {
        case ValueType::U8: return raw & 0xFFu;
        case ValueType::U16: return raw & 0xFFFFu;
        case ValueType::U32: return raw;
        case ValueType::I16: return static_cast<int16_t>(raw & 0xFFFFu);
        case ValueType::I32: return static_cast<int32_t>(raw);
        case ValueType::Q17: return static_cast<int32_t>(raw) / kQ17One;
    }
    return 0.0;
}

// --------------------------------------------------------------- profile

const ObjectDef& DeviceProfile::object(const std::string& name) const {
    auto it = objects.find(name);
    if (it == objects.end()) throw ProfileError("unknown object '" + name + "'");
    return it->second;
}

static DeviceProfile parse_device_profile_impl(const std::string& json_text);

DeviceProfile parse_device_profile(const std::string& json_text) {
    try {
        return parse_device_profile_impl(json_text);
    } catch (const json::exception& e) {
        throw ProfileError(std::string("malformed profile: ") + e.what());
    }
}

static DeviceProfile parse_device_profile_impl(const std::string& json_text) {
    json root;
    try {
        root = json::parse(json_text, nullptr, true, /*ignore_comments=*/true);
    } catch (const json::parse_error& e) {
        throw ProfileError(std::string("invalid JSON: ") + e.what());
    }
    if (!root.is_object()) throw ProfileError("top level must be a JSON object");

    Checker c;
    DeviceProfile p;
    check_keys(root,
               {"name", "node_id", "sdo", "sync_period_ms", "telemetry_only_while_running", "objects",
                "configure", "run", "stop", "status", "alive_object", "device_info", "telemetry"},
               "profile", c);

    p.name = "unnamed device";
    if (root.contains("name")) {
        if (root["name"].is_string()) p.name = root["name"].get<std::string>();
        else c.error("profile", "'name' must be a string");
    }
    if (auto n = get_uint(root, "node_id", "profile", c, 127)) {
        if (*n == 0) c.error("profile", "'node_id' must be 1..127");
        p.node_id = static_cast<uint8_t>(*n);
    }

    if (root.contains("sdo")) {
        const json& s = root["sdo"];
        check_keys(s, {"timeout_ms", "size_indicated_writes", "read_subindex"}, "sdo", c);
        if (auto n = get_uint(s, "timeout_ms", "sdo", c, 10000, false)) p.sdo_timeout_ms = static_cast<uint32_t>(*n);
        p.sdo_size_indicated = get_bool(s, "size_indicated_writes", true, "sdo", c);
        if (s.contains("read_subindex") && !s["read_subindex"].is_null()) {
            if (auto n = get_uint(s, "read_subindex", "sdo", c, 255)) p.sdo_read_subindex = static_cast<uint8_t>(*n);
        }
    }
    if (auto n = get_uint(root, "sync_period_ms", "profile", c, 10000, false)) p.sync_period_ms = static_cast<uint32_t>(*n);
    p.telemetry_only_while_running = get_bool(root, "telemetry_only_while_running", true, "profile", c);

    // objects first: everything else refers to them by name
    if (!root.contains("objects") || !root["objects"].is_object()) {
        c.error("objects", "missing or not an object");
    } else {
        for (auto it = root["objects"].begin(); it != root["objects"].end(); ++it) {
            if (!it.key().empty() && it.key()[0] == '_') continue;
            const std::string where = "objects." + it.key();
            const json& o = it.value();
            if (!o.is_object()) {
                c.error(where, "must be {\"index\", \"sub\", \"type\"}");
                continue;
            }
            check_keys(o, {"index", "sub", "type"}, where, c);
            ObjectDef def;
            def.name = it.key();
            if (auto n = get_uint(o, "index", where, c, 0xFFFF)) def.index = static_cast<uint16_t>(*n);
            if (auto n = get_uint(o, "sub", where, c, 0xFF, false)) def.subindex = static_cast<uint8_t>(*n);
            if (auto t = o.contains("type") ? to_type(o["type"]) : std::nullopt) {
                def.type = *t;
            } else {
                c.error(where, "'type' must be one of u8, u16, u32, i16, i32, q17");
            }
            p.objects[def.name] = def;
        }
    }

    p.configure = parse_steps(root, "configure", p, c);
    p.run = parse_steps(root, "run", p, c);
    p.stop = parse_steps(root, "stop", p, c);

    if (!root.contains("status") || !root["status"].is_object()) {
        c.error("status", "missing {\"object\", \"running_value\"}");
    } else {
        const json& s = root["status"];
        check_keys(s, {"object", "running_value"}, "status", c);
        p.status_object = get_object_ref(s, "object", "status", p, c);
        if (s.contains("running_value") && s["running_value"].is_number())
            p.status_running_value = s["running_value"].get<double>();
        else
            c.error("status", "'running_value' must be a number");
    }

    if (!root.contains("alive_object") || !root["alive_object"].is_string()) {
        c.error("alive_object", "missing (name of an object the heartbeat reads)");
    } else {
        p.alive_object = root["alive_object"].get<std::string>();
        if (!p.objects.count(p.alive_object)) c.error("alive_object", "unknown object '" + p.alive_object + "'");
    }

    if (!root.contains("device_info") || !root["device_info"].is_object()) {
        c.error("device_info", "missing");
    } else {
        const json& info = root["device_info"];
        check_keys(info, {"vendor_id", "product_code", "revision", "serial"}, "device_info", c);
        p.vendor_id = parse_info_field(info, "vendor_id", p, c);
        p.product_code = parse_info_field(info, "product_code", p, c);
        p.revision = parse_info_field(info, "revision", p, c);
        p.serial = parse_info_field(info, "serial", p, c);
    }

    if (!root.contains("telemetry") || !root["telemetry"].is_array()) {
        c.error("telemetry", "missing or not an array");
    } else {
        std::set<uint32_t> cob_ids;
        for (size_t i = 0; i < root["telemetry"].size(); ++i) {
            const json& t = root["telemetry"][i];
            const std::string where = "telemetry[" + std::to_string(i) + "]";
            if (!t.is_object()) {
                c.error(where, "must be an object");
                continue;
            }
            check_keys(t, {"kind", "source", "cob_id", "object", "period_ms", "type", "scale", "skip"}, where, c);
            if (get_bool(t, "skip", false, where, c)) continue;

            TelemetryChannel ch;
            auto kind = t.contains("kind") && t["kind"].is_string() ? to_kind(t["kind"].get<std::string>())
                                                                     : std::nullopt;
            if (!kind) {
                c.error(where, "'kind' must be position, speed or current");
                continue;
            }
            ch.kind = *kind;
            if (t.contains("scale")) {
                if (t["scale"].is_number()) ch.scale = t["scale"].get<double>();
                else c.error(where, "'scale' must be a number");
            }

            const std::string source =
                t.contains("source") && t["source"].is_string() ? t["source"].get<std::string>() : "";
            if (source == "tpdo") {
                ch.source = TelemetrySource::Tpdo;
                if (auto n = get_uint(t, "cob_id", where, c, 0x7FF)) {
                    ch.cob_id = static_cast<uint32_t>(*n);
                    if (!cob_ids.insert(ch.cob_id).second) c.error(where, "duplicate cob_id");
                }
                if (auto ty = t.contains("type") ? to_type(t["type"]) : std::nullopt) ch.type = *ty;
                else c.error(where, "'type' must be one of u8, u16, u32, i16, i32, q17");
            } else if (source == "sdo_poll") {
                ch.source = TelemetrySource::SdoPoll;
                ch.object = get_object_ref(t, "object", where, p, c);
                if (p.objects.count(ch.object)) ch.type = p.objects.at(ch.object).type;
                if (auto n = get_uint(t, "period_ms", where, c, 60000)) {
                    if (*n == 0) c.error(where, "'period_ms' must be >= 1");
                    ch.period_ms = static_cast<uint32_t>(*n);
                }
            } else {
                c.error(where, "'source' must be \"tpdo\" or \"sdo_poll\"");
                continue;
            }
            p.telemetry.push_back(ch);
        }
    }

    if (!c.ok()) throw ProfileError(c.report());
    return p;
}

DeviceProfile load_device_profile(const std::string& path) {
    std::ifstream in(path);
    if (!in) throw ProfileError("cannot open device profile " + path);
    std::stringstream ss;
    ss << in.rdbuf();
    try {
        return parse_device_profile(ss.str());
    } catch (const ProfileError& e) {
        throw ProfileError(path + ": " + e.what());
    }
}

}  // namespace wizard
