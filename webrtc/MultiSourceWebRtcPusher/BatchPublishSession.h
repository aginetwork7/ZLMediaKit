/*
 * Copyright (c) 2016-present The ZLMediaKit project authors. All Rights Reserved.
 *
 * This file is part of ZLMediaKit(https://github.com/ZLMediaKit/ZLMediaKit).
 *
 * Use of this source code is governed by MIT-like license that can be found in the
 * LICENSE file in the root of the source tree. All contributing project authors
 * may be found in the AUTHORS file in the root of the source tree.
 */

#ifndef ZLMEDIAKIT_BATCH_PUBLISH_SESSION_H
#define ZLMEDIAKIT_BATCH_PUBLISH_SESSION_H

#include <map>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "json/json.h"
#include "Rtsp/RtspMediaSourceImp.h"
#include "webrtc/WebRtcTransport.h"

namespace mediakit {

class BatchPublishSession {
public:
    using Ptr = std::shared_ptr<BatchPublishSession>;

    enum class SessionState {
        Init = 0,
        Negotiated,
        Active,
        Closing,
        Closed,
    };

    enum class StreamState {
        Idle = 0,
        Bound,
        Receiving,
        Reclaiming,
    };

    struct StreamContext {
        std::string stream_id;
        std::string audio_mid;
        std::string video_mid;
        uint32_t audio_ssrc = 0;
        uint32_t video_ssrc = 0;
        std::string current_source_id;
        bool receiving = false;
        uint64_t last_rtp_ms = 0;
        StreamState state = StreamState::Idle;
    };

    struct SourceBinding {
        std::string source_id;
        std::string play_app;
        std::string play_stream;
        std::string stream_id;
        bool online = false;
    };

    BatchPublishSession(std::string app, std::string nvr_id, std::string transport_id, size_t stream_count);

    bool initFromSdp(const RtcSession &answer_sdp, const std::string &rtsp_sdp);

    bool bindSource(const std::string &source_id, std::string &stream_id, bool &reused, std::string &reason);
    bool unbindSource(const std::string &source_id, std::string &stream_id, bool &reused);

    void clearAllSource();

    bool onRtp(TrackType type, const std::string &mid, uint32_t ssrc, uint64_t stamp_ms, const RtpPacket::Ptr &rtp);

    Json::Value getInfo() const;
    Json::Value getSummary() const;
    std::vector<std::string> getBoundSourceIds() const;
    bool getSourceBinding(const std::string &source_id, SourceBinding &binding) const;

    const std::string &getApp() const;
    const std::string &getNvrId() const;
    const std::string &getTransportId() const;
    SessionState getState() const;

private:
    std::shared_ptr<RtspMediaSourceImp> getOrCreateSource(const std::string &source_id);
    StreamContext *findStreamByMid(const std::string &mid);
    StreamContext *findStreamBySsrc(uint32_t ssrc, TrackType type);
    StreamContext *findStreamById(const std::string &stream_id);
    bool allocFreeStreamId(std::string &stream_id);
    void releaseStreamId(const std::string &stream_id);
    static const char *sessionStateToStr(SessionState state);
    static const char *streamStateToStr(StreamState state);

private:
    std::string _app;
    std::string _nvr_id;
    std::string _transport_id;
    size_t _stream_count = 0;
    std::string _rtsp_sdp;
    SessionState _state = SessionState::Init;

    std::vector<StreamContext> _streams;
    std::deque<std::string> _free_stream_ids;
    std::unordered_map<std::string, SourceBinding> _source_bindings;
    std::unordered_map<uint32_t, std::string> _audio_ssrc_to_stream;
    std::unordered_map<uint32_t, std::string> _video_ssrc_to_stream;
    std::unordered_map<std::string, std::shared_ptr<RtspMediaSourceImp>> _source_map;
    std::unordered_map<std::string, std::string> _mid_to_stream;

    mutable std::recursive_mutex _mtx;
};

} // namespace mediakit

#endif // ZLMEDIAKIT_BATCH_PUBLISH_SESSION_H
