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

#include "Common/config.h"
#include "Util/NoticeCenter.h"
#include "Util/logger.h"
#include "Util/util.h"
#include "RtspReplayCatalog.h"
#include "RtspReplayReader.h"
#include "RtspReplayTimeline.h"

#include <atomic>
#include <stdexcept>

using namespace std;
using namespace toolkit;

namespace mediakit {

static atomic<int> s_replay_session_count{0};

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

bool RtspReplaySourceFactory::canHandle(const string &stream_id) {
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

static void increaseReplaySessionCountOrThrow() {
    GET_CONFIG(int, maxReplaySessions, Rtsp::kMaxReplaySessionCount);
    if (maxReplaySessions <= 0) {
        s_replay_session_count++;
        return;
    }

    int current = s_replay_session_count.load();
    do {
        if (current >= maxReplaySessions) {
            throw ReplayLimitException("replay session limit reached");
        }
    } while (!s_replay_session_count.compare_exchange_weak(current, current + 1));
}

void createReplaySession(const string &schema, const string &vhost, const string &stream_id, string &out_session_stream) {
    RtspReplaySourceFactory::create(schema, vhost, stream_id, out_session_stream);
}

void RtspReplaySourceFactory::create(const string &schema, const string &vhost, const string &stream_id, string &out_session_stream) {
    out_session_stream.clear();

    if (!canHandle(stream_id)) {
        return;
    }

    RtspReplayRequest request;
    try {
        request = RtspReplayCatalog::parseRequest(schema, vhost, stream_id);
    } catch (const exception &ex) {
        WarnL << "replay: invalid request: " << stream_id << ", err=" << ex.what();
        return;
    }

    auto catalog = RtspReplayCatalog::build(request);
    if (catalog.segments.empty()) {
        WarnL << "replay: no recording files found for: " << stream_id;
        return;
    }

    bool counted = false;
    try {
        increaseReplaySessionCountOrThrow();
        counted = true;

        auto session_stream = request.deviceId + "/" + request.channelId + "/" + request.streamType + "/sid_" + makeRandStr(8);
        auto timeline = std::make_shared<RtspReplayTimeline>(request.windowBeginAtMs, request.windowEndAtMs);

        ProtocolOption option;
        option.enable_mp4 = false;
        option.enable_hls = false;
        option.enable_hls_fmp4 = false;
        option.max_track = 16;

        MediaTuple tuple = {vhost, "replay", session_stream, ""};
        auto reader = std::make_shared<RtspReplayReader>(tuple, catalog, option);
        reader->bindTimeline(timeline);
        if (!reader->start(0, true, false)) {
            throw std::runtime_error("failed to start replay reader");
        }

        auto listener_tag = reader.get();
        auto released = std::make_shared<std::atomic<bool>>(false);
        NoticeCenter::Instance().addListener(listener_tag, Broadcast::kBroadcastMediaChanged, [session_stream, listener_tag, released](BroadcastMediaChangedArgs) {
            if (!bRegist && sender.getMediaTuple().stream == session_stream) {
                if (!released->exchange(true)) {
                    s_replay_session_count--;
                }
                NoticeCenter::Instance().delListener(listener_tag, Broadcast::kBroadcastMediaChanged);
            }
        });

        out_session_stream = session_stream;
        InfoL << "replay: session starting, stream=" << session_stream
              << ", files=" << catalog.segments.size();
    } catch (const ReplayLimitException &) {
        throw;
    } catch (const exception &ex) {
        if (counted) {
            s_replay_session_count--;
        }
        WarnL << "replay: failed to create session: " << ex.what();
    }
}

} // namespace mediakit

#endif // ENABLE_MP4
