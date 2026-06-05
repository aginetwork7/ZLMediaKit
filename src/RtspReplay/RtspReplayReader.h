/*
 * Copyright (c) 2016-present The ZLMediaKit project authors. All Rights Reserved.
 *
 * This file is part of ZLMediaKit(https://github.com/ZLMediaKit/ZLMediaKit).
 *
 * Use of this source code is governed by MIT-like license that can be found in the
 * LICENSE file in the root of the source tree. All contributing project authors
 * may be found in the AUTHORS file in the root of the source tree.
 */

#ifndef SRC_RTSPREPLAY_RTSPREPLAYREADER_H_
#define SRC_RTSPREPLAY_RTSPREPLAYREADER_H_

#ifdef ENABLE_MP4

#include <cstdint>

#include "RtspReplayTypes.h"
#include "Record/MP4Demuxer.h"
#include "Common/MultiMediaSourceMuxer.h"

namespace mediakit {

class RtspReplayReader : public std::enable_shared_from_this<RtspReplayReader>, public MediaSourceEvent {
public:
    using Ptr = std::shared_ptr<RtspReplayReader>;

    struct PerfStats {
        int64_t _setupProbeOpenMs = 0;
        int64_t _startTotalMs = 0;
        int64_t _demuxOpenMs = 0;
        int64_t _primeTrackMs = 0;
    };

    RtspReplayReader(const MediaTuple &tuple, const RtspReplayCatalogResult &catalog, const ProtocolOption &option, toolkit::EventPoller::Ptr poller = nullptr);

    bool start(uint64_t sample_ms = 0, bool ref_self = true, bool file_repeat = false);
    void stop();
    const PerfStats &getPerfStats() const;

    uint64_t firstPlayableAt() const;
    uint64_t currentAt() const;

private:
    bool seekTo(MediaSource &sender, uint32_t stamp) override;
    bool pause(MediaSource &sender, bool pause) override;
    bool speed(MediaSource &sender, float speed) override;
    bool close(MediaSource &sender) override;
    MediaOriginType getOriginType(MediaSource &sender) const override;
    std::string getOriginUrl(MediaSource &sender) const override;
    toolkit::EventPoller::Ptr getOwnerPoller(MediaSource &sender) override;

private:
    void setup(const MediaTuple &tuple, const RtspReplayCatalogResult &catalog, const ProtocolOption &option, toolkit::EventPoller::Ptr poller);
    bool readSample();
    bool readNextSample();
    bool openSegmentByOffset(uint32_t target_offset_ms);
    bool openSegmentByIndex(size_t segment_index, uint64_t local_seek_ms);
    size_t locateSegmentByAbsolute(uint64_t abs_ms) const;
    Frame::Ptr readFrameWithSegmentSwitch(bool &keyFrame, bool &eof);

    uint32_t getCurrentOffset() const;
    void setCurrentOffset(uint32_t offset_ms, bool sync_timeline);
    bool seekToOffset(uint32_t offset_seek_ms, bool allow_tail_fallback, bool reopen_demux = true);

    uint32_t absoluteToOffset(uint64_t abs_ms) const;
    uint64_t offsetToAbsolute(uint32_t offset_ms) const;
    Frame::Ptr remapFrameToSessionNpt(const Frame::Ptr &frame) const;

    uint64_t clampToWindow(uint64_t abs_ms) const;
    void onStarted(uint64_t actual_at_ms);
    void onProgressed(uint64_t actual_at_ms);
    void onSeekCompleted(uint64_t actual_at_ms);
    uint64_t resolvePlayTargetFromNpt(uint32_t npt_ms) const;
    uint32_t currentNptMs() const;

private:
    bool _file_repeat = false;
    bool _have_video = false;
    bool _paused = false;
    bool _started = false;
    float _speed = 1.0f;

    uint32_t _last_dts = 0;
    uint32_t _seek_to = 0;

    uint64_t _base_file_begin_at_ms = 0;
    uint64_t _window_begin_at_ms = 0;
    uint64_t _window_end_at_ms = 0;
    uint64_t _window_begin_offset_ms = 0; // window_begin offset relative to the file start time 
    uint64_t _window_end_offset_ms = 0; // window_end offset relative to the file start time
    uint64_t _current_at_ms = 0;
    uint64_t _session_origin_offset_ms = 0;
    uint64_t _active_segment_begin_offset_ms = 0;
    uint64_t _active_segment_end_offset_ms = 0;
    size_t _active_segment_index = 0;

    std::string _origin_url;

    RtspReplayCatalogResult _catalog;

    std::recursive_mutex _mtx;
    toolkit::Ticker _seek_ticker;
    toolkit::Timer::Ptr _timer;

    MP4Demuxer::Ptr _demuxer;
    MultiMediaSourceMuxer::Ptr _muxer;
    toolkit::EventPoller::Ptr _poller;
    PerfStats _perf_stats;
};

} // namespace mediakit

#endif // ENABLE_MP4
#endif // SRC_RTSPREPLAY_RTSPREPLAYREADER_H_
