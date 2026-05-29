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

#include "RtspReplaySourceFactory.h"

#include "Common/MediaSource.h"
#include "Common/config.h"
#include "Thread/WorkThreadPool.h"
#include "Util/NoticeCenter.h"
#include "Util/logger.h"
#include "Util/util.h"
#include "RtspReplayCatalog.h"
#include "RtspReplayReader.h"
#include "RtspReplayTimeline.h"

#include <atomic>
#include <chrono>
#include <stdexcept>

using namespace std;
using namespace toolkit;

namespace mediakit {

static void releaseReplaySession(const string &session_stream, void *listener_tag, const std::shared_ptr<std::atomic<bool>> &released) {
    if (!released->exchange(true)) {
        NoticeCenter::Instance().delListener(listener_tag, Broadcast::kBroadcastMediaChanged);
    }
}

static bool isDigits(const string &s) {
    if (s.empty()) {
        return false;
    }
    for (auto ch : s) {
        if (ch < '0' || ch > '9') {
            return false;
        }
    }
    return true;
}

bool RtspReplaySourceFactory::validateStreamKey(const string &stream_id) {
    auto parts = split(stream_id, "/");
    if (parts.size() != 5) {
        return false;
    }
    if (parts[0].empty() || parts[1].empty()) {
        return false;
    }
    if (parts[2].size() < 2 || parts[2][0] != 's') {
        return false;
    }
    if (parts[3].size() < 2 || parts[3][0] != 'b' || !isDigits(parts[3].substr(1))) {
        return false;
    }
    if (parts[4].size() < 2 || parts[4][0] != 'e' || !isDigits(parts[4].substr(1))) {
        return false;
    }
    return true;
}

void createReplaySession(const string &schema, const string &vhost, const string &stream_id, string &out_session_stream) {
    RtspReplaySourceFactory::create(schema, vhost, stream_id, out_session_stream);
}

void RtspReplaySourceFactory::create(const string &schema, const string &vhost, const string &stream_id, string &out_session_stream) {
    auto create_begin = std::chrono::steady_clock::now();
    int64_t parse_ms = 0;

    out_session_stream.clear();

    if (!validateStreamKey(stream_id)) {
        return;
    }

    RtspReplayRequest request;
    try {
        auto parse_begin = std::chrono::steady_clock::now();
        request = RtspReplayCatalog::parseRequest(schema, vhost, stream_id);
        parse_ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - parse_begin).count();
    } catch (const exception &ex) {
        WarnL << "replay: invalid request: " << stream_id << ", err=" << ex.what();
        return;
    }

    auto build_begin = std::chrono::steady_clock::now();
    auto catalog = RtspReplayCatalog::build(request);
    auto build_ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - build_begin).count();
    if (catalog.segments.empty()) {
        WarnL << "replay: no recording files found for: " << stream_id;
        return;
    }

    try {
        auto session_stream = request.deviceId + "/" + request.channelId + "/" + request.streamType + "/sid_" + makeRandStr(8);
        auto timeline = std::make_shared<RtspReplayTimeline>(request.windowBeginAtMs, request.windowEndAtMs);

        ProtocolOption option;
        option.enable_mp4 = false;
        option.enable_hls = false;
        option.enable_hls_fmp4 = false;
        option.max_track = 16;

        GET_CONFIG(string, replay_app, Rtsp::kReplayAppName);
        MediaTuple tuple = {vhost, replay_app, session_stream, ""};
        auto reader_setup_begin = std::chrono::steady_clock::now();
        auto reader = std::make_shared<RtspReplayReader>(tuple, catalog, option);
        auto reader_setup_ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - reader_setup_begin).count();
        reader->bindTimeline(timeline);
        if (!reader->start(0, true, false)) {
            throw std::runtime_error("failed to start replay reader");
        }
        const auto &reader_perf = reader->getPerfStats();

        auto listener_tag = reader.get();
        auto released = std::make_shared<std::atomic<bool>>(false);
        NoticeCenter::Instance().addListener(listener_tag, Broadcast::kBroadcastMediaChanged, [session_stream, listener_tag, released](BroadcastMediaChangedArgs) {
            if (!bRegist && sender.getMediaTuple().stream == session_stream) {
                releaseReplaySession(session_stream, listener_tag, released);
            }
        });

        GET_CONFIG(int, max_wait_ms, General::kMaxStreamWaitTimeMS);
        auto cleanup_delay_ms = max_wait_ms + 5000;
        auto replay_app_name = replay_app;
        WorkThreadPool::Instance().getPoller()->doDelayTask(cleanup_delay_ms, [schema, vhost, replay_app_name, session_stream, listener_tag, released]() {
            auto src = MediaSource::find(schema, vhost, replay_app_name, session_stream, false);
            if (src) {
                // Source is registered and still active.
                return 0;
            }
            WarnL << "replay: cleanup inactive prepared session, stream=" << session_stream;
            releaseReplaySession(session_stream, listener_tag, released);
            return 0;
        });

        out_session_stream = session_stream;
        auto create_total_ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - create_begin).count();
        InfoL << "replay perf: create stream=" << session_stream
              << ", total_ms=" << create_total_ms
              << ", parse_ms=" << parse_ms
              << ", catalog_build_ms=" << build_ms
              << ", reader_setup_ms=" << reader_setup_ms
              << ", probe_open_ms=" << reader_perf.setup_probe_open_ms
              << ", start_total_ms=" << reader_perf.start_total_ms
              << ", demux_open_ms=" << reader_perf.demux_open_ms
              << ", prime_track_ms=" << reader_perf.prime_track_ms
              << ", seek_ms=" << reader_perf.seek_ms;
        InfoL << "replay: session started, stream=" << session_stream
              << ", files count=" << catalog.segments.size();
    } catch (const std::exception &ex) {
        WarnL << "replay: failed to create session: " << ex.what();
    }
}

} // namespace mediakit

#endif // ENABLE_MP4
