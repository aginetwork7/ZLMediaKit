/*
 * Copyright (c) 2016-present The ZLMediaKit project authors. All Rights Reserved.
 *
 * This file is part of ZLMediaKit(https://github.com/ZLMediaKit/ZLMediaKit).
 *
 * Use of this source code is governed by MIT-like license that can be found in the
 * LICENSE file in the root of the source tree. All contributing project authors
 * may be found in the AUTHORS file in the root of the source tree.
 */

#include "RtspReplayCatalog.h"

#include "Common/config.h"
#include "Record/RecordFileName.h"
#include "Util/File.h"
#include "Util/util.h"

#include <algorithm>
#include <ctime>
#include <dirent.h>
#include <stdexcept>
#include <sys/stat.h>

using namespace std;
using namespace toolkit;

namespace mediakit {

// 判定临时文件「是否仍在被写入」的宽限期。它必须盖住录像落盘的全部滞后：mp4 的 stdio 写缓存
// (record.fileBufSize)、fmp4 分片要等到下一个关键帧才收尾、以及网络文件系统的属性缓存。
// 取大只会让中断残留的临时文件多留一小段尾巴；取小则会误判正在录制的文件已经结束，削掉回放
// 时间轴的末端，所以这里偏向保守。
// Grace period used to decide whether a temp file is still being written. It has to cover every
// source of write lag: the mp4 stdio buffer (record.fileBufSize), fmp4 fragments only being sealed
// on the next key frame, and attribute caching on network filesystems. Erring large merely leaves a
// short tail on temp files stranded by an interrupted recording; erring small would misjudge a file
// that is still recording and cut the end off the replay timeline, so this leans conservative.
static constexpr uint64_t kTempWriteGraceMs = 30 * 1000;

static bool isDateDir(const string &name) {
    return name.size() == 10 && name[4] == '-' && name[7] == '-';
}

static string epochToDateStr(time_t sec) {
    struct tm t = {};
    gmtime_r(&sec, &t);
    char buf[16] = {};
    snprintf(buf, sizeof(buf), "%04d-%02d-%02d", t.tm_year + 1900, t.tm_mon + 1, t.tm_mday);
    return buf;
}

RtspReplayRequest RtspReplayCatalog::parseRequest(const string &schema, const string &vhost, const string &streamId) {
    auto parts = split(streamId, "/");
    if (parts.size() != 5) {
        throw invalid_argument("invalid replay stream id");
    }

    // device/channel/stream_type 会被拼进录像目录路径。File::absolutePath() 默认不允许越过根目录，
    // 这里仍显式拒绝 '.'/'..' 等可疑段做纵深防御，避免将来换成别的拼接方式时失守。
    // device/channel/stream_type end up inside a filesystem path. File::absolutePath() already refuses
    // to escape its root; suspicious segments are still rejected explicitly as defence in depth so a
    // future change of the path assembly cannot silently lose that guarantee.
    auto is_safe_segment = [](const string &value) {
        return !value.empty() && value != "." && value.find("..") == string::npos &&
               value.find('/') == string::npos && value.find('\\') == string::npos;
    };
    if (!is_safe_segment(parts[0]) || !is_safe_segment(parts[1])) {
        throw invalid_argument("invalid replay device/channel");
    }

    if (parts[2].empty() || parts[2][0] != 's' || !is_safe_segment(parts[2])) {
        throw invalid_argument("invalid replay stream type");
    }

    if (parts[3].size() < 2 || parts[3][0] != 'b' || parts[4].size() < 2 || parts[4][0] != 'e') {
        throw invalid_argument("invalid replay begin/end");
    }

    uint64_t begin_ms = 0;
    uint64_t end_ms = 0;
    auto begin_token = parts[3].substr(1);
    auto end_token = parts[4].substr(1);
    try {
        size_t begin_pos = 0;
        size_t end_pos = 0;
        begin_ms = stoull(begin_token, &begin_pos);
        end_ms = stoull(end_token, &end_pos);
        // 必须整串消费，否则 "b123abc" 之类会被静默截断成另一个时间窗
        // The whole token must be consumed, otherwise "b123abc" is silently reinterpreted as a
        // different time window instead of being rejected
        if (begin_pos != begin_token.size() || end_pos != end_token.size()) {
            throw invalid_argument("invalid replay timestamp");
        }
    } catch (...) {
        throw invalid_argument("invalid replay timestamp");
    }

    // Backward compatibility: replay URLs may use Unix seconds or milliseconds. Current Unix
    // timestamps in seconds are about 1e9, while milliseconds are about 1e12, so this boundary
    // treats values below 1e12 as seconds and normalizes them to milliseconds.
    constexpr uint64_t kSecMsThreshold = 1000000000000ULL;
    if (begin_ms < kSecMsThreshold) {
        begin_ms *= 1000;
    }
    if (end_ms < kSecMsThreshold) {
        end_ms *= 1000;
    }

    if (begin_ms >= end_ms) {
        throw invalid_argument("invalid replay window");
    }

    RtspReplayRequest request;
    request._schema = schema;
    request._vhost = vhost;
    request._device_id = parts[0];
    request._channel_id = parts[1];
    request._stream_type = parts[2];
    request._window_begin_at_ms = begin_ms;
    request._window_end_at_ms = end_ms;
    return request;
}

RtspReplayCatalogResult RtspReplayCatalog::build(const RtspReplayRequest &request) {
    RtspReplayCatalogResult ret;
    ret._window_begin_at_ms = request._window_begin_at_ms;
    ret._window_end_at_ms = request._window_end_at_ms;

    auto now_ms = (uint64_t)time(nullptr) * 1000;
    auto temp_end_ms = std::min(now_ms, request._window_end_at_ms);

    GET_CONFIG(string, recordPath, Protocol::kMP4SavePath);
    GET_CONFIG(string, recordAppName, Record::kAppName);
    GET_CONFIG(bool, enableVhost, General::kEnableVhost);

    const string liveApp = "live";
    string relativePath;
    if (enableVhost) {
        relativePath = request._vhost + "/" + recordAppName + "/" + liveApp + "/" + request._device_id + "/" + request._channel_id + "/" + request._stream_type;
    } else {
        relativePath = recordAppName + "/" + liveApp + "/" + request._device_id + "/" + request._channel_id + "/" + request._stream_type;
    }
    auto recordDir = File::absolutePath(relativePath, recordPath);

    auto beginSec = (time_t)(request._window_begin_at_ms / 1000);
    auto endSec = (time_t)(request._window_end_at_ms / 1000);
    // Date dirs are coarse buckets; widen by +/-1 day to avoid edge misses.
    auto dateBegin = epochToDateStr(beginSec - 86400);
    auto dateEnd = epochToDateStr(endSec + 86400);

    auto pDir = opendir(recordDir.c_str());
    if (!pDir) {
        // 与「确实没有录像」区分开：权限/挂载/路径错误必须能从日志看出来
        // Keep this distinguishable from a legitimate "no recordings" result: permission, mount or
        // path errors must be visible in the log
        WarnL << "replay: cannot open record dir: " << recordDir << ", err=" << strerror(errno);
        return ret;
    }

    while (auto entry = readdir(pDir)) {
        string dateName = entry->d_name;
        if (!isDateDir(dateName)) {
            continue;
        }
        if (dateName < dateBegin || dateName > dateEnd) {
            continue;
        }

        auto datePath = recordDir + "/" + dateName;
        struct stat st = {};
        if (stat(datePath.c_str(), &st) != 0 || !S_ISDIR(st.st_mode)) {
            continue;
        }

        auto pSubDir = opendir(datePath.c_str());
        if (!pSubDir) {
            continue;
        }

        while (auto sub_entry = readdir(pSubDir)) {
            string fname = sub_entry->d_name;
            if (fname.empty()) {
                continue;
            }

            // ZLM writes an in-progress MP4 under a dot-prefixed temporary name, then renames it
            // to the regular record-file name only after the segment has been finalized.
            if (fname[0] == '.') {
                time_t tempStart = 0;
                if (!parseTempRecordFileName(fname, tempStart, nullptr)) {
                    continue;
                }

                auto tempPath = datePath + "/" + fname;
                struct stat tempStat = {};
                if (stat(tempPath.c_str(), &tempStat) != 0) {
                    // 刚好被 finalize 成正式文件名或已被删除，交给正式文件那一支处理
                    // Just finalized into its final name, or already removed; the regular branch covers it
                    continue;
                }

                auto fileBeginMs = (uint64_t)tempStart * 1000;
                // 临时文件的终点取「最后一次写入时间」，而不是无条件假定它一直录到现在。
                // 仍在录制的文件 mtime 紧跟当前时刻，加上宽限期后必然盖过 temp_end_ms，既保证"立刻拉当前时刻前若干秒"的语义
                // 也不会因为储存等异常伪造出一段延续至今的录像。
                auto liveUntilMs = (uint64_t)tempStat.st_mtime * 1000 + kTempWriteGraceMs;
                auto fileEndMs = std::min(temp_end_ms, liveUntilMs);
                if (fileEndMs <= fileBeginMs) {
                    continue;
                }
                if (fileEndMs <= request._window_begin_at_ms || fileBeginMs >= request._window_end_at_ms) {
                    continue;
                }

                RtspReplaySegment seg;
                seg._file_path = std::move(tempPath);
                seg._begin_at_ms = fileBeginMs;
                seg._end_at_ms = fileEndMs;
                ret._segments.emplace_back(std::move(seg));
                continue;
            }

            time_t fileStart = 0;
            time_t fileEnd = 0;
            if (!parseRecordFileName(fname, fileStart, fileEnd)) {
                continue;
            }

            auto fileBeginMs = (uint64_t)fileStart * 1000;
            auto fileEndMs = (uint64_t)fileEnd * 1000;
            // Keep only files that overlap the requested playback window.
            if (fileEndMs <= request._window_begin_at_ms || fileBeginMs >= request._window_end_at_ms) {
                continue;
            }

            RtspReplaySegment seg;
            seg._file_path = datePath + "/" + fname;
            seg._begin_at_ms = fileBeginMs;
            seg._end_at_ms = fileEndMs;
            ret._segments.emplace_back(std::move(seg));
        }
        closedir(pSubDir);
    }
    closedir(pDir);

    sort(ret._segments.begin(), ret._segments.end(), [](const RtspReplaySegment &l, const RtspReplaySegment &r) {
        if (l._begin_at_ms != r._begin_at_ms) {
            return l._begin_at_ms < r._begin_at_ms;
        }
        return l._file_path < r._file_path;
    });

    // 文件录制时，_begin 是录制开始的墙上时钟秒，_end 由录制时算出的时长推得不够精确，上一段_end可能和下一段_begin重叠，两者并不保证首尾相接
    // 一旦相邻分片重叠，播放跨过分片边界时全局 dts 就会倒退，迫使 paced sender 把缓存整批冲出去，表现为画面顿挫。
    // 因此要做裁剪保证后一个分片的起点即前一个分片的终点。
    // 与最近一个保留下来的分片比较：next 完全落在 cur 内时丢弃 next，保留 cur 的完整时长，避免把 cur 的后半段裁掉。
    // Compare against the last kept segment: when next lies entirely inside cur, drop next and keep cur whole,
    // instead of cutting off cur's tail.
    size_t last = 0;
    for (size_t i = 1; i < ret._segments.size(); ++i) {
        auto &cur = ret._segments[last];
        auto &next = ret._segments[i];
        if (next._end_at_ms <= cur._end_at_ms) {
            next._end_at_ms = next._begin_at_ms;
            continue;
        }
        if (cur._end_at_ms > next._begin_at_ms) {
            cur._end_at_ms = next._begin_at_ms;
        }
        last = i;
    }
    // 裁剪后可能留下零长度分片(被前一个分片完全覆盖，或与更长的后一个分片同一秒开始)，直接丢弃，
    // 否则 reader 会为一个取不出任何帧的分片白白开一次 mp4。
    ret._segments.erase(
        remove_if(ret._segments.begin(), ret._segments.end(),
                  [](const RtspReplaySegment &seg) { return seg._end_at_ms <= seg._begin_at_ms; }),
        ret._segments.end());
    for (auto &seg : ret._segments) {
        seg._duration_ms = seg._end_at_ms - seg._begin_at_ms;
    }
    return ret;
}

} // namespace mediakit
