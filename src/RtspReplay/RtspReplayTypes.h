/*
 * Copyright (c) 2016-present The ZLMediaKit project authors. All Rights Reserved.
 *
 * This file is part of ZLMediaKit(https://github.com/ZLMediaKit/ZLMediaKit).
 *
 * Use of this source code is governed by MIT-like license that can be found in the
 * LICENSE file in the root of the source tree. All contributing project authors
 * may be found in the AUTHORS file in the root of the source tree.
 */

#ifndef SRC_RTSPREPLAY_RTSPREPLAYTYPES_H_
#define SRC_RTSPREPLAY_RTSPREPLAYTYPES_H_

#include <stdint.h>
#include <string>
#include <vector>

namespace mediakit {

// URL 解析后的规范化 replay 请求。
// _window_begin_at_ms / _window_end_at_ms 为 epoch 毫秒，构成半开区间 [begin, end)。
// Canonical replay request after url parsing.
// _window_begin_at_ms / _window_end_at_ms are epoch milliseconds forming the half-open interval
// [begin, end).
struct RtspReplayRequest {
    std::string _schema;
    std::string _vhost;
    std::string _device_id;
    std::string _channel_id;
    std::string _stream_type;
    uint64_t _window_begin_at_ms = 0;
    uint64_t _window_end_at_ms = 0;
};

// 与请求窗口相交的单个录像分片。
// _begin_at_ms / _end_at_ms 为 epoch 毫秒，半开区间 [begin, end)；_duration_ms == end - begin。
// One recording segment intersecting the requested window.
// _begin_at_ms / _end_at_ms are epoch milliseconds forming the half-open interval [begin, end);
// _duration_ms == _end_at_ms - _begin_at_ms.
struct RtspReplaySegment {
    std::string _file_path;
    uint64_t _begin_at_ms = 0;
    uint64_t _end_at_ms = 0;
    uint64_t _duration_ms = 0;
};

// RtspReplayReader 消费的分片目录；_segments 按 _begin_at_ms 升序且互不重叠(允许有空洞)。
// 窗口字段回显请求窗口，单位同上。
// The segment catalog consumed by RtspReplayReader; _segments is sorted by _begin_at_ms and
// non-overlapping (gaps are allowed). The window fields echo the request window, same unit.
struct RtspReplayCatalogResult {
    std::vector<RtspReplaySegment> _segments;
    uint64_t _window_begin_at_ms = 0;
    uint64_t _window_end_at_ms = 0;
};

} // namespace mediakit

#endif // SRC_RTSPREPLAY_RTSPREPLAYTYPES_H_
