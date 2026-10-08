#pragma once

// JSON helpers shared by the device profile (device_profile.cpp) and the
// command set (command_set.cpp) parsers. Internal to device-gateway: callers
// outside the parsers use load_device_profile / load_command_set instead.

#include <cstdint>
#include <optional>
#include <set>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "device_profile.h"

namespace wizard::profile_json {

using nlohmann::json;

// Collects every problem so one run of the gateway reports them all.
class Checker {
public:
    explicit Checker(std::string what = "device profile") : what_(std::move(what)) {}
    void error(const std::string& where, const std::string& what);
    bool ok() const { return errors_.empty(); }
    std::string report() const;

private:
    std::string what_;
    std::vector<std::string> errors_;
};

// Keys starting with '_' are comments. Any other unknown key is an error so
// that a typo ("perod_ms") does not silently fall back to a default.
void check_keys(const json& obj, const std::set<std::string>& allowed, const std::string& where, Checker& c);

// Integers may be written as JSON numbers or as "0x..." strings.
std::optional<uint64_t> to_uint(const json& v);
std::optional<uint64_t> get_uint(const json& obj, const std::string& key, const std::string& where, Checker& c,
                                 uint64_t max, bool required = true);
bool get_bool(const json& obj, const std::string& key, bool fallback, const std::string& where, Checker& c);

// Name of an object that must exist in the device profile.
std::string get_object_ref(const json& obj, const std::string& key, const std::string& where,
                           const DeviceProfile& p, Checker& c);

// A sequence of write steps (configure / run / stop / ...), array under root[key].
// Every object must exist in the device profile and every value must fit its type.
std::vector<WriteStep> parse_steps(const json& root, const std::string& key, const DeviceProfile& p, Checker& c,
                                   bool required = true);

}  // namespace wizard::profile_json
