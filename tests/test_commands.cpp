// Commands (device-gateway/commands/commands.h):
//   CommandSet      loading + checking commands/<device>_commands.json, start / live-update steps
//   CommandManager  UI commands -> device sequences, against the fake SOLO PICO (tests/fake_pico.h)
#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <fstream>
#include <sstream>
#include <string>
#include <thread>

#include <nlohmann/json.hpp>

#include "canopen/device_profile.h"
#include "commands/commands.h"
#include "fake_pico.h"

using namespace wizard;
using namespace wizard::test;
using namespace std::chrono_literals;
using nlohmann::json;

// =============================================================== CommandSet

namespace {

const std::string kProfilePath = std::string(WIZARD_SOURCE_DIR) + "/device-gateway/canopen/devices/solopico.json";
const std::string kCommandsPath =
    std::string(WIZARD_SOURCE_DIR) + "/device-gateway/commands/solopico_commands.json";

DeviceProfile profile() { return load_device_profile(kProfilePath); }

// A small valid command set; tests break one thing at a time.
json valid() {
    return json::parse(R"({
        "device": "SOLO PICO",
        "direction": { "object": "motor_direction", "cw": 1, "ccw": 0 },
        "enable": [ { "object": "drive_enable", "value": 1 } ],
        "stop":   [ { "object": "drive_enable", "value": 0 } ],
        "modes": {
            "torque": { "unit": "mA", "enter": [ { "object": "control_mode", "value": 1 } ],
                        "reference": "torque_reference", "scale": 0.001, "min": 0, "max": 10000, "ramp_rate": 0.5 }
        },
        "legacy_run": { "mode": "torque", "setpoint": 5750, "direction": "cw" }
    })");
}

// Parses and returns the error text ("" if it parsed).
std::string error_of(const json& j) {
    try {
        parse_command_set(j.dump(), profile());
        return "";
    } catch (const ProfileError& e) {
        return e.what();
    }
}

}  // namespace

TEST(CommandSet, ShippedFileLoads) {
    const DeviceProfile p = profile();
    const CommandSet cs = load_command_set(kCommandsPath, p);

    EXPECT_EQ(cs.device, "SOLO PICO");
    EXPECT_EQ(cs.direction_object, "motor_direction");
    ASSERT_FALSE(cs.enable.empty());
    ASSERT_FALSE(cs.stop.empty());
    EXPECT_EQ(cs.stop.back().object, "drive_enable");  // disabling the drive is the last stop step
    EXPECT_EQ(cs.stop.back().value, 0);
    ASSERT_FALSE(cs.home.empty());

    const MotionModeDef* torque = cs.mode(MotionMode::Torque);
    const MotionModeDef* speed = cs.mode(MotionMode::Speed);
    const MotionModeDef* position = cs.mode(MotionMode::Position);
    ASSERT_TRUE(torque && speed && position);
    EXPECT_EQ(torque->reference, "torque_reference");
    EXPECT_EQ(speed->reference, "speed_reference");
    EXPECT_EQ(position->reference, "position_reference");
    EXPECT_TRUE(torque->uses_direction);
    EXPECT_FALSE(position->uses_direction);
    EXPECT_TRUE(position->reference_first);
    EXPECT_EQ(cs.mode(MotionMode::Stop), nullptr);
    EXPECT_EQ(cs.mode(MotionMode::Home), nullptr);

    // Each mode writes its control mode when it starts.
    for (const MotionModeDef* m : {torque, speed, position}) {
        ASSERT_FALSE(m->enter.empty()) << m->name;
        EXPECT_EQ(m->enter.front().object, "control_mode") << m->name;
    }

    // The legacy RUN is a valid setpoint of its mode.
    const MotionModeDef* run_mode = cs.mode(cs.legacy_run.mode);
    ASSERT_TRUE(run_mode);
    EXPECT_EQ(run_mode->clamp(cs.legacy_run.setpoint), cs.legacy_run.setpoint);
}

TEST(CommandSet, ClampAndScale) {
    MotionModeDef m;
    m.min = 0;
    m.max = 10000;
    m.scale = 0.001;
    EXPECT_EQ(m.clamp(-5), 0);
    EXPECT_EQ(m.clamp(5750), 5750);
    EXPECT_EQ(m.clamp(20000), 10000);
    EXPECT_DOUBLE_EQ(m.reference_value(5750), 5.75);
    EXPECT_DOUBLE_EQ(m.reference_value(20000), 10.0);
}

TEST(CommandSet, ValidMinimalFileParses) { EXPECT_EQ(error_of(valid()), ""); }

TEST(CommandSet, RejectsWrongDevice) {
    json j = valid();
    j["device"] = "OTHER DRIVE";
    EXPECT_NE(error_of(j).find("device profile is 'SOLO PICO'"), std::string::npos);
}

TEST(CommandSet, RejectsUnknownObjectsAndKeys) {
    json j = valid();
    j["modes"]["torque"]["reference"] = "no_such_object";
    j["enable"][0]["object"] = "also_missing";
    j["modes"]["torque"]["ramp_rte"] = 1;  // typo
    const std::string e = error_of(j);
    EXPECT_NE(e.find("unknown object 'no_such_object'"), std::string::npos) << e;
    EXPECT_NE(e.find("unknown object 'also_missing'"), std::string::npos) << e;
    EXPECT_NE(e.find("unknown key 'ramp_rte'"), std::string::npos) << e;
    EXPECT_NE(e.find("3 problem(s) in command set"), std::string::npos) << e;
}

TEST(CommandSet, RejectsUnknownMode) {
    json j = valid();
    j["modes"]["velocity"] = j["modes"]["torque"];
    EXPECT_NE(error_of(j).find("unknown mode 'velocity'"), std::string::npos);
}

TEST(CommandSet, RejectsBadLimits) {
    json j = valid();
    j["modes"]["torque"]["min"] = 100;
    j["modes"]["torque"]["max"] = 10;
    EXPECT_NE(error_of(j).find("'min' must be <= 'max'"), std::string::npos);

    // Integer object with a fractional scale: setpoints would not be writable.
    j = valid();
    j["modes"]["speed"] = {{"reference", "speed_reference"}, {"scale", 0.5}, {"min", 0}, {"max", 100}};
    EXPECT_NE(error_of(j).find("'scale' must be an integer"), std::string::npos);
}

TEST(CommandSet, RejectsLegacyRunOutsideLimits) {
    json j = valid();
    j["legacy_run"]["setpoint"] = 20000;
    EXPECT_NE(error_of(j).find("outside modes.torque"), std::string::npos);

    j = valid();
    j["legacy_run"]["mode"] = "speed";  // not defined in this file
    EXPECT_NE(error_of(j).find("'mode' must name a mode defined"), std::string::npos);

    j = valid();
    j["legacy_run"]["direction"] = "left";
    EXPECT_NE(error_of(j).find("'direction' must be"), std::string::npos);
}

TEST(CommandSet, DirectionRequiredOnlyWhenUsed) {
    json j = valid();
    j.erase("direction");
    EXPECT_NE(error_of(j).find("direction: missing"), std::string::npos);

    j["modes"]["torque"]["uses_direction"] = false;  // no mode needs it any more
    EXPECT_EQ(error_of(j), "");
}

TEST(CommandSet, HomeIsOptional) {
    json j = valid();
    ASSERT_FALSE(j.contains("home"));
    EXPECT_EQ(error_of(j), "");
}

TEST(CommandSet, DeviceProfileStillRejectsCommandKeys) {
    // The profile parser stays strict: commands must not creep back into solopico.json.
    std::string text;
    {
        std::ifstream in(kProfilePath);
        std::stringstream ss;
        ss << in.rdbuf();
        text = ss.str();
    }
    json p = json::parse(text, nullptr, true, true);
    p["modes"] = json::object();
    try {
        parse_device_profile(p.dump());
        FAIL() << "expected ProfileError";
    } catch (const ProfileError& e) {
        EXPECT_NE(std::string(e.what()).find("unknown key 'modes'"), std::string::npos);
    }
}

namespace {

std::vector<std::string> objects_of(const std::vector<WriteStep>& steps) {
    std::vector<std::string> out;
    for (const auto& s : steps) out.push_back(s.object);
    return out;
}

}  // namespace

TEST(CommandSet, TorqueStartSteps) {
    const CommandSet cs = load_command_set(kCommandsPath, profile());
    const auto steps = cs.start_steps(MotionMode::Torque, 5750, MotionDirection::Cw);
    EXPECT_EQ(objects_of(steps),
              (std::vector<std::string>{"control_mode", "motor_direction", "drive_enable", "torque_reference"}));
    EXPECT_EQ(steps[0].value, 1);           // torque control
    EXPECT_TRUE(steps[0].verify);
    EXPECT_EQ(steps[1].value, cs.direction_cw);
    EXPECT_DOUBLE_EQ(steps[3].value, 5.75);  // mA -> A
    EXPECT_DOUBLE_EQ(steps[3].ramp_rate, cs.mode(MotionMode::Torque)->ramp_rate);

    const auto ccw = cs.start_steps(MotionMode::Torque, 5750, MotionDirection::Ccw);
    EXPECT_EQ(ccw[1].value, cs.direction_ccw);
}

TEST(CommandSet, PositionStartStepsWriteTargetBeforeEnable) {
    const CommandSet cs = load_command_set(kCommandsPath, profile());
    const auto steps = cs.start_steps(MotionMode::Position, -4000, MotionDirection::Ccw);
    EXPECT_EQ(objects_of(steps),
              (std::vector<std::string>{"control_mode", "position_reference", "drive_enable"}));  // no direction
    EXPECT_EQ(steps[0].value, 2);
    EXPECT_EQ(steps[1].value, -4000);
    EXPECT_EQ(steps[1].ramp_rate, 0);
}

TEST(CommandSet, StartStepsClampTheSetpoint) {
    const CommandSet cs = load_command_set(kCommandsPath, profile());
    const MotionModeDef* speed = cs.mode(MotionMode::Speed);
    const auto steps = cs.start_steps(MotionMode::Speed, speed->max + 5000, MotionDirection::Cw);
    EXPECT_EQ(steps.back().object, "speed_reference");
    EXPECT_EQ(steps.back().value, speed->max);
    EXPECT_EQ(cs.reference_step(MotionMode::Speed, -10).value, speed->min);
}

TEST(CommandSet, StartStepsForUndefinedModeThrow) {
    json j = valid();  // torque only
    const CommandSet cs = parse_command_set(j.dump(), profile());
    EXPECT_THROW(cs.start_steps(MotionMode::Speed, 100, MotionDirection::Cw), ProfileError);
    EXPECT_THROW(cs.start_steps(MotionMode::Home, 0, MotionDirection::Cw), ProfileError);
}

// =============================================================== CommandManager

namespace {

// Translator + manager on a fake PICO, with the shipped files (ramps removed by default).
struct Rig {
    explicit Rig(Config c = pico_setup()) : cfg(std::move(c)), translator(make_translator(pico, cfg)),
                                            manager(*translator, cfg.cs) {}
    FakePico pico;
    Config cfg;
    std::unique_ptr<TestTranslator> translator;
    CommandManager manager;

    bool torque(int32_t ma, MotionDirection d = MotionDirection::Cw) {
        return manager.handle({MotionMode::Torque, ma, d});
    }
    bool speed(int32_t rpm, MotionDirection d = MotionDirection::Cw) {
        return manager.handle({MotionMode::Speed, rpm, d});
    }
    bool position(int32_t counts) { return manager.handle({MotionMode::Position, counts, MotionDirection::Cw}); }
    bool stop() { return manager.handle({MotionMode::Stop, 0, MotionDirection::Cw}); }
    bool home() { return manager.handle({MotionMode::Home, 0, MotionDirection::Cw}); }

    double q17(uint16_t index) { return decode_value(ValueType::Q17, pico.get(index)); }
    int32_t i32(uint16_t index) { return static_cast<int32_t>(pico.get(index)); }
};

// Indexes written, in order.
std::vector<uint16_t> indexes(const std::vector<SdoWrite>& w) {
    std::vector<uint16_t> out;
    for (const auto& x : w) out.push_back(x.index);
    return out;
}

// Position of the first write to 'index' with 'value', or -1.
int first_write(const std::vector<SdoWrite>& w, uint16_t index, uint32_t value) {
    for (size_t i = 0; i < w.size(); ++i)
        if (w[i].index == index && w[i].value == value) return static_cast<int>(i);
    return -1;
}

constexpr uint16_t kControlMode = 0x3016, kDriveEnable = 0x3008, kDirection = 0x300C, kTorque = 0x3004,
                   kSpeed = 0x3005, kPosition = 0x301B, kResetPosition = 0x301F;

}  // namespace

TEST(CommandManager, TorqueStartsFromStopped) {
    Rig r;
    ASSERT_TRUE(r.torque(2000));
    EXPECT_TRUE(r.translator->is_running());
    EXPECT_EQ(r.manager.active_mode(), MotionMode::Torque);
    EXPECT_EQ(r.pico.get(kControlMode), 1u);
    EXPECT_EQ(r.pico.get(kDirection), static_cast<uint32_t>(r.cfg.cs.direction_cw));
    EXPECT_EQ(r.pico.get(kDriveEnable), 1u);
    EXPECT_DOUBLE_EQ(r.q17(kTorque), 2.0);  // mA -> A
}

TEST(CommandManager, LegacyRunIsTheConfiguredTorqueRun) {
    Rig r;
    ASSERT_TRUE(r.manager.handle(r.cfg.cs.run_stop_command(true)));
    EXPECT_EQ(r.manager.active_mode(), r.cfg.cs.legacy_run.mode);
    EXPECT_DOUBLE_EQ(r.q17(kTorque), r.cfg.run_torque());
    ASSERT_TRUE(r.manager.handle(r.cfg.cs.run_stop_command(false)));
    EXPECT_FALSE(r.translator->is_running());
    EXPECT_EQ(r.pico.get(kDriveEnable), 0u);
}

TEST(CommandManager, SameModeAndDirectionIsALiveUpdate) {
    Rig r;
    ASSERT_TRUE(r.torque(2000));
    r.pico.clear_writes();
    ASSERT_TRUE(r.torque(3500));
    // Only the reference moves: no stop, no mode switch, no re-enable.
    for (uint16_t idx : indexes(r.pico.writes())) EXPECT_EQ(idx, kTorque);
    EXPECT_DOUBLE_EQ(r.q17(kTorque), 3.5);
    EXPECT_TRUE(r.translator->is_running());
}

TEST(CommandManager, DirectionChangeStopsFirst) {
    Rig r;
    ASSERT_TRUE(r.torque(2000, MotionDirection::Cw));
    r.pico.clear_writes();
    ASSERT_TRUE(r.torque(2000, MotionDirection::Ccw));
    const auto w = r.pico.writes();
    const int disabled = first_write(w, kDriveEnable, 0);
    const int new_dir = first_write(w, kDirection, static_cast<uint32_t>(r.cfg.cs.direction_ccw));
    const int enabled = first_write(w, kDriveEnable, 1);
    ASSERT_GE(disabled, 0);
    ASSERT_GE(new_dir, 0);
    ASSERT_GE(enabled, 0);
    EXPECT_LT(disabled, new_dir);  // never reverses while the drive is enabled
    EXPECT_LT(new_dir, enabled);
    EXPECT_EQ(r.pico.get(kDriveEnable), 1u);
}

TEST(CommandManager, ModeChangeStopsFirst) {
    Rig r;
    ASSERT_TRUE(r.torque(2000));
    r.pico.clear_writes();
    ASSERT_TRUE(r.speed(1500));
    const auto w = r.pico.writes();
    const int disabled = first_write(w, kDriveEnable, 0);
    const int speed_mode = first_write(w, kControlMode, 0);
    ASSERT_GE(disabled, 0);
    ASSERT_GE(speed_mode, 0);
    EXPECT_LT(disabled, speed_mode);  // control mode only switches with the drive off
    EXPECT_EQ(r.pico.get(kControlMode), 0u);
    EXPECT_EQ(r.i32(kSpeed), 1500);
    EXPECT_EQ(r.pico.get(kTorque), 0u);  // the stop sequence zeroed the torque reference
    EXPECT_EQ(r.manager.active_mode(), MotionMode::Speed);
}

TEST(CommandManager, PositionWritesTargetBeforeEnableAndIgnoresDirection) {
    Rig r;
    ASSERT_TRUE(r.position(-4000));
    const auto w = r.pico.writes();
    const int target = first_write(w, kPosition, static_cast<uint32_t>(-4000));
    const int enabled = first_write(w, kDriveEnable, 1);
    ASSERT_GE(target, 0);
    ASSERT_GE(enabled, 0);
    EXPECT_LT(target, enabled);
    EXPECT_FALSE(find_write(w, kDirection));
    EXPECT_EQ(r.pico.get(kControlMode), 2u);

    // CW and CCW are the same position command: a live update, not a restart.
    r.pico.clear_writes();
    ASSERT_TRUE(r.manager.handle({MotionMode::Position, 8000, MotionDirection::Ccw}));
    for (uint16_t idx : indexes(r.pico.writes())) EXPECT_EQ(idx, kPosition);
    EXPECT_EQ(r.i32(kPosition), 8000);
}

TEST(CommandManager, SetpointsAreClamped) {
    Rig r;
    const MotionModeDef* speed = r.cfg.cs.mode(MotionMode::Speed);
    ASSERT_TRUE(r.speed(speed->max + 10000));
    EXPECT_EQ(r.i32(kSpeed), speed->max);
    ASSERT_TRUE(r.speed(-50));  // live update below the minimum
    EXPECT_EQ(r.i32(kSpeed), speed->min);
}

TEST(CommandManager, HomeOnlyWhileStopped) {
    Rig r;
    ASSERT_TRUE(r.home());
    EXPECT_TRUE(find_write(r.pico.writes(), kResetPosition));
    EXPECT_FALSE(r.translator->is_running());

    ASSERT_TRUE(r.torque(2000));
    r.pico.clear_writes();
    EXPECT_FALSE(r.home());  // refused
    EXPECT_TRUE(r.pico.writes().empty());
    EXPECT_TRUE(r.translator->is_running());  // and the motor keeps running
}

TEST(CommandManager, StopAlwaysStops) {
    Rig r;
    ASSERT_TRUE(r.speed(1000));
    ASSERT_TRUE(r.stop());
    EXPECT_FALSE(r.translator->is_running());
    EXPECT_EQ(r.manager.active_mode(), std::nullopt);
    EXPECT_EQ(r.pico.get(kDriveEnable), 0u);
    EXPECT_TRUE(r.stop());  // STOP while stopped is harmless
}

TEST(CommandManager, UndefinedModeIsRefused) {
    Config c = pico_setup();
    c.cs.modes.erase(MotionMode::Speed);
    Rig r(c);
    EXPECT_FALSE(r.speed(1000));
    EXPECT_TRUE(r.pico.writes().empty());
}

TEST(CommandManager, FailedStartLeavesTheDriveStopped) {
    Rig r;
    ASSERT_TRUE(r.translator->configure());
    r.pico.reject_writes.insert(kDirection);  // PICO rejects a step half-way through the start
    EXPECT_FALSE(r.torque(2000));
    EXPECT_FALSE(r.translator->is_running());
    EXPECT_EQ(r.manager.active_mode(), std::nullopt);
    EXPECT_GE(first_write(r.pico.writes(), kDriveEnable, 0), 0);  // the stop sequence ran
    EXPECT_EQ(r.pico.get(kDriveEnable), 0u);
}

TEST(CommandManager, StoppedElsewhereMeansNextCommandRestarts) {
    Rig r;
    ASSERT_TRUE(r.torque(2000));
    ASSERT_TRUE(r.translator->stop());  // e.g. session end or shutdown, not via the manager
    r.pico.clear_writes();
    ASSERT_TRUE(r.torque(2000));  // same command again: must start, not "live update" a stopped drive
    EXPECT_TRUE(find_write(r.pico.writes(), kControlMode));
    EXPECT_EQ(r.pico.get(kDriveEnable), 1u);
}

TEST(CommandManager, RequestStopInterruptsARampAtOnce) {
    // What the gateway's command reader does when a STOP arrives during a long start ramp.
    Rig r(ramp_setup());
    std::atomic<bool> started{true};
    std::thread runner([&] { started = r.torque(2000); });  // 0 -> 2 A at 0.5 A/s: 4 s
    std::this_thread::sleep_for(500ms);
    const auto t0 = std::chrono::steady_clock::now();
    r.translator->request_stop();
    runner.join();
    EXPECT_LT(std::chrono::steady_clock::now() - t0, 3s);  // did not finish the ramp
    EXPECT_FALSE(started);
    ASSERT_TRUE(r.stop());  // the queued STOP
    EXPECT_FALSE(r.translator->is_running());
    EXPECT_EQ(r.pico.get(kDriveEnable), 0u);
    EXPECT_LT(r.q17(kTorque), 0.01);
}
