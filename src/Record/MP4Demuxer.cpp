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

#include <algorithm>
#include <ctime>
#include "MP4Demuxer.h"
#include "Util/File.h"
#include "Util/logger.h"
#include "Extension/Factory.h"

using namespace std;
using namespace toolkit;

namespace mediakit {

static uint64_t getDurationFromRecordFileName(const string &file) {
    auto pos = file.rfind('/');
    auto name = (pos == string::npos) ? file : file.substr(pos + 1);
    if (name.size() < 43 || !end_with(name, ".mp4")) {
        return 0;
    }

    auto base = name.substr(0, name.size() - 4);
    auto sep = base.find('_');
    if (sep == string::npos || sep < 19 || base.size() < sep + 20) {
        return 0;
    }

    struct tm start_tm = {};
    struct tm end_tm = {};
    if (!strptime(base.substr(0, 19).c_str(), "%Y-%m-%d-%H-%M-%S", &start_tm)) {
        return 0;
    }
    if (!strptime(base.substr(sep + 1, 19).c_str(), "%Y-%m-%d-%H-%M-%S", &end_tm)) {
        return 0;
    }

    auto start_sec = timegm(&start_tm);
    auto end_sec = timegm(&end_tm);
    if (end_sec <= start_sec) {
        return 0;
    }
    return (uint64_t)(end_sec - start_sec) * 1000;
}

MP4Demuxer::~MP4Demuxer() {
    closeMP4();
}

void MP4Demuxer::openMP4(const string &file) {
    closeMP4();

    _mp4_file = std::make_shared<MP4FileDisk>();
    _mp4_file->openFile(file.data(), "rb+");
    _mov_reader = _mp4_file->createReader();
    getAllTracks();
    _duration_ms = mov_reader_getduration(_mov_reader.get());
}

void MP4Demuxer::closeMP4() {
    _mov_reader.reset();
    _mp4_file.reset();
}

int MP4Demuxer::getAllTracks() {
    static mov_reader_trackinfo_t s_on_track = {
            [](void *param, uint32_t track, uint8_t object, int width, int height, const void *extra, size_t bytes) {
                //onvideo
                MP4Demuxer *thiz = (MP4Demuxer *)param;
                thiz->onVideoTrack(track,object,width,height,extra,bytes);
            },
            [](void *param, uint32_t track, uint8_t object, int channel_count, int bit_per_sample, int sample_rate, const void *extra, size_t bytes) {
                //onaudio
                MP4Demuxer *thiz = (MP4Demuxer *)param;
                thiz->onAudioTrack(track,object,channel_count,bit_per_sample,sample_rate,extra,bytes);
            },
            [](void *param, uint32_t track, uint8_t object, const void *extra, size_t bytes) {
                //onsubtitle, do nothing
            }
    };
    return mov_reader_getinfo(_mov_reader.get(),&s_on_track,this);
}

void MP4Demuxer::onVideoTrack(uint32_t track, uint8_t object, int width, int height, const void *extra, size_t bytes) {
    auto video = Factory::getTrackByCodecId(getCodecByMovId(object));
    if (!video) {
        return;
    }
    video->setIndex(track);
    _tracks.emplace(track, video);
    if (extra && bytes) {
        video->setExtraData((uint8_t *)extra, bytes);
    }
}

void MP4Demuxer::onAudioTrack(uint32_t track, uint8_t object, int channel_count, int bit_per_sample, int sample_rate, const void *extra, size_t bytes) {
    auto audio = Factory::getTrackByCodecId(getCodecByMovId(object), sample_rate, channel_count, bit_per_sample / channel_count);
    if (!audio) {
        return;
    }
    audio->setIndex(track);
    _tracks.emplace(track, audio);
    if (extra && bytes) {
        audio->setExtraData((uint8_t *)extra, bytes);
    }
}

int64_t MP4Demuxer::seekTo(int64_t stamp_ms) {
    if(0 != mov_reader_seek(_mov_reader.get(),&stamp_ms)){
        return -1;
    }
    return stamp_ms;
}

struct Context {
    Context(MP4Demuxer *ptr) : thiz(ptr) {}
    MP4Demuxer *thiz;
    int flags = 0;
    int64_t pts = 0;
    int64_t dts = 0;
    uint32_t track_id = 0;
    BufferRaw::Ptr buffer;
};

Frame::Ptr MP4Demuxer::readFrame(bool &keyFrame, bool &eof) {
    keyFrame = false;
    eof = false;

    static mov_reader_onread2 mov_onalloc = [](void *param, uint32_t track_id, size_t bytes, int64_t pts, int64_t dts, int flags) -> void * {
        Context *ctx = (Context *) param;
        ctx->pts = pts;
        ctx->dts = dts;
        ctx->flags = flags;
        ctx->track_id = track_id;

        ctx->buffer = ctx->thiz->_buffer_pool.obtain2();
        ctx->buffer->setCapacity(bytes + 1);
        ctx->buffer->setSize(bytes);
        return ctx->buffer->data();
    };

    Context ctx(this);
    auto ret = mov_reader_read2(_mov_reader.get(), mov_onalloc, &ctx);
    switch (ret) {
        case 0 : {
            eof = true;
            return nullptr;
        }

        case 1 : {
            keyFrame = ctx.flags & MOV_AV_FLAG_KEYFREAME;
            return makeFrame(ctx.track_id, ctx.buffer, ctx.pts, ctx.dts);
        }

        default : {
            eof = true;
            WarnL << "读取mp4文件数据失败:" << ret;
            return nullptr;
        }
    }
}

Frame::Ptr MP4Demuxer::makeFrame(uint32_t track_id, Buffer::Ptr buf, int64_t pts, int64_t dts) {
    auto it = _tracks.find(track_id);
    if (it == _tracks.end()) {
        return nullptr;
    }
    Frame::Ptr ret;
    auto codec = it->second->getCodecId();
    switch (codec) {
        case CodecH264:
        case CodecH265: {
            auto bytes = buf->size();
            auto data = buf->data();
            auto offset = 0u;
            while (offset < bytes) {
                uint32_t frame_len;
                memcpy(&frame_len, data + offset, 4);
                frame_len = ntohl(frame_len);
                if (frame_len + offset + 4 > bytes) {
                    return nullptr;
                }
                memcpy(data + offset, "\x00\x00\x00\x01", 4);
                offset += (frame_len + 4);
            }
            ret = Factory::getFrameFromBuffer(codec, std::move(buf), dts, pts);
            break;
        }

        default: {
            ret = Factory::getFrameFromBuffer(codec, std::move(buf), dts, pts);
            break;
        }
    }
    if (ret) {
        ret->setIndex(track_id);
        it->second->inputFrame(ret);
    }
    return ret;
}

vector<Track::Ptr> MP4Demuxer::getTracks(bool ready) const {
    vector<Track::Ptr> ret;
    for (auto &pr : _tracks) {
        if (ready && !pr.second->ready()) {
            continue;
        }
        ret.push_back(pr.second);
    }
    return ret;
}

uint64_t MP4Demuxer::getDurationMS() const {
    return _duration_ms;
}

/////////////////////////////////////////////////////////////////////////////////

void MultiMP4Demuxer::openMP4(const string &files_string) {
    std::vector<std::string> files;
    if (File::is_dir(files_string)) {
        File::scanDir(files_string, [&](const string &path, bool is_dir) {
            if (!is_dir && end_with(path, ".mp4")) {
                files.emplace_back(path);
            }
            return true;
        }, true);
        std::sort(files.begin(), files.end());
    } else {
        files = split(files_string, ";");
    }

    uint64_t duration_ms = 0;
    for (auto &file : files) {
        auto demuxer = std::make_shared<MP4Demuxer>();
        demuxer->openMP4(file);
        auto file_duration_ms = demuxer->getDurationMS();
        if (!file_duration_ms) {
            file_duration_ms = getDurationFromRecordFileName(file);
            if (file_duration_ms) {
                WarnL << "fallback duration from filename for fmp4: " << file << ", duration_ms=" << file_duration_ms;
            } else {
                // Keep timeline monotonic to avoid dropping files by duplicate map key.
                file_duration_ms = 1;
                WarnL << "invalid mp4 duration, use 1ms fallback: " << file;
            }
        }
        _demuxers.emplace(duration_ms, demuxer);
        duration_ms += file_duration_ms;
    }
    _total_duration_ms = duration_ms;
    CHECK(!_demuxers.empty());
    _it = _demuxers.begin();
    for (auto &track : _it->second->getTracks(false)) {
        auto clone_track(track->clone());
        clone_track->setIndex(clone_track->getTrackType());
        _tracks.emplace(clone_track->getIndex(), clone_track);
        DebugL << "track index: " << track->getIndex() << " -> " << clone_track->getIndex();
    }
}

uint64_t MultiMP4Demuxer::getDurationMS() const {
    return _total_duration_ms;
}

void MultiMP4Demuxer::closeMP4() {
    _demuxers.clear();
    _it = _demuxers.end();
    _tracks.clear();
    _total_duration_ms = 0;
}

int64_t MultiMP4Demuxer::seekTo(int64_t stamp_ms) {
    if (stamp_ms >= (int64_t)getDurationMS()) {
        return -1;
    }
    _it = std::prev(_demuxers.upper_bound(stamp_ms));
    return _it->first + _it->second->seekTo(stamp_ms - _it->first);
}

Frame::Ptr MultiMP4Demuxer::readFrame(bool &keyFrame, bool &eof) {
    for (;;) {
        auto ret = _it->second->readFrame(keyFrame, eof);
        if (ret) {
            ret->setIndex(ret->getTrackType());
            auto it = _tracks.find(ret->getIndex());
            if (it != _tracks.end()) {
                auto ret2 = std::make_shared<FrameStamp>(ret);
                ret2->setStamp(_it->first + ret->dts(), _it->first + ret->pts());
                ret = std::move(ret2);
                it->second->inputFrame(ret);
            }
        }
        if (eof && _it != _demuxers.end()) {
            // 切换到下一个文件
            if (++_it == _demuxers.end()) {
                // 已经是最后一个文件了
                eof = true;
                return nullptr;
            }
            // 下一个文件从头开始播放
            _it->second->seekTo(0);
            continue;
        }
        return ret;
    }
}

std::vector<Track::Ptr> MultiMP4Demuxer::getTracks(bool trackReady) const {
    std::vector<Track::Ptr> ret;
    for (auto &pr : _tracks) {
        if (!trackReady || pr.second->ready()) {
            ret.emplace_back(pr.second);
        }
    }
    return ret;
}

}//namespace mediakit
#endif// ENABLE_MP4
