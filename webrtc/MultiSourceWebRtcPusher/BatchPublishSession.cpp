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
#include "BatchPublishRules.h"

#include <algorithm>
#include <cctype>

#include "Common/MediaSource.h"
#include "Rtsp/Rtsp.h"
#include "Util/logger.h"
#include "Util/util.h"

using namespace std;

namespace mediakit {

// A slot that got no RTP of its source for this long is no longer reported as receiving.
static constexpr uint64_t kReceivingTimeoutMs = 5000;

BatchPublishSession::BatchPublishSession(string app, string nvr_id, string transport_id, size_t slot_count)
    : _app(std::move(app))
    , _nvr_id(std::move(nvr_id))
    , _transport_id(std::move(transport_id))
    , _slot_count(slot_count) {
    _state = SessionState::Init;
}

bool BatchPublishSession::initFromSdp(const RtcSession &answer_sdp, const string &rtsp_sdp) {
    lock_guard<recursive_mutex> lck(_mtx);
    _state = SessionState::Negotiated;
    _rtsp_sdp = rtsp_sdp;

    vector<batch::AvPair> av_pairs;
    auto layout_error = batch::collectAvPairs(answer_sdp, av_pairs);
    if (!layout_error.empty()) {
        WarnL << "batch sdp layout invalid, " << layout_error << ", app=" << _app << ", nvrId=" << _nvr_id
              << ", streamCount=" << _slot_count;
        return false;
    }
    for (auto &pair : av_pairs) {
        if (!pair.second->plan.empty()) {
            _video_vp8 = strcasecmp(pair.second->plan[0].codec.data(), "VP8") == 0;
        }
    }

    if (av_pairs.size() != _slot_count) {
        WarnL << "batch sdp pair count mismatch, app=" << _app << ", nvrId=" << _nvr_id
              << ", streamCount=" << _slot_count << ", avPairs=" << av_pairs.size();
        return false;
    }

    _slots.clear();
    _free_slot_ids.clear();
    _source_bindings.clear();
    _mid_to_slot.clear();
    _audio_ssrc_to_slot.clear();
    _video_ssrc_to_slot.clear();

    for (size_t i = 0; i < _slot_count; ++i) {
        SlotContext ctx;
        ctx.slot_id = to_string(i);
        ctx.audio_mid = av_pairs[i].first->mid;
        ctx.video_mid = av_pairs[i].second->mid;
        ctx.audio_ssrc = av_pairs[i].first->getRtpSSRC();
        ctx.video_ssrc = av_pairs[i].second->getRtpSSRC();
        ctx.state = SlotState::Idle;

        _mid_to_slot[ctx.audio_mid] = ctx.slot_id;
        _mid_to_slot[ctx.video_mid] = ctx.slot_id;
        if (ctx.audio_ssrc) {
            _audio_ssrc_to_slot[ctx.audio_ssrc] = ctx.slot_id;
        }
        if (ctx.video_ssrc) {
            _video_ssrc_to_slot[ctx.video_ssrc] = ctx.slot_id;
        }

        _slots.emplace_back(std::move(ctx));
        _free_slot_ids.emplace_back(_slots.back().slot_id);
    }

    _state = SessionState::Active;

    return true;
}

void BatchPublishSession::setListener(std::weak_ptr<MediaSourceEvent> listener) {
    lock_guard<recursive_mutex> lck(_mtx);
    _listener = std::move(listener);
}

bool BatchPublishSession::bindSource(const string &source_id, size_t max_viewers, string &slot_id, bool &reused, string &reason) {
    lock_guard<recursive_mutex> lck(_mtx);
    reused = false;

    if (_state != SessionState::Active) {
        reason = "sessionNotActive";
        return false;
    }

    auto it = _source_bindings.find(source_id);
    if (it != _source_bindings.end()) {
        reused = true;
        // A repeated bind refreshes the viewer limit, and lets a source whose stream registration was
        // rejected (e.g. the name was still held) try again on its next RTP.
        it->second.max_viewers = max_viewers;
        if (_rejected_sources.count(source_id)) {
            if (isNameHeldByOther(source_id)) {
                reason = "sourceIdInUse";
                return false;
            }
            _rejected_sources.erase(source_id);
            InfoL << "batch source retry after rejection, app=" << _app << ", nvrId=" << _nvr_id << ", sourceId=" << source_id;
        }
        slot_id = it->second.slot_id;
        reason = "reused";
        return true;
    }

    // Another publisher holds the name: the first RTP could not register the stream, so refuse now
    // instead of replying success to a source that would never play.
    if (isNameHeldByOther(source_id)) {
        reason = "sourceIdInUse";
        return false;
    }

    if (!allocFreeSlotId(slot_id)) {
        reason = "noAvailableStreamId";
        return false;
    }

    auto ctx = findSlotById(slot_id);
    if (!ctx) {
        reason = "stateReclaimed";
        return false;
    }

    ctx->current_source_id = source_id;
    ctx->state = SlotState::Bound;
    ctx->receiving = false;

    SourceBinding binding;
    binding.source_id = source_id;
    binding.play_app = _app;
    binding.play_stream = source_id;
    binding.slot_id = slot_id;
    binding.max_viewers = max_viewers;
    _source_bindings[source_id] = std::move(binding);

    getOrCreateSource(source_id);
    return true;
}

void BatchPublishSession::unbindSource(const string &source_id, string &slot_id, bool &not_bound) {
    lock_guard<recursive_mutex> lck(_mtx);
    not_bound = false;

    auto it = _source_bindings.find(source_id);
    if (it == _source_bindings.end()) {
        not_bound = true;
        return;
    }

    slot_id = it->second.slot_id;
    _source_bindings.erase(it);

    auto ctx = findSlotById(slot_id);
    if (ctx) {
        ctx->current_source_id.clear();
        ctx->receiving = false;
        ctx->state = SlotState::Idle;
    }

    releaseSlotId(slot_id);
    releaseSource(source_id);
}

void BatchPublishSession::closeAndReleaseAllSources() {
    lock_guard<recursive_mutex> lck(_mtx);
    _state = SessionState::Closing;
    _source_bindings.clear();
    while (!_source_map.empty()) {
        // Copy the key: releaseSource erases the entry it refers to.
        auto source_id = _source_map.begin()->first;
        releaseSource(source_id);
    }
    _rejected_sources.clear();
    _free_slot_ids.clear();
    for (auto &ctx : _slots) {
        ctx.current_source_id.clear();
        ctx.receiving = false;
        ctx.state = SlotState::Idle;
        _free_slot_ids.emplace_back(ctx.slot_id);
    }
    _state = SessionState::Closed;
}

bool BatchPublishSession::onRtp(TrackType type, const string &mid, uint32_t ssrc, uint64_t stamp_ms, const RtpPacket::Ptr &rtp) {
    lock_guard<recursive_mutex> lck(_mtx);

    SlotContext *ctx = nullptr;
    if (!mid.empty()) {
        ctx = findSlotByMid(mid);
    }
    if (!ctx && ssrc) {
        ctx = findSlotBySsrc(ssrc, type);
    }
    if (!ctx) {
        return false;
    }

    if (type == TrackAudio && !ctx->audio_ssrc && ssrc) {
        ctx->audio_ssrc = ssrc;
        _audio_ssrc_to_slot[ssrc] = ctx->slot_id;
    }
    if (type == TrackVideo && !ctx->video_ssrc && ssrc) {
        ctx->video_ssrc = ssrc;
        _video_ssrc_to_slot[ssrc] = ctx->slot_id;
    }

    // RTP on a slot with no (usable) source is dropped and does not count as receiving.
    if (ctx->current_source_id.empty() || _rejected_sources.count(ctx->current_source_id)) {
        return false;
    }

    ctx->last_rtp_ms = stamp_ms;
    ctx->receiving = true;
    ctx->state = SlotState::Receiving;

    auto src = getOrCreateSource(ctx->current_source_id);
    if (!src) {
        return false;
    }

    // Mark where an edge key frame starts so the GOP cache lets new WHEP viewers start from it.
    // The first write registers the stream; a failure only drops this source, the session goes on.
    try {
        auto key_pos = type == TrackVideo && _video_vp8 && batch::isEdgeKeyFrameStart(rtp->getPayload(), rtp->getPayloadSize());
        src->onWrite(rtp, key_pos);
    } catch (std::exception &ex) {
        WarnL << "batch source rejected, app=" << _app << ", nvrId=" << _nvr_id << ", sourceId=" << ctx->current_source_id << ": " << ex.what();
        _rejected_sources.emplace(ctx->current_source_id);
        _source_map.erase(ctx->current_source_id);
        return false;
    }
    return true;
}

// Fields shared by getInfo and getSummary; the caller holds _mtx.
Json::Value BatchPublishSession::makeHeader() const {
    Json::Value data;
    data["app"] = _app;
    data["stream"] = _nvr_id;
    data["transportId"] = _transport_id;
    data["streamCount"] = (Json::UInt64)_slot_count;
    data["state"] = sessionStateToStr(_state);
    data["freeStreamCount"] = (Json::UInt64)_free_slot_ids.size();
    return data;
}

Json::Value BatchPublishSession::getInfo() const {
    lock_guard<recursive_mutex> lck(_mtx);
    auto data = makeHeader();

    auto now_ms = toolkit::getCurrentMillisecond();
    Json::Value streams(Json::arrayValue);
    for (auto &ctx : _slots) {
        auto receiving = isReceiving(ctx, now_ms);
        Json::Value item;
        item["streamId"] = ctx.slot_id;
        item["audioMid"] = ctx.audio_mid;
        item["videoMid"] = ctx.video_mid;
        item["audioSsrc"] = (Json::UInt64)ctx.audio_ssrc;
        item["videoSsrc"] = (Json::UInt64)ctx.video_ssrc;
        item["sourceId"] = ctx.current_source_id;
        item["receiving"] = receiving;
        // A source that stopped sending is reported as bound until RTP comes again.
        item["state"] = slotStateToStr(ctx.state == SlotState::Receiving && !receiving ? SlotState::Bound : ctx.state);
        streams.append(item);
    }

    Json::Value sources(Json::arrayValue);
    for (auto &it : _source_bindings) {
        Json::Value item;
        item["sourceId"] = it.second.source_id;
        item["streamId"] = it.second.slot_id;
        item["playApp"] = it.second.play_app;
        item["playStream"] = it.second.play_stream;
        // The stream registration was rejected (the name was taken after the bind); RTP is dropped
        // until the source is bound again, so a rejected source is not online.
        auto rejected = _rejected_sources.count(it.second.source_id) > 0;
        item["online"] = !rejected;
        item["rejected"] = rejected;
        item["maxViewers"] = (Json::UInt64)it.second.max_viewers;
        sources.append(item);
    }

    Json::Value free_streams(Json::arrayValue);
    for (auto &id : _free_slot_ids) {
        free_streams.append(id);
    }

    data["streams"] = std::move(streams);
    data["sources"] = std::move(sources);
    data["freeStreams"] = std::move(free_streams);
    return data;
}

Json::Value BatchPublishSession::getSummary() const {
    lock_guard<recursive_mutex> lck(_mtx);
    auto data = makeHeader();

    Json::UInt64 bound_count = 0;
    Json::UInt64 receiving_count = 0;
    auto now_ms = toolkit::getCurrentMillisecond();
    for (const auto &stream : _slots) {
        if (stream.state == SlotState::Bound || stream.state == SlotState::Receiving) {
            ++bound_count;
        }
        if (isReceiving(stream, now_ms)) {
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

BatchRtspMediaSource::Ptr BatchPublishSession::getOrCreateSource(const string &source_id) {
    auto it = _source_map.find(source_id);
    if (it != _source_map.end()) {
        return it->second;
    }

    MediaTuple tuple;
    tuple.vhost = DEFAULT_VHOST;
    tuple.app = _app;
    tuple.stream = source_id;

    auto src = std::make_shared<BatchRtspMediaSource>(tuple);
    src->setListener(_listener);
    src->setSdp(_rtsp_sdp);
    _source_map[source_id] = src;
    InfoL << "batch source online, app=" << _app << ", nvrId=" << _nvr_id << ", sourceId=" << source_id;
    return src;
}

bool BatchPublishSession::isNameHeldByOther(const string &source_id) const {
    auto held = MediaSource::find(RTSP_SCHEMA, DEFAULT_VHOST, _app, source_id);
    if (!held) {
        return false;
    }
    auto it = _source_map.find(source_id);
    return it == _source_map.end() || it->second.get() != held.get();
}

// releaseSource unregisters the stream right away: other holders (such as a getMediaList call) may
// keep the object alive for a while, and the next session's stream of the same name must not collide.
void BatchPublishSession::releaseSource(const string &source_id) {
    _rejected_sources.erase(source_id);
    auto it = _source_map.find(source_id);
    if (it == _source_map.end()) {
        return;
    }
    auto src = std::move(it->second);
    _source_map.erase(it);
    src->release();
    InfoL << "batch source offline, app=" << _app << ", nvrId=" << _nvr_id << ", sourceId=" << source_id;
}

BatchPublishSession::SlotContext *BatchPublishSession::findSlotByMid(const string &mid) {
    auto it = _mid_to_slot.find(mid);
    if (it == _mid_to_slot.end()) {
        return nullptr;
    }
    return findSlotById(it->second);
}

// A slot id is the index of its slot in _slots (see initFromSdp).
BatchPublishSession::SlotContext *BatchPublishSession::findSlotById(const string &slot_id) {
    if (slot_id.empty() || slot_id.size() > 4 || !all_of(slot_id.begin(), slot_id.end(), [](unsigned char c) { return isdigit(c); })) {
        return nullptr;
    }
    auto index = (size_t)stoul(slot_id);
    if (index >= _slots.size() || _slots[index].slot_id != slot_id) {
        return nullptr;
    }
    return &_slots[index];
}

bool BatchPublishSession::isReceiving(const SlotContext &ctx, uint64_t now_ms) {
    return ctx.receiving && now_ms - ctx.last_rtp_ms < kReceivingTimeoutMs;
}

BatchPublishSession::SlotContext *BatchPublishSession::findSlotBySsrc(uint32_t ssrc, TrackType type) {
    const auto *map = type == TrackAudio ? &_audio_ssrc_to_slot : &_video_ssrc_to_slot;
    auto it = map->find(ssrc);
    if (it == map->end()) {
        return nullptr;
    }
    return findSlotById(it->second);
}

bool BatchPublishSession::allocFreeSlotId(string &slot_id) {
    if (_free_slot_ids.empty()) {
        return false;
    }
    slot_id = _free_slot_ids.front();
    _free_slot_ids.pop_front();
    return true;
}

void BatchPublishSession::releaseSlotId(const string &slot_id) {
    auto it = std::find(_free_slot_ids.begin(), _free_slot_ids.end(), slot_id);
    if (it == _free_slot_ids.end()) {
        _free_slot_ids.emplace_back(slot_id);
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

const char *BatchPublishSession::slotStateToStr(SlotState state) {
    switch (state) {
        case SlotState::Idle: return "Idle";
        case SlotState::Bound: return "Bound";
        case SlotState::Receiving: return "Receiving";
        default: return "Unknown";
    }
}

} // namespace mediakit
