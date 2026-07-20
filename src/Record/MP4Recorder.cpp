/*
 * Copyright (c) 2016-present The ZLMediaKit project authors. All Rights Reserved.
 *
 * This file is part of ZLMediaKit(https://github.com/ZLMediaKit/ZLMediaKit).
 *
 * Use of this source code is governed by MIT-like license that can be found in the
 * LICENSE file in the root of the source tree. All contributing project authors
 * may be found in the AUTHORS file in the root of the source tree.
 */

#ifdef ENABLE_MP4
#include <ctime>
#include <sys/stat.h>
#include <dirent.h>
#include "Util/File.h"
#include "Common/config.h"
#include "MP4Recorder.h"
#include "MP4Demuxer.h"
#include "RecordFileName.h"
#include "Thread/WorkThreadPool.h"
#include "MP4Muxer.h"

using namespace std;
using namespace toolkit;

namespace mediakit {

static string getUTCTimeStr(const char *fmt) {
    auto now = ::time(nullptr);
    struct tm tm = {};
    if (!gmtime_r(&now, &tm)) {
        return "";
    }
    char buf[128] = {0};
    auto len = std::strftime(buf, sizeof(buf), fmt, &tm);
    if (len == 0) {
        return "";
    }
    return std::string(buf, len);
}

MP4Recorder::MP4Recorder(const MediaTuple &tuple, const string &path, size_t max_second) {
    // ///record 业务逻辑//////  [AUTO-TRANSLATED:2e78931a]
    // ///record Business Logic//////
    static_cast<MediaTuple &>(_info) = tuple;
    _info.folder = path;
    GET_CONFIG(uint32_t, s_max_second, Protocol::kMP4MaxSecond);
    _max_second = max_second ? max_second : s_max_second;
}

MP4Recorder::~MP4Recorder() {
    try {
        flush();
        closeFile();
    } catch (std::exception &ex) {
        WarnL << ex.what();
    }
}

void MP4Recorder::createFile() {
    closeFile();
    _warned_non_g711_audio_for_file = false;
    auto date = getUTCTimeStr("%Y-%m-%d");
    auto file_name = date + "-" + getUTCTimeStr("%H-%M-%S") + "-" + std::to_string(_file_index++) + ".mp4";
    auto full_path = _info.folder + date + "/" + file_name;
    auto full_path_tmp = _info.folder + date + "/." + file_name;

    // ///record 业务逻辑//////  [AUTO-TRANSLATED:2e78931a]
    // ///record Business Logic//////
    _info.start_time = ::time(NULL);
    _info.file_name = file_name;
    _info.file_path = full_path;
    GET_CONFIG(string, appName, Record::kAppName);
    _info.url = appName + "/" + _info.app + "/" + _info.stream + "/" + date + "/" + file_name;

    try {
        _muxer = std::make_shared<MP4Muxer>();
        TraceL << "Open tmp mp4 file: " << full_path_tmp;
        _muxer->openMP4(full_path_tmp);
        for (auto &track :_tracks) {
            // 添加track  [AUTO-TRANSLATED:80ae762a]
            // Add track
            _muxer->addTrack(track);
        }
        _full_path_tmp = full_path_tmp;
    } catch (std::exception &ex) {
        WarnL << ex.what();
    }
}

void MP4Recorder::asyncClose() {
    auto muxer = _muxer;
    auto full_path_tmp = _full_path_tmp;
    auto info = _info;
    TraceL << "Start close tmp mp4 file: " << full_path_tmp;
    WorkThreadPool::Instance().getExecutor()->async([muxer, full_path_tmp, info]() mutable {
        auto duration_ms = muxer->getDuration();
        info.time_len = duration_ms / 1000.0f;
        // 关闭mp4可能非常耗时，所以要放在后台线程执行  [AUTO-TRANSLATED:a7378a11]
        // Closing mp4 can be very time-consuming, so it should be executed in the background thread
        TraceL << "Closing tmp mp4 file: " << full_path_tmp;
        muxer->closeMP4();
        TraceL << "Closed tmp mp4 file: " << full_path_tmp;
        if (!full_path_tmp.empty()) {
            // 获取文件大小  [AUTO-TRANSLATED:7b90eb41]
            // Get file size
            info.file_size = File::fileSize(full_path_tmp);
            if (info.file_size < 1024) {
                // 录像文件太小，删除之  [AUTO-TRANSLATED:923d27c3]
                // The recording file is too small, delete it
                File::delete_file(full_path_tmp);
                return;
            }

            // 根据实际录制时长计算 End 时间，重建文件名为 start_end 格式
            // Compute end time from actual duration, rebuild filename with start_end scheme
            // 半开区间 [Begin, End)：End = Start + 截断秒数，且至少为 Start + 1
            // Half-open interval [Begin, End): End = Start + truncated seconds, at least Start + 1
            time_t duration_sec = (time_t)(duration_ms / 1000);
            if (duration_sec < 1) {
                duration_sec = 1;
            }
            time_t end_time = info.start_time + duration_sec;

            // 从原始文件名提取 file index（最后一个 '-' 与 '.mp4' 之间的部分）
            // Extract file index from original filename (between last '-' and '.mp4')
            auto dot_pos = info.file_name.rfind(kRecordFileSuffix);
            auto dash_pos = (dot_pos != std::string::npos) ? info.file_name.rfind('-', dot_pos) : std::string::npos;
            std::string index_str = "0";
            if (dash_pos != std::string::npos && dot_pos != std::string::npos) {
                index_str = info.file_name.substr(dash_pos + 1, dot_pos - dash_pos - 1);
            }

            auto new_name = makeRecordFileName(info.start_time, end_time, index_str);

            // 用新文件名替换 info 中的路径
            // Replace paths in info with new filename
            auto old_name_pos = info.file_path.rfind(info.file_name);
            if (old_name_pos != std::string::npos) {
                info.file_path = info.file_path.substr(0, old_name_pos) + new_name;
            }
            auto old_url_pos = info.url.rfind(info.file_name);
            if (old_url_pos != std::string::npos) {
                info.url = info.url.substr(0, old_url_pos) + new_name;
            }
            info.file_name = new_name;

            // 临时文件名改成正式文件名，防止mp4未完成时被访问  [AUTO-TRANSLATED:541a6f00]
            // Change the temporary file name to the official file name to prevent access to the mp4 before it is completed
            rename(full_path_tmp.data(), info.file_path.data());
        }
        TraceL << "Emit mp4 record event: " << info.file_path;
        // 触发mp4录制切片生成事件  [AUTO-TRANSLATED:9959dcd4]
        // Trigger mp4 recording slice generation event
        NOTICE_EMIT(BroadcastRecordMP4Args, Broadcast::kBroadcastRecordMP4, info);
    });
}

void MP4Recorder::closeFile() {
    if (_muxer) {
        asyncClose();
        _muxer = nullptr;
    }
}

void MP4Recorder::flush() {
    if (_muxer) {
        _muxer->flush();
    }
}

bool MP4Recorder::inputFrame(const Frame::Ptr &frame) {
    if (frame->getTrackType() == TrackAudio && !isG711Codec(frame->getCodecId())) {
        if (!_warned_non_g711_audio_for_file) {
            WarnL << "-----mp4 record: skip non-G711 audio, file will contain no audio, codec="
                  << getCodecName(frame->getCodecId()) << ", file=" << _info.file_name;
            _warned_non_g711_audio_for_file = true;
        }
        return false;
    }

    auto stamp_inc = _delta_stamp[frame->getTrackType()].relativeStamp(frame->pts(), false);
    if (!_muxer || (stamp_inc > int64_t(_max_second) * 1000 && (!_have_video || frame->keyFrame()))) {
        // 成立条件  [AUTO-TRANSLATED:8c9c6083]
        // Conditions for establishment
        // 1、_muxer为空  [AUTO-TRANSLATED:fa236097]
        // 1. _muxer is empty
        // 2、到了切片时间，并且只有音频  [AUTO-TRANSLATED:212e9d23]
        // 2. It's time to slice, and there is only audio
        // 3、到了切片时间，有视频并且遇到视频的关键帧  [AUTO-TRANSLATED:fa4a71ad]
        // 3. It's time to slice, there is video and a video keyframe is encountered
        createFile();
        for (auto &ref : _delta_stamp) {
            ref.reset();
        }
    }

    if (_muxer) {
        // 生成mp4文件  [AUTO-TRANSLATED:76a8d77c]
        // Generate mp4 file
        return _muxer->inputFrame(frame);
    }
    return false;
}

bool MP4Recorder::addTrack(const Track::Ptr &track) {
    if (track->getTrackType() == TrackAudio && !isG711Codec(track->getCodecId())) {
        // Keep MP4 recording audio codec deterministic: only accept G711 tracks.
        return true;
    }

    // 保存所有的track，为创建MP4MuxerFile做准备  [AUTO-TRANSLATED:815c2486]
    // Save all tracks in preparation for creating MP4MuxerFile
    _tracks.emplace_back(track);
    if (track->getTrackType() == TrackVideo) {
        _have_video = true;
    }
    return true;
}

void MP4Recorder::resetTracks() {
    closeFile();
    _tracks.clear();
    _have_video = false;
    _warned_non_g711_audio_for_file = false;
}

// 递归扫描目录中断电或异常退出导致的孤儿临时mp4文件并恢复
// Recursively scan for orphan temp mp4 files caused by power failure or abnormal exit and recover them
static void recoverOrphansInDir(const string &dir, int &recovered, int &skipped) {
    auto pDir = opendir(dir.c_str());
    if (!pDir) return;
    while (auto entry = readdir(pDir)) {
        string name = entry->d_name;
        if (name == "." || name == "..") continue;
        string path = dir + "/" + name;

        // 用 lstat 避免跟随符号链接导致环路递归
        struct stat lst;
        if (lstat(path.c_str(), &lst) != 0) continue;
        if (S_ISLNK(lst.st_mode)) continue;

        if (S_ISDIR(lst.st_mode)) {
            recoverOrphansInDir(path, recovered, skipped);
            continue;
        }
        // 只处理隐藏的 mp4 临时文件
        if (name[0] != '.' || name.size() <= kRecordFileSuffixLen + 1 ||
            name.compare(name.size() - kRecordFileSuffixLen, kRecordFileSuffixLen, kRecordFileSuffix) != 0) {
            continue;
        }

        auto base = name.substr(1, name.size() - kRecordFileSuffixLen - 1); // 去掉前导 '.' 和末尾 '.mp4'
        if (base.size() < kRecordTimeStrLen) {
            WarnL << "Orphan file has unexpected name format, deleting: " << path;
            File::delete_file(path);
            continue;
        }
        struct tm start_tm = {};
        if (!strptime(base.substr(0, kRecordTimeStrLen).c_str(), kRecordTimeFormat, &start_tm)) {
            WarnL << "Failed to parse start time from orphan file, deleting: " << path;
            File::delete_file(path);
            continue;
        }
        auto last_dash = base.rfind('-');
        string index_str = (last_dash != string::npos && last_dash >= kRecordTimeStrLen) ? base.substr(last_dash + 1) : "0";

        try {
            MP4Demuxer demuxer;
            demuxer.openMP4(path);
            auto duration_ms = demuxer.getDurationMS();
            demuxer.closeMP4();

            time_t start_time = timegm(&start_tm);
            time_t duration_sec = (time_t)(duration_ms / 1000);
            if (duration_sec < 1) {
                duration_sec = 1;
            }
            time_t end_time = start_time + duration_sec;

            auto new_name = makeRecordFileName(start_time, end_time, index_str);
            auto new_path = dir + "/" + new_name;
            if (0 == ::rename(path.c_str(), new_path.c_str())) {
                InfoL << "Recovered orphan recording: " << path << " -> " << new_path;
                ++recovered;
            } else {
                WarnL << "Failed to rename orphan recording: " << path << " -> " << new_path;
            }
        } catch (std::exception &ex) {
            // 不删除：可能是权限问题或临时 IO 错误，下次启动可重试
            WarnL << "Cannot open orphan recording, skipping: " << path << ", error: " << ex.what();
            ++skipped;
        }
    }
    closedir(pDir);
}

void MP4Recorder::recoverOrphanRecordings() {
    GET_CONFIG(string, recordPath, Protocol::kMP4SavePath);
    GET_CONFIG(string, recordAppName, Record::kAppName);
    if (recordPath.empty()) {
        return;
    }

    auto absPath = File::absolutePath("", recordPath);
    auto filePath = recordAppName + "/";
    auto recoverRoot = File::absolutePath(filePath, absPath);
    int recovered = 0, skipped = 0;
    recoverOrphansInDir(recoverRoot, recovered, skipped);
    if (recovered > 0 || skipped > 0) {
        InfoL << "Orphan recording recovery complete: recovered=" << recovered << ", skipped=" << skipped;
    }
}

} /* namespace mediakit */

#endif //ENABLE_MP4
