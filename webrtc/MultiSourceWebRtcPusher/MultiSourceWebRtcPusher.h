/*
 * Copyright (c) 2016-present The ZLMediaKit project authors. All Rights Reserved.
 *
 * This file is part of ZLMediaKit(https://github.com/ZLMediaKit/ZLMediaKit).
 *
 * Use of this source code is governed by MIT-like license that can be found in the
 * LICENSE file in the root of the source tree. All contributing project authors
 * may be found in the AUTHORS file in the root of the source tree.
 */

#ifndef ZLMEDIAKIT_MULTI_SOURCE_WEBRTC_PUSHER_H
#define ZLMEDIAKIT_MULTI_SOURCE_WEBRTC_PUSHER_H

#include "BatchPublishSessionManager.h"
#include "BatchPublishSession.h"
#include "webrtc/WebRtcTransport.h"

namespace mediakit {

class MultiSourceWebRtcPusher : public WebRtcTransportImp {
public:
    using Ptr = std::shared_ptr<MultiSourceWebRtcPusher>;

    static Ptr create(const toolkit::EventPoller::Ptr &poller,
                      const MediaInfo &info,
                      const ProtocolOption &option,
                      size_t stream_count,
                      WebRtcTransport::Role role,
                      WebRtcTransport::SignalingProtocols signaling_protocols);

    const MediaInfo &getMediaInfo() const;

protected:
    void onStartWebRTC() override;
    void onDestory() override;
    void onCheckSdp(SdpType type, RtcSession &sdp) override;
    void onRtcConfigure(RtcConfigure &configure) const override;
    void onRecvRtp(MediaTrack &track, const std::string &rid, RtpPacket::Ptr rtp) override;
    bool enableDatachannelEcho() const override;
#ifdef ENABLE_SCTP
    void OnSctpAssociationMessageReceived(RTC::SctpAssociation *sctpAssociation,
                                          uint16_t streamId,
                                          uint32_t ppid,
                                          const uint8_t *msg,
                                          size_t len) override;
#endif

private:
    MultiSourceWebRtcPusher(const toolkit::EventPoller::Ptr &poller,
                            const MediaInfo &info,
                            const ProtocolOption &option,
                            size_t stream_count);

    void handleBind(const Json::Value &root, const std::string &request_id, uint16_t dc_stream_id, uint32_t ppid);
    void handleUnbind(const Json::Value &root, const std::string &request_id, uint16_t dc_stream_id, uint32_t ppid);
    void sendReply(const std::string &act,
                   const std::string &request_id,
                   const std::string &source_id,
                   const std::string &stream_id,
                   uint16_t dc_stream_id,
                   uint32_t ppid,
                   int err_code,
                   const std::string &err_reason);

private:
    MediaInfo _media_info;
    ProtocolOption _option;
    size_t _stream_count = 0;
    BatchPublishSession::Ptr _batch_session;
};

} // namespace mediakit

#endif // ZLMEDIAKIT_MULTI_SOURCE_WEBRTC_PUSHER_H
