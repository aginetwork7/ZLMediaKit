/*
 * Copyright (c) 2016-present The ZLMediaKit project authors. All Rights Reserved.
 *
 * This file is part of ZLMediaKit(https://github.com/ZLMediaKit/ZLMediaKit).
 *
 * Use of this source code is governed by MIT-like license that can be found in the
 * LICENSE file in the root of the source tree. All contributing project authors
 * may be found in the AUTHORS file in the root of the source tree.
 */

#include "RtspReplayTimeline.h"

#include <limits>
#include <stdexcept>

namespace mediakit {

RtspReplayTimeline::RtspReplayTimeline(uint64_t window_begin_at_ms, uint64_t window_end_at_ms)
    : _window_begin_at_ms(window_begin_at_ms), _window_end_at_ms(window_end_at_ms) {
    if (_window_begin_at_ms >= _window_end_at_ms) {
        throw std::invalid_argument("invalid replay window");
    }
}

void RtspReplayTimeline::setSessionOrigin(uint64_t actual_at_ms) {
    if (_started) {
        throw std::runtime_error("replay session origin already set");
    }
    _session_origin_at_ms = _window_begin_at_ms;
    _current_at_ms = clampToWindow(actual_at_ms);
    _started = true;
}

void RtspReplayTimeline::onStarted(uint64_t actual_at_ms) {
    setSessionOrigin(actual_at_ms);
}

void RtspReplayTimeline::onProgressed(uint64_t actual_at_ms) {
    if (!_started) {
        throw std::runtime_error("replay timeline not started");
    }
    _current_at_ms = clampToWindow(actual_at_ms);
}

void RtspReplayTimeline::onSeekCompleted(uint64_t actual_at_ms) {
    if (!_started) {
        throw std::runtime_error("replay timeline not started");
    }
    _current_at_ms = clampToWindow(actual_at_ms);
}

void RtspReplayTimeline::onPauseChanged(bool paused) {
    _paused = paused;
}

void RtspReplayTimeline::onSpeedChanged(float speed) {
    if (speed <= 0.0f) {
        throw std::invalid_argument("invalid replay speed");
    }
    _playback_speed = speed;
}

uint64_t RtspReplayTimeline::resolvePlayTargetFromNpt(uint32_t npt_ms) const {
    if (!_started) {
        throw std::runtime_error("replay timeline not started");
    }
    auto target = _window_begin_at_ms + npt_ms;
    return clampToWindow(target);
}

uint64_t RtspReplayTimeline::clampToWindow(uint64_t abs_ms) const {
    if (abs_ms < _window_begin_at_ms) {
        return _window_begin_at_ms;
    }
    auto window_max = _window_end_at_ms - 1;
    if (abs_ms > window_max) {
        return window_max;
    }
    return abs_ms;
}

uint64_t RtspReplayTimeline::sessionOriginAt() const {
    return _window_begin_at_ms;
}

uint64_t RtspReplayTimeline::currentAt() const {
    return _current_at_ms;
}

uint32_t RtspReplayTimeline::currentNptMs() const {
    if (!_started || _current_at_ms < _window_begin_at_ms) {
        return 0;
    }
    auto delta = _current_at_ms - _window_begin_at_ms;
    if (delta > std::numeric_limits<uint32_t>::max()) {
        return std::numeric_limits<uint32_t>::max();
    }
    return static_cast<uint32_t>(delta);
}

float RtspReplayTimeline::playbackSpeed() const {
    return _playback_speed;
}

bool RtspReplayTimeline::paused() const {
    return _paused;
}

} // namespace mediakit
