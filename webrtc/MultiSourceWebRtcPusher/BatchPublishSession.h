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
#include <unordered_set>
#include <vector>

#include "json/json.h"
#include "Rtsp/RtspMediaSourceImp.h"
#include "webrtc/WebRtcTransport.h"

namespace mediakit {

// Naming: a batch session is published as app/<nvrId>; it holds slots (one negotiated audio/video
// m-line pair each, identified by slot_id = "0", "1", ...); each bound source (camera) is played as
// app/<sourceId>. The JSON of getInfo/getSummary and the DataChannel replies keep their wire names:
// "stream" is the nvrId, "streamId"/"streams"/"*StreamCount" refer to slots, "playStream" is the
// source's media stream.
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

    // Idle -> Bound on whip.bind, Bound -> Receiving on the first RTP of the bound source,
    // back to Idle on whip.unbind or when the session closes.
    enum class SlotState {
        Idle = 0,
        Bound,
        Receiving,
    };

    // One negotiated audio/video m-line pair.
    struct SlotContext {
        std::string slot_id;
        std::string audio_mid;
        std::string video_mid;
        uint32_t audio_ssrc = 0;
        uint32_t video_ssrc = 0;
        std::string current_source_id;
        // Set by RTP of the bound source; reported as receiving only while last_rtp_ms is recent.
        bool receiving = false;
        uint64_t last_rtp_ms = 0;
        SlotState state = SlotState::Idle;
    };

    // A source (camera) bound to a slot; viewers play it at play_app/play_stream (= source_id).
    struct SourceBinding {
        std::string source_id;
        std::string play_app;
        std::string play_stream;
        std::string slot_id;
        // WHEP viewer limit for this source; 0 means the default.
        size_t max_viewers = 0;
    };

    BatchPublishSession(std::string app, std::string nvr_id, std::string transport_id, size_t slot_count);

    // Listener of every source this session creates (the owning batch pusher).
    void setListener(std::weak_ptr<MediaSourceEvent> listener);

    // Builds the slots from the negotiated answer and moves the session to Active. Application
    // m-lines are skipped; the rest must be audio/video pairs in that order (audio, video, audio,
    // video, ...), and the pair count must equal slot_count. Slot i takes pair i and is named
    // "<i>"; each slot keeps both MIDs and the a=ssrc of each m-line (0 when absent). rtsp_sdp is
    // the SDP every source of the session is created with. Returns false, with the session left
    // Negotiated and no slots, on any layout error; the caller then rejects the offer.
    bool initFromSdp(const RtcSession &answer_sdp, const std::string &rtsp_sdp);

    // Binds source_id to a free slot, or returns the slot it already holds (reused=true; a repeated
    // bind refreshes max_viewers and lets a rejected source retry registration). Returns false with a
    // reason when the session is not active or no slot is free.
    bool bindSource(const std::string &source_id, size_t max_viewers, std::string &slot_id, bool &reused, std::string &reason);
    // Releases source_id's slot and unregisters its MediaSource. Idempotent: not_bound=true means
    // source_id was not bound, and slot_id is then left unchanged.
    void unbindSource(const std::string &source_id, std::string &slot_id, bool &not_bound);

    // Closes the session (state Closed, so every later bindSource fails) and releases every source
    // and slot; called when the session's transport is destroyed or replaced by a newer one.
    void closeAndReleaseAllSources();

    // Routes one RTP packet to the source bound on its slot. The slot is found by MID first, then
    // by the m-line's SSRC (learned from the first packet when the SDP had none). The first write
    // of a source registers its stream. Returns false, and drops the packet, when no slot matches,
    // the slot has no source, the source's registration was rejected (until it is bound again), or
    // the write throws (the source is then marked rejected). Holds the session lock for the whole
    // call, including the write: closeAndReleaseAllSources/unbindSource may run on another poller
    // and must not unregister a source while a packet is still being written to it.
    bool onRtp(TrackType type, const std::string &mid, uint32_t ssrc, uint64_t stamp_ms, const RtpPacket::Ptr &rtp);

    Json::Value getInfo() const;
    Json::Value getSummary() const;
    std::vector<std::string> getBoundSourceIds() const;
    bool getSourceBinding(const std::string &source_id, SourceBinding &binding) const;

    const std::string &getApp() const;
    const std::string &getNvrId() const;
    const std::string &getTransportId() const;

private:
    std::shared_ptr<RtspMediaSourceImp> getOrCreateSource(const std::string &source_id);
    void releaseSource(const std::string &source_id);
    SlotContext *findSlotByMid(const std::string &mid);
    SlotContext *findSlotBySsrc(uint32_t ssrc, TrackType type);
    SlotContext *findSlotById(const std::string &slot_id);
    Json::Value makeHeader() const;
    // Whether the slot got RTP of its bound source recently.
    static bool isReceiving(const SlotContext &ctx, uint64_t now_ms);
    bool allocFreeSlotId(std::string &slot_id);
    void releaseSlotId(const std::string &slot_id);
    static const char *sessionStateToStr(SessionState state);
    static const char *slotStateToStr(SlotState state);

private:
    std::string _app;
    std::string _nvr_id;
    std::string _transport_id;
    std::weak_ptr<MediaSourceEvent> _listener;
    size_t _slot_count = 0;
    std::string _rtsp_sdp;
    // Only the edge's VP8 payloads carry a key frame marker the GOP cache can use.
    bool _video_vp8 = false;
    SessionState _state = SessionState::Init;

    std::vector<SlotContext> _slots;
    std::deque<std::string> _free_slot_ids;
    std::unordered_map<std::string, SourceBinding> _source_bindings;
    std::unordered_map<uint32_t, std::string> _audio_ssrc_to_slot;
    std::unordered_map<uint32_t, std::string> _video_ssrc_to_slot;
    std::unordered_map<std::string, std::shared_ptr<RtspMediaSourceImp>> _source_map;
    // Sources whose stream could not be registered (the name is taken by another publisher); their
    // RTP is dropped until they are unbound, so one bad source never breaks the batch session.
    std::unordered_set<std::string> _rejected_sources;
    std::unordered_map<std::string, std::string> _mid_to_slot;

    mutable std::recursive_mutex _mtx;
};

} // namespace mediakit

#endif // ZLMEDIAKIT_BATCH_PUBLISH_SESSION_H
