#include <gtest/gtest.h>

#include "message.h"

using namespace wizard;

TEST(EncodeFrame, HeaderIsTypeThenLittleEndianLength) {
    Message m;
    m.type = MessageType::Ping;
    m.payload = {};
    auto bytes = encode_frame(m);
    ASSERT_EQ(bytes.size(), kHeaderSize);
    EXPECT_EQ(bytes[0], static_cast<uint8_t>(MessageType::Ping));
    EXPECT_EQ(bytes[1], 0);
    EXPECT_EQ(bytes[2], 0);
}

TEST(MessageParserTest, ReturnsNulloptOnIncompleteHeader) {
    MessageParser parser;
    uint8_t partial[2] = {0x01, 0x00};
    parser.feed(partial, 2);
    EXPECT_FALSE(parser.try_parse().has_value());
}

TEST(MessageParserTest, ReturnsNulloptOnIncompletePayload) {
    MessageParser parser;
    Message m;
    m.type = MessageType::PositionEvent;
    m.payload = std::vector<uint8_t>(12, 0xAB);
    auto full = encode_frame(m);
    // Solo entregamos el header + parte del payload
    parser.feed(full.data(), kHeaderSize + 5);
    EXPECT_FALSE(parser.try_parse().has_value());
}

TEST(MessageParserTest, ParsesSingleCompleteFrame) {
    MessageParser parser;
    Message m;
    m.type = MessageType::Pong;
    m.payload = {0x01, 0x02, 0x03};
    auto full = encode_frame(m);
    parser.feed(full.data(), full.size());
    auto parsed = parser.try_parse();
    ASSERT_TRUE(parsed.has_value());
    EXPECT_EQ(*parsed, m);
}

TEST(MessageParserTest, ParsesTwoFramesFedTogether) {
    MessageParser parser;
    Message a{MessageType::Ping, {}};
    Message b{MessageType::Pong, {0x42}};
    auto encoded_a = encode_frame(a);
    auto encoded_b = encode_frame(b);
    parser.feed(encoded_a.data(), encoded_a.size());
    parser.feed(encoded_b.data(), encoded_b.size());

    auto first = parser.try_parse();
    ASSERT_TRUE(first.has_value());
    EXPECT_EQ(*first, a);

    auto second = parser.try_parse();
    ASSERT_TRUE(second.has_value());
    EXPECT_EQ(*second, b);

    EXPECT_FALSE(parser.try_parse().has_value());
}

TEST(MessageParserTest, HandlesFrameSplitAcrossMultipleFeeds) {
    MessageParser parser;
    Message m{MessageType::Ping, {0x11, 0x22, 0x33, 0x44}};
    auto full = encode_frame(m);

    // Alimentamos byte a byte para simular llegada fragmentada por el socket.
    for (size_t i = 0; i < full.size(); ++i) {
        parser.feed(&full[i], 1);
        if (i + 1 < full.size()) {
            EXPECT_FALSE(parser.try_parse().has_value());
        }
    }
    auto parsed = parser.try_parse();
    ASSERT_TRUE(parsed.has_value());
    EXPECT_EQ(*parsed, m);
}

TEST(PayloadCodec, PositionEventRoundTrip) {
    PositionEventPayload p{1'000'000ULL, -12345};
    auto m = make_position_event(p);
    auto parsed = parse_position_event(m);
    ASSERT_TRUE(parsed.has_value());
    EXPECT_EQ(parsed->timestamp_us, p.timestamp_us);
    EXPECT_EQ(parsed->position, p.position);
}

TEST(PayloadCodec, SpeedEventRoundTrip) {
    SpeedEventPayload p{2'000'000ULL, 6789};
    auto m = make_speed_event(p);
    auto parsed = parse_speed_event(m);
    ASSERT_TRUE(parsed.has_value());
    EXPECT_EQ(parsed->timestamp_us, p.timestamp_us);
    EXPECT_EQ(parsed->speed, p.speed);
}

TEST(PayloadCodec, CurrentEventRoundTrip) {
    CurrentEventPayload p{3'000'000ULL, -321};
    auto m = make_current_event(p);
    auto parsed = parse_current_event(m);
    ASSERT_TRUE(parsed.has_value());
    EXPECT_EQ(parsed->timestamp_us, p.timestamp_us);
    EXPECT_EQ(parsed->current, p.current);
}

TEST(PayloadCodec, DeviceInfoRequestHasEmptyPayload) {
    auto m = make_device_info_request();
    EXPECT_EQ(m.type, MessageType::DeviceInfoRequest);
    EXPECT_TRUE(m.payload.empty());
}

TEST(PayloadCodec, DeviceInfoResponseRoundTrip) {
    DeviceInfoResponsePayload p{0x1234, 0x5678, 0x0001, 0xDEADBEEF};
    auto m = make_device_info_response(p);
    auto parsed = parse_device_info_response(m);
    ASSERT_TRUE(parsed.has_value());
    EXPECT_EQ(parsed->vendor_id, p.vendor_id);
    EXPECT_EQ(parsed->product_code, p.product_code);
    EXPECT_EQ(parsed->revision, p.revision);
    EXPECT_EQ(parsed->serial, p.serial);
}

TEST(PayloadCodec, ParseRejectsWrongType) {
    Message m{MessageType::Ping, std::vector<uint8_t>(12, 0)};
    EXPECT_FALSE(parse_position_event(m).has_value());
}

TEST(PayloadCodec, ParseRejectsWrongSize) {
    Message m{MessageType::PositionEvent, std::vector<uint8_t>(5, 0)};
    EXPECT_FALSE(parse_position_event(m).has_value());
}

TEST(PayloadCodec, SetRunStopCommandRoundTripRun) {
    auto m = make_set_run_stop_command(true);
    EXPECT_EQ(m.type, MessageType::SetRunStopCommand);
    auto parsed = parse_set_run_stop_command(m);
    ASSERT_TRUE(parsed.has_value());
    EXPECT_TRUE(*parsed);
}

TEST(PayloadCodec, SetRunStopCommandRoundTripStop) {
    auto m = make_set_run_stop_command(false);
    auto parsed = parse_set_run_stop_command(m);
    ASSERT_TRUE(parsed.has_value());
    EXPECT_FALSE(*parsed);
}

TEST(PayloadCodec, RunStopStatusEventRoundTrip) {
    RunStopStatusPayload p{4'000'000ULL, true};
    auto m = make_run_stop_status_event(p);
    auto parsed = parse_run_stop_status_event(m);
    ASSERT_TRUE(parsed.has_value());
    EXPECT_EQ(parsed->timestamp_us, p.timestamp_us);
    EXPECT_EQ(parsed->running, p.running);
}

TEST(PayloadCodec, McuStatusEventRoundTrip) {
    auto m = make_mcu_status_event(true);
    auto parsed = parse_mcu_status_event(m);
    ASSERT_TRUE(parsed.has_value());
    EXPECT_TRUE(*parsed);

    auto m2 = make_mcu_status_event(false);
    auto parsed2 = parse_mcu_status_event(m2);
    ASSERT_TRUE(parsed2.has_value());
    EXPECT_FALSE(*parsed2);
}

TEST(Message, SetMotionCommandEncodesLikeThePythonUi) {
    // TORQUE 5750 mA CW -> struct.pack('<BiB', 1, 5750, 0) = 01 76 16 00 00 00
    Message m = make_set_motion_command({MotionMode::Torque, 5750, MotionDirection::Cw});
    EXPECT_EQ(m.type, MessageType::SetMotionCommand);
    EXPECT_EQ(m.payload, (std::vector<uint8_t>{0x01, 0x76, 0x16, 0x00, 0x00, 0x00}));
}

TEST(Message, SetMotionCommandRoundTrip) {
    auto p = parse_set_motion_command(make_set_motion_command({MotionMode::Position, -4000, MotionDirection::Cw}));
    ASSERT_TRUE(p);
    EXPECT_EQ(p->mode, MotionMode::Position);
    EXPECT_EQ(p->setpoint, -4000);
    EXPECT_EQ(p->direction, MotionDirection::Cw);

    // SPEED 1500 rpm CCW, bytes as sent by ui.py
    p = parse_set_motion_command(Message{MessageType::SetMotionCommand, {0x02, 0xDC, 0x05, 0x00, 0x00, 0x01}});
    ASSERT_TRUE(p);
    EXPECT_EQ(p->mode, MotionMode::Speed);
    EXPECT_EQ(p->setpoint, 1500);
    EXPECT_EQ(p->direction, MotionDirection::Ccw);
}

TEST(Message, SetMotionCommandRejectsBadPayloads) {
    EXPECT_FALSE(parse_set_motion_command(Message{MessageType::SetMotionCommand, {0x01, 0, 0, 0, 0}}));     // 5 bytes
    EXPECT_FALSE(parse_set_motion_command(Message{MessageType::SetMotionCommand, {0x05, 0, 0, 0, 0, 0}}));  // mode 5
    EXPECT_FALSE(parse_set_motion_command(Message{MessageType::SetMotionCommand, {0x01, 0, 0, 0, 0, 2}}));  // direction 2
    EXPECT_FALSE(parse_set_motion_command(Message{MessageType::SetRunStopCommand, {0x01, 0, 0, 0, 0, 0}})); // wrong type
}
