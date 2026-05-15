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

static bool parseFileTimestamps(const string &name, time_t &start_sec, time_t &end_sec) {
    if (name.size() < 43 || name.substr(name.size() - 4) != ".mp4") {
        return false;
    }

    auto base = name.substr(0, name.size() - 4);
    auto sep = base.find('_');
    if (sep == string::npos || sep < 19) {
        return false;
    }

    struct tm start_tm = {};
    if (!strptime(base.substr(0, 19).c_str(), "%Y-%m-%d-%H-%M-%S", &start_tm)) {
        return false;
    }

    auto end_part = base.substr(sep + 1);
    if (end_part.size() < 19) {
        return false;
    }

    struct tm end_tm = {};
    if (!strptime(end_part.substr(0, 19).c_str(), "%Y-%m-%d-%H-%M-%S", &end_tm)) {
        return false;
    }

    start_sec = timegm(&start_tm);
    end_sec = timegm(&end_tm);
    return start_sec > 0 && end_sec > start_sec;
}

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

RtspReplayRequest RtspReplayCatalog::parseRequest(const string &schema, const string &vhost, const string &stream_id) {
    auto parts = split(stream_id, "/");
    if (parts.size() != 5) {
        throw invalid_argument("invalid replay stream id");
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
    request.schema = schema;
    request.vhost = vhost;
    request.deviceId = parts[0];
    request.channelId = parts[1];
    request.streamType = parts[2];
    request.windowBeginAtMs = begin_ms;
    request.windowEndAtMs = end_ms;
    return request;
}

RtspReplayCatalogResult RtspReplayCatalog::build(const RtspReplayRequest &request) {
    RtspReplayCatalogResult ret;
    ret.windowBeginAtMs = request.windowBeginAtMs;
    ret.windowEndAtMs = request.windowEndAtMs;

    GET_CONFIG(string, recordPath, Protocol::kMP4SavePath);
    GET_CONFIG(string, recordAppName, Record::kAppName);
    GET_CONFIG(bool, enableVhost, General::kEnableVhost);

    const string live_app = "live";
    string rel_path;
    if (enableVhost) {
        rel_path = request.vhost + "/" + recordAppName + "/" + live_app + "/" + request.deviceId + "/" + request.channelId + "/" + request.streamType;
    } else {
        rel_path = recordAppName + "/" + live_app + "/" + request.deviceId + "/" + request.channelId + "/" + request.streamType;
    }
    auto record_dir = File::absolutePath(rel_path, recordPath);

    auto begin_sec = (time_t)(request.windowBeginAtMs / 1000);
    auto end_sec = (time_t)(request.windowEndAtMs / 1000);
    auto date_begin = epochToDateStr(begin_sec - 86400);
    auto date_end = epochToDateStr(end_sec + 86400);

    auto pDir = opendir(record_dir.c_str());
    if (!pDir) {
        return ret;
    }

    while (auto entry = readdir(pDir)) {
        string date_name = entry->d_name;
        if (!isDateDir(date_name)) {
            continue;
        }
        if (date_name < date_begin || date_name > date_end) {
            continue;
        }

        auto date_path = record_dir + "/" + date_name;
        struct stat st = {};
        if (stat(date_path.c_str(), &st) != 0 || !S_ISDIR(st.st_mode)) {
            continue;
        }

        auto pSubDir = opendir(date_path.c_str());
        if (!pSubDir) {
            continue;
        }

        while (auto sub_entry = readdir(pSubDir)) {
            string fname = sub_entry->d_name;
            if (fname.empty() || fname[0] == '.') {
                continue;
            }

            time_t file_start = 0;
            time_t file_end = 0;
            if (!parseFileTimestamps(fname, file_start, file_end)) {
                continue;
            }

            auto file_begin_ms = (uint64_t)file_start * 1000;
            auto file_end_ms = (uint64_t)file_end * 1000;
            if (file_end_ms <= request.windowBeginAtMs || file_begin_ms >= request.windowEndAtMs) {
                continue;
            }

            RtspReplaySegment seg;
            seg.filePath = date_path + "/" + fname;
            seg.beginAtMs = file_begin_ms;
            seg.endAtMs = file_end_ms;
            seg.durationMs = file_end_ms - file_begin_ms;
            ret.segments.emplace_back(std::move(seg));
        }
        closedir(pSubDir);
    }
    closedir(pDir);

    sort(ret.segments.begin(), ret.segments.end(), [](const RtspReplaySegment &l, const RtspReplaySegment &r) {
        if (l.beginAtMs != r.beginAtMs) {
            return l.beginAtMs < r.beginAtMs;
        }
        return l.filePath < r.filePath;
    });
    return ret;
}

} // namespace mediakit
