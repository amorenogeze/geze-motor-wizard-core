#pragma once

// Everything about UI commands in one place:
//   CommandSet      what each command does, loaded from commands/<device>_commands.json
//                   (object NAMES only; the device profile maps names to addresses)
//   CommandManager  which steps to run for a command, given what is running now
//
// Nothing here knows CANopen: the steps go to a DeviceTranslator.

#include <cstdint>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include "canopen/device_profile.h"
#include "device_translator.h"
#include "message.h"

namespace wizard {

// =============================================================== CommandSet

// What each UI command does, loaded from commands/<device>_commands.json.
// Object NAMES only ("torque_reference"): the device profile maps names to
// addresses, so the same command file works for any transport that provides
// the same object names.

// One motion mode of SetMotionCommand (0x43): torque, speed or position.
struct MotionModeDef {
    std::string name;               // "torque", "speed", "position"
    std::string unit;               // unit of the socket setpoint, for logs ("mA", "rpm", "counts")
    std::vector<WriteStep> enter;   // switches the drive into this mode (e.g. control_mode = 1)
    std::string reference;          // object that receives the setpoint
    double scale = 1.0;             // reference value = setpoint * scale (e.g. mA -> A: 0.001)
    int32_t min = 0;                // setpoint limits in socket units; out-of-range is clamped
    int32_t max = 0;
    double ramp_rate = 0.0;         // reference units per second; 0 = write the target directly
    bool uses_direction = true;     // write the direction object when entering the mode
    bool reference_first = false;   // write the reference before 'enable' (position: no move to a stale target)

    int32_t clamp(int32_t setpoint) const { return setpoint < min ? min : (setpoint > max ? max : setpoint); }
    double reference_value(int32_t setpoint) const { return clamp(setpoint) * scale; }
};

struct LegacyRun {  // what SetRunStopCommand (0x40) RUN means
    MotionMode mode = MotionMode::Torque;
    int32_t setpoint = 0;
    MotionDirection direction = MotionDirection::Cw;
};

struct CommandSet {
    std::string device;                    // must match DeviceProfile::name

    std::string direction_object;          // e.g. motor_direction
    double direction_cw = 1;
    double direction_ccw = 0;

    std::vector<WriteStep> enable;         // starts the drive (e.g. drive_enable = 1)
    std::vector<WriteStep> stop;           // always runs every step, even after a failure
    std::vector<WriteStep> home;           // current position becomes 0

    std::map<MotionMode, MotionModeDef> modes;  // only Torque / Speed / Position
    LegacyRun legacy_run;

    // nullptr if the mode is not defined in the file.
    const MotionModeDef* mode(MotionMode m) const;

    // The steps that start 'mode' from STOPPED, in order:
    //   enter steps -> direction (if the mode uses it) ->
    //   reference_first ? reference, enable : enable, reference (ramped at the mode's ramp_rate)
    // The setpoint is clamped to the mode's limits. Throws ProfileError if the
    // mode is not defined.
    std::vector<WriteStep> start_steps(MotionMode m, int32_t setpoint, MotionDirection direction) const;

    // The single step that moves a running mode's reference to a new setpoint
    // (live update), ramped at the mode's ramp_rate. Throws like start_steps.
    WriteStep reference_step(MotionMode m, int32_t setpoint) const;

    // SetRunStopCommand (0x40) as a SetMotionCommand: RUN = legacy_run, STOP = STOP.
    SetMotionPayload run_stop_command(bool run) const;
};

// Parse + validate against the device profile (every object must exist there,
// every value and limit must fit the object's type). Throws ProfileError
// listing every problem found.
CommandSet parse_command_set(const std::string& json_text, const DeviceProfile& device);
CommandSet load_command_set(const std::string& path, const DeviceProfile& device);


// =============================================================== CommandManager

// Turns UI commands into device sequences. Decides WHAT to do; the
// DeviceTranslator does it (object names -> addresses, ramps, STOP safety).
//
//   SetMotionCommand (0x43)
//     TORQUE / SPEED / POSITION while stopped        -> start the mode
//     same mode + direction while running            -> live update: only the reference moves (ramped)
//     other mode or direction while running          -> stop, then start the new mode
//     STOP                                           -> stop sequence
//     HOME                                           -> home steps; refused while running
//   SetRunStopCommand (0x40): converted first with CommandSet::run_stop_command
//     (RUN = legacy_run, STOP = STOP), then handled like the above
//
// The setpoint is clamped to the mode's limits. Direction is ignored for modes
// without uses_direction (position). One command at a time: calls are serialized.
class CommandManager {
public:
    CommandManager(DeviceTranslator& translator, const CommandSet& commands);

    // True if the command was carried out. The caller answers with the
    // device's run/stop status either way (RunStopStatusEvent).
    bool handle(const SetMotionPayload& cmd);

    // Mode currently running, if any (empty when stopped).
    std::optional<MotionMode> active_mode() const;

    // One-line description for logs, e.g. "SPEED 1500 rpm CCW".
    std::string describe(const SetMotionPayload& cmd) const;

private:
    struct Active {
        MotionMode mode;
        MotionDirection direction;
    };

    bool start_mode(MotionMode mode, int32_t setpoint, MotionDirection direction);
    bool stop_locked();

    DeviceTranslator& translator_;
    const CommandSet& commands_;
    mutable std::mutex mutex_;
    std::optional<Active> active_;  // valid only while translator_.is_running()
};

}  // namespace wizard
