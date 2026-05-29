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

#include "RtspReplayTypes.h"
#include "RtspReplayTimeline.h"
#include "Record/MP4Demuxer.h"
#include "Common/MultiMediaSourceMuxer.h"

namespace mediakit {

class RtspReplayReader : public std::enable_shared_from_this<RtspReplayReader>, public MediaSourceEvent {
public:
    using Ptr = std::shared_ptr<RtspReplayReader>;

    RtspReplayReader(const MediaTuple &tuple, const RtspReplayCatalogResult &catalog, const ProtocolOption &option, toolkit::EventPoller::Ptr poller = nullptr);

    void bindTimeline(const std::shared_ptr<RtspReplayTimeline> &timeline);
    bool start(uint64_t sample_ms = 0, bool ref_self = true, bool file_repeat = false);
    void stop();

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
    bool openSegmentByDemuxStamp(uint32_t target_demux_ms);
    bool openSegmentByIndex(size_t segment_index, uint64_t local_seek_ms);
    size_t locateSegmentByAbsolute(uint64_t abs_ms) const;
    Frame::Ptr readFrameWithSegmentSwitch(bool &keyFrame, bool &eof);

    uint32_t getCurrentDemuxStamp() const;
    void setCurrentDemuxStamp(uint32_t stamp, bool sync_timeline);
    bool seekToDemux(uint32_t stamp_seek, bool allow_tail_fallback, bool reopen_demux = true);

    uint32_t absoluteToDemux(uint64_t abs_ms) const;
    uint64_t demuxToAbsolute(uint32_t demux_ms) const;
    Frame::Ptr remapFrameToSessionNpt(const Frame::Ptr &frame) const;

private:
    bool _file_repeat = false;
    bool _have_video = false;
    bool _paused = false;
    bool _timeline_started = false;
    float _speed = 1.0f;

    uint32_t _last_dts = 0;
    uint32_t _seek_to = 0;

    uint64_t _base_file_begin_at_ms = 0;
    uint64_t _window_begin_demux_ms = 0;
    uint64_t _window_end_demux_ms = 0;
    uint64_t _session_origin_demux_ms = 0;
    uint64_t _active_segment_begin_demux_ms = 0;
    uint64_t _active_segment_end_demux_ms = 0;
    size_t _active_segment_index = 0;

    std::string _file_list;
    std::string _origin_url;

    RtspReplayCatalogResult _catalog;
    std::shared_ptr<RtspReplayTimeline> _timeline;

    std::recursive_mutex _mtx;
    toolkit::Ticker _seek_ticker;
    toolkit::Timer::Ptr _timer;

    MP4Demuxer::Ptr _demuxer;
    MultiMediaSourceMuxer::Ptr _muxer;
    toolkit::EventPoller::Ptr _poller;
};

} // namespace mediakit

#endif // ENABLE_MP4
#endif // SRC_RTSPREPLAY_RTSPREPLAYREADER_H_
