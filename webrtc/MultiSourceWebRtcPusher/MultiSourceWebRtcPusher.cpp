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
                                                             size_t stream_count,
                                                             WebRtcTransport::Role role,
                                                             WebRtcTransport::SignalingProtocols signaling_protocols) {
    MultiSourceWebRtcPusher::Ptr pusher(new MultiSourceWebRtcPusher(poller, info, option, stream_count), [](MultiSourceWebRtcPusher *ptr) {
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
                                                 const ProtocolOption &option,
                                                 size_t stream_count)
    : WebRtcTransportImp(poller)
    , _media_info(info)
    , _option(option)
    , _stream_count(stream_count) {
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
    if (!_batch_session->initFromSdp(*_answer_sdp, _answer_sdp->toRtspSdp())) {
        onShutdown(SockException(Err_other, "batch stream table init failed"));
        return;
    }

    if (!BatchPublishSessionManager::Instance().addSession(_batch_session)) {
        onShutdown(SockException(Err_other, "already publishing"));
        return;
    }
    InfoL << "batch session created, app=" << _media_info.app << ", nvrId=" << _media_info.stream
          << ", streamCount=" << _stream_count << ", transportId=" << getIdentifier();
}

void MultiSourceWebRtcPusher::onDestory() {
    if (_batch_session) {
        _batch_session->clearAllSource();
        BatchPublishSessionManager::Instance().removeSession(_batch_session->getApp(), _batch_session->getNvrId());
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
    CHECK(av_count == _stream_count,
          StrPrinter << "batch offer streamCount mismatch, required=" << _stream_count << ", got=" << av_count);
}

void MultiSourceWebRtcPusher::onRtcConfigure(RtcConfigure &configure) const {
    WebRtcTransportImp::onRtcConfigure(configure);
    configure.audio.direction = configure.video.direction = RtpDirection::recvonly;
    configure.audio.preferred_codec = { CodecG711A };
    configure.video.preferred_codec = { CodecH264 };
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
    if (!ok) {
        static toolkit::Ticker s_warn_ticker;
        if (s_warn_ticker.elapsedTime() > 2000) {
            WarnL << "drop rtp for unbound stream, app=" << _media_info.app << ", nvrId=" << _media_info.stream
                  << ", mid=" << mid << ", ssrc=" << rtp->getSSRC();
            s_warn_ticker.resetTime();
        }
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

    Json::Value root;
    Json::Reader reader;
    auto body = string((const char *)msg, len);
    if (!reader.parse(body, root)) {
        sendReply("", "", "", "", streamId, ppid, kCodeInvalidArgs, "invalidPayload");
        return;
    }

    auto request_id = root["id"].asString();
    auto act = root["act"].asString();
    if (!_batch_session) {
        sendReply(act, request_id, "", "", streamId, ppid, kCodeNotFound, "sessionNotFound");
        return;
    }

    try {
    if (act == "whip.bind") {
        handleBind(root, request_id, streamId, ppid);
        return;
    }
    if (act == "whip.unbind") {
        handleUnbind(root, request_id, streamId, ppid);
        return;
    }

    sendReply(act, request_id, "", "", streamId, ppid, kCodeInvalidArgs, "invalidAct");
    } catch (std::exception &ex) {
        WarnL << "batch datachannel handle exception, app=" << _media_info.app << ", nvrId=" << _media_info.stream
              << ", act=" << act << ", err=" << ex.what();
        sendReply(act, request_id, "", "", streamId, ppid, kCodeException, "internalException");
    }
}
#endif

void MultiSourceWebRtcPusher::handleBind(const Json::Value &root,
                                        const string &request_id,
                                        uint16_t dc_stream_id,
                                        uint32_t ppid) {
    auto data = root["data"];
    auto nvr_id = data["nvrId"].asString();
    auto source_id = data["sourceId"].asString();

    if (nvr_id.empty()) {
        sendReply("whip.bind", request_id, source_id, "", dc_stream_id, ppid, kCodeInvalidArgs, "invalidNvrId");
        return;
    }
    if (source_id.empty()) {
        sendReply("whip.bind", request_id, source_id, "", dc_stream_id, ppid, kCodeInvalidArgs, "invalidSourceId");
        return;
    }
    if (nvr_id != _media_info.stream) {
        sendReply("whip.bind", request_id, source_id, "", dc_stream_id, ppid, kCodeInvalidArgs, "invalidNvrId");
        return;
    }

    string stream_id;
    string reason;
    bool reused = false;
    auto ok = _batch_session->bindSource(source_id, stream_id, reused, reason);
    if (!ok) {
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
    auto data = root["data"];
    auto nvr_id = data["nvrId"].asString();
    auto source_id = data["sourceId"].asString();

    if (nvr_id.empty()) {
        sendReply("whip.unbind", request_id, source_id, "", dc_stream_id, ppid, kCodeInvalidArgs, "invalidNvrId");
        return;
    }
    if (source_id.empty()) {
        sendReply("whip.unbind", request_id, source_id, "", dc_stream_id, ppid, kCodeInvalidArgs, "invalidSourceId");
        return;
    }
    if (nvr_id != _media_info.stream) {
        sendReply("whip.unbind", request_id, source_id, "", dc_stream_id, ppid, kCodeInvalidArgs, "invalidNvrId");
        return;
    }

    string stream_id;
    bool reused = false;
    auto ok = _batch_session->unbindSource(source_id, stream_id, reused);
    if (!ok) {
        sendReply("whip.unbind", request_id, source_id, "", dc_stream_id, ppid, kCodeOtherFailed, "internalException");
        return;
    }

    sendReply("whip.unbind", request_id, source_id, stream_id, dc_stream_id, ppid, kCodeSuccess, reused ? "reused" : "");
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
