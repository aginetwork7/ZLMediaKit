/*
 * Copyright (c) 2016-present The ZLMediaKit project authors. All Rights Reserved.
 *
 * This file is part of ZLMediaKit(https://github.com/ZLMediaKit/ZLMediaKit).
 *
 * Use of this source code is governed by MIT-like license that can be found in the
 * LICENSE file in the root of the source tree. All contributing project authors
 * may be found in the AUTHORS file in the root of the source tree.
 */

#include "MultiSourceWebRtcPusher.h"

#include "Util/logger.h"

using namespace std;
using namespace toolkit;

namespace mediakit {

// DataChannel RPC between the edge and this pusher (whip.bind / whip.unbind requests and their
// replies); the contract is in the PushPull design doc, zlm_whip_batch_design.md section 5.
namespace {
constexpr int kCodeSuccess = 0;
constexpr int kCodeOtherFailed = -1;
constexpr int kCodeInvalidArgs = -300;
constexpr int kCodeException = -400;
constexpr int kCodeNotFound = -500;
}

MultiSourceWebRtcPusher::Ptr MultiSourceWebRtcPusher::create(const EventPoller::Ptr &poller,
                                                             const MediaInfo &info,
                                                             const ProtocolOption &option,
                                                             WebRtcTransport::Role role,
                                                             WebRtcTransport::SignalingProtocols signaling_protocols) {
    MultiSourceWebRtcPusher::Ptr pusher(new MultiSourceWebRtcPusher(poller, info, option), [](MultiSourceWebRtcPusher *ptr) {
        ptr->onDestory();
        delete ptr;
    });

    pusher->setRole(role);
    pusher->setSignalingProtocols(signaling_protocols);
    pusher->onCreate();
    return pusher;
}

MultiSourceWebRtcPusher::MultiSourceWebRtcPusher(const EventPoller::Ptr &poller,
                                                 const MediaInfo &info,
                                                 const ProtocolOption &option)
    : WebRtcTransportImp(poller)
    , _media_info(info) {
    // ProtocolOption does not apply to the batch's sources: each one is a plain RtspMediaSource.
}

const MediaInfo &MultiSourceWebRtcPusher::getMediaInfo() const {
    return _media_info;
}

void MultiSourceWebRtcPusher::onStartWebRTC() {
    WebRtcTransportImp::onStartWebRTC();

    if (!canRecvRtp()) {
        onShutdown(SockException(Err_other, "batch pusher direction is invalid"));
        return;
    }

    _batch_session = std::make_shared<BatchPublishSession>(_media_info.app, _media_info.stream, getIdentifier(), _stream_count);
    _batch_session->setListener(std::static_pointer_cast<MultiSourceWebRtcPusher>(shared_from_this()));
    if (!_batch_session->initFromSdp(*_answer_sdp, _answer_sdp->toRtspSdp())) {
        onShutdown(SockException(Err_other, "batch stream table init failed"));
        return;
    }

    if (auto old = BatchPublishSessionManager::Instance().replaceSession(_batch_session)) {
        WarnL << "batch session replaced, app=" << _media_info.app << ", nvrId=" << _media_info.stream
              << ", oldTransportId=" << old->getTransportId() << ", newTransportId=" << getIdentifier();
        if (auto old_transport = WebRtcTransportManager::Instance().getItem(old->getTransportId())) {
            old_transport->safeShutdown(SockException(Err_shutdown, "replaced by new batch session"));
        }
    }
    InfoL << "batch session created, app=" << _media_info.app << ", nvrId=" << _media_info.stream
          << ", streamCount=" << _stream_count << ", transportId=" << getIdentifier();
}

bool MultiSourceWebRtcPusher::close(MediaSource &sender) {
    return false;
}

int MultiSourceWebRtcPusher::totalReaderCount(MediaSource &sender) {
    return sender.readerCount();
}

MediaOriginType MultiSourceWebRtcPusher::getOriginType(MediaSource &sender) const {
    return MediaOriginType::rtc_push;
}

string MultiSourceWebRtcPusher::getOriginUrl(MediaSource &sender) const {
    return _media_info.full_url;
}

std::shared_ptr<SockInfo> MultiSourceWebRtcPusher::getOriginSock(MediaSource &sender) const {
    return static_pointer_cast<SockInfo>(getSession());
}

toolkit::EventPoller::Ptr MultiSourceWebRtcPusher::getOwnerPoller(MediaSource &sender) {
    return getPoller();
}

float MultiSourceWebRtcPusher::getLossRate(MediaSource &sender, TrackType type) {
    return WebRtcTransportImp::getLossRate(type);
}

void MultiSourceWebRtcPusher::onDestory() {
    if (_batch_session) {
        _batch_session->clearAllSources();
        BatchPublishSessionManager::Instance().removeSession(_batch_session->getApp(), _batch_session->getNvrId(), _batch_session);
        InfoL << "batch session closed, app=" << _batch_session->getApp() << ", nvrId=" << _batch_session->getNvrId()
              << ", transportId=" << getIdentifier();
        _batch_session = nullptr;
    }
    WebRtcTransportImp::onDestory();
}

void MultiSourceWebRtcPusher::onCheckSdp(SdpType type, RtcSession &sdp) {
    if (type != SdpType::offer) {
        WebRtcTransportImp::onCheckSdp(type, sdp);
        return;
    }

    size_t av_count = 0;
    bool expect_audio = true;
    for (auto &m : sdp.media) {
        if (m.type == TrackApplication) {
            continue;
        }
        // RTP is routed to its stream by SSRC: without a=ssrc every m-line of the same payload type
        // maps to the first one, and the RTP of all sources would be written to stream 0.
        CHECK(m.getRtpSSRC(), "batch offer requires a=ssrc on every audio/video m-line, mid=", m.mid);
        if (expect_audio) {
            CHECK(m.type == TrackAudio, "batch offer layout invalid, expect audio then video");
            expect_audio = false;
            continue;
        }
        CHECK(m.type == TrackVideo, "batch offer layout invalid, expect video after audio");
        expect_audio = true;
        ++av_count;
    }

    CHECK(expect_audio, "batch offer layout invalid, dangling audio track without video");
    CHECK(av_count > 0, "batch offer has no audio/video pair");
    _stream_count = av_count;
}

void MultiSourceWebRtcPusher::onRtcConfigure(RtcConfigure &configure) const {
    WebRtcTransportImp::onRtcConfigure(configure);
    configure.audio.direction = configure.video.direction = RtpDirection::recvonly;
    configure.audio.preferred_codec = { CodecG711A };
    // The edge sends its packed frames as VP8: browsers depacketize them without parsing the payload.
    configure.video.preferred_codec = { CodecVP8, CodecH264 };
}

bool MultiSourceWebRtcPusher::enableDatachannelEcho() const {
    return false;
}

void MultiSourceWebRtcPusher::onRecvRtp(MediaTrack &track, const string &rid, RtpPacket::Ptr rtp) {
    if (!_batch_session || !rtp) {
        return;
    }

    auto mid = track.media ? track.media->mid : "";
    auto ok = _batch_session->onRtp(rtp->type, mid, rtp->getSSRC(), toolkit::getCurrentMillisecond(), rtp);
    if (!ok && _drop_warn_ticker.elapsedTime() > 2000) {
        // onRtp drops RTP that maps to no stream, to a stream with no bound source, or to a source whose
        // stream registration was rejected.
        WarnL << "drop rtp: no stream, unbound stream or rejected source, app=" << _media_info.app << ", nvrId=" << _media_info.stream
              << ", mid=" << mid << ", ssrc=" << rtp->getSSRC();
        _drop_warn_ticker.resetTime();
    }
}

#ifdef ENABLE_SCTP
void MultiSourceWebRtcPusher::OnSctpAssociationMessageReceived(RTC::SctpAssociation *sctpAssociation,
                                                               uint16_t streamId,
                                                               uint32_t ppid,
                                                               const uint8_t *msg,
                                                               size_t len) {
    WebRtcTransport::OnSctpAssociationMessageReceived(sctpAssociation, streamId, ppid, msg, len);

    if (!msg || !len) {
        return;
    }

    // This runs on usrsctp's receive callback: no exception may leave it.
    string request_id;
    string act;
    try {
        Json::Value root;
        Json::Reader reader;
        auto body = string((const char *)msg, len);
        if (!reader.parse(body, root) || !root.isObject()) {
            WarnL << "batch datachannel invalid payload, app=" << _media_info.app << ", nvrId=" << _media_info.stream
                  << ", size=" << len;
            sendReply("", "", "", "", streamId, ppid, kCodeInvalidArgs, "invalidPayload");
            return;
        }

        request_id = root["id"].isString() ? root["id"].asString() : "";
        act = root["act"].isString() ? root["act"].asString() : "";
        if (!_batch_session) {
            sendReply(act, request_id, "", "", streamId, ppid, kCodeNotFound, "sessionNotFound");
            return;
        }
        if (!root["data"].isObject()) {
            sendReply(act, request_id, "", "", streamId, ppid, kCodeInvalidArgs, "invalidData");
            return;
        }
        if (act == "whip.bind") {
            handleBind(root, request_id, streamId, ppid);
            return;
        }
        if (act == "whip.unbind") {
            handleUnbind(root, request_id, streamId, ppid);
            return;
        }

        WarnL << "batch datachannel invalid act, app=" << _media_info.app << ", nvrId=" << _media_info.stream << ", act=" << act;
        sendReply(act, request_id, "", "", streamId, ppid, kCodeInvalidArgs, "invalidAct");
    } catch (std::exception &ex) {
        WarnL << "batch datachannel handle exception, app=" << _media_info.app << ", nvrId=" << _media_info.stream
              << ", act=" << act << ", err=" << ex.what();
        try {
            sendReply(act, request_id, "", "", streamId, ppid, kCodeException, "internalException");
        } catch (...) {
        }
    }
}
#endif

void MultiSourceWebRtcPusher::handleBind(const Json::Value &root,
                                        const string &request_id,
                                        uint16_t dc_stream_id,
                                        uint32_t ppid) {
    auto &data = root["data"];
    string source_id;
    if (!checkSourceArgs("whip.bind", data, request_id, dc_stream_id, ppid, source_id)) {
        return;
    }
    // 0 means the default WHEP viewer limit.
    auto max_viewers = data["maxViewers"].isUInt() ? (size_t)data["maxViewers"].asUInt() : 0;

    string stream_id;
    string reason;
    bool reused = false;
    auto ok = _batch_session->bindSource(source_id, max_viewers, stream_id, reused, reason);
    if (!ok) {
        WarnL << "whip.bind failed, app=" << _media_info.app << ", nvrId=" << _media_info.stream << ", sourceId=" << source_id
              << ", reason=" << reason;
        sendReply("whip.bind", request_id, source_id, "", dc_stream_id, ppid, kCodeOtherFailed,
                  reason.empty() ? "noAvailableStreamId" : reason);
        return;
    }

    sendReply("whip.bind", request_id, source_id, stream_id, dc_stream_id, ppid, kCodeSuccess, reused ? "reused" : "");
}

void MultiSourceWebRtcPusher::handleUnbind(const Json::Value &root,
                                          const string &request_id,
                                          uint16_t dc_stream_id,
                                          uint32_t ppid) {
    string source_id;
    if (!checkSourceArgs("whip.unbind", root["data"], request_id, dc_stream_id, ppid, source_id)) {
        return;
    }

    string stream_id;
    bool reused = false;
    _batch_session->unbindSource(source_id, stream_id, reused);
    sendReply("whip.unbind", request_id, source_id, stream_id, dc_stream_id, ppid, kCodeSuccess, reused ? "reused" : "");
}

bool MultiSourceWebRtcPusher::checkSourceArgs(const string &act,
                                              const Json::Value &data,
                                              const string &request_id,
                                              uint16_t dc_stream_id,
                                              uint32_t ppid,
                                              string &source_id) {
    auto nvr_id = data["nvrId"].isString() ? data["nvrId"].asString() : "";
    source_id = data["sourceId"].isString() ? data["sourceId"].asString() : "";
    const char *reason = nullptr;
    if (nvr_id.empty() || nvr_id != _media_info.stream) {
        reason = "invalidNvrId";
    } else if (source_id.empty()) {
        reason = "invalidSourceId";
    }
    if (!reason) {
        return true;
    }
    WarnL << act << " rejected, app=" << _media_info.app << ", nvrId=" << _media_info.stream << ", requestNvrId=" << nvr_id
          << ", sourceId=" << source_id << ", reason=" << reason;
    sendReply(act, request_id, source_id, "", dc_stream_id, ppid, kCodeInvalidArgs, reason);
    return false;
}

void MultiSourceWebRtcPusher::sendReply(const string &act,
                                       const string &request_id,
                                       const string &source_id,
                                       const string &stream_id,
                                       uint16_t dc_stream_id,
                                       uint32_t ppid,
                                       int err_code,
                                       const string &err_reason) {
    Json::Value root;
    Json::Value data;
    root["id"] = request_id;
    root["act"] = act;
    data["nvrId"] = _media_info.stream;
    if (!source_id.empty()) {
        data["sourceId"] = source_id;
    }
    if (!stream_id.empty()) {
        data["streamId"] = stream_id;
    }
    root["data"] = std::move(data);

    if (err_code != kCodeSuccess || !err_reason.empty()) {
        Json::Value err;
        err["code"] = err_code;
        if (!err_reason.empty()) {
            err["reason"] = err_reason;
        }
        root["error"] = std::move(err);
    }

    auto payload = root.toStyledString();
    sendDatachannel(dc_stream_id, ppid, payload.data(), payload.size());
}

} // namespace mediakit
