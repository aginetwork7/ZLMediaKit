/*
 * Copyright (c) 2016-present The ZLMediaKit project authors. All Rights Reserved.
 *
 * This file is part of ZLMediaKit(https://github.com/ZLMediaKit/ZLMediaKit).
 *
 * Use of this source code is governed by MIT-like license that can be found in the
 * LICENSE file in the root of the source tree. All contributing project authors
 * may be found in the AUTHORS file in the root of the source tree.
 */

#ifndef ZLMEDIAKIT_MASKPROTOCOL_H
#define ZLMEDIAKIT_MASKPROTOCOL_H

#include <cstdint>
#include <functional>
#include <string>

namespace mediakit {

// media service <-> edge-mask 打码会话协议（以 edge-mask 仓库的 docs/socket_protocol.md 为准）：
// 每条消息 = 8 字节头 + 负载，整数一律小端序。
//   头：magic u16 = 0x4D45（"EM"）、version u8 = 1、type u8、length u32（负载字节数）
//   控制消息的负载是 JSON；FRAME / OUT_FRAME 的负载是 epoch u32、flags u32（bit0 关键帧）、pts i64（微秒）+ 压缩帧
// Session protocol between the media service and edge-mask (authoritative: edge-mask docs/socket_protocol.md).
// Each message = 8-byte header + payload, little-endian integers.
namespace MaskProto {

constexpr uint16_t kMagic = 0x4D45;
constexpr uint8_t kVersion = 1;
constexpr size_t kHeaderSize = 8;
constexpr size_t kMaxPayload = 16 * 1024 * 1024;
constexpr size_t kFrameFieldsSize = 16; // epoch u32 + flags u32 + pts_us i64
constexpr uint32_t kFlagKeyFrame = 0x1;

enum Type : uint8_t {
    kOpen = 0x01,      // -> 引擎：源流规格 + 打码参数 / -> engine: source spec + mask params
    kOpenAck = 0x02,   // <- 引擎：输出方案 / <- engine: output plan
    kBusy = 0x03,      // <- 引擎：拒绝（reason = busy / engine-unavailable / bad-request），随后断开
    kError = 0x04,     // <- 引擎：会话出错，随后断开 / <- engine: session failed, then disconnects
    kFrame = 0x10,     // -> 引擎：一个压缩帧 / -> engine: one access unit
    kOutFrame = 0x11,  // <- 引擎：打码后的压缩帧，带回输入的 epoch 和 pts / <- engine: masked access unit
    kFlush = 0x20,     // -> 引擎：seek，JSON {"epoch":N} / -> engine: seek
    kSpeed = 0x21,     // -> 引擎：改倍速，JSON {"speed":x,"epoch":N} / -> engine: speed change
    kSpeedAck = 0x22,  // <- 引擎：{"ok":true,...} / {"ok":false,"reason","detail"}
    kClose = 0x30,     // -> 引擎：结束会话 / -> engine: end of session
};

struct Message {
    uint8_t type = 0;
    std::string payload;
};

struct FrameInfo {
    uint32_t epoch = 0;
    uint32_t flags = 0;
    int64_t pts_us = 0;
    const char *data = nullptr;
    size_t size = 0;
    bool keyFrame() const { return (flags & kFlagKeyFrame) != 0; }
};

// 编码一条消息（头 + 负载）/ Encode one message (header + payload)
std::string encode(uint8_t type, const std::string &payload);
// 编码 FRAME / OUT_FRAME / Encode FRAME / OUT_FRAME
std::string encodeFrame(uint8_t type, uint32_t epoch, bool key_frame, int64_t pts_us, const char *data, size_t size);
// 解析 FRAME / OUT_FRAME 的负载；格式错误返回 false / Parse a FRAME / OUT_FRAME payload; false if malformed
bool parseFrame(const Message &msg, FrameInfo &out);

// 流式解码：喂入任意切分的字节，凑齐一条消息就回调一次；格式错误时返回 false，调用方应断开连接
// Streaming decoder: feed arbitrarily split bytes, one callback per complete message; returns false
// on a malformed stream, after which the caller must drop the connection
class Decoder {
public:
    using onMessage = std::function<void(const Message &msg)>;
    explicit Decoder(onMessage cb) : _cb(std::move(cb)) {}
    bool input(const char *data, size_t size);

private:
    std::string _buf;
    onMessage _cb;
};

} // namespace MaskProto
} // namespace mediakit

#endif // ZLMEDIAKIT_MASKPROTOCOL_H
