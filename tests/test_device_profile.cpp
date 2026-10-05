#include <gtest/gtest.h>

#include <string>

#include "canopen/device_profile.h"

using namespace wizard;

namespace {

// Smallest valid profile; tests patch pieces of it.
std::string minimal_profile(const std::string& extra_objects = "", const std::string& run = "[]",
                            const std::string& telemetry = "[]") {
    return R"({
      "name": "test", "node_id": 5,
      "objects": { "enable": {"index": "0x3008", "type": "u32"},
                   "fw": {"index": 12346, "type": "u32"} )" + extra_objects + R"( },
      "configure": [], "run": )" + run + R"(, "stop": [],
      "status": {"object": "enable", "running_value": 1},
      "alive_object": "fw",
      "device_info": {"vendor_id": {"value": "0x1234"}, "product_code": {"object": "fw"},
                      "revision": {"value": 0}, "serial": {"value": 0}},
      "telemetry": )" + telemetry + "}";
}

std::string error_of(const std::string& json) {
    try {
        parse_device_profile(json);
    } catch (const ProfileError& e) {
        return e.what();
    }
    return "";
}

}  // namespace

TEST(DeviceProfile, ShippedSoloPicoProfileLoads) {
    DeviceProfile p = load_device_profile(std::string(WIZARD_SOURCE_DIR) +
                                          "/device-gateway/canopen/devices/solopico.json");
    EXPECT_EQ(p.name, "SOLO PICO");
    EXPECT_EQ(p.node_id, 1);
    EXPECT_FALSE(p.sdo_size_indicated);
    EXPECT_EQ(p.sync_period_ms, 0u);  // SYNC off: it made the real PICO stop answering SDOs
    EXPECT_EQ(p.object("current_limit").index, 0x3003);
    EXPECT_EQ(p.object("current_limit").type, ValueType::Q17);
    EXPECT_EQ(p.object("tpdo_position_cobid").subindex, 1);

    // The skipped identification step and skipped Iq channel are not loaded.
    for (const auto& s : p.configure) EXPECT_NE(s.object, "motor_identification");
    ASSERT_EQ(p.telemetry.size(), 3u);  // skipped TPDO variants not loaded
    for (const auto& ch : p.telemetry) EXPECT_EQ(ch.source, TelemetrySource::SdoPoll);
    EXPECT_EQ(p.telemetry[0].object, "position_feedback");
    EXPECT_EQ(p.telemetry[1].kind, TelemetryKind::Velocity);
    EXPECT_DOUBLE_EQ(p.telemetry[2].scale, 1000.0);

    // RUN ramps the torque up, STOP ramps it down before disabling the drive.
    ASSERT_FALSE(p.run.empty());
    EXPECT_EQ(p.run.back().object, "torque_reference");
    EXPECT_DOUBLE_EQ(p.run.back().ramp_rate, 0.5);
    ASSERT_GE(p.stop.size(), 2u);
    EXPECT_EQ(p.stop.front().object, "torque_reference");
    EXPECT_DOUBLE_EQ(p.stop.front().ramp_rate, 0.5);
    EXPECT_EQ(p.stop.front().ramp_start_object, "dc_motor_current");
    EXPECT_EQ(p.stop.back().object, "drive_enable");
}

TEST(DeviceProfile, MinimalProfileParses) {
    DeviceProfile p = parse_device_profile(minimal_profile());
    EXPECT_EQ(p.node_id, 5);
    EXPECT_EQ(p.object("fw").index, 0x303A);        // decimal index accepted
    EXPECT_EQ(p.vendor_id.value, 0x1234u);          // hex string accepted
    EXPECT_EQ(*p.product_code.object, "fw");
    EXPECT_TRUE(p.sdo_size_indicated);              // default
}

TEST(DeviceProfile, HexStringStepValue) {
    DeviceProfile p = parse_device_profile(
        minimal_profile("", R"([{"object": "enable", "value": "0x80000281"}])"));
    ASSERT_EQ(p.run.size(), 1u);
    EXPECT_EQ(encode_value(ValueType::U32, p.run[0].value), 0x80000281u);
}

TEST(DeviceProfile, RampMsParsed) {
    DeviceProfile p = parse_device_profile(
        minimal_profile("", R"([{"object": "enable", "value": 1, "ramp_ms": 500}])"));
    ASSERT_EQ(p.run.size(), 1u);
    EXPECT_EQ(p.run[0].ramp_ms, 500u);
    EXPECT_THROW(parse_device_profile(
                     minimal_profile("", R"([{"object": "enable", "value": 1, "ramp_ms": 70000}])")),
                 ProfileError);
}

TEST(DeviceProfile, RampRateAndStartObject) {
    DeviceProfile p = parse_device_profile(minimal_profile(
        "", R"([{"object": "enable", "value": 1, "ramp_rate": 0.5, "ramp_start_object": "fw"}])"));
    EXPECT_DOUBLE_EQ(p.run[0].ramp_rate, 0.5);
    EXPECT_EQ(p.run[0].ramp_start_object, "fw");
    // both ramp kinds at once, a negative rate, or a start object without a ramp: errors
    EXPECT_THROW(parse_device_profile(minimal_profile(
                     "", R"([{"object": "enable", "value": 1, "ramp_ms": 10, "ramp_rate": 1}])")),
                 ProfileError);
    EXPECT_THROW(parse_device_profile(minimal_profile(
                     "", R"([{"object": "enable", "value": 1, "ramp_rate": -1}])")),
                 ProfileError);
    EXPECT_THROW(parse_device_profile(minimal_profile(
                     "", R"([{"object": "enable", "value": 1, "ramp_start_object": "fw"}])")),
                 ProfileError);
    EXPECT_THROW(parse_device_profile(minimal_profile(
                     "", R"([{"object": "enable", "value": 1, "ramp_rate": 1, "ramp_start_object": "nope"}])")),
                 ProfileError);
}

TEST(DeviceProfile, TypoInKeyIsAnError) {
    std::string json = minimal_profile();
    json.replace(json.find("\"alive_object\""), 14, "\"alive_objekt\"");
    const std::string err = error_of(json);
    EXPECT_NE(err.find("unknown key 'alive_objekt'"), std::string::npos) << err;
}

TEST(DeviceProfile, UnknownObjectInStep) {
    const std::string err = error_of(minimal_profile("", R"([{"object": "nope", "value": 1}])"));
    EXPECT_NE(err.find("run[0]: unknown object 'nope'"), std::string::npos) << err;
}

TEST(DeviceProfile, ValueMustFitObjectType) {
    const std::string err = error_of(minimal_profile(R"(, "small": {"index": 1, "type": "u8"})",
                                                     R"([{"object": "small", "value": 300}])"));
    EXPECT_NE(err.find("does not fit type u8"), std::string::npos) << err;
}

TEST(DeviceProfile, TelemetryChecks) {
    const std::string err = error_of(minimal_profile(
        "", "[]",
        R"([{"kind": "position", "source": "tpdo", "cob_id": "0x281", "type": "i32"},
            {"kind": "speed",    "source": "tpdo", "cob_id": "0x281", "type": "i32"},
            {"kind": "torque",   "source": "tpdo", "cob_id": "0x282", "type": "i32"},
            {"kind": "current",  "source": "sdo_poll", "object": "missing", "period_ms": 0}])"));
    EXPECT_NE(err.find("duplicate cob_id"), std::string::npos) << err;
    EXPECT_NE(err.find("'kind' must be"), std::string::npos) << err;
    EXPECT_NE(err.find("unknown object 'missing'"), std::string::npos) << err;
    EXPECT_NE(err.find("'period_ms' must be >= 1"), std::string::npos) << err;
}

TEST(DeviceProfile, WrongTypesAreReportedNotThrownRaw) {
    std::string json = minimal_profile();
    json.replace(json.find("\"test\""), 6, "42");  // name must be a string
    const std::string err = error_of(json);
    EXPECT_NE(err.find("'name' must be a string"), std::string::npos) << err;

    const std::string err2 = error_of(minimal_profile("", R"([{"object": "enable", "value": 1, "verify": "yes"}])"));
    EXPECT_NE(err2.find("'verify' must be true or false"), std::string::npos) << err2;
}

TEST(DeviceProfile, InvalidJson) {
    EXPECT_NE(error_of("{ not json").find("invalid JSON"), std::string::npos);
}

TEST(DeviceProfileValues, Q17MatchesSoloFixedPoint) {
    EXPECT_EQ(encode_value(ValueType::Q17, 1.5), 0x00030000u);  // 1.5 * 2^17
    EXPECT_EQ(encode_value(ValueType::Q17, 2.0), 0x00040000u);
    EXPECT_DOUBLE_EQ(decode_value(ValueType::Q17, 0x00030000u), 1.5);
    EXPECT_DOUBLE_EQ(decode_value(ValueType::Q17, encode_value(ValueType::Q17, -0.25)), -0.25);
}

TEST(DeviceProfileValues, IntegerTypes) {
    EXPECT_EQ(encode_value(ValueType::I32, -2), 0xFFFFFFFEu);
    EXPECT_DOUBLE_EQ(decode_value(ValueType::I16, 0x0000FFFFu), -1.0);
    EXPECT_DOUBLE_EQ(decode_value(ValueType::U8, 0x12345678u), 0x78);
    EXPECT_THROW(encode_value(ValueType::U8, 256), ProfileError);
    EXPECT_THROW(encode_value(ValueType::U32, -1), ProfileError);
    EXPECT_THROW(encode_value(ValueType::I32, 0.5), ProfileError);
    EXPECT_EQ(value_size(ValueType::U8), 1);
    EXPECT_EQ(value_size(ValueType::I16), 2);
    EXPECT_EQ(value_size(ValueType::Q17), 4);
}
