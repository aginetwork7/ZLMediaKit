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

struct RtspReplayRequest {
    std::string _schema;
    std::string _vhost;
    std::string _deviceId;
    std::string _channelId;
    std::string _streamType;
    uint64_t _windowBeginAtMs = 0;
    uint64_t _windowEndAtMs = 0;
};

struct RtspReplaySegment {
    std::string _filePath;
    uint64_t _beginAtMs = 0;
    uint64_t _endAtMs = 0;
    uint64_t _durationMs = 0;
};

struct RtspReplayCatalogResult {
    std::vector<RtspReplaySegment> _segments;
    uint64_t _windowBeginAtMs = 0;
    uint64_t _windowEndAtMs = 0;
};

} // namespace mediakit

#endif // SRC_RTSPREPLAY_RTSPREPLAYTYPES_H_
