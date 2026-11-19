/*
 * Checks BatchPublishSession (WHIP Batch): SDP layout validation, slot binding and release,
 * session close, and RTP routing by MID and SSRC. Exits non-zero on the first failed check.
 */

#include <cstdlib>
#include <iostream>
#include <sstream>
#include <string>
#include "Util/logger.h"
#include "Rtsp/Rtsp.h"
#include "../webrtc/MultiSourceWebRtcPusher/BatchPublishSession.h"

using namespace std;
using namespace toolkit;
using namespace mediakit;

static int s_failed = 0;

#define EXPECT(cond)                                                                                                   \
    do {                                                                                                               \
        if (!(cond)) {                                                                                                 \
            cerr << "FAIL " << __LINE__ << ": " #cond << endl;                                                       \
            ++s_failed;                                                                                                \
        }                                                                                                              \
    } while (0)

// One m-line: kind is "audio", "video" or "application"; ssrc 0 leaves out a=ssrc.
static string mline(const string &kind, const string &mid, uint32_t ssrc) {
    stringstream ss;
    if (kind == "application") {
        ss << "m=application 9 UDP/DTLS/SCTP webrtc-datachannel\r\n"
           << "c=IN IP4 0.0.0.0\r\n"
           << "a=mid:" << mid << "\r\n"
           << "a=sctp-port:5000\r\n";
        return ss.str();
    }
    auto pt = kind == "audio" ? 111 : 96;
    ss << "m=" << kind << " 9 UDP/TLS/RTP/SAVPF " << pt << "\r\n"
       << "c=IN IP4 0.0.0.0\r\n"
       << "a=rtcp:9 IN IP4 0.0.0.0\r\n"
       << "a=ice-ufrag:abcd\r\n"
       << "a=ice-pwd:abcdefghijklmnopqrstuvwx\r\n"
       << "a=fingerprint:sha-256 00:11:22:33:44:55:66:77:88:99:AA:BB:CC:DD:EE:FF:00:11:22:33:44:55:66:77:88:99:AA:BB:CC:DD:EE:FF\r\n"
       << "a=setup:active\r\n"
       << "a=mid:" << mid << "\r\n"
       << "a=recvonly\r\n"
       << "a=rtcp-mux\r\n";
    if (kind == "audio") {
        ss << "a=rtpmap:111 opus/48000/2\r\n";
    } else {
        ss << "a=rtpmap:96 VP8/90000\r\n";
    }
    if (ssrc) {
        ss << "a=ssrc:" << ssrc << " cname:test\r\n";
    }
    return ss.str();
}

static RtcSession makeSdp(const string &mlines) {
    string sdp = "v=0\r\n"
                 "o=- 1 2 IN IP4 127.0.0.1\r\n"
                 "s=-\r\n"
                 "t=0 0\r\n"
                 "a=group:BUNDLE 0\r\n"
                 "a=msid-semantic: WMS\r\n";
    RtcSession session;
    session.loadFrom(sdp + mlines);
    return session;
}

static const string kRtspSdp = "v=0\r\n"
                               "o=- 0 0 IN IP4 0.0.0.0\r\n"
                               "s=test\r\n"
                               "t=0 0\r\n"
                               "m=audio 0 RTP/AVP 111\r\n"
                               "a=rtpmap:111 opus/48000/2\r\n"
                               "a=control:trackID=0\r\n"
                               "m=video 0 RTP/AVP 96\r\n"
                               "a=rtpmap:96 VP8/90000\r\n"
                               "a=control:trackID=1\r\n";

static RtpPacket::Ptr makeRtp(uint32_t ssrc) {
    auto rtp = RtpPacket::create();
    rtp->setCapacity(RtpPacket::kRtpTcpHeaderSize + RtpPacket::kRtpHeaderSize + 4);
    rtp->setSize(RtpPacket::kRtpTcpHeaderSize + RtpPacket::kRtpHeaderSize + 4);
    memset(rtp->data(), 0, rtp->size());
    auto header = rtp->getHeader();
    header->version = 2;
    header->pt = 96;
    header->ssrc = htonl(ssrc);
    rtp->type = TrackVideo;
    return rtp;
}

// Two slots: (a0, v0) and (a1, v1), with a data channel in between.
static BatchPublishSession::Ptr makeActiveSession() {
    auto session = std::make_shared<BatchPublishSession>("live", "nvr1", "transport1", 2);
    auto sdp = makeSdp(mline("audio", "a0", 1000) + mline("video", "v0", 2000) + mline("application", "dc", 0)
                       + mline("audio", "a1", 1001) + mline("video", "v1", 0));
    EXPECT(session->initFromSdp(sdp, kRtspSdp));
    return session;
}

static void testSdpLayout() {
    auto pair = mline("audio", "a0", 1000) + mline("video", "v0", 2000);

    // Valid: one pair per slot; application m-lines are skipped.
    {
        BatchPublishSession session("live", "nvr1", "t", 1);
        EXPECT(session.initFromSdp(makeSdp(mline("application", "dc", 0) + pair), kRtspSdp));
        EXPECT(session.getInfo()["state"].asString() == "Active");
        EXPECT(session.getInfo()["streams"].size() == 1);
    }
    // Video before audio.
    {
        BatchPublishSession session("live", "nvr1", "t", 1);
        EXPECT(!session.initFromSdp(makeSdp(mline("video", "v0", 2000) + mline("audio", "a0", 1000)), kRtspSdp));
    }
    // Audio without a video.
    {
        BatchPublishSession session("live", "nvr1", "t", 2);
        EXPECT(!session.initFromSdp(makeSdp(pair + mline("audio", "a1", 1001)), kRtspSdp));
    }
    // Two audio m-lines in a row.
    {
        BatchPublishSession session("live", "nvr1", "t", 1);
        EXPECT(!session.initFromSdp(makeSdp(mline("audio", "a0", 1000) + mline("audio", "a1", 1001)), kRtspSdp));
    }
    // Pair count differs from the slot count.
    {
        BatchPublishSession session("live", "nvr1", "t", 2);
        EXPECT(!session.initFromSdp(makeSdp(pair), kRtspSdp));
    }
}

static void testBindUnbind() {
    string slot_id, reason;
    bool reused = false, not_bound = false;

    // Not active before initFromSdp.
    {
        BatchPublishSession session("live", "nvr1", "t", 1);
        EXPECT(!session.bindSource("s1", 0, slot_id, reused, reason));
        EXPECT(reason == "sessionNotActive");
    }

    auto session = makeActiveSession();

    // Slots are handed out in order.
    EXPECT(session->bindSource("s1", 4, slot_id, reused, reason));
    EXPECT(slot_id == "0" && !reused);
    EXPECT(session->bindSource("s2", 0, slot_id, reused, reason));
    EXPECT(slot_id == "1" && !reused);

    // A repeated bind keeps the slot and refreshes max_viewers.
    EXPECT(session->bindSource("s1", 8, slot_id, reused, reason));
    EXPECT(slot_id == "0" && reused && reason == "reused");
    BatchPublishSession::SourceBinding binding;
    EXPECT(session->getSourceBinding("s1", binding));
    EXPECT(binding.max_viewers == 8 && binding.play_app == "live" && binding.play_stream == "s1");

    // Every slot is taken.
    EXPECT(!session->bindSource("s3", 0, slot_id, reused, reason));
    EXPECT(reason == "noAvailableStreamId");
    EXPECT(session->getSummary()["freeStreamCount"].asUInt64() == 0);
    EXPECT(session->getSummary()["boundStreamCount"].asUInt64() == 2);

    // Unbind frees the slot for the next source; a second unbind is a no-op.
    slot_id.clear();
    session->unbindSource("s1", slot_id, not_bound);
    EXPECT(slot_id == "0" && !not_bound);
    slot_id = "unchanged";
    session->unbindSource("s1", slot_id, not_bound);
    EXPECT(not_bound && slot_id == "unchanged");
    EXPECT(!session->getSourceBinding("s1", binding));

    EXPECT(session->bindSource("s3", 0, slot_id, reused, reason));
    EXPECT(slot_id == "0" && !reused);
    EXPECT(session->getBoundSourceIds().size() == 2);

    // Closing releases everything and refuses later binds.
    session->closeAndReleaseAllSources();
    EXPECT(session->getBoundSourceIds().empty());
    EXPECT(session->getInfo()["state"].asString() == "Closed");
    EXPECT(session->getSummary()["freeStreamCount"].asUInt64() == 2);
    EXPECT(!session->bindSource("s4", 0, slot_id, reused, reason));
    EXPECT(reason == "sessionNotActive");
}

static void testRtpRouting() {
    auto session = makeActiveSession();
    auto rtp = makeRtp(2000);

    // No slot for an unknown MID or SSRC.
    EXPECT(!session->onRtp(TrackVideo, "unknown", 0, 1, rtp));
    EXPECT(!session->onRtp(TrackVideo, "", 9999, 1, rtp));
    // A known slot with no source bound drops the packet and is not receiving.
    EXPECT(!session->onRtp(TrackVideo, "v0", 2000, 1, rtp));
    EXPECT(!session->onRtp(TrackVideo, "", 2000, 1, rtp));
    EXPECT(session->getSummary()["receivingStreamCount"].asUInt64() == 0);

    // Slot 1's video m-line had no a=ssrc: the first packet routed by MID teaches its SSRC.
    EXPECT(!session->onRtp(TrackVideo, "v1", 3001, 1, rtp));
    auto info = session->getInfo();
    EXPECT(info["streams"][1]["videoSsrc"].asUInt64() == 3001);
    EXPECT(info["streams"][0]["videoSsrc"].asUInt64() == 2000);
    EXPECT(info["streams"][1]["audioSsrc"].asUInt64() == 1001);

    // An SSRC is matched against the track type: audio SSRC 1000 does not route a video packet.
    EXPECT(!session->onRtp(TrackVideo, "", 1000, 1, rtp));
}

int main() {
    Logger::Instance().add(std::make_shared<ConsoleChannel>("stderr", LError));
    testSdpLayout();
    testBindUnbind();
    testRtpRouting();
    if (s_failed) {
        cerr << s_failed << " check(s) failed" << endl;
        return EXIT_FAILURE;
    }
    cout << "test_batch_publish_session: all checks passed" << endl;
    return EXIT_SUCCESS;
}
