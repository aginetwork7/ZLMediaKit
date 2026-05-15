/*
 * Copyright (c) 2016-present The ZLMediaKit project authors. All Rights Reserved.
 *
 * This file is part of ZLMediaKit(https://github.com/ZLMediaKit/ZLMediaKit).
 *
 * Use of this source code is governed by MIT-like license that can be found in the
 * LICENSE file in the root of the source tree. All contributing project authors
 * may be found in the AUTHORS file in the root of the source tree.
 */

#ifndef SRC_RTSPREPLAY_RTSPREPLAYTIMELINE_H_
#define SRC_RTSPREPLAY_RTSPREPLAYTIMELINE_H_

#include <stdint.h>

namespace mediakit {

class RtspReplayTimeline {
public:
    RtspReplayTimeline(uint64_t window_begin_at_ms, uint64_t window_end_at_ms);

    void setSessionOrigin(uint64_t actual_at_ms);
    void onStarted(uint64_t actual_at_ms);
    void onProgressed(uint64_t actual_at_ms);
    void onSeekCompleted(uint64_t actual_at_ms);
    void onPauseChanged(bool paused);
    void onSpeedChanged(float speed);

    uint64_t resolvePlayTargetFromNpt(uint32_t npt_ms) const;
    uint64_t clampToWindow(uint64_t abs_ms) const;

    uint64_t sessionOriginAt() const;
    uint64_t currentAt() const;
    uint32_t currentNptMs() const;
    float playbackSpeed() const;
    bool paused() const;

private:
    uint64_t _window_begin_at_ms = 0;
    uint64_t _window_end_at_ms = 0;
    uint64_t _session_origin_at_ms = 0;
    uint64_t _current_at_ms = 0;
    float _playback_speed = 1.0f;
    bool _paused = false;
    bool _started = false;
};

} // namespace mediakit

#endif // SRC_RTSPREPLAY_RTSPREPLAYTIMELINE_H_
