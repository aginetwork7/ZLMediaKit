/*
 * Copyright (c) 2016-present The ZLMediaKit project authors. All Rights Reserved.
 *
 * This file is part of ZLMediaKit(https://github.com/ZLMediaKit/ZLMediaKit).
 *
 * Use of this source code is governed by MIT-like license that can be found in the
 * LICENSE file in the root of the source tree. All contributing project authors
 * may be found in the AUTHORS file in the root of the source tree.
 */

#ifndef SRC_RTSPREPLAY_RTSPREPLAYCATALOG_H_
#define SRC_RTSPREPLAY_RTSPREPLAYCATALOG_H_

#include <string>

#include "RtspReplayTypes.h"

namespace mediakit {

class RtspReplayCatalog {
public:
    // 解析 replay stream id "{device}/{channel}/s{type}/b{begin}/e{end}"。
    // begin/end 接受秒或毫秒(小于 1e12 视为秒)，统一归一化为 epoch 毫秒；窗口为半开区间 [begin, end)。
    // 格式非法、时间戳非法或 begin >= end 时抛 std::invalid_argument。
    // Parse the replay stream id "{device}/{channel}/s{type}/b{begin}/e{end}".
    // begin/end accept seconds or milliseconds (values below 1e12 are treated as seconds) and are
    // normalized to epoch milliseconds; the window is the half-open interval [begin, end).
    // Throws std::invalid_argument on a malformed id, a malformed timestamp or begin >= end.
    static RtspReplayRequest parseRequest(const std::string &schema, const std::string &vhost, const std::string &stream_id);
    // 扫描录像目录，返回与请求窗口相交的分片，按 _begin_at_ms 升序。
    // 不抛异常：无录像、目录不存在或不可读时都返回空 _segments(后者会打 WarnL)。
    // Scan the record tree and return the segments overlapping the request window, sorted by
    // _begin_at_ms. Never throws: missing recordings as well as a missing/unreadable directory yield
    // empty _segments (the latter also logs a warning).
    static RtspReplayCatalogResult build(const RtspReplayRequest &request);
};

} // namespace mediakit

#endif // SRC_RTSPREPLAY_RTSPREPLAYCATALOG_H_
