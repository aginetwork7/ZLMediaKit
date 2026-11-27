/*
 * Copyright (c) 2016-present The ZLMediaKit project authors. All Rights Reserved.
 *
 * This file is part of ZLMediaKit(https://github.com/ZLMediaKit/ZLMediaKit).
 *
 * Use of this source code is governed by MIT-like license that can be found in the
 * LICENSE file in the root of the source tree. All contributing project authors
 * may be found in the AUTHORS file in the root of the source tree.
 */

#ifndef ZLMEDIAKIT_RTSPMUXER_H
#define ZLMEDIAKIT_RTSPMUXER_H

#include "Extension/Frame.h"
#include "Common/MediaSink.h"
#include "Common/Stamp.h"
#include "RtpCodec.h"

namespace mediakit{

class RingDelegateHelper : public toolkit::RingDelegate<RtpPacket::Ptr> {
public:
    using onRtp = std::function<void(RtpPacket::Ptr in, bool is_key)> ;

    RingDelegateHelper(onRtp on_rtp) {
        _on_rtp = std::move(on_rtp);
    }

    void onWrite(RtpPacket::Ptr in, bool is_key) override {
        _on_rtp(std::move(in), is_key);
    }

private:
    onRtp _on_rtp;
};

/**
* rtsp生成器
 * RTSP generator
 
 * [AUTO-TRANSLATED:2a72d801]
*/
class RtspMuxer : public MediaSinkInterface {
public:
    using Ptr = std::shared_ptr<RtspMuxer>;

    /**
     * 构造函数
     * Constructor
     
     * [AUTO-TRANSLATED:41469869]
     */
    RtspMuxer(const TitleSdp::Ptr &title = nullptr);

    /**
     * 获取完整的SDP字符串
     * @return SDP字符串
     * Get the complete SDP string
     * @return SDP string
     
     * [AUTO-TRANSLATED:f5d1b0a6]
     */
    std::string getSdp() ;

    /**
     * 获取rtp环形缓存
     * @return
     * Get the RTP ring buffer
     * @return
     
     * [AUTO-TRANSLATED:644e8634]
     */
    RtpRing::RingType::Ptr getRtpRing() const;

    // Set absolute time base (Unix ms) for RTP extension timestamp generation.
    // When set, extension time is computed as base + current RTP NPT(ms).
    void setRtpExtTimeBaseMS(uint64_t base_ms);
    uint64_t getRtpExtTimeBaseMS() const { return _rtp_ext_time_base_ms; }

    // 直播时 RTCP SR 的 NTP 时间直接按帧时间戳线性换算，不做音视频对齐。用于各 track 时间戳已在同一时间轴上、
    // 但到达有固定先后的流（打码流：视频经引擎晚到，音频直通）：对齐会在开头把视频的 NTP 拉到音频上，
    // 之后再修正回来，用 SR 定时的播放器（ffmpeg、VLC）会看到一次画面跳变
    // Live only: the RTCP SR NTP time follows the frame stamps linearly, without aligning tracks. For streams whose
    // tracks already share one timeline but arrive with a fixed offset (masked streams: video comes late through
    // the engine, audio passes through): aligning would first pull the video NTP onto the audio and later correct
    // it back, a one-time jump for players timed by SR (ffmpeg, VLC)
    void setNtpFollowStamp(bool on) { _ntp_follow_stamp = on; }

    /**
     * 添加ready状态的track
     * Add a ready state track
     
     * [AUTO-TRANSLATED:2d8138b3]
     */
    bool addTrack(const Track::Ptr & track) override;

    /**
     * 写入帧数据
     * @param frame 帧
     * Write frame data
     * @param frame Frame
     
     * [AUTO-TRANSLATED:b7c92013]
     */
    bool inputFrame(const Frame::Ptr &frame) override;

    /**
     * 刷新输出所有frame缓存
     * Flush all frame buffers
     
     * [AUTO-TRANSLATED:adaea568]
     */
    void flush() override;

    /**
     * seek后重置编码器内部缓存，避免输出旧时间点残帧
     */
    void dropCachedFrame();

    /**
     * 重置所有track
     * Reset all tracks
     
     
     * [AUTO-TRANSLATED:f203fa3e]
     */
    void resetTracks() override ;

private:
    void onRtp(RtpPacket::Ptr in, bool is_key);
    void trySyncTrack();
    uint64_t calcRtpExtTimeMS(const RtpPacket::Ptr &in) const;
    void injectRtpExtUnixSec(RtpPacket::Ptr &in, uint32_t unix_sec) const;

private:
    bool _live = true;
    bool _ntp_follow_stamp = false;
    bool _have_stamp_base = false;
    uint64_t _stamp_base = 0;
    bool _track_existed[2] = { false, false };

    uint8_t _index {0};
    uint64_t _ntp_stamp_start;
    uint64_t _rtp_ext_time_base_ms = 0;
    bool _video_ext_bootstrapped = false;
    std::string _sdp;

    struct TrackInfo {
        Stamp stamp;
        uint32_t rtp_stamp { 0 };
        uint64_t ntp_stamp { 0 };
        RtpCodec::Ptr encoder;
    };

    std::unordered_map<int, TrackInfo> _tracks;
    RtpRing::RingType::Ptr _rtpRing;
    RtpRing::RingType::Ptr _rtpInterceptor;
};


} /* namespace mediakit */

#endif //ZLMEDIAKIT_RTSPMUXER_H
