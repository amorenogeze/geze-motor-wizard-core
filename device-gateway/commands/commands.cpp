#include "commands.h"

#include <cmath>
#include <fstream>
#include <limits>
#include <iostream>
#include <sstream>

#include "canopen/profile_json.h"

namespace wizard {

namespace {

using namespace profile_json;

const std::map<std::string, MotionMode> kModeNames = {
    {"torque", MotionMode::Torque},
    {"speed", MotionMode::Speed},
    {"position", MotionMode::Position},
};

std::optional<int32_t> get_int32(const json& obj, const std::string& key, const std::string& where, Checker& c) {
    if (!obj.contains(key)) {
        c.error(where, "missing '" + key + "'");
        return std::nullopt;
    }
    const json& v = obj[key];
    if (!v.is_number_integer() || v.get<int64_t>() < std::numeric_limits<int32_t>::min() ||
        v.get<int64_t>() > std::numeric_limits<int32_t>::max()) {
        c.error(where, "'" + key + "' must be a 32-bit integer");
        return std::nullopt;
    }
    return static_cast<int32_t>(v.get<int64_t>());
}

std::optional<double> get_number(const json& obj, const std::string& key, const std::string& where, Checker& c,
                                 bool required) {
    if (!obj.contains(key)) {
        if (required) c.error(where, "missing '" + key + "'");
        return std::nullopt;
    }
    if (!obj[key].is_number()) {
        c.error(where, "'" + key + "' must be a number");
        return std::nullopt;
    }
    return obj[key].get<double>();
}

// The value must be writable to the object; reports instead of throwing.
void check_fits(const DeviceProfile& device, const std::string& object, double value, const std::string& where,
                const std::string& what, Checker& c) {
    if (!device.objects.count(object)) return;  // already reported as unknown object
    try {
        encode_value(device.objects.at(object).type, value);
    } catch (const ProfileError& e) {
        c.error(where, what + ": " + e.what() + " (object '" + object + "')");
    }
}

MotionModeDef parse_mode(const json& m, const std::string& name, const DeviceProfile& device, Checker& c) {
    const std::string where = "modes." + name;
    MotionModeDef def;
    def.name = name;
    check_keys(m, {"unit", "enter", "reference", "scale", "min", "max", "ramp_rate", "uses_direction",
                   "reference_first"},
               where, c);

    if (m.contains("unit")) {
        if (m["unit"].is_string()) def.unit = m["unit"].get<std::string>();
        else c.error(where, "'unit' must be a string");
    }
    def.enter = parse_steps(m, "enter", device, c, /*required=*/false);
    def.reference = get_object_ref(m, "reference", where, device, c);

    if (auto s = get_number(m, "scale", where, c, false)) {
        if (*s == 0) c.error(where, "'scale' must not be 0");
        else def.scale = *s;
    }
    auto lo = get_int32(m, "min", where, c);
    auto hi = get_int32(m, "max", where, c);
    if (lo && hi) {
        def.min = *lo;
        def.max = *hi;
        if (def.min > def.max) c.error(where, "'min' must be <= 'max'");
    }
    if (auto r = get_number(m, "ramp_rate", where, c, false)) {
        if (*r < 0) c.error(where, "'ramp_rate' must be >= 0 (0 = no ramp)");
        else def.ramp_rate = *r;
    }
    def.uses_direction = get_bool(m, "uses_direction", true, where, c);
    def.reference_first = get_bool(m, "reference_first", false, where, c);

    // Every setpoint in [min, max] must be writable: integer objects need an
    // integer scale, and both limits must fit the object's type.
    if (device.objects.count(def.reference)) {
        const ValueType t = device.objects.at(def.reference).type;
        if (t != ValueType::Q17 && std::floor(def.scale) != def.scale)
            c.error(where, "'scale' must be an integer for integer object '" + def.reference + "'");
        else if (lo && hi) {
            check_fits(device, def.reference, def.min * def.scale, where, "'min' x 'scale'", c);
            check_fits(device, def.reference, def.max * def.scale, where, "'max' x 'scale'", c);
        }
    }
    return def;
}

}  // namespace

const MotionModeDef* CommandSet::mode(MotionMode m) const {
    auto it = modes.find(m);
    return it == modes.end() ? nullptr : &it->second;
}

namespace {

const MotionModeDef& require_mode(const CommandSet& cs, MotionMode mode) {
    const MotionModeDef* def = cs.mode(mode);
    if (!def) throw ProfileError("mode " + std::to_string(static_cast<int>(mode)) + " is not defined in the command set");
    return *def;
}

}  // namespace

SetMotionPayload CommandSet::run_stop_command(bool run) const {
    if (!run) return {MotionMode::Stop, 0, MotionDirection::Cw};
    return {legacy_run.mode, legacy_run.setpoint, legacy_run.direction};
}

WriteStep CommandSet::reference_step(MotionMode mode, int32_t setpoint) const {
    const MotionModeDef& def = require_mode(*this, mode);
    WriteStep step;
    step.object = def.reference;
    step.value = def.reference_value(setpoint);
    step.ramp_rate = def.ramp_rate;
    return step;
}

std::vector<WriteStep> CommandSet::start_steps(MotionMode mode, int32_t setpoint, MotionDirection direction) const {
    const MotionModeDef& def = require_mode(*this, mode);
    std::vector<WriteStep> steps = def.enter;
    if (def.uses_direction) {
        WriteStep dir;
        dir.object = direction_object;
        dir.value = direction == MotionDirection::Ccw ? direction_ccw : direction_cw;
        steps.push_back(dir);
    }
    const WriteStep reference = reference_step(mode, setpoint);
    if (def.reference_first) steps.push_back(reference);
    steps.insert(steps.end(), enable.begin(), enable.end());
    if (!def.reference_first) steps.push_back(reference);
    return steps;
}

static CommandSet parse_command_set_impl(const std::string& json_text, const DeviceProfile& device) {
    json root;
    try {
        root = json::parse(json_text, nullptr, true, /*ignore_comments=*/true);
    } catch (const json::parse_error& e) {
        throw ProfileError(std::string("invalid JSON: ") + e.what());
    }
    if (!root.is_object()) throw ProfileError("top level must be a JSON object");

    Checker c("command set");
    CommandSet cs;
    check_keys(root, {"device", "direction", "enable", "stop", "home", "modes", "legacy_run"}, "commands", c);

    if (!root.contains("device") || !root["device"].is_string()) {
        c.error("device", "missing (name of the device profile these commands are for)");
    } else {
        cs.device = root["device"].get<std::string>();
        if (cs.device != device.name)
            c.error("device", "commands are for '" + cs.device + "' but the device profile is '" + device.name + "'");
    }

    cs.enable = parse_steps(root, "enable", device, c);
    cs.stop = parse_steps(root, "stop", device, c);
    cs.home = parse_steps(root, "home", device, c, /*required=*/false);

    if (!root.contains("modes") || !root["modes"].is_object() || root["modes"].empty()) {
        c.error("modes", "missing (at least one of torque, speed, position)");
    } else {
        for (auto it = root["modes"].begin(); it != root["modes"].end(); ++it) {
            if (!it.key().empty() && it.key()[0] == '_') continue;
            auto known = kModeNames.find(it.key());
            if (known == kModeNames.end()) {
                c.error("modes", "unknown mode '" + it.key() + "' (torque, speed, position)");
                continue;
            }
            if (!it.value().is_object()) {
                c.error("modes." + it.key(), "must be an object");
                continue;
            }
            cs.modes[known->second] = parse_mode(it.value(), it.key(), device, c);
        }
    }

    bool any_direction = false;
    for (const auto& [m, def] : cs.modes) any_direction = any_direction || def.uses_direction;
    if (root.contains("direction")) {
        const json& d = root["direction"];
        if (!d.is_object()) {
            c.error("direction", "must be {\"object\", \"cw\", \"ccw\"}");
        } else {
            check_keys(d, {"object", "cw", "ccw"}, "direction", c);
            cs.direction_object = get_object_ref(d, "object", "direction", device, c);
            if (auto v = get_number(d, "cw", "direction", c, true)) cs.direction_cw = *v;
            if (auto v = get_number(d, "ccw", "direction", c, true)) cs.direction_ccw = *v;
            check_fits(device, cs.direction_object, cs.direction_cw, "direction", "'cw'", c);
            check_fits(device, cs.direction_object, cs.direction_ccw, "direction", "'ccw'", c);
        }
    } else if (any_direction) {
        c.error("direction", "missing, but a mode has \"uses_direction\": true");
    }

    if (!root.contains("legacy_run") || !root["legacy_run"].is_object()) {
        c.error("legacy_run", "missing {\"mode\", \"setpoint\", \"direction\"} (what SetRunStopCommand RUN does)");
    } else {
        const json& r = root["legacy_run"];
        check_keys(r, {"mode", "setpoint", "direction"}, "legacy_run", c);
        const std::string mode = r.contains("mode") && r["mode"].is_string() ? r["mode"].get<std::string>() : "";
        auto known = kModeNames.find(mode);
        if (known == kModeNames.end() || !cs.modes.count(known->second)) {
            c.error("legacy_run", "'mode' must name a mode defined in 'modes'");
        } else {
            cs.legacy_run.mode = known->second;
            if (auto sp = get_int32(r, "setpoint", "legacy_run", c)) {
                const MotionModeDef& def = cs.modes.at(known->second);
                if (*sp < def.min || *sp > def.max)
                    c.error("legacy_run", "'setpoint' " + std::to_string(*sp) + " is outside modes." + mode + " [" +
                                              std::to_string(def.min) + ", " + std::to_string(def.max) + "]");
                cs.legacy_run.setpoint = *sp;
            }
        }
        const std::string dir =
            r.contains("direction") && r["direction"].is_string() ? r["direction"].get<std::string>() : "";
        if (dir == "cw") cs.legacy_run.direction = MotionDirection::Cw;
        else if (dir == "ccw") cs.legacy_run.direction = MotionDirection::Ccw;
        else c.error("legacy_run", "'direction' must be \"cw\" or \"ccw\"");
    }

    if (!c.ok()) throw ProfileError(c.report());
    return cs;
}

CommandSet parse_command_set(const std::string& json_text, const DeviceProfile& device) {
    try {
        return parse_command_set_impl(json_text, device);
    } catch (const json::exception& e) {
        throw ProfileError(std::string("malformed command set: ") + e.what());
    }
}

CommandSet load_command_set(const std::string& path, const DeviceProfile& device) {
    std::ifstream in(path);
    if (!in) throw ProfileError("cannot open command set " + path);
    std::stringstream ss;
    ss << in.rdbuf();
    try {
        return parse_command_set(ss.str(), device);
    } catch (const ProfileError& e) {
        throw ProfileError(path + ": " + e.what());
    }
}

// =============================================================== CommandManager


namespace {

const char* mode_name(MotionMode m) {
    switch (m) {
        case MotionMode::Stop: return "STOP";
        case MotionMode::Torque: return "TORQUE";
        case MotionMode::Speed: return "SPEED";
        case MotionMode::Position: return "POSITION";
        case MotionMode::Home: return "HOME";
    }
    return "?";
}

}  // namespace

CommandManager::CommandManager(DeviceTranslator& translator, const CommandSet& commands)
    : translator_(translator), commands_(commands) {}

std::string CommandManager::describe(const SetMotionPayload& cmd) const {
    std::ostringstream ss;
    ss << mode_name(cmd.mode);
    if (const MotionModeDef* def = commands_.mode(cmd.mode)) {
        ss << " " << cmd.setpoint << (def->unit.empty() ? "" : " " + def->unit);
        if (def->uses_direction) ss << (cmd.direction == MotionDirection::Ccw ? " CCW" : " CW");
    }
    return ss.str();
}

std::optional<MotionMode> CommandManager::active_mode() const {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!translator_.is_running() || !active_) return std::nullopt;
    return active_->mode;
}

bool CommandManager::stop_locked() {
    active_.reset();
    return translator_.stop();
}

bool CommandManager::handle(const SetMotionPayload& cmd) {
    std::lock_guard<std::mutex> lock(mutex_);
    const std::string what = describe(cmd);
    const bool running = translator_.is_running();
    if (!running) active_.reset();  // stopped elsewhere (session end, node lost, failed step)

    switch (cmd.mode) {
        case MotionMode::Stop:
            std::cout << "command " << what << "\n";
            return stop_locked();

        case MotionMode::Home:
            if (running) {
                std::cerr << "command HOME refused: motor is running (send STOP first)\n";
                return false;
            }
            if (commands_.home.empty()) {
                std::cerr << "command HOME refused: no 'home' steps in the command set\n";
                return false;
            }
            std::cout << "command HOME\n";
            return translator_.execute(commands_.home);

        case MotionMode::Torque:
        case MotionMode::Speed:
        case MotionMode::Position:
            break;
    }

    const MotionModeDef* def = commands_.mode(cmd.mode);
    if (!def) {
        std::cerr << "command " << what << " refused: mode not defined in the command set\n";
        return false;
    }
    if (def->clamp(cmd.setpoint) != cmd.setpoint)
        std::cerr << "command " << what << ": setpoint clamped to " << def->clamp(cmd.setpoint) << " (limits "
                  << def->min << ".." << def->max << ")\n";
    // Direction only matters for modes that use it: POSITION CW and CCW are the same command.
    const MotionDirection dir = def->uses_direction ? cmd.direction : MotionDirection::Cw;

    if (running && active_ && active_->mode == cmd.mode && active_->direction == dir) {
        std::cout << "command " << what << ": live update\n";
        if (translator_.execute({commands_.reference_step(cmd.mode, cmd.setpoint)})) return true;
        std::cerr << "live update failed, stopping\n";
        stop_locked();
        return false;
    }

    if (running) {
        std::cout << "command " << what << ": mode or direction change, stopping first\n";
        stop_locked();
    }
    return start_mode(cmd.mode, cmd.setpoint, dir);
}

bool CommandManager::start_mode(MotionMode mode, int32_t setpoint, MotionDirection direction) {
    std::cout << "command " << describe({mode, setpoint, direction}) << ": start\n";
    if (!translator_.start(commands_.start_steps(mode, setpoint, direction))) {
        active_.reset();
        return false;
    }
    active_ = Active{mode, direction};
    return true;
}

}  // namespace wizard
