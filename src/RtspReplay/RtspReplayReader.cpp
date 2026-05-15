/*
 * Copyright (c) 2016-present The ZLMediaKit project authors. All Rights Reserved.
 *
 * This file is part of ZLMediaKit(https://github.com/ZLMediaKit/ZLMediaKit).
 *
 * Use of this source code is governed by MIT-like license that can be found in the
 * LICENSE file in the root of the source tree. All contributing project authors
 * may be found in the AUTHORS file in the root of the source tree.
 */

#ifdef ENABLE_MP4

#include "RtspReplayReader.h"

#include "Common/config.h"
#include "Extension/Frame.h"
#include "Thread/WorkThreadPool.h"
#include "Util/logger.h"

#include <limits>
#include <stdexcept>

using namespace std;
using namespace toolkit;

namespace mediakit {

RtspReplayReader::RtspReplayReader(const MediaTuple &tuple, const RtspReplayCatalogResult &catalog, const ProtocolOption &option, toolkit::EventPoller::Ptr poller) {
    setup(tuple, catalog, option, std::move(poller));
}

void RtspReplayReader::setup(const MediaTuple &tuple, const RtspReplayCatalogResult &catalog, const ProtocolOption &option, toolkit::EventPoller::Ptr poller) {
    if (catalog.segments.empty()) {
        throw std::runtime_error("replay catalog is empty");
    }
    if (catalog.windowBeginAtMs >= catalog.windowEndAtMs) {
        throw std::runtime_error("invalid replay window");
    }

    _catalog = catalog;
    _base_file_begin_at_ms = _catalog.segments.front().beginAtMs;
    _window_begin_demux_ms = absoluteToDemux(_catalog.windowBeginAtMs);
    _window_end_demux_ms = absoluteToDemux(_catalog.windowEndAtMs);

    _file_list.reserve(_catalog.segments.size() * 128);
    for (size_t i = 0; i < _catalog.segments.size(); ++i) {
        if (i > 0) {
            _file_list.push_back(';');
        }
        _file_list.append(_catalog.segments[i].filePath);
    }
    _origin_url = _file_list;

    _poller = poller ? std::move(poller) : WorkThreadPool::Instance().getPoller();

    _demuxer = std::make_shared<MultiMP4Demuxer>();
    _demuxer->openMP4(_file_list);

    if (tuple.stream.empty()) {
        return;
    }

    // Replay should expose window duration to clients via SDP range, not full underlying file duration.
    auto replay_window_dur_sec = (_catalog.windowEndAtMs - _catalog.windowBeginAtMs) / 1000.0f;
    _muxer = std::make_shared<MultiMediaSourceMuxer>(tuple, replay_window_dur_sec, option);
    auto tracks = _demuxer->getTracks(false);
    if (tracks.empty()) {
        throw std::runtime_error("invalid replay tracks");
    }

    for (auto &track : tracks) {
        _muxer->addTrack(track);
        if (track->getTrackType() == TrackVideo) {
            _have_video = true;
        }
    }
    _muxer->addTrackCompleted();
}

void RtspReplayReader::bindTimeline(const std::shared_ptr<RtspReplayTimeline> &timeline) {
    _timeline = timeline;
}

bool RtspReplayReader::start(uint64_t sample_ms, bool ref_self, bool file_repeat) {
    GET_CONFIG(uint32_t, sampleMS, Record::kSampleMS);

    auto strong_self = shared_from_this();
    setCurrentDemuxStamp(_window_begin_demux_ms, false);

    if (_muxer) {
        while (!_muxer->isAllTrackReady() && readNextSample()) {
            // keep priming until tracks are ready
        }
        _muxer->setMediaListener(strong_self);
    }

    if (!seekToDemux((uint32_t)_window_begin_demux_ms, true)) {
        return false;
    }

    auto actual_at = demuxToAbsolute(getCurrentDemuxStamp());
    if (_timeline) {
        _timeline->onStarted(actual_at);
        _timeline->onPauseChanged(false);
        _timeline->onSpeedChanged(1.0f);
        _session_origin_demux_ms = absoluteToDemux(_timeline->sessionOriginAt());
        _timeline_started = true;
    }
    if (_muxer) {
        _muxer->setTimeStamp(0);
    }

    _file_repeat = file_repeat;
    auto timer_sec = (sample_ms ? sample_ms : sampleMS) / 1000.0f;
    if (ref_self) {
        _timer = std::make_shared<Timer>(timer_sec, [strong_self]() {
            lock_guard<recursive_mutex> lck(strong_self->_mtx);
            return strong_self->readSample();
        }, _poller);
    } else {
        weak_ptr<RtspReplayReader> weak_self = strong_self;
        _timer = std::make_shared<Timer>(timer_sec, [weak_self]() {
            auto strong_self_2 = weak_self.lock();
            if (!strong_self_2) {
                return false;
            }
            lock_guard<recursive_mutex> lck(strong_self_2->_mtx);
            return strong_self_2->readSample();
        }, _poller);
    }
    return true;
}

void RtspReplayReader::stop() {
    _timer = nullptr;
}

uint64_t RtspReplayReader::firstPlayableAt() const {
    if (_timeline_started && _timeline) {
        return _timeline->sessionOriginAt();
    }
    return demuxToAbsolute((uint32_t)_window_begin_demux_ms);
}

uint64_t RtspReplayReader::currentAt() const {
    if (_timeline_started && _timeline) {
        return _timeline->currentAt();
    }
    return demuxToAbsolute(getCurrentDemuxStamp());
}

bool RtspReplayReader::readSample() {
    if (_paused) {
        _seek_ticker.resetTime();
        return true;
    }

    bool keyFrame = false;
    bool eof = false;
    auto cur_stamp = getCurrentDemuxStamp();
    while (!eof && _last_dts < cur_stamp) {
        auto frame = _demuxer->readFrame(keyFrame, eof);
        if (!frame) {
            continue;
        }
        _last_dts = frame->dts();
        if (_window_end_demux_ms > 0 && _last_dts >= _window_end_demux_ms) {
            eof = true;
            break;
        }
        if (_muxer) {
            _muxer->inputFrame(remapFrameToSessionNpt(frame));
        }
    }

    if (_timeline_started && _timeline) {
        auto progress_stamp = cur_stamp;
        if ((uint64_t)progress_stamp < _window_begin_demux_ms) {
            progress_stamp = (uint32_t)_window_begin_demux_ms;
        }
        if (_window_end_demux_ms > 0 && (uint64_t)progress_stamp >= _window_end_demux_ms) {
            progress_stamp = (uint32_t)(_window_end_demux_ms - 1);
        }
        _timeline->onProgressed(demuxToAbsolute(progress_stamp));
    }

    GET_CONFIG(bool, file_repeat, Record::kFileRepeat);
    if (eof && (file_repeat || _file_repeat)) {
        return seekToDemux((uint32_t)_window_begin_demux_ms, true);
    }
    return !eof;
}

bool RtspReplayReader::readNextSample() {
    bool keyFrame = false;
    bool eof = false;
    auto frame = _demuxer->readFrame(keyFrame, eof);
    if (!frame) {
        return false;
    }
    if (_muxer) {
        _muxer->inputFrame(remapFrameToSessionNpt(frame));
    }
    setCurrentDemuxStamp(frame->dts(), false);
    return true;
}

uint32_t RtspReplayReader::getCurrentDemuxStamp() const {
    return (uint32_t)(_seek_to + !_paused * _speed * _seek_ticker.elapsedTime());
}

void RtspReplayReader::setCurrentDemuxStamp(uint32_t stamp, bool sync_timeline) {
    auto old_stamp = getCurrentDemuxStamp();
    _seek_to = stamp;
    _last_dts = stamp;
    _seek_ticker.resetTime();

    if (old_stamp != stamp && _muxer) {
        if (sync_timeline && _timeline_started && _timeline) {
            _timeline->onSeekCompleted(demuxToAbsolute(stamp));
            _muxer->setTimeStamp(_timeline->currentNptMs());
        } else {
            _muxer->setTimeStamp(stamp);
        }
    }
}

bool RtspReplayReader::seekToDemux(uint32_t stamp_seek, bool allow_tail_fallback) {
    uint64_t target_seek = stamp_seek;
    if (target_seek < _window_begin_demux_ms) {
        target_seek = _window_begin_demux_ms;
    }
    if (_window_end_demux_ms > 0 && target_seek >= _window_end_demux_ms) {
        target_seek = _window_end_demux_ms - 1;
    }
    if (target_seek > _demuxer->getDurationMS()) {
        return false;
    }

    if (_muxer && _timeline_started) {
        // 在读取seek后首帧之前先重同步paced sender，避免先flush旧缓存再发旧包。
        auto npt_seek = target_seek > _session_origin_demux_ms ? (target_seek - _session_origin_demux_ms) : 0;
        _muxer->resetPacedSender(npt_seek);
    }

    auto stamp = _demuxer->seekTo((int64_t)target_seek);
    if (stamp == -1) {
        return false;
    }

    if (!_have_video) {
        setCurrentDemuxStamp((uint32_t)stamp, true);
        return true;
    }

    bool keyFrame = false;
    bool eof = false;
    while (!eof) {
        auto frame = _demuxer->readFrame(keyFrame, eof);
        if (!frame) {
            continue;
        }
        if (keyFrame || frame->keyFrame() || frame->configFrame()) {
            if (_muxer) {
                _muxer->inputFrame(remapFrameToSessionNpt(frame));
            }
            setCurrentDemuxStamp(frame->dts(), true);
            return true;
        }
    }

    if (!allow_tail_fallback) {
        return false;
    }

    WarnL << "replay seek reached tail without forward keyframe, fallback to stamp=" << stamp;
    setCurrentDemuxStamp((uint32_t)stamp, true);
    return true;
}

uint32_t RtspReplayReader::absoluteToDemux(uint64_t abs_ms) const {
    if (abs_ms <= _base_file_begin_at_ms) {
        return 0;
    }
    auto delta = abs_ms - _base_file_begin_at_ms;
    if (delta > std::numeric_limits<uint32_t>::max()) {
        return std::numeric_limits<uint32_t>::max();
    }
    return (uint32_t)delta;
}

uint64_t RtspReplayReader::demuxToAbsolute(uint32_t demux_ms) const {
    return _base_file_begin_at_ms + demux_ms;
}

Frame::Ptr RtspReplayReader::remapFrameToSessionNpt(const Frame::Ptr &frame) const {
    if (!frame || !_timeline_started) {
        return frame;
    }

    auto dts = (int64_t)frame->dts() - (int64_t)_session_origin_demux_ms;
    auto pts = (int64_t)frame->pts() - (int64_t)_session_origin_demux_ms;
    if (dts < 0) {
        dts = 0;
    }
    if (pts < dts) {
        pts = dts;
    }

    auto stamped = std::make_shared<FrameStamp>(frame);
    stamped->setStamp(dts, pts);
    return stamped;
}

bool RtspReplayReader::seekTo(MediaSource &sender, uint32_t stamp) {
    pause(sender, false);
    if (!_timeline_started || !_timeline) {
        return false;
    }

    auto target_abs = _timeline->resolvePlayTargetFromNpt(stamp);
    auto target_demux = absoluteToDemux(target_abs);
    TraceL << getOriginUrl(sender) << ",npt_ms:" << stamp << ",target_abs:" << target_abs << ",target_demux:" << target_demux;
    return seekToDemux(target_demux, true);
}

bool RtspReplayReader::pause(MediaSource &sender, bool pause_value) {
    if (_paused == pause_value) {
        return true;
    }
    setCurrentDemuxStamp(getCurrentDemuxStamp(), true);
    _paused = pause_value;
    if (_timeline_started && _timeline) {
        _timeline->onPauseChanged(pause_value);
    }
    TraceL << getOriginUrl(sender) << ",pause:" << pause_value;
    return true;
}

bool RtspReplayReader::speed(MediaSource &sender, float speed_value) {
    if (speed_value < 0.1f || speed_value > 20.0f) {
        WarnL << "invalid replay speed:" << speed_value;
        return false;
    }

    setCurrentDemuxStamp(getCurrentDemuxStamp(), true);
    _paused = false;
    if (_speed == speed_value) {
        return true;
    }

    _speed = speed_value;
    if (_timeline_started && _timeline) {
        _timeline->onPauseChanged(false);
        _timeline->onSpeedChanged(speed_value);
    }
    TraceL << getOriginUrl(sender) << ",speed:" << speed_value;
    return true;
}

bool RtspReplayReader::close(MediaSource &sender) {
    _timer = nullptr;
    WarnL << "close replay media: " << sender.getUrl();
    return true;
}

MediaOriginType RtspReplayReader::getOriginType(MediaSource &sender) const {
    return MediaOriginType::mp4_vod;
}

string RtspReplayReader::getOriginUrl(MediaSource &sender) const {
    return _origin_url;
}

toolkit::EventPoller::Ptr RtspReplayReader::getOwnerPoller(MediaSource &sender) {
    return _poller;
}

} // namespace mediakit

#endif // ENABLE_MP4
