/*
 * Copyright (c) 2016-present The ZLMediaKit project authors. All Rights Reserved.
 *
 * This file is part of ZLMediaKit(https://github.com/ZLMediaKit/ZLMediaKit).
 *
 * Use of this source code is governed by MIT-like license that can be found in the
 * LICENSE file in the root of the source tree. All contributing project authors
 * may be found in the AUTHORS file in the root of the source tree.
 */

#ifndef ZLMEDIAKIT_MASKSOURCE_H
#define ZLMEDIAKIT_MASKSOURCE_H

#include <atomic>
#include <functional>
#include <list>
#include <memory>
#include <mutex>
#include <string>

#include "Common/MediaSource.h"
#include "Common/MultiMediaSourceMuxer.h"
#include "Extension/Frame.h"
#include "MaskEngineClient.h"
#include "Network/Session.h"

namespace mediakit {

// URL 里的打码参数（mask=true&fps=15&speed_fps=10&res=720，方案文档 6.6）；0 表示用 edge-mask 的默认值
// Mask parameters on a play URL (mask=true&fps=15&speed_fps=10&res=720); 0 leaves the choice to edge-mask
struct MaskRequest {
    bool enabled = false;
    int fps = 0;
    int speed_fps = 0;
    int res = 0;

    static MaskRequest parse(const std::string &params);
    // 打码流名后缀，同一路源流相同参数的直播打码流共享 / Suffix of the masked stream; live viewers with equal params share it
    std::string tag() const;
};

// 打码流：旁路订阅一路源流（直播源或回放会话）的帧，视频送 edge-mask 打码、音频直通，用引擎的输出生成一路
// 新的 RTSP 流（MaskProxy / MaskSink，方案文档第 8、9 章）。打码流只由引擎输出构成，任何失败都关闭打码流，
// 绝不回退到原始画面（fail-closed）。
// - DESCRIBE 只生成打码流（按源流编码格式出 SDP），不连引擎；PLAY 时由 RtspSession 调用 acquire 建立引擎会话。
// - 直播打码流按（源流，打码参数）共享；回放打码流跟随回放会话一对一。
// - 无人观看超过宽限期、源流消失、引擎出错时关闭。
// A masked stream: taps the frames of a source stream (live, or a replay session), sends video through
// edge-mask and passes audio through, and publishes the engine output as a new RTSP stream. It consists
// of engine output only; any failure closes it and never falls back to raw video (fail-closed).
class MaskSource : public MediaSourceEvent, public std::enable_shared_from_this<MaskSource> {
public:
    using Ptr = std::shared_ptr<MaskSource>;
    using FindCB = std::function<void(const MediaSource::Ptr &src)>;
    using AcquireCB = std::function<void(bool ok, const std::string &code)>;
    using SpeedCB = std::function<void(bool ok, const std::string &reason)>;

    // 为已找到的源流 src 找到或创建打码流；cb 在 session 线程回调，失败时为 nullptr
    // Find or create the masked stream for source src; cb runs on the session's poller, nullptr on failure
    static void findAsync(const MediaInfo &info, const MaskRequest &req, const MediaSource::Ptr &src,
                          const std::shared_ptr<toolkit::Session> &session, FindCB cb);

    // PLAY 前调用：确保打码流的 edge-mask 会话已建立，cb 在 caller 线程回调。play_src 不是打码流时返回 false 且不回调
    // Called before PLAY: make sure the edge-mask session is up, cb runs on caller. Returns false (and never
    // calls cb) when play_src is not a masked stream
    static bool acquire(const MediaSource &play_src, const toolkit::EventPoller::Ptr &caller, AcquireCB cb);

    // PLAY 带 Scale / Speed 时调用：先问引擎（SPEED，等 SPEED_ACK），cb 在 caller 线程回调；ok 时调用方再执行
    // play_src->speed()，打码会话在那里切到新 epoch。play_src 不是打码流时返回 false 且不回调
    // Called when PLAY carries Scale / Speed: ask the engine first (SPEED, wait for SPEED_ACK); cb runs on caller.
    // On ok the caller then calls play_src->speed(), where the mask session switches to the new epoch. Returns
    // false (and never calls cb) when play_src is not a masked stream
    static bool requestSpeed(const MediaSource &play_src, float speed, const toolkit::EventPoller::Ptr &caller, SpeedCB cb);

    MaskSource(MediaSource::Ptr src, toolkit::EventPoller::Ptr poller, MediaTuple out_tuple, MaskRequest req, bool replay);
    ~MaskSource() override;

private:
    enum class State { kIdle, kOpening, kActive, kClosed };

    bool setup();
    bool attachSource(bool use_gop);
    void activate(const toolkit::EventPoller::Ptr &caller, AcquireCB cb);
    void onOpened(const MaskEngineClient::OpenResult &result);
    void onSourceFrame(const Frame::Ptr &frame);
    void onEngineFrame(const MaskProto::FrameInfo &frame);
    void checkStall();
    void askSpeed(float speed, const toolkit::EventPoller::Ptr &caller, SpeedCB cb);
    // 回放背压：引擎发送缓冲积压时暂停回放读取，排空后恢复（与用户的暂停合并）
    // Replay back-pressure: pause reading the replay while the engine send buffer is backed up (combined with user pause)
    void setBackpressure(bool on);
    void applySourcePause();
    void startGraceTimer();
    void shutdown(const std::string &reason);
    Json::Value buildOpenRequest() const;

    // MediaSourceEvent
    MediaOriginType getOriginType(MediaSource &sender) const override;
    std::string getOriginUrl(MediaSource &sender) const override;
    bool seekTo(MediaSource &sender, uint32_t stamp) override;
    bool pause(MediaSource &sender, bool pause) override;
    bool speed(MediaSource &sender, float speed) override;
    bool close(MediaSource &sender) override;
    int totalReaderCount(MediaSource &sender) override;
    void onReaderChanged(MediaSource &sender, int size) override;
    toolkit::EventPoller::Ptr getOwnerPoller(MediaSource &sender) override;

private:
    bool _replay;
    MaskRequest _req;
    MediaTuple _out_tuple;
    std::string _origin_url;
    std::weak_ptr<MediaSource> _src;
    toolkit::EventPoller::Ptr _poller;

    State _state = State::kIdle;
    CodecId _codec = CodecInvalid;
    Track::Ptr _video_track;
    float _speed = 1.0f;
    std::atomic<bool> _user_paused { false };

    std::mutex _muxer_mtx; // _muxer 在其他线程也会读取（观看人数、seek）/ _muxer is also read from other threads
    MultiMediaSourceMuxer::Ptr _muxer;
    MultiMediaSourceMuxer::RingType::RingReader::Ptr _reader;
    MaskEngineClient::Ptr _client;
    std::list<std::pair<toolkit::EventPoller::Ptr, AcquireCB>> _waiters;

    // 只在 _poller 线程访问 / Accessed on _poller only
    FrameMerger _merger { FrameMerger::h264_prefix };
    uint32_t _epoch = 0;
    bool _in_gate = true;   // 等关键帧再送引擎 / wait for a keyframe before sending to the engine
    bool _out_gate = true;  // 等关键帧再分发引擎输出 / wait for a keyframe before publishing engine output
    uint64_t _sent_since_output = 0;
    bool _backpressure = false;     // 回放背压中 / replay back-pressure active
    bool _await_seek = false;       // seek 后等新位置的关键帧 / after a seek, wait for a keyframe at the new position
    uint64_t _seek_target_ms = 0;
    float _granted_speed = 0;       // 引擎已接受、待生效的倍速 / speed accepted by the engine, not applied yet
    uint32_t _granted_epoch = 0;
    bool _b_frames = false;         // 源流含 B 帧（按 pts != dts 判断）/ source has B-frames (pts != dts)
    uint64_t _last_audio_dts = 0;
    bool _have_audio_dts = false;
    toolkit::Ticker _last_output;
    toolkit::Ticker _drop_log;
    toolkit::EventPoller::DelayTask::Ptr _grace_timer;
    toolkit::EventPoller::DelayTask::Ptr _stall_timer;
};

} // namespace mediakit

#endif // ZLMEDIAKIT_MASKSOURCE_H
