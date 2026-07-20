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
    if (catalog._segments.empty()) {
        throw std::runtime_error("replay catalog is empty");
    }
    if (catalog._window_begin_at_ms >= catalog._window_end_at_ms) {
        throw std::runtime_error("invalid replay window");
    }

    if (tuple.stream.empty()) {
        return;
    }

    _catalog = catalog;
    _base_file_begin_at_ms = _catalog._segments.front()._begin_at_ms;
    _window_begin_at_ms = _catalog._window_begin_at_ms;
    _window_end_at_ms = _catalog._window_end_at_ms;
    _window_begin_offset_ms = absoluteToOffset(_window_begin_at_ms);
    _window_end_offset_ms = absoluteToOffset(_window_end_at_ms);

    std::string file_list;
    file_list.reserve(_catalog._segments.size() * 128);
    for (size_t i = 0; i < _catalog._segments.size(); ++i) {
        if (i > 0) {
            file_list.push_back(';');
        }
        file_list.append(_catalog._segments[i]._file_path);
    }
    _origin_url = file_list;

    _poller = poller ? std::move(poller) : WorkThreadPool::Instance().getPoller();

   
    auto replay_window_dur_sec = (_catalog._window_end_at_ms - _catalog._window_begin_at_ms) / 1000.0f;
    _muxer = std::make_shared<MultiMediaSourceMuxer>(tuple, replay_window_dur_sec, option);
    size_t probe_index = 0;
    for (size_t i = 0; i < _catalog._segments.size(); ++i) {
        const auto &seg = _catalog._segments[i];
        if (_catalog._window_begin_at_ms >= seg._begin_at_ms && _catalog._window_begin_at_ms < seg._end_at_ms) {
            probe_index = i;
            break;
        }
    }

    const auto &probe_segment = _catalog._segments[probe_index];
    auto probe_demuxer = std::make_shared<MP4Demuxer>();
    auto probe_open_begin = std::chrono::steady_clock::now();
    probe_demuxer->openMP4(probe_segment._file_path);
    _perf_stats._setup_probe_open_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
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

uint64_t RtspReplayReader::clampToWindow(uint64_t abs_ms) const {
    if (abs_ms < _window_begin_at_ms) {
        return _window_begin_at_ms;
    }
    auto window_max = _window_end_at_ms - 1;
    if (abs_ms > window_max) {
        return window_max;
    }
    return abs_ms;
}

void RtspReplayReader::onStarted(uint64_t actual_at_ms) {
    if (_started) {
        // Should never happen; guard instead of re-anchoring the already-started timeline.
        WarnL << "replay timeline already started, ignore duplicate onStarted";
        return;
    }
    _current_at_ms = clampToWindow(actual_at_ms);
    _started = true;
}

void RtspReplayReader::onProgressed(uint64_t actual_at_ms) {
    if (!_started) {
        WarnL << "replay timeline not started, ignore onProgressed";
        return;
    }
    _current_at_ms = clampToWindow(actual_at_ms);
}

void RtspReplayReader::onSeekCompleted(uint64_t actual_at_ms) {
    if (!_started) {
        WarnL << "replay timeline not started, ignore onSeekCompleted";
        return;
    }
    _current_at_ms = clampToWindow(actual_at_ms);
}

uint64_t RtspReplayReader::resolvePlayTargetFromNpt(uint32_t npt_ms) const {
    if (!_started) {
        WarnL << "replay timeline not started, clamp seek target to window begin";
        return _window_begin_at_ms;
    }
    auto target = _window_begin_at_ms + npt_ms;
    return clampToWindow(target);
}

uint32_t RtspReplayReader::currentNptMs() const {
    if (!_started || _current_at_ms < _window_begin_at_ms) {
        return 0;
    }
    auto delta = _current_at_ms - _window_begin_at_ms;
    if (delta > std::numeric_limits<uint32_t>::max()) {
        return std::numeric_limits<uint32_t>::max();
    }
    return static_cast<uint32_t>(delta);
}

bool RtspReplayReader::start() {
    lock_guard<recursive_mutex> lck(_mtx);

    if (_timer) {
        return true;
    }

    auto start_begin = std::chrono::steady_clock::now();
    int64_t demux_open_ms = 0;
    int64_t prime_track_ms = 0;

    if (!_demuxer) {
        auto demux_open_begin = std::chrono::steady_clock::now();
        if (!openSegmentByOffset((uint32_t)_window_begin_offset_ms)) {
            return false;
        }
        demux_open_ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - demux_open_begin).count();
    }

    auto strong_self = shared_from_this();
    setCurrentOffset(_window_begin_offset_ms, false);

    // Mark the session as started and fix the NPT origin BEFORE priming tracks so
    // that primed frames are remapped to 0-based NPT. Otherwise primed frames carry
    // the large file-offset stamp, and the paced sender baselines on that big value,
    // forcing a dts-decrease cache flush once the first playback frame arrives.
    _paused = false;
    onStarted(offsetToAbsolute(getCurrentOffset()));

    if (_muxer) {
        auto prime_track_begin = std::chrono::steady_clock::now();
        while (!_muxer->isAllTrackReady() && readNextSample()) {
            // keep priming until tracks are ready
        }
        prime_track_ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - prime_track_begin).count();
        _muxer->setMediaListener(strong_self);
    }

    // Priming repeatedly pushes the file offset into the muxer timestamp via
    // setCurrentOffset(); re-anchor the source timeline to 0 afterwards.
    if (_muxer) {
        _muxer->setRtpExtTimeBaseMS(_window_begin_at_ms);
        _muxer->setTimeStamp(0);
    }

    auto start_total_ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start_begin).count();
    _perf_stats._start_total_ms = start_total_ms;
    _perf_stats._demux_open_ms = demux_open_ms;
    _perf_stats._prime_track_ms = prime_track_ms;

    GET_CONFIG(uint32_t, sampleMS, Record::kSampleMS);
    auto timer_sec = sampleMS / 1000.0f;

    _timer = std::make_shared<Timer>(timer_sec, [strong_self]() {
        lock_guard<recursive_mutex> lck(strong_self->_mtx);
        return strong_self->readSample();
    }, _poller);

    return true;
}

void RtspReplayReader::stop() {
    lock_guard<recursive_mutex> lck(_mtx);
    _timer = nullptr;
}

const RtspReplayReader::PerfStats &RtspReplayReader::getPerfStats() const {
    return _perf_stats;
}


bool RtspReplayReader::readSample() {
    if (_paused) {
        _seek_ticker.resetTime();
        return true;
    }

    bool keyFrame = false;
    bool eof = false;
    auto cur_offset = getCurrentOffset();
    while (!eof && _last_dts < cur_offset) {
        auto frame = readFrameWithSegmentSwitch(keyFrame, eof);
        if (!frame) {
            // No frame available this tick (transient demuxer miss or eof). Position has not
            // advanced, so re-looping would spin on _mtx; bail out and retry next timer tick.
            break;
        }
        _last_dts = frame->dts();
        if (_window_end_offset_ms > 0 && _last_dts >= _window_end_offset_ms) {
            eof = true;
            break;
        }
        // Hole-forward: a genuine recording gap leaves the next playable frame far beyond the
        // real-time playhead (its dts jumps by a whole gap, not the sub-frame overshoot of normal
        // catch-up). Snap the timeline (and paced sender) forward onto this frame instead of
        // letting getCurrentOffset() crawl through the gap in real time with no output, which would
        // stall the TCP stream long enough for the client to declare a timeout and TEARDOWN.
        // The threshold keeps normal playback (where each batch's last frame slightly overshoots
        // cur_offset) from being mistaken for a hole and triggering spurious resyncs.
        static constexpr uint32_t kHoleForwardThresholdMs = 1000;
        if (_last_dts > cur_offset + kHoleForwardThresholdMs) {
            setCurrentOffset(_last_dts, true);
            cur_offset = _last_dts;
            if (_muxer) {
                _muxer->resetPacedSender(currentNptMs());
            }
        }
        if (_muxer) {
            _muxer->inputFrame(remapFrameToSessionNpt(frame));                                                                                                                                  
        }
    }

    if (_started) {
        auto progress_offset = cur_offset;
        if ((uint64_t)progress_offset < _window_begin_offset_ms) {
            progress_offset = (uint32_t)_window_begin_offset_ms;
        }
        if (_window_end_offset_ms > 0 && (uint64_t)progress_offset >= _window_end_offset_ms) {
            progress_offset = (uint32_t)(_window_end_offset_ms - 1);
        }
        onProgressed(offsetToAbsolute(progress_offset));
    }

    GET_CONFIG(bool, file_repeat, Record::kFileRepeat);
    if (eof && (file_repeat)) {
        return seekToOffset((uint32_t)_window_begin_offset_ms);
    }
    if(eof){
        _timer = nullptr;
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
    setCurrentOffset(frame->dts(), false);
    return true;
}

bool RtspReplayReader::openSegmentByIndex(size_t segment_index, uint64_t local_seek_ms) {
    if (segment_index >= _catalog._segments.size()) {
        return false;
    }

    const auto &segment = _catalog._segments[segment_index];
    auto demuxer = std::make_shared<MP4Demuxer>();
    try {
        demuxer->openMP4(segment._file_path);
    } catch (std::exception &ex) {
        // A corrupted/unreadable segment must not crash the timer thread; return false so the
        // caller skips it and advances to the next segment.
        WarnL << "replay: failed to open MP4 segment: " << segment._file_path << ", err: " << ex.what();
        return false;
    }

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
    _active_segment_begin_offset_ms = absoluteToOffset(segment._begin_at_ms);
    _active_segment_end_offset_ms = absoluteToOffset(segment._end_at_ms);
    return true;
}

size_t RtspReplayReader::locateSegmentByAbsolute(uint64_t abs_ms) const {
    for (size_t i = 0; i < _catalog._segments.size(); ++i) {
        const auto &segment = _catalog._segments[i];
        if (abs_ms < segment._begin_at_ms) {
            // Target hits a gap, return the first segment after the gap.
            return i;
        }
        if (abs_ms < segment._end_at_ms) {
            return i;
        }
    }
    return _catalog._segments.size();
}

bool RtspReplayReader::openSegmentByOffset(uint32_t target_offset_ms) {
    auto target_abs_ms = offsetToAbsolute(target_offset_ms);
    auto segment_index = locateSegmentByAbsolute(target_abs_ms);
    if (segment_index >= _catalog._segments.size()) {
        return false;
    }

    const auto &segment = _catalog._segments[segment_index];
    uint64_t local_seek_ms = 0;
    if (target_abs_ms > segment._begin_at_ms) {
        local_seek_ms = target_abs_ms - segment._begin_at_ms;
    }
    return openSegmentByIndex(segment_index, local_seek_ms);
}

Frame::Ptr RtspReplayReader::readFrameWithSegmentSwitch(bool &keyFrame, bool &eof) {
    keyFrame = false;
    eof = false;

    while (_demuxer) {
        auto frame = _demuxer->readFrame(keyFrame, eof);
        if (frame) {
            auto global_dts = _active_segment_begin_offset_ms + (uint64_t)frame->dts();
            auto global_pts = _active_segment_begin_offset_ms + (uint64_t)frame->pts();
            if (global_pts < global_dts) {
                global_pts = global_dts;
            }

            if (global_dts < _window_begin_offset_ms) {
                continue;
            }
            if (_active_segment_end_offset_ms > 0 && global_dts >= _active_segment_end_offset_ms) {
                eof = true;
            } else if (_window_end_offset_ms > 0 && global_dts >= _window_end_offset_ms) {
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
        while (next_segment_index < _catalog._segments.size()) {
            const auto &next_segment = _catalog._segments[next_segment_index];
            if (_window_end_offset_ms > 0 && absoluteToOffset(next_segment._begin_at_ms) >= _window_end_offset_ms) {
                eof = true;
                return nullptr;
            }
            if (openSegmentByIndex(next_segment_index, 0)) {
                eof = false;
                break;
            }
            ++next_segment_index;
        }

        if (next_segment_index >= _catalog._segments.size()) {
            eof = true;
            return nullptr;
        }
    }

    eof = true;
    return nullptr;
}

uint32_t RtspReplayReader::getCurrentOffset() const {
    // Compute in double to avoid 32-bit float mantissa loss: the offset is relative to
    // the first segment start and can span many days (tens of millions of ms), well beyond
    // float's ~16.7M precise integer range, which would otherwise corrupt progress/seek.
    auto advanced = _paused ? 0.0 : (double)_speed * _seek_ticker.elapsedTime();
    return (uint32_t)((double)_seek_to + advanced);
}

void RtspReplayReader::setCurrentOffset(uint32_t offset_ms, bool sync_timeline) {
    auto old_offset = getCurrentOffset();
    _seek_to = offset_ms;
    _last_dts = offset_ms;
    _seek_ticker.resetTime();

    if (old_offset != offset_ms && _muxer) {
        if (sync_timeline && _started) {
            onSeekCompleted(offsetToAbsolute(offset_ms));
            _muxer->setTimeStamp(currentNptMs());
        } else {
            _muxer->setTimeStamp(offset_ms);
        }
    }
}

bool RtspReplayReader::seekToOffset(uint32_t offset_seek_ms) {
    uint64_t target_seek = offset_seek_ms;
    if (target_seek < _window_begin_offset_ms) {
        target_seek = _window_begin_offset_ms;
    }
    if (_window_end_offset_ms > 0 && target_seek >= _window_end_offset_ms) {
        target_seek = _window_end_offset_ms - 1;
    }

    auto target_abs_ms = offsetToAbsolute((uint32_t)target_seek);
    auto target_segment_index = locateSegmentByAbsolute(target_abs_ms);
    if (target_segment_index >= _catalog._segments.size()) {
        WarnL << "replay seek beyond last segment, tail fallback, target_abs_ms=" << target_abs_ms;
        if (_muxer && _started) {
            _muxer->resetPacedSender((uint32_t)offsetToSessionNpt(target_seek));
        }
        setCurrentOffset((uint32_t)target_seek, true);
        return true;
    }

    const auto &target_segment = _catalog._segments[target_segment_index];
    auto target_in_segment = target_abs_ms >= target_segment._begin_at_ms && target_abs_ms < target_segment._end_at_ms;

    size_t segment_index = target_segment_index;
    while (segment_index < _catalog._segments.size()) {
        const auto &segment = _catalog._segments[segment_index];
        auto prefer_before_target = segment_index == target_segment_index && target_in_segment;
        uint64_t local_target_ms = 0;
        if (segment_index == target_segment_index && target_abs_ms > segment._begin_at_ms) {
            local_target_ms = target_abs_ms - segment._begin_at_ms;
        }

        // Only reopen the demuxer when we must: no demuxer yet, or the target lies in a
        // different segment than the one currently open. Otherwise reuse it and seek in place.
        auto need_reopen = !_demuxer || _active_segment_index != segment_index;
        if (need_reopen && !openSegmentByIndex(segment_index, local_target_ms)) {
            ++segment_index;
            continue;
        }

        if (!need_reopen && _demuxer->seekTo((int64_t)local_target_ms) == -1) {
            ++segment_index;
            continue;
        }

        bool key_frame = false;
        bool eof = false;
        while (!eof) {
            auto frame = _demuxer->readFrame(key_frame, eof);
            if (!frame) {
                break;
            }

            auto playable = !_have_video || key_frame || frame->keyFrame() || frame->configFrame();
            if (!playable) {
                continue;
            }

            auto global_dts = _active_segment_begin_offset_ms + (uint64_t)frame->dts();
            auto global_pts = _active_segment_begin_offset_ms + (uint64_t)frame->pts();
            if (global_pts < global_dts) {
                global_pts = global_dts;
            }
            if (global_dts < _window_begin_offset_ms) {
                continue;
            }
            if (_window_end_offset_ms > 0 && global_dts >= _window_end_offset_ms) {
                break;
            }

            auto stamped = std::make_shared<FrameStamp>(frame);
            stamped->setStamp((int64_t)global_dts, (int64_t)global_pts);
            if (_muxer) {
                if (_started) {
                    // Re-anchor the paced sender on the actually located frame instead of the
                    // requested seek position. For hole-forward hits the playable frame is later
                    // than the request, and for in-segment hits the nearest key frame may be
                    // earlier; baselining on the request causes a dts-decrease cache flush (visible stall).
                    auto npt_actual = offsetToSessionNpt(global_dts);
                    _muxer->resetPacedSender((uint32_t)npt_actual);
                }
                _muxer->inputFrame(remapFrameToSessionNpt(stamped));
            }
            setCurrentOffset((uint32_t)global_dts, true);

            DebugL << "replay seek: target_abs_ms=" << target_abs_ms
                   << ", seek_hit_type=" << (prefer_before_target ? "in-segment" : "hole-forward")
                   << ", segment_index=" << segment_index
                   << ", actual_play_abs_ms=" << offsetToAbsolute((uint32_t)global_dts)
                   << ", delta_ms=" << ((int64_t)offsetToAbsolute((uint32_t)global_dts) - (int64_t)target_abs_ms);
            return true;
        }

        ++segment_index;
    }

    WarnL << "replay seek fallback without playable frame, target_abs_ms=" << target_abs_ms;
    if (_muxer && _started) {
        auto npt_seek = offsetToSessionNpt(target_seek);
        _muxer->resetPacedSender((uint32_t)npt_seek);
    }
    setCurrentOffset((uint32_t)target_seek, true);
    return true;
}

uint32_t RtspReplayReader::absoluteToOffset(uint64_t abs_ms) const {
    if (abs_ms <= _base_file_begin_at_ms) {
        return 0;
    }
    auto delta = abs_ms - _base_file_begin_at_ms;
    if (delta > std::numeric_limits<uint32_t>::max()) {
        return std::numeric_limits<uint32_t>::max();
    }
    return (uint32_t)delta;
}

uint64_t RtspReplayReader::offsetToAbsolute(uint32_t offset_ms) const {
    return _base_file_begin_at_ms + offset_ms;
}

uint64_t RtspReplayReader::offsetToSessionNpt(uint64_t offset_ms) const {
    return offset_ms > _window_begin_offset_ms ? (offset_ms - _window_begin_offset_ms) : 0;
}

Frame::Ptr RtspReplayReader::remapFrameToSessionNpt(const Frame::Ptr &frame) const {
    if (!frame || !_started) {
        return frame;
    }

    auto dts = (int64_t)offsetToSessionNpt(frame->dts());
    auto pts = (int64_t)offsetToSessionNpt(frame->pts());
    if (pts < dts) {
        pts = dts;
    }

    auto stamped = std::make_shared<FrameStamp>(frame);
    stamped->setStamp(dts, pts);
    return stamped;
}

bool RtspReplayReader::seekTo(MediaSource &sender, uint32_t stamp) {
    lock_guard<recursive_mutex> lck(_mtx);

    if (!_started) {
        if (!start()) {
            return false;
        }
    }

    // Seek implies resuming playback. Only clear the paused flag here; the timeline is
    // re-anchored by the seekToOffset() below on the actually located frame, so calling
    // pause(false) (which would setCurrentOffset on the stale position) is redundant.
    _paused = false;

    auto target_abs = resolvePlayTargetFromNpt(stamp);
    auto target_offset = absoluteToOffset(target_abs);
    TraceL << getOriginUrl(sender) << ",npt_ms:" << stamp << ",target_abs:" << target_abs << ",target_offset:" << target_offset;
    return seekToOffset(target_offset);
}

bool RtspReplayReader::pause(MediaSource &sender, bool pause_value) {
    lock_guard<recursive_mutex> lck(_mtx);

    if (!_started) {
        if (!start()) {
            return false;
        }
    }

    if (_paused == pause_value) {
        return true;
    }
    setCurrentOffset(getCurrentOffset(), true);
    _paused = pause_value;
    TraceL << getOriginUrl(sender) << ",pause:" << pause_value;
    return true;
}

bool RtspReplayReader::speed(MediaSource &sender, float speed_value) {
    lock_guard<recursive_mutex> lck(_mtx);

    if (!_started) {
        if (!start()) {
            return false;
        }
    }

    if (speed_value < 0.1f || speed_value > 20.0f) {
        WarnL << "invalid replay speed:" << speed_value;
        return false;
    }

    setCurrentOffset(getCurrentOffset(), true);
    _paused = false;
    if (_speed == speed_value) {
        return true;
    }

    _speed = speed_value;
    if (_muxer) {
        _muxer->setSpeed(speed_value);
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
