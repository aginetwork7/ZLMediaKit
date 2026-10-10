/*
 * Copyright (c) 2016-present The ZLMediaKit project authors. All Rights Reserved.
 *
 * This file is part of ZLMediaKit(https://github.com/ZLMediaKit/ZLMediaKit).
 *
 * Use of this source code is governed by MIT-like license that can be found in the
 * LICENSE file in the root of the source tree. All contributing project authors
 * may be found in the AUTHORS file in the root of the source tree.
 */

#include "BatchPublishSession.h"

#include <algorithm>

#include "Common/MediaSource.h"
#include "Rtsp/Rtsp.h"
#include "Util/logger.h"

using namespace std;

namespace mediakit {

BatchPublishSession::BatchPublishSession(string app, string nvr_id, string transport_id, size_t stream_count)
    : _app(std::move(app))
    , _nvr_id(std::move(nvr_id))
    , _transport_id(std::move(transport_id))
    , _stream_count(stream_count) {
    _state = SessionState::Init;
}

bool BatchPublishSession::initFromSdp(const RtcSession &answer_sdp, const string &rtsp_sdp) {
    lock_guard<recursive_mutex> lck(_mtx);
    _state = SessionState::Negotiated;
    _rtsp_sdp = rtsp_sdp;

    vector<pair<const RtcMedia *, const RtcMedia *>> av_pairs;
    av_pairs.reserve(_stream_count);

    const RtcMedia *pending_audio = nullptr;
    for (auto &m : answer_sdp.media) {
        if (m.type == TrackApplication) {
            continue;
        }

        if (!pending_audio) {
            if (m.type != TrackAudio) {
                WarnL << "batch sdp layout invalid, expect audio then video, app=" << _app
                      << ", nvrId=" << _nvr_id << ", streamCount=" << _stream_count;
                return false;
            }
            pending_audio = &m;
            continue;
        }

        if (m.type != TrackVideo) {
            WarnL << "batch sdp layout invalid, expect video after audio, app=" << _app
                  << ", nvrId=" << _nvr_id << ", streamCount=" << _stream_count;
            return false;
        }

        av_pairs.emplace_back(pending_audio, &m);
        pending_audio = nullptr;
    }

    if (pending_audio) {
        WarnL << "batch sdp layout invalid, dangling audio track without video, app=" << _app
              << ", nvrId=" << _nvr_id << ", streamCount=" << _stream_count;
        return false;
    }

    if (av_pairs.size() != _stream_count) {
        WarnL << "batch sdp pair count mismatch, app=" << _app << ", nvrId=" << _nvr_id
              << ", streamCount=" << _stream_count << ", avPairs=" << av_pairs.size();
        return false;
    }

    _streams.clear();
    _free_stream_ids.clear();
    _source_bindings.clear();
    _mid_to_stream.clear();
    _audio_ssrc_to_stream.clear();
    _video_ssrc_to_stream.clear();

    for (size_t i = 0; i < _stream_count; ++i) {
        StreamContext ctx;
        ctx.stream_id = to_string(i);
        ctx.audio_mid = av_pairs[i].first->mid;
        ctx.video_mid = av_pairs[i].second->mid;
        ctx.audio_ssrc = av_pairs[i].first->getRtpSSRC();
        ctx.video_ssrc = av_pairs[i].second->getRtpSSRC();
        ctx.state = StreamState::Idle;

        _mid_to_stream[ctx.audio_mid] = ctx.stream_id;
        _mid_to_stream[ctx.video_mid] = ctx.stream_id;
        if (ctx.audio_ssrc) {
            _audio_ssrc_to_stream[ctx.audio_ssrc] = ctx.stream_id;
        }
        if (ctx.video_ssrc) {
            _video_ssrc_to_stream[ctx.video_ssrc] = ctx.stream_id;
        }

        _streams.emplace_back(std::move(ctx));
        _free_stream_ids.emplace_back(_streams.back().stream_id);
    }

    _state = SessionState::Active;

    return true;
}

bool BatchPublishSession::bindSource(const string &source_id, string &stream_id, bool &reused, string &reason) {
    lock_guard<recursive_mutex> lck(_mtx);
    reused = false;

    if (_state != SessionState::Active) {
        reason = "sessionNotActive";
        return false;
    }

    auto it = _source_bindings.find(source_id);
    if (it != _source_bindings.end()) {
        reused = true;
        stream_id = it->second.stream_id;
        reason = "reused";
        return true;
    }

    if (!allocFreeStreamId(stream_id)) {
        reason = "noAvailableStreamId";
        return false;
    }

    auto ctx = findStreamById(stream_id);
    if (!ctx) {
        reason = "stateReclaimed";
        return false;
    }

    ctx->current_source_id = source_id;
    ctx->state = StreamState::Bound;
    ctx->receiving = false;

    SourceBinding binding;
    binding.source_id = source_id;
    binding.play_app = _app;
    binding.play_stream = source_id;
    binding.stream_id = stream_id;
    binding.online = true;
    _source_bindings[source_id] = std::move(binding);

    getOrCreateSource(source_id);
    return true;
}

bool BatchPublishSession::unbindSource(const string &source_id, string &stream_id, bool &reused) {
    lock_guard<recursive_mutex> lck(_mtx);
    reused = false;

    auto it = _source_bindings.find(source_id);
    if (it == _source_bindings.end()) {
        reused = true;
        return true;
    }

    stream_id = it->second.stream_id;
    _source_bindings.erase(it);

    auto ctx = findStreamById(stream_id);
    if (ctx) {
        ctx->state = StreamState::Reclaiming;
        ctx->current_source_id.clear();
        ctx->receiving = false;
        ctx->state = StreamState::Idle;
    }

    releaseStreamId(stream_id);

    _source_map.erase(source_id);
    return true;
}

void BatchPublishSession::clearAllSource() {
    lock_guard<recursive_mutex> lck(_mtx);
    _state = SessionState::Closing;
    _source_bindings.clear();
    _source_map.clear();
    _free_stream_ids.clear();
    for (auto &ctx : _streams) {
        ctx.current_source_id.clear();
        ctx.receiving = false;
        ctx.state = StreamState::Idle;
        _free_stream_ids.emplace_back(ctx.stream_id);
    }
    _state = SessionState::Closed;
}

bool BatchPublishSession::onRtp(TrackType type, const string &mid, uint32_t ssrc, uint64_t stamp_ms, const RtpPacket::Ptr &rtp) {
    lock_guard<recursive_mutex> lck(_mtx);

    StreamContext *ctx = nullptr;
    if (!mid.empty()) {
        ctx = findStreamByMid(mid);
    }
    if (!ctx && ssrc) {
        ctx = findStreamBySsrc(ssrc, type);
    }
    if (!ctx) {
        return false;
    }

    if (type == TrackAudio && !ctx->audio_ssrc && ssrc) {
        ctx->audio_ssrc = ssrc;
        _audio_ssrc_to_stream[ssrc] = ctx->stream_id;
    }
    if (type == TrackVideo && !ctx->video_ssrc && ssrc) {
        ctx->video_ssrc = ssrc;
        _video_ssrc_to_stream[ssrc] = ctx->stream_id;
    }

    ctx->last_rtp_ms = stamp_ms;
    ctx->receiving = true;
    ctx->state = StreamState::Receiving;

    if (ctx->current_source_id.empty()) {
        return false;
    }

    auto src = getOrCreateSource(ctx->current_source_id);
    if (!src) {
        return false;
    }

    src->onWrite(rtp, false);
    return true;
}

Json::Value BatchPublishSession::getInfo() const {
    lock_guard<recursive_mutex> lck(_mtx);
    Json::Value data;
    data["app"] = _app;
    data["stream"] = _nvr_id;
    data["transportId"] = _transport_id;
    data["streamCount"] = (Json::UInt64)_stream_count;
    data["state"] = sessionStateToStr(_state);
    data["freeStreamCount"] = (Json::UInt64)_free_stream_ids.size();

    Json::Value streams(Json::arrayValue);
    for (auto &ctx : _streams) {
        Json::Value item;
        item["streamId"] = ctx.stream_id;
        item["audioMid"] = ctx.audio_mid;
        item["videoMid"] = ctx.video_mid;
        item["audioSsrc"] = (Json::UInt64)ctx.audio_ssrc;
        item["videoSsrc"] = (Json::UInt64)ctx.video_ssrc;
        item["sourceId"] = ctx.current_source_id;
        item["receiving"] = ctx.receiving;
        item["state"] = streamStateToStr(ctx.state);
        streams.append(item);
    }

    Json::Value sources(Json::arrayValue);
    for (auto &it : _source_bindings) {
        Json::Value item;
        item["sourceId"] = it.second.source_id;
        item["streamId"] = it.second.stream_id;
        item["playApp"] = it.second.play_app;
        item["playStream"] = it.second.play_stream;
        item["online"] = it.second.online;
        sources.append(item);
    }

    Json::Value free_streams(Json::arrayValue);
    for (auto &id : _free_stream_ids) {
        free_streams.append(id);
    }

    data["streams"] = std::move(streams);
    data["sources"] = std::move(sources);
    data["freeStreams"] = std::move(free_streams);
    return data;
}

Json::Value BatchPublishSession::getSummary() const {
    lock_guard<recursive_mutex> lck(_mtx);
    Json::Value data;
    data["app"] = _app;
    data["stream"] = _nvr_id;
    data["transportId"] = _transport_id;
    data["streamCount"] = (Json::UInt64)_stream_count;
    data["state"] = sessionStateToStr(_state);
    data["freeStreamCount"] = (Json::UInt64)_free_stream_ids.size();

    Json::UInt64 bound_count = 0;
    Json::UInt64 receiving_count = 0;
    for (const auto &stream : _streams) {
        if (stream.state == StreamState::Bound || stream.state == StreamState::Receiving) {
            ++bound_count;
        }
        if (stream.receiving) {
            ++receiving_count;
        }
    }
    data["boundStreamCount"] = bound_count;
    data["receivingStreamCount"] = receiving_count;
    return data;
}

vector<string> BatchPublishSession::getBoundSourceIds() const {
    lock_guard<recursive_mutex> lck(_mtx);
    vector<string> source_ids;
    source_ids.reserve(_source_bindings.size());
    for (const auto &binding : _source_bindings) {
        source_ids.emplace_back(binding.first);
    }
    return source_ids;
}

bool BatchPublishSession::getSourceBinding(const string &source_id, SourceBinding &binding) const {
    lock_guard<recursive_mutex> lck(_mtx);
    auto it = _source_bindings.find(source_id);
    if (it == _source_bindings.end()) {
        return false;
    }
    binding = it->second;
    return true;
}

const string &BatchPublishSession::getApp() const {
    return _app;
}

const string &BatchPublishSession::getNvrId() const {
    return _nvr_id;
}

const string &BatchPublishSession::getTransportId() const {
    return _transport_id;
}

BatchPublishSession::SessionState BatchPublishSession::getState() const {
    return _state;
}

shared_ptr<RtspMediaSourceImp> BatchPublishSession::getOrCreateSource(const string &source_id) {
    auto it = _source_map.find(source_id);
    if (it != _source_map.end()) {
        return it->second;
    }

    MediaTuple tuple;
    tuple.vhost = DEFAULT_VHOST;
    tuple.app = _app;
    tuple.stream = source_id;

    auto src = std::make_shared<RtspMediaSourceImp>(tuple);
    src->setSdp(_rtsp_sdp);
    _source_map[source_id] = src;
    InfoL << "batch source online, app=" << _app << ", nvrId=" << _nvr_id << ", sourceId=" << source_id;
    return src;
}

BatchPublishSession::StreamContext *BatchPublishSession::findStreamByMid(const string &mid) {
    auto it = _mid_to_stream.find(mid);
    if (it == _mid_to_stream.end()) {
        return nullptr;
    }
    auto &stream_id = it->second;
    for (auto &ctx : _streams) {
        if (ctx.stream_id == stream_id) {
            return &ctx;
        }
    }
    return nullptr;
}

BatchPublishSession::StreamContext *BatchPublishSession::findStreamById(const string &stream_id) {
    for (auto &ctx : _streams) {
        if (ctx.stream_id == stream_id) {
            return &ctx;
        }
    }
    return nullptr;
}

BatchPublishSession::StreamContext *BatchPublishSession::findStreamBySsrc(uint32_t ssrc, TrackType type) {
    const auto *map = type == TrackAudio ? &_audio_ssrc_to_stream : &_video_ssrc_to_stream;
    auto it = map->find(ssrc);
    if (it == map->end()) {
        return nullptr;
    }
    auto &stream_id = it->second;
    for (auto &ctx : _streams) {
        if (ctx.stream_id == stream_id) {
            return &ctx;
        }
    }
    return nullptr;
}

bool BatchPublishSession::allocFreeStreamId(string &stream_id) {
    if (_free_stream_ids.empty()) {
        return false;
    }
    stream_id = _free_stream_ids.front();
    _free_stream_ids.pop_front();
    return true;
}

void BatchPublishSession::releaseStreamId(const string &stream_id) {
    auto it = std::find(_free_stream_ids.begin(), _free_stream_ids.end(), stream_id);
    if (it == _free_stream_ids.end()) {
        _free_stream_ids.emplace_back(stream_id);
    }
}

const char *BatchPublishSession::sessionStateToStr(SessionState state) {
    switch (state) {
        case SessionState::Init: return "Init";
        case SessionState::Negotiated: return "Negotiated";
        case SessionState::Active: return "Active";
        case SessionState::Closing: return "Closing";
        case SessionState::Closed: return "Closed";
        default: return "Unknown";
    }
}

const char *BatchPublishSession::streamStateToStr(StreamState state) {
    switch (state) {
        case StreamState::Idle: return "Idle";
        case StreamState::Bound: return "Bound";
        case StreamState::Receiving: return "Receiving";
        case StreamState::Reclaiming: return "Reclaiming";
        default: return "Unknown";
    }
}

} // namespace mediakit
