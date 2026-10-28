/*
 * Copyright (c) 2016-present The ZLMediaKit project authors. All Rights Reserved.
 *
 * This file is part of ZLMediaKit(https://github.com/ZLMediaKit/ZLMediaKit).
 *
 * Use of this source code is governed by MIT-like license that can be found in the
 * LICENSE file in the root of the source tree. All contributing project authors
 * may be found in the AUTHORS file in the root of the source tree.
 */

#include "MaskProtocol.h"

namespace mediakit {
namespace MaskProto {

namespace {

template <typename T>
void putLE(std::string &out, T v) {
    for (size_t i = 0; i < sizeof(T); ++i) {
        out.push_back(static_cast<char>((static_cast<uint64_t>(v) >> (8 * i)) & 0xFF));
    }
}

template <typename T>
T getLE(const char *p) {
    auto u = reinterpret_cast<const uint8_t *>(p);
    uint64_t v = 0;
    for (size_t i = 0; i < sizeof(T); ++i) {
        v |= static_cast<uint64_t>(u[i]) << (8 * i);
    }
    return static_cast<T>(v);
}

void putHeader(std::string &out, uint8_t type, size_t length) {
    putLE<uint16_t>(out, kMagic);
    out.push_back(static_cast<char>(kVersion));
    out.push_back(static_cast<char>(type));
    putLE<uint32_t>(out, static_cast<uint32_t>(length));
}

bool isFrame(uint8_t type) {
    return type == kFrame || type == kOutFrame;
}

} // namespace

std::string encode(uint8_t type, const std::string &payload) {
    std::string out;
    out.reserve(kHeaderSize + payload.size());
    putHeader(out, type, payload.size());
    out.append(payload);
    return out;
}

std::string encodeFrame(uint8_t type, uint32_t epoch, bool key_frame, int64_t pts_us, const char *data, size_t size) {
    std::string out;
    out.reserve(kHeaderSize + kFrameFieldsSize + size);
    putHeader(out, type, kFrameFieldsSize + size);
    putLE<uint32_t>(out, epoch);
    putLE<uint32_t>(out, key_frame ? kFlagKeyFrame : 0);
    putLE<int64_t>(out, pts_us);
    out.append(data, size);
    return out;
}

bool parseFrame(const Message &msg, FrameInfo &out) {
    if (!isFrame(msg.type) || msg.payload.size() < kFrameFieldsSize) {
        return false;
    }
    const char *p = msg.payload.data();
    out.epoch = getLE<uint32_t>(p);
    out.flags = getLE<uint32_t>(p + 4);
    out.pts_us = getLE<int64_t>(p + 8);
    out.data = p + kFrameFieldsSize;
    out.size = msg.payload.size() - kFrameFieldsSize;
    return true;
}

bool Decoder::input(const char *data, size_t size) {
    _buf.append(data, size);
    size_t offset = 0;
    while (_buf.size() - offset >= kHeaderSize) {
        const char *h = _buf.data() + offset;
        if (getLE<uint16_t>(h) != kMagic || static_cast<uint8_t>(h[2]) != kVersion) {
            return false;
        }
        size_t length = getLE<uint32_t>(h + 4);
        if (length > kMaxPayload) {
            return false;
        }
        if (_buf.size() - offset < kHeaderSize + length) {
            break;
        }
        Message msg;
        msg.type = static_cast<uint8_t>(h[3]);
        msg.payload.assign(h + kHeaderSize, length);
        offset += kHeaderSize + length;
        if (isFrame(msg.type) && length < kFrameFieldsSize) {
            return false;
        }
        _cb(msg);
    }
    _buf.erase(0, offset);
    return true;
}

} // namespace MaskProto
} // namespace mediakit
