/*
 * Copyright (c) 2016-present The ZLMediaKit project authors. All Rights Reserved.
 *
 * This file is part of ZLMediaKit(https://github.com/ZLMediaKit/ZLMediaKit).
 *
 * Use of this source code is governed by MIT-like license that can be found in the
 * LICENSE file in the root of the source tree. All contributing project authors
 * may be found in the AUTHORS file in the root of the source tree.
 */

#ifndef ZLMEDIAKIT_BATCH_PUBLISH_RULES_H
#define ZLMEDIAKIT_BATCH_PUBLISH_RULES_H

#include <cstdint>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

#include "webrtc/Sdp.h"

// Input rules of WHIP Batch shared by the pusher and the batch session, kept apart so tests can
// call them directly.
namespace mediakit {
namespace batch {

// sourceId becomes the stream name app/sourceId that WHEP viewers play, so it must be one path
// segment that needs no URL encoding: no '/', '\\', "..", whitespace, control or URL-reserved
// characters, and at most kMaxSourceIdLen bytes. The edge's live and playback ids
// (<mac>-<ch>-<sub>[-<startAt>-<endAt>-<sessionId>]) pass.
constexpr size_t kMaxSourceIdLen = 256;

inline bool isValidSourceId(const std::string &source_id) {
    if (source_id.empty() || source_id.size() > kMaxSourceIdLen || source_id.find("..") != std::string::npos) {
        return false;
    }
    for (unsigned char ch : source_id) {
        if (ch <= 0x20 || ch == 0x7F || strchr("/\\?#&%\"'<>", ch)) {
            return false;
        }
    }
    return true;
}

using AvPair = std::pair<const RtcMedia *, const RtcMedia *>;

// Pairs the audio/video m-lines of a batch SDP: application m-lines are skipped, the rest must be
// audio, video, audio, video, ... Returns an empty string and fills pairs on success, otherwise
// the layout error. The offer check and the session's slot table both use it, so an offer that
// passes the check always builds the table.
inline std::string collectAvPairs(const RtcSession &sdp, std::vector<AvPair> &pairs) {
    pairs.clear();
    const RtcMedia *pending_audio = nullptr;
    for (auto &m : sdp.media) {
        if (m.type == TrackApplication) {
            continue;
        }
        if (!pending_audio) {
            if (m.type != TrackAudio) {
                return "expect audio then video";
            }
            pending_audio = &m;
            continue;
        }
        if (m.type != TrackVideo) {
            return "expect video after audio";
        }
        pairs.emplace_back(pending_audio, &m);
        pending_audio = nullptr;
    }
    if (pending_audio) {
        return "dangling audio track without video";
    }
    return "";
}

// The edge publishes its packed frames as VP8 RTP payloads: start code 00 00 00 01, then a ref byte
// with 0x20 set and a flag byte with both 0x20 and 0x80 set (& 0xA0 == 0xA0) on a key frame. Only the
// packet that starts a frame (S=1, PID=0) is checked. Encrypted payloads never set the bits, so they
// have no key frames. payload is the RTP payload, starting with the VP8 payload descriptor.
inline bool isEdgeKeyFrameStart(const uint8_t *payload, size_t size) {
    if (size < 1 || (payload[0] & 0x17) != 0x10) {
        return false;
    }
    size_t offset = 1;
    if (payload[0] & 0x80) { // X: extension byte follows
        if (size < 2) {
            return false;
        }
        auto ext = payload[1];
        offset = 2;
        if (ext & 0x80) { // I: PictureID, 2 bytes when its M bit is set
            if (size <= offset) {
                return false;
            }
            offset += (payload[offset] & 0x80) ? 2 : 1;
        }
        if (ext & 0x40) { // L: TL0PICIDX
            ++offset;
        }
        if (ext & 0x30) { // T/K: TID/KEYIDX
            ++offset;
        }
    }
    if (size < offset + 6) {
        return false;
    }
    auto frame = payload + offset;
    return frame[0] == 0 && frame[1] == 0 && frame[2] == 0 && frame[3] == 1 && (frame[4] & 0x20) && (frame[5] & 0xA0) == 0xA0;
}

} // namespace batch
} // namespace mediakit

#endif // ZLMEDIAKIT_BATCH_PUBLISH_RULES_H
