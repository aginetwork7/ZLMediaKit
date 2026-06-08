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

bool RtspReplaySourceFactory::validateStreamKey(const string &streamId) {
    // Reuse parseRequest as the single validation source so the two paths can't drift; here it
    // is only a cheap non-throwing gate.
    try {
        RtspReplayCatalog::parseRequest("", "", streamId);
        return true;
    } catch (const exception &) {
        return false;
    }
}

void createReplaySession(const string &schema, const string &vhost, const string &streamId, string &out_session_stream) {
    RtspReplaySourceFactory::create(schema, vhost, streamId, out_session_stream);
}

void RtspReplaySourceFactory::create(const string &schema, const string &vhost, const string &streamId, string &out_session_stream) {
    auto create_begin = std::chrono::steady_clock::now();
    int64_t parse_ms = 0;
    out_session_stream.clear();
    RtspReplayRequest request;

    try {
        auto parse_begin = std::chrono::steady_clock::now();
        request = RtspReplayCatalog::parseRequest(schema, vhost, streamId);
        parse_ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - parse_begin).count();
    } catch (const exception &ex) {
        WarnL << "replay: invalid request: " << streamId << ", err=" << ex.what();
        return;
    }

    auto build_begin = std::chrono::steady_clock::now();
    auto catalog = RtspReplayCatalog::build(request);
    auto build_ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - build_begin).count();
    if (catalog._segments.empty()) {
        WarnL << "replay: no recording files found for: " << streamId;
        return;
    }

    try {
        auto session_stream = request._device_id + "/" + request._channel_id + "/" + request._stream_type + "/sid_" + makeRandStr(8);

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
        if (!reader->start(0, true, false)) {
            throw std::runtime_error("failed to start replay reader");
        }
        const auto &reader_perf = reader->getPerfStats();

        // Use the reader's raw pointer purely as the NoticeCenter listener key (it is never
        // dereferenced through this tag). The reader stays alive because its own repeating Timer
        // captures a shared_ptr to itself, so the tag remains a valid unique id until cleanup.
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
              << ", probe_open_ms=" << reader_perf._setup_probe_open_ms
              << ", start_total_ms=" << reader_perf._start_total_ms
              << ", demux_open_ms=" << reader_perf._demux_open_ms
              << ", prime_track_ms=" << reader_perf._prime_track_ms;
        InfoL << "replay: session started, stream=" << session_stream
              << ", files count=" << catalog._segments.size();
    } catch (const std::exception &ex) {
        WarnL << "replay: failed to create session: " << ex.what();
    }
}

} // namespace mediakit

#endif // ENABLE_MP4
