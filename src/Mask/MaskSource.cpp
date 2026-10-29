/*
 * Copyright (c) 2016-present The ZLMediaKit project authors. All Rights Reserved.
 *
 * This file is part of ZLMediaKit(https://github.com/ZLMediaKit/ZLMediaKit).
 *
 * Use of this source code is governed by MIT-like license that can be found in the
 * LICENSE file in the root of the source tree. All contributing project authors
 * may be found in the AUTHORS file in the root of the source tree.
 */

#include "MaskSource.h"

#include <cstdlib>
#include <mutex>
#include <unordered_map>

#include "Common/Parser.h"
#include "Common/config.h"
#include "Common/macros.h"
#include "Extension/Factory.h"
#include "Util/logger.h"

using namespace std;
using namespace toolkit;

namespace mediakit {

namespace {

// 打码流注册表：输出流 shortUrl -> MaskSource。注册表持有打码流，关闭（shutdown）时移除
// Masked streams by output shortUrl. The registry owns them until shutdown removes them
mutex s_mtx;
unordered_map<string, MaskSource::Ptr> s_sources;

MaskSource::Ptr lookup(const string &key) {
    lock_guard<mutex> lck(s_mtx);
    auto it = s_sources.find(key);
    return it == s_sources.end() ? nullptr : it->second;
}

// 已知含 B 帧的源流（按源流 shortUrl）：会话中途发现 B 帧时记下，观看端重连后 OPEN 带 b_frames=true
// Sources known to carry B-frames (by source shortUrl): remembered when found mid-session, so the next OPEN after
// the viewer reconnects says b_frames=true
mutex s_bframe_mtx;
unordered_map<string, bool> s_bframe_sources;

bool parseBool(const string &v) {
    return v == "1" || strcasecmp(v.data(), "true") == 0 || strcasecmp(v.data(), "yes") == 0;
}

// 等打码流注册完成（muxer 在 addTrackCompleted 后注册 RTSP 源）/ Wait for the masked stream to be registered
void waitRegistered(const MediaTuple &tuple, const EventPoller::Ptr &poller, int remain_ms, const MaskSource::FindCB &cb) {
    if (auto src = MediaSource::find(RTSP_SCHEMA, tuple.vhost, tuple.app, tuple.stream, false)) {
        cb(src);
        return;
    }
    if (remain_ms <= 0) {
        WarnL << "mask: masked stream not registered in time: " << tuple.shortUrl();
        cb(nullptr);
        return;
    }
    poller->doDelayTask(20, [tuple, poller, remain_ms, cb]() {
        waitRegistered(tuple, poller, remain_ms - 20, cb);
        return 0;
    });
}

} // namespace

MaskRequest MaskRequest::parse(const string &params) {
    MaskRequest req;
    if (params.empty()) {
        return req;
    }
    auto args = Parser::parseArgs(params);
    req.enabled = parseBool(args["mask"]);
    req.fps = atoi(args["fps"].data());
    req.speed_fps = atoi(args["speed_fps"].data());
    req.res = atoi(args["res"].data());
    return req;
}

string MaskRequest::tag() const {
    return StrPrinter << "mask_f" << fps << "_s" << speed_fps << "_r" << res;
}

void MaskSource::findAsync(const MediaInfo &info, const MaskRequest &req, const MediaSource::Ptr &src,
                           const shared_ptr<Session> &session, FindCB cb) {
    auto session_poller = session->getPoller();
    weak_ptr<Session> weak_session = session;
    auto reply = [session_poller, weak_session, cb](const MediaSource::Ptr &result) {
        session_poller->async([weak_session, cb, result]() {
            if (weak_session.lock()) {
                cb(result);
            }
        }, false);
    };
    // 打码流只提供 RTSP（edgebox 只用 RTSP 拉打码流）/ Masked streams are RTSP only
    if (info.schema != RTSP_SCHEMA) {
        WarnL << "mask: only rtsp is supported, schema=" << info.schema << ", url=" << info.getUrl();
        reply(nullptr);
        return;
    }

    GET_CONFIG(string, replay_app, Rtsp::kReplayAppName);
    const auto &src_tuple = src->getMediaTuple();
    bool replay = src_tuple.app == replay_app;
    MediaTuple out { src_tuple.vhost, src_tuple.app, src_tuple.stream + "/" + req.tag() };

    EventPoller::Ptr src_poller;
    try {
        src_poller = src->getOwnerPoller();
    } catch (std::exception &ex) {
        WarnL << "mask: source has no owner poller: " << src_tuple.shortUrl() << ", " << ex.what();
        reply(nullptr);
        return;
    }
    src_poller->async([src, src_poller, out, req, replay, session_poller, reply]() {
        auto mask = lookup(out.shortUrl());
        if (mask && mask->_state == State::kClosed) {
            // 正在关闭（打码流还没注销），这次请求失败，观看端重试即可 / Closing (not yet unregistered): fail this request, the client retries
            reply(nullptr);
            return;
        }
        if (!mask) {
            mask = make_shared<MaskSource>(src, src_poller, out, req, replay);
            if (!mask->setup()) {
                reply(nullptr);
                return;
            }
            lock_guard<mutex> lck(s_mtx);
            s_sources[out.shortUrl()] = mask;
        }
        // 打码流在源流全部 track 就绪后才注册：回放源要读到关键帧才就绪，GOP 长（实测 30 秒）的摄像头会超过 3 秒，
        // 所以按普通流的等待上限 general.maxStreamWaitMS 等
        // The masked stream registers once the source's tracks are ready; a replay source needs a key frame first,
        // which can take over 3 s with long-GOP cameras, so wait as long as for a normal stream (general.maxStreamWaitMS)
        GET_CONFIG(int, wait_ms, General::kMaxStreamWaitTimeMS);
        session_poller->async([out, session_poller, reply]() { waitRegistered(out, session_poller, wait_ms, reply); }, false);
    });
}

bool MaskSource::acquire(const MediaSource &play_src, const EventPoller::Ptr &caller, AcquireCB cb) {
    auto mask = lookup(play_src.getMediaTuple().shortUrl());
    if (!mask) {
        return false;
    }
    mask->_poller->async([mask, caller, cb]() { mask->activate(caller, cb); });
    return true;
}

bool MaskSource::requestSpeed(const MediaSource &play_src, float speed, const EventPoller::Ptr &caller, SpeedCB cb) {
    auto mask = lookup(play_src.getMediaTuple().shortUrl());
    if (!mask) {
        return false;
    }
    mask->_poller->async([mask, speed, caller, cb]() { mask->askSpeed(speed, caller, cb); });
    return true;
}

MaskSource::MaskSource(MediaSource::Ptr src, EventPoller::Ptr poller, MediaTuple out_tuple, MaskRequest req, bool replay)
    : _replay(replay), _req(req), _out_tuple(std::move(out_tuple)), _origin_url(src->getUrl()), _src(src), _poller(std::move(poller)) {}

MaskSource::~MaskSource() {
    if (_client) {
        _client->close();
    }
}

bool MaskSource::setup() {
    auto src = _src.lock();
    auto src_muxer = src ? src->getMuxer() : nullptr;
    if (!src_muxer) {
        WarnL << "mask: source has no muxer: " << _origin_url;
        return false;
    }
    ProtocolOption option;
    option.enable_rtsp = true;
    option.enable_rtmp = false;
    option.enable_hls = false;
    option.enable_hls_fmp4 = false;
    option.enable_mp4 = false;
    option.enable_ts = false;
    option.enable_fmp4 = false;
    option.enable_audio = true;
    option.add_mute_audio = false;
    option.auto_close = false;
    // 源流的帧已经处理过时间戳并按节奏送出，打码流原样使用 / Source frames are already stamped and paced
    option.modify_stamp = ProtocolOption::kModifyStampOff;
    option.paced_sender_ms = 0;

    {
        lock_guard<mutex> lck(_muxer_mtx);
        _muxer = make_shared<MultiMediaSourceMuxer>(_out_tuple, 0.0, option);
    }
    _muxer->setMediaListener(shared_from_this());
    for (auto &track : src_muxer->getTracks(true)) {
        if (track->getTrackType() == TrackVideo) {
            if (track->getCodecId() != CodecH264 && track->getCodecId() != CodecH265) {
                WarnL << "mask: unsupported video codec " << track->getCodecName() << ": " << _origin_url;
                return false;
            }
            _codec = track->getCodecId();
            _video_track = track;
        }
        // SDP 按源流编码格式生成；打码后的 SPS/PPS 由编码器带在每个 IDR 前
        // The SDP follows the source codec; the encoder's SPS/PPS travel in-band before every IDR
        _muxer->addTrack(track->clone());
    }
    if (!_video_track) {
        WarnL << "mask: source has no video: " << _origin_url;
        return false;
    }
    _muxer->addTrackCompleted();
    {
        lock_guard<mutex> lck(s_bframe_mtx);
        _b_frames = s_bframe_sources.count(src->getMediaTuple().shortUrl()) > 0;
    }
    // 打码流在 DESCRIBE 时就要能找到，而画面要到 PLAY 建立引擎会话后才有：先送视频的 SPS/PPS（不含画面）让视频
    // track 就绪，音频直通（音频不打码）；纯视频源直接注册
    // The masked stream must be findable at DESCRIBE while its video only starts after PLAY: feed the video
    // SPS/PPS (no picture) so the video track gets ready and pass audio through (audio is not masked);
    // a video-only source is registered right away
    if (auto video = dynamic_pointer_cast<VideoTrack>(_video_track)) {
        for (auto &frame : video->getConfigFrames()) {
            _muxer->inputFrame(frame);
        }
    }
    if (!attachSource(false)) {
        return false;
    }
    _muxer->registRtspWithoutData();
    // DESCRIBE 后一直没有 PLAY 的打码流（如探测连接）也按宽限期回收
    // A masked stream described but never played (e.g. a probe connection) is reclaimed after the grace period too
    startGraceTimer();
    InfoL << "mask: created " << _out_tuple.shortUrl() << " for " << _origin_url;
    return true;
}

Json::Value MaskSource::buildOpenRequest() const {
    Json::Value req;
    req["stream"] = _out_tuple.shortUrl();
    req["trace_id"] = _origin_url;
    req["epoch"] = _epoch;
    Json::Value source;
    source["codec"] = _codec == CodecH264 ? "h264" : "h265";
    if (auto video = dynamic_pointer_cast<VideoTrack>(_video_track)) {
        source["width"] = video->getVideoWidth();
        source["height"] = video->getVideoHeight();
        source["fps"] = video->getVideoFps();
    }
    // 码率未知时为 0（引擎按输出档位表取码率，不受原始码率限制）/ 0 when unknown (the engine then uses its tier table)
    source["bitrate_kbps"] = _video_track->getBitRate() / 1000;
    // B 帧按 pts != dts 判断（DESCRIBE 后已经在看源流的帧）。没看到时按无 B 帧：摄像头一般是 IPPP；
    // 会话中途发现 B 帧时关闭打码流并记下，重连后的 OPEN 带 true（见 onSourceFrame）
    // B-frames are detected by pts != dts (frames have been seen since DESCRIBE). None seen means no B-frames
    // (cameras are usually IPPP); finding them mid-session closes the stream and remembers it (see onSourceFrame)
    source["b_frames"] = _b_frames;
    req["source"] = source;
    Json::Value params;
    params["fps"] = _req.fps;
    params["speed_fps"] = _req.speed_fps;
    params["res"] = _req.res;
    params["speed"] = _speed;
    req["params"] = params;
    return req;
}

bool MaskSource::attachSource(bool use_gop) {
    auto src = _src.lock();
    auto src_muxer = src ? src->getMuxer() : nullptr;
    if (!src_muxer) {
        return false;
    }
    weak_ptr<MaskSource> weak_self = shared_from_this();
    // 订阅者计入源流观看人数，让源流（含按需拉流的 ext 代理、回放会话）保持运行
    // The reader counts as a viewer of the source, keeping it (on-demand ext proxies, replay sessions) alive
    _reader = src_muxer->getFrameRing(1)->attach(_poller, use_gop);
    _reader->setReadCB([weak_self](const Frame::Ptr &frame) {
        if (auto strong_self = weak_self.lock()) {
            strong_self->onSourceFrame(frame);
        }
    });
    _reader->setDetachCB([weak_self]() {
        if (auto strong_self = weak_self.lock()) {
            strong_self->shutdown("source closed");
        }
    });
    return true;
}

void MaskSource::activate(const EventPoller::Ptr &caller, AcquireCB cb) {
    switch (_state) {
        case State::kActive:
            caller->async([cb]() { cb(true, ""); });
            return;
        case State::kClosed:
            caller->async([cb]() { cb(false, "closed"); });
            return;
        case State::kOpening:
            _waiters.emplace_back(caller, std::move(cb));
            return;
        case State::kIdle:
            break;
    }
    _waiters.emplace_back(caller, std::move(cb));
    _state = State::kOpening;
    GET_CONFIG(string, socket_path, Mask::kSocketPath);
    GET_CONFIG(int, open_timeout_ms, Mask::kOpenTimeoutMS);
    _client = make_shared<MaskEngineClient>(_poller);
    weak_ptr<MaskSource> weak_self = shared_from_this();
    _client->open(socket_path, buildOpenRequest(), open_timeout_ms, [weak_self](const MaskEngineClient::OpenResult &result) {
        if (auto strong_self = weak_self.lock()) {
            strong_self->onOpened(result);
        }
    });
}

void MaskSource::onOpened(const MaskEngineClient::OpenResult &result) {
    if (_state != State::kOpening) {
        return;
    }
    if (!result.ok) {
        WarnL << "mask: edge-mask rejected " << _out_tuple.shortUrl() << ": " << result.code << " " << result.message;
        auto waiters = std::move(_waiters);
        _waiters.clear();
        for (auto &w : waiters) {
            auto cb = w.second;
            auto code = result.code;
            w.first->async([cb, code]() { cb(false, code); });
        }
        shutdown("open failed: " + result.code);
        return;
    }

    _state = State::kActive;
    InfoL << "mask: session opened " << _out_tuple.shortUrl() << ", output " << result.ack.toStyledString();
    weak_ptr<MaskSource> weak_self = shared_from_this();
    _client->setOnFrame([weak_self](const MaskProto::FrameInfo &frame) {
        if (auto strong_self = weak_self.lock()) {
            strong_self->onEngineFrame(frame);
        }
    });
    _client->setOnError([weak_self](const string &reason) {
        if (auto strong_self = weak_self.lock()) {
            strong_self->shutdown("edge-mask: " + reason);
        }
    });
    _client->setOnDrain([weak_self]() {
        if (auto strong_self = weak_self.lock()) {
            strong_self->setBackpressure(false);
        }
    });
    // 重新订阅，从最近的 GOP 开始送引擎，加快出画面 / Re-attach from the latest GOP so the first masked picture comes sooner
    if (!attachSource(true)) {
        shutdown("source closed before open");
        return;
    }
    _last_output.resetTime();
    _stall_timer = _poller->doDelayTask(1000, [weak_self]() -> uint64_t {
        auto strong_self = weak_self.lock();
        if (!strong_self || strong_self->_state != State::kActive) {
            return 0;
        }
        strong_self->checkStall();
        return 1000;
    });

    auto waiters = std::move(_waiters);
    _waiters.clear();
    for (auto &w : waiters) {
        auto cb = w.second;
        w.first->async([cb]() { cb(true, ""); });
    }
}

void MaskSource::onSourceFrame(const Frame::Ptr &frame) {
    if (_state == State::kClosed) {
        return;
    }
    if (frame->getTrackType() == TrackAudio) {
        // 音频不进引擎，直通；重新订阅时 GOP 缓存里的音频会再来一遍，按时间戳去重
        // Audio bypasses the engine; re-attaching replays the GOP cache, so drop audio already passed through
        if (_have_audio_dts && frame->dts() <= _last_audio_dts) {
            return;
        }
        _have_audio_dts = true;
        _last_audio_dts = frame->dts();
        _muxer->inputFrame(frame);
        return;
    }
    if (frame->getTrackType() != TrackVideo) {
        return;
    }
    if (!_b_frames && frame->pts() != frame->dts()) {
        // 发现 B 帧：引擎按无 B 帧做了低延迟解码，继续送画面会出错。记下后关闭（会话已建立时），观看端重连后按 B 帧重新开
        // B-frames found: the engine decodes in low-latency mode assuming none, so pictures would break. Remember it
        // and close (if the session is up); the viewer reconnects and the next OPEN declares B-frames
        _b_frames = true;
        if (auto src = _src.lock()) {
            lock_guard<mutex> lck(s_bframe_mtx);
            s_bframe_sources[src->getMediaTuple().shortUrl()] = true;
        }
        if (_state == State::kActive) {
            shutdown("source has B-frames, reopen with b_frames=true");
            return;
        }
    }
    if (_state != State::kActive) {
        // 引擎会话建立前不处理视频 / No video before the engine session is up
        return;
    }
    // 把同一时间戳的 SPS/PPS/切片合成一个访问单元 / Merge SPS/PPS/slices of one timestamp into an access unit
    _merger.inputFrame(frame, [this](uint64_t dts, uint64_t pts, const Buffer::Ptr &buffer, bool key_frame) {
        if (_await_seek) {
            // seek 后在新位置的关键帧之前，丢掉旧位置还在路上的帧（时间戳是会话 npt，与 seek 目标同一时间轴）
            // After a seek, drop frames of the old position still in flight until a keyframe near the target
            // (stamps are session npt, the same timeline as the seek target)
            static constexpr int64_t kSeekToleranceMS = 10 * 1000;
            if (!key_frame || std::llabs((int64_t)dts - (int64_t)_seek_target_ms) > kSeekToleranceMS) {
                return;
            }
            _await_seek = false;
        }
        if (_in_gate) {
            if (!key_frame) {
                return;
            }
            _in_gate = false;
        }
        GET_CONFIG(size_t, max_pending, Mask::kMaxPendingFrames);
        if (_replay) {
            // 回放不丢帧（下载生成文件不能缺内容）：积压超过阈值时暂停回放读取，引擎发送缓冲排空后恢复
            // Replays never drop frames (generated downloads must be complete): pause reading the replay when the
            // backlog exceeds the threshold, resume when the engine send buffer drains
            _client->sendFrame(_epoch, key_frame, pts, buffer->data(), buffer->size());
            ++_sent_since_output;
            if (!_backpressure && _client->pendingMessages() > max_pending) {
                setBackpressure(true);
            }
            return;
        }
        if (_client->pendingMessages() > max_pending) {
            // 直播：引擎处理不过来时丢到下一个关键帧，绝不送出未打码的帧 / Live: engine overloaded, drop to the next keyframe
            _in_gate = true;
            if (_drop_log.elapsedTime() > 5000) {
                _drop_log.resetTime();
                WarnL << "mask: edge-mask backlog, dropping to next keyframe: " << _out_tuple.shortUrl();
            }
            return;
        }
        _client->sendFrame(_epoch, key_frame, pts, buffer->data(), buffer->size());
        ++_sent_since_output;
    });
}

void MaskSource::setBackpressure(bool on) {
    if (_backpressure == on || _state == State::kClosed) {
        return;
    }
    _backpressure = on;
    if (_drop_log.elapsedTime() > 5000) {
        _drop_log.resetTime();
        InfoL << "mask: replay back-pressure " << (on ? "on" : "off") << ": " << _out_tuple.shortUrl();
    }
    applySourcePause();
}

void MaskSource::applySourcePause() {
    if (!_replay) {
        return;
    }
    if (auto src = _src.lock()) {
        src->pause(_user_paused || _backpressure);
    }
}

void MaskSource::onEngineFrame(const MaskProto::FrameInfo &frame) {
    if (_state != State::kActive || frame.epoch != _epoch) {
        // seek 前的残帧 / Leftovers from before a seek
        return;
    }
    if (_out_gate) {
        if (!frame.keyFrame()) {
            return;
        }
        _out_gate = false;
    }
    // 引擎带回输入的 pts（微秒），换回毫秒；输出没有 B 帧，dts = pts
    // The engine echoes the input pts (microseconds); back to ms. The output has no B-frames, so dts = pts
    const uint64_t pts_ms = frame.pts_us > 0 ? static_cast<uint64_t>(frame.pts_us / 1000) : 0;
    auto out = Factory::getFrameFromPtr(_codec, frame.data, frame.size, pts_ms, pts_ms);
    if (!out) {
        return;
    }
    // 负载在回调返回后就失效，muxer 可能缓存帧，必须拷贝 / The payload dies after this callback and the muxer may cache frames
    _muxer->inputFrame(Frame::getCacheAbleFrame(out));
    _sent_since_output = 0;
    _last_output.resetTime();
}

void MaskSource::checkStall() {
    GET_CONFIG(uint32_t, stall_ms, Mask::kStallTimeoutMS);
    if (_sent_since_output > 0 && _last_output.elapsedTime() > stall_ms) {
        shutdown("edge-mask produced no output for " + to_string(_last_output.elapsedTime()) + "ms");
    }
}

void MaskSource::startGraceTimer() {
    if (_grace_timer) {
        _grace_timer->cancel();
    }
    GET_CONFIG(int, grace_sec, Mask::kGraceSec);
    weak_ptr<MaskSource> weak_self = shared_from_this();
    _grace_timer = _poller->doDelayTask(grace_sec * 1000, [weak_self]() {
        auto strong_self = weak_self.lock();
        if (strong_self && strong_self->_muxer && strong_self->_muxer->totalReaderCount() == 0) {
            strong_self->shutdown("no viewer");
        }
        return 0;
    });
}

void MaskSource::shutdown(const string &reason) {
    if (_state == State::kClosed) {
        return;
    }
    InfoL << "mask: close " << _out_tuple.shortUrl() << ", reason: " << reason;
    _state = State::kClosed;
    // 从注册表移除放到最后（异步）：注册表持有本对象，移除后它可能立即析构
    // Remove from the registry last (async): the registry owns this object, which may die right after
    auto waiters = std::move(_waiters);
    _waiters.clear();
    for (auto &w : waiters) {
        auto cb = w.second;
        w.first->async([cb]() { cb(false, "closed"); });
    }
    if (_grace_timer) {
        _grace_timer->cancel();
        _grace_timer = nullptr;
    }
    if (_stall_timer) {
        _stall_timer->cancel();
        _stall_timer = nullptr;
    }
    if (_client) {
        _client->close();
        _client = nullptr;
    }
    _reader = nullptr;
    // 注销打码流会断开所有观看者；可能处在 muxer 自己的调用栈里，延后释放
    // Dropping the muxer unregisters the masked stream and disconnects its viewers; we may be inside the
    // muxer's own call stack, so release it later
    shared_ptr<MultiMediaSourceMuxer> muxer;
    {
        lock_guard<mutex> lck(_muxer_mtx);
        muxer = std::move(_muxer);
    }
    auto key = _out_tuple.shortUrl();
    auto self = shared_from_this();
    _poller->async([muxer, key, self]() mutable {
        muxer = nullptr;
        {
            lock_guard<mutex> lck(s_mtx);
            auto it = s_sources.find(key);
            if (it != s_sources.end() && it->second == self) {
                s_sources.erase(it);
            }
        }
        self = nullptr;
    }, false);
}

MediaOriginType MaskSource::getOriginType(MediaSource &sender) const {
    return MediaOriginType::unknown;
}

string MaskSource::getOriginUrl(MediaSource &sender) const {
    return _origin_url;
}

// seek / 暂停 / 倍速只对回放打码流有效：转给回放会话，同步返回结果（RtspSession 立即读取时间戳），
// 打码会话的状态切换在 _poller 上完成
// seek / pause / speed apply to masked replays only: forwarded to the replay session synchronously (RtspSession
// reads the timestamp right away); the mask session's own state changes run on _poller
bool MaskSource::seekTo(MediaSource &sender, uint32_t stamp) {
    auto src = _src.lock();
    if (!_replay || !src) {
        return false;
    }
    // 先在 _poller 上切到新 epoch、关闸门、发 FLUSH，再让回放会话 seek：新位置的帧到达时一定已经属于新 epoch；
    // 旧位置还在路上的帧由 _await_seek 按时间戳丢掉
    // Switch to the new epoch, close the gates and send FLUSH on _poller first, then seek the replay: frames of
    // the new position always arrive under the new epoch; old frames still in flight are dropped by _await_seek
    auto self = shared_from_this();
    _poller->sync([self, stamp]() {
        if (self->_state == State::kClosed) {
            return;
        }
        ++self->_epoch;
        self->_merger.clear();
        self->_in_gate = true;
        self->_out_gate = true;
        self->_await_seek = true;
        self->_seek_target_ms = stamp;
        if (self->_state == State::kActive) {
            self->_client->flush(self->_epoch);
        }
    });
    bool ok = src->seekTo(stamp);
    if (!ok) {
        return false;
    }
    {
        lock_guard<mutex> lck(_muxer_mtx);
        if (_muxer) {
            // 丢掉 seek 前已缓存的打码帧，时间轴对齐到回放会话的新位置
            // Drop masked frames cached before the seek and align to the replay's new position
            _muxer->resetPacedSender(stamp);
            _muxer->setTimeStamp(src->getTimeStamp(TrackInvalid));
        }
    }
    return true;
}

bool MaskSource::pause(MediaSource &sender, bool pause) {
    auto src = _src.lock();
    if (!_replay || !src) {
        return false;
    }
    _user_paused = pause;
    // 背压暂停中时，用户的“恢复”不真正恢复读取，等引擎排空 / While back-pressured, a user resume waits for the drain
    weak_ptr<MaskSource> weak_self = shared_from_this();
    _poller->async([weak_self]() {
        if (auto strong_self = weak_self.lock()) {
            strong_self->applySourcePause();
        }
    });
    return true;
}

void MaskSource::askSpeed(float speed, const EventPoller::Ptr &caller, SpeedCB cb) {
    // 直播打码流、引擎会话还没建立、倍速没变：不用问引擎，交给 speed() 处理
    // Live masked stream, engine session not up, or unchanged speed: nothing to ask, speed() handles it
    if (!_replay || _state != State::kActive || speed == _speed) {
        _granted_speed = speed;
        _granted_epoch = _epoch;
        caller->async([cb]() { cb(true, ""); });
        return;
    }
    GET_CONFIG(int, open_timeout_ms, Mask::kOpenTimeoutMS);
    const uint32_t epoch = _epoch + 1;
    weak_ptr<MaskSource> weak_self = shared_from_this();
    _client->requestSpeed(speed, epoch, open_timeout_ms, [weak_self, speed, epoch, caller, cb](bool ok, const string &reason) {
        auto strong_self = weak_self.lock();
        if (!strong_self || strong_self->_state != State::kActive) {
            caller->async([cb]() { cb(false, "closed"); });
            return;
        }
        if (ok) {
            // 引擎已按新倍速重建分支；等调用方执行 speed() 时再切 epoch（届时回放会话才真正改倍速）
            // The engine rebuilt the branch for the new speed; the epoch switches when the caller runs speed()
            strong_self->_granted_speed = speed;
            strong_self->_granted_epoch = epoch;
        } else {
            WarnL << "mask: edge-mask refused speed " << speed << " for " << strong_self->_out_tuple.shortUrl() << ": " << reason;
        }
        caller->async([cb, ok, reason]() { cb(ok, reason); });
    });
}

bool MaskSource::speed(MediaSource &sender, float speed) {
    auto src = _src.lock();
    if (!_replay || !src || !src->speed(speed)) {
        return false;
    }
    weak_ptr<MaskSource> weak_self = shared_from_this();
    _poller->async([weak_self, speed]() {
        auto strong_self = weak_self.lock();
        if (!strong_self || strong_self->_state == State::kClosed) {
            return;
        }
        strong_self->_speed = speed;
        if (strong_self->_state == State::kActive && strong_self->_granted_speed == speed &&
            strong_self->_granted_epoch != strong_self->_epoch) {
            // 引擎已确认：从新倍速的关键帧开始、用新 epoch 送帧；旧 epoch 的输出不再分发
            // Confirmed by the engine: send from a keyframe at the new speed under the new epoch; old output is dropped
            strong_self->_epoch = strong_self->_granted_epoch;
            strong_self->_merger.clear();
            strong_self->_in_gate = true;
            strong_self->_out_gate = true;
        }
        strong_self->_granted_speed = 0;
    });
    return true;
}

bool MaskSource::close(MediaSource &sender) {
    weak_ptr<MaskSource> weak_self = shared_from_this();
    _poller->async([weak_self]() {
        if (auto strong_self = weak_self.lock()) {
            strong_self->shutdown("closed by api");
        }
    });
    return true;
}

int MaskSource::totalReaderCount(MediaSource &sender) {
    lock_guard<mutex> lck(_muxer_mtx);
    return _muxer ? _muxer->totalReaderCount() : 0;
}

void MaskSource::onReaderChanged(MediaSource &sender, int size) {
    weak_ptr<MaskSource> weak_self = shared_from_this();
    _poller->async([weak_self, size]() {
        auto strong_self = weak_self.lock();
        if (!strong_self || strong_self->_state == State::kClosed) {
            return;
        }
        if (size == 0) {
            strong_self->startGraceTimer();
        } else if (strong_self->_grace_timer) {
            strong_self->_grace_timer->cancel();
            strong_self->_grace_timer = nullptr;
        }
    });
}

EventPoller::Ptr MaskSource::getOwnerPoller(MediaSource &sender) {
    return _poller;
}

} // namespace mediakit
