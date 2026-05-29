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

#include <algorithm>
#include <chrono>
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

    if (tuple.stream.empty()) {
        return;
    }

    auto replay_window_dur_sec = (_catalog.windowEndAtMs - _catalog.windowBeginAtMs) / 1000.0f;
    _muxer = std::make_shared<MultiMediaSourceMuxer>(tuple, replay_window_dur_sec, option);
    size_t probe_index = 0;
    for (size_t i = 0; i < _catalog.segments.size(); ++i) {
        const auto &seg = _catalog.segments[i];
        if (_catalog.windowBeginAtMs >= seg.beginAtMs && _catalog.windowBeginAtMs < seg.endAtMs) {
            probe_index = i;
            break;
        }
    }

    const auto &probe_segment = _catalog.segments[probe_index];
    auto probe_demuxer = std::make_shared<MP4Demuxer>();
    auto probe_open_begin = std::chrono::steady_clock::now();
    probe_demuxer->openMP4(probe_segment.filePath);
    _perf_stats.setup_probe_open_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - probe_open_begin)
            .count();

    auto tracks = probe_demuxer->getTracks(false);
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
    lock_guard<recursive_mutex> lck(_mtx);

    if (_timer) {
        return true;
    }

    auto start_begin = std::chrono::steady_clock::now();
    int64_t demux_open_ms = 0;
    int64_t prime_track_ms = 0;
    int64_t seek_ms = 0;

    GET_CONFIG(uint32_t, sampleMS, Record::kSampleMS);

    if (!_demuxer) {
        auto demux_open_begin = std::chrono::steady_clock::now();
        if (!openSegmentByDemuxStamp((uint32_t)_window_begin_demux_ms)) {
            return false;
        }
        demux_open_ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - demux_open_begin).count();
    }

    auto strong_self = shared_from_this();
    setCurrentDemuxStamp(_window_begin_demux_ms, false);

    if (_muxer) {
        auto prime_track_begin = std::chrono::steady_clock::now();
        while (!_muxer->isAllTrackReady() && readNextSample()) {
            // keep priming until tracks are ready
        }
        prime_track_ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - prime_track_begin).count();
        _muxer->setMediaListener(strong_self);
    }

    auto seek_begin = std::chrono::steady_clock::now();
    if (!seekToDemux((uint32_t)_window_begin_demux_ms, true, false)) {
        return false;
    }
    seek_ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - seek_begin).count();

    auto actual_at = demuxToAbsolute(getCurrentDemuxStamp());
    auto ext_base_ms = demuxToAbsolute((uint32_t)_window_begin_demux_ms);
    if (_timeline) {
        _timeline->onStarted(actual_at);
        _timeline->onPauseChanged(false);
        _timeline->onSpeedChanged(1.0f);
        _session_origin_demux_ms = absoluteToDemux(_timeline->sessionOriginAt());
        ext_base_ms = _timeline->sessionOriginAt();
        _timeline_started = true;
    }
    if (_muxer) {
        _muxer->setRtpExtTimeBaseMS(ext_base_ms);
        _muxer->setTimeStamp(0);
    }

    auto start_total_ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start_begin).count();
    _perf_stats.start_total_ms = start_total_ms;
    _perf_stats.demux_open_ms = demux_open_ms;
    _perf_stats.prime_track_ms = prime_track_ms;
    _perf_stats.seek_ms = seek_ms;

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
    lock_guard<recursive_mutex> lck(_mtx);
    _timer = nullptr;
}

const RtspReplayReader::PerfStats &RtspReplayReader::getPerfStats() const {
    return _perf_stats;
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
        auto frame = readFrameWithSegmentSwitch(keyFrame, eof);
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
        return seekToDemux((uint32_t)_window_begin_demux_ms, true, true);
    }
    return !eof;
}

bool RtspReplayReader::readNextSample() {
    bool keyFrame = false;
    bool eof = false;
    auto frame = readFrameWithSegmentSwitch(keyFrame, eof);
    if (!frame) {
        return false;
    }
    if (_muxer) {
        _muxer->inputFrame(remapFrameToSessionNpt(frame));
    }
    setCurrentDemuxStamp(frame->dts(), false);
    return true;
}

bool RtspReplayReader::openSegmentByIndex(size_t segment_index, uint64_t local_seek_ms) {
    if (segment_index >= _catalog.segments.size()) {
        return false;
    }

    const auto &segment = _catalog.segments[segment_index];
    auto demuxer = std::make_shared<MP4Demuxer>();
    demuxer->openMP4(segment.filePath);

    auto duration_ms = demuxer->getDurationMS();
    auto seek_ms = local_seek_ms;
    if (duration_ms > 0 && seek_ms >= duration_ms) {
        seek_ms = duration_ms - 1;
    }

    if (demuxer->seekTo((int64_t)seek_ms) == -1) {
        if (seek_ms == 0 || demuxer->seekTo(0) == -1) {
            return false;
        }
    }

    _demuxer = std::move(demuxer);
    _active_segment_index = segment_index;
    _active_segment_begin_demux_ms = absoluteToDemux(segment.beginAtMs);
    _active_segment_end_demux_ms = absoluteToDemux(segment.endAtMs);
    return true;
}

size_t RtspReplayReader::locateSegmentByAbsolute(uint64_t abs_ms) const {
    for (size_t i = 0; i < _catalog.segments.size(); ++i) {
        const auto &segment = _catalog.segments[i];
        if (abs_ms < segment.beginAtMs) {
            // Target hits a gap, return the first segment after the gap.
            return i;
        }
        if (abs_ms < segment.endAtMs) {
            return i;
        }
    }
    return _catalog.segments.size();
}

bool RtspReplayReader::openSegmentByDemuxStamp(uint32_t target_demux_ms) {
    auto target_abs_ms = demuxToAbsolute(target_demux_ms);
    auto segment_index = locateSegmentByAbsolute(target_abs_ms);
    if (segment_index >= _catalog.segments.size()) {
        return false;
    }

    const auto &segment = _catalog.segments[segment_index];
    uint64_t local_seek_ms = 0;
    if (target_abs_ms > segment.beginAtMs) {
        local_seek_ms = target_abs_ms - segment.beginAtMs;
    }
    return openSegmentByIndex(segment_index, local_seek_ms);
}

Frame::Ptr RtspReplayReader::readFrameWithSegmentSwitch(bool &keyFrame, bool &eof) {
    keyFrame = false;
    eof = false;

    while (_demuxer) {
        auto frame = _demuxer->readFrame(keyFrame, eof);
        if (frame) {
            auto global_dts = _active_segment_begin_demux_ms + (uint64_t)frame->dts();
            auto global_pts = _active_segment_begin_demux_ms + (uint64_t)frame->pts();
            if (global_pts < global_dts) {
                global_pts = global_dts;
            }

            if (global_dts < _window_begin_demux_ms) {
                continue;
            }
            if (_active_segment_end_demux_ms > 0 && global_dts >= _active_segment_end_demux_ms) {
                eof = true;
            } else if (_window_end_demux_ms > 0 && global_dts >= _window_end_demux_ms) {
                eof = true;
            } else {
                auto stamped = std::make_shared<FrameStamp>(frame);
                stamped->setStamp((int64_t)global_dts, (int64_t)global_pts);
                return stamped;
            }
        }

        if (!eof) {
            return nullptr;
        }

        auto next_segment_index = _active_segment_index + 1;
        while (next_segment_index < _catalog.segments.size()) {
            const auto &next_segment = _catalog.segments[next_segment_index];
            if (_window_end_demux_ms > 0 && absoluteToDemux(next_segment.beginAtMs) >= _window_end_demux_ms) {
                eof = true;
                return nullptr;
            }
            if (openSegmentByIndex(next_segment_index, 0)) {
                eof = false;
                break;
            }
            ++next_segment_index;
        }

        if (next_segment_index >= _catalog.segments.size()) {
            eof = true;
            return nullptr;
        }
    }

    eof = true;
    return nullptr;
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

bool RtspReplayReader::seekToDemux(uint32_t stamp_seek, bool allow_tail_fallback, bool reopen_demux) {
    uint64_t target_seek = stamp_seek;
    if (target_seek < _window_begin_demux_ms) {
        target_seek = _window_begin_demux_ms;
    }
    if (_window_end_demux_ms > 0 && target_seek >= _window_end_demux_ms) {
        target_seek = _window_end_demux_ms - 1;
    }

    auto target_abs_ms = demuxToAbsolute((uint32_t)target_seek);
    auto target_segment_index = locateSegmentByAbsolute(target_abs_ms);
    if (target_segment_index >= _catalog.segments.size()) {
        return false;
    }

    const auto &target_segment = _catalog.segments[target_segment_index];
    auto target_in_segment = target_abs_ms >= target_segment.beginAtMs && target_abs_ms < target_segment.endAtMs;

    if (_muxer && _timeline_started) {
        auto npt_seek = target_seek > _session_origin_demux_ms ? (target_seek - _session_origin_demux_ms) : 0;
        _muxer->resetPacedSender(npt_seek);
    }

    size_t segment_index = target_segment_index;
    while (segment_index < _catalog.segments.size()) {
        const auto &segment = _catalog.segments[segment_index];
        auto prefer_before_target = segment_index == target_segment_index && target_in_segment;
        uint64_t local_target_ms = 0;
        if (segment_index == target_segment_index && target_abs_ms > segment.beginAtMs) {
            local_target_ms = target_abs_ms - segment.beginAtMs;
        }

        auto need_reopen = reopen_demux || !_demuxer || _active_segment_index != segment_index;
        if (need_reopen && !openSegmentByIndex(segment_index, local_target_ms)) {
            ++segment_index;
            reopen_demux = true;
            continue;
        }

        if (!need_reopen && _demuxer->seekTo((int64_t)local_target_ms) == -1) {
            ++segment_index;
            reopen_demux = true;
            continue;
        }

        bool key_frame = false;
        bool eof = false;
        while (!eof) {
            auto frame = _demuxer->readFrame(key_frame, eof);
            if (!frame) {
                continue;
            }

            auto playable = !_have_video || key_frame || frame->keyFrame() || frame->configFrame();
            if (!playable) {
                continue;
            }

            auto global_dts = _active_segment_begin_demux_ms + (uint64_t)frame->dts();
            auto global_pts = _active_segment_begin_demux_ms + (uint64_t)frame->pts();
            if (global_pts < global_dts) {
                global_pts = global_dts;
            }
            if (global_dts < _window_begin_demux_ms) {
                continue;
            }
            if (_window_end_demux_ms > 0 && global_dts >= _window_end_demux_ms) {
                break;
            }

            auto stamped = std::make_shared<FrameStamp>(frame);
            stamped->setStamp((int64_t)global_dts, (int64_t)global_pts);
            if (_muxer) {
                _muxer->inputFrame(remapFrameToSessionNpt(stamped));
            }
            setCurrentDemuxStamp((uint32_t)global_dts, true);

            DebugL << "replay seek: target_abs_ms=" << target_abs_ms
                   << ", seek_hit_type=" << (prefer_before_target ? "in-segment" : "hole-forward")
                   << ", segment_index=" << segment_index
                   << ", actual_play_abs_ms=" << demuxToAbsolute((uint32_t)global_dts)
                   << ", delta_ms=" << ((int64_t)demuxToAbsolute((uint32_t)global_dts) - (int64_t)target_abs_ms);
            return true;
        }

        ++segment_index;
        reopen_demux = true;
    }

    if (!allow_tail_fallback) {
        return false;
    }

    WarnL << "replay seek fallback without playable frame, target_abs_ms=" << target_abs_ms;
    setCurrentDemuxStamp((uint32_t)target_seek, true);
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
    lock_guard<recursive_mutex> lck(_mtx);

    if (!_timeline_started || !_timeline) {
        if (!start(0, true, false)) {
            return false;
        }
    }

    pause(sender, false);

    auto target_abs = _timeline->resolvePlayTargetFromNpt(stamp);
    auto target_demux = absoluteToDemux(target_abs);
    TraceL << getOriginUrl(sender) << ",npt_ms:" << stamp << ",target_abs:" << target_abs << ",target_demux:" << target_demux;
    return seekToDemux(target_demux, true);
}

bool RtspReplayReader::pause(MediaSource &sender, bool pause_value) {
    lock_guard<recursive_mutex> lck(_mtx);

    if (!_timeline_started) {
        if (!start(0, true, false)) {
            return false;
        }
    }

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
    lock_guard<recursive_mutex> lck(_mtx);

    if (!_timeline_started) {
        if (!start(0, true, false)) {
            return false;
        }
    }

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
    lock_guard<recursive_mutex> lck(_mtx);
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
