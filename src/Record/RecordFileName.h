/*
 * Copyright (c) 2016-present The ZLMediaKit project authors. All Rights Reserved.
 *
 * This file is part of ZLMediaKit(https://github.com/ZLMediaKit/ZLMediaKit).
 *
 * Use of this source code is governed by MIT-like license that can be found in the
 * LICENSE file in the root of the source tree. All contributing project authors
 * may be found in the AUTHORS file in the root of the source tree.
 */

#ifndef SRC_RECORD_RECORDFILENAME_H_
#define SRC_RECORD_RECORDFILENAME_H_

#include <cstdio>
#include <ctime>
#include <string>

namespace mediakit {

// Recording file name scheme (UTC): "<start>_<end>-<index>.mp4"
// where <start> / <end> are "YYYY-MM-DD-HH-MM-SS" (kRecordTimeStrLen chars).
// The producer (MP4Recorder) and the consumer (RtspReplayCatalog) MUST share this
// single definition so the format cannot silently drift; a mismatch would make
// replay quietly fail to index recordings.
static constexpr const char *kRecordTimeFormat = "%Y-%m-%d-%H-%M-%S";
static constexpr size_t kRecordTimeStrLen = 19; // length of "YYYY-MM-DD-HH-MM-SS"
static constexpr const char *kRecordFileSuffix = ".mp4";
static constexpr size_t kRecordFileSuffixLen = 4;
// Minimal length: <start>(19) + '_'(1) + <end>(19) + ".mp4"(4); the trailing
// "-<index>" is producer-only and not required by the parser.
static constexpr size_t kRecordFileNameMinLen = kRecordTimeStrLen * 2 + 1 + kRecordFileSuffixLen;

// Build the canonical final file name from start/end seconds (UTC) and a numeric index string.
inline std::string makeRecordFileName(time_t start_sec, time_t end_sec, const std::string &index) {
    struct tm start_tm = {};
    struct tm end_tm = {};
    gmtime_r(&start_sec, &start_tm);
    gmtime_r(&end_sec, &end_tm);
    char buf[128] = {0};
    snprintf(buf, sizeof(buf),
             "%04d-%02d-%02d-%02d-%02d-%02d_%04d-%02d-%02d-%02d-%02d-%02d-%s.mp4",
             start_tm.tm_year + 1900, start_tm.tm_mon + 1, start_tm.tm_mday,
             start_tm.tm_hour, start_tm.tm_min, start_tm.tm_sec,
             end_tm.tm_year + 1900, end_tm.tm_mon + 1, end_tm.tm_mday,
             end_tm.tm_hour, end_tm.tm_min, end_tm.tm_sec,
             index.c_str());
    return buf;
}

// Parse start/end seconds (UTC) from a canonical final file name.
// Returns false if the name does not match the scheme or end <= start.
inline bool parseRecordFileName(const std::string &name, time_t &start_sec, time_t &end_sec) {
    if (name.size() < kRecordFileNameMinLen ||
        name.compare(name.size() - kRecordFileSuffixLen, kRecordFileSuffixLen, kRecordFileSuffix) != 0) {
        return false;
    }

    auto base = name.substr(0, name.size() - kRecordFileSuffixLen);
    auto sep = base.find('_');
    if (sep == std::string::npos || sep < kRecordTimeStrLen) {
        return false;
    }

    struct tm start_tm = {};
    if (!strptime(base.substr(0, kRecordTimeStrLen).c_str(), kRecordTimeFormat, &start_tm)) {
        return false;
    }

    auto end_part = base.substr(sep + 1);
    if (end_part.size() < kRecordTimeStrLen) {
        return false;
    }

    struct tm end_tm = {};
    if (!strptime(end_part.substr(0, kRecordTimeStrLen).c_str(), kRecordTimeFormat, &end_tm)) {
        return false;
    }

    start_sec = timegm(&start_tm);
    end_sec = timegm(&end_tm);
    return start_sec > 0 && end_sec > start_sec;
}

} // namespace mediakit

#endif // SRC_RECORD_RECORDFILENAME_H_
