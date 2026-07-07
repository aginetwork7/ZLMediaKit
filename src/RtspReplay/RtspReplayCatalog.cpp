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

    if (parts[0].empty() || parts[1].empty()) {
        throw invalid_argument("invalid replay device/channel");
    }

    if (parts[2].empty() || parts[2][0] != 's') {
        throw invalid_argument("invalid replay stream type");
    }

    if (parts[3].size() < 2 || parts[3][0] != 'b' || parts[4].size() < 2 || parts[4][0] != 'e') {
        throw invalid_argument("invalid replay begin/end");
    }

    uint64_t begin_ms = 0;
    uint64_t end_ms = 0;
    try {
        begin_ms = stoull(parts[3].substr(1));
        end_ms = stoull(parts[4].substr(1));
    } catch (...) {
        throw invalid_argument("invalid replay timestamp");
    }

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

            if (fname[0] == '.') {
                time_t tempStart = 0;
                if (!parseTempRecordFileName(fname, tempStart, nullptr)) {
                    continue;
                }

                auto fileBeginMs = (uint64_t)tempStart * 1000;
                auto fileEndMs = temp_end_ms;
                if (fileEndMs <= fileBeginMs) {
                    continue;
                }
                if (fileEndMs <= request._window_begin_at_ms || fileBeginMs >= request._window_end_at_ms) {
                    continue;
                }

                RtspReplaySegment seg;
                seg._file_path = datePath + "/" + fname;
                seg._begin_at_ms = fileBeginMs;
                seg._end_at_ms = fileEndMs;
                seg._duration_ms = fileEndMs - fileBeginMs;
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
            seg._duration_ms = fileEndMs - fileBeginMs;
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
    return ret;
}

} // namespace mediakit
