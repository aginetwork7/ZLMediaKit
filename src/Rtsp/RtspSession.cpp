/*
 * Copyright (c) 2016-present The ZLMediaKit project authors. All Rights Reserved.
 *
 * This file is part of ZLMediaKit(https://github.com/ZLMediaKit/ZLMediaKit).
 *
 * Use of this source code is governed by MIT-like license that can be found in the
 * LICENSE file in the root of the source tree. All contributing project authors
 * may be found in the AUTHORS file in the root of the source tree.
 */

#include <chrono>
#include <fstream>
#include <iomanip>
#include <sys/stat.h>
#include "Common/config.h"
#include "UDPServer.h"
#include "RtspSession.h"
#include "Util/MD5.h"
#include "Util/base64.h"
#include "RtpMultiCaster.h"
#include "Rtcp/RtcpContext.h"
#include "json/json.h"

#ifdef ENABLE_OPENSSL
#include <openssl/sha.h>
#endif

using namespace std;
using namespace toolkit;

namespace mediakit {

struct RtspAuthFileCache {
    string path;
    string username;
    string password;
    time_t mtime = 0;
    off_t size = -1;
    bool loaded = false;
};

static RtspAuthFileCache g_rtspAuthFileCache;
static recursive_mutex g_mtxRtspAuthFileCache;

static bool getFileStat(const string &path, time_t &mtime, off_t &size) {
    struct stat st = {0};
    if (::stat(path.c_str(), &st) != 0) {
        return false;
    }
    mtime = st.st_mtime;
    size = st.st_size;
    return true;
}

// Load RTSP auth credentials from a JSON file.
// File format: {"username":"xxx","password":"xxx"}
static bool loadRtspAuthFile(const string &path, string &username, string &password) {
    if (path.empty()) {
        ErrorL << "rtsp authFile path is empty";
        return false;
    }

    lock_guard<recursive_mutex> lock(g_mtxRtspAuthFileCache);
    auto &cache = g_rtspAuthFileCache;
    auto same_path = cache.loaded && cache.path == path;

    time_t mtime = 0;
    off_t size = -1;
    if (!getFileStat(path, mtime, size)) {
        ErrorL << "rtsp authFile stat failed: " << path;
        return false;
    }

    if (same_path && cache.mtime == mtime && cache.size == size) {
        username = cache.username;
        password = cache.password;
        return true;
    }

    ifstream ifs(path);
    if (!ifs.is_open()) {
        ErrorL << "rtsp authFile open failed: " << path;
        return false;
    }

    Json::Value root;
    Json::CharReaderBuilder builder;
    string errs;
    if (!Json::parseFromStream(builder, ifs, &root, &errs)) {
        ErrorL << "rtsp authFile parse failed: " << path << ", err: " << errs;
        return false;
    }

    auto new_username = root["username"].asString();
    auto new_password = root["password"].asString();
    if (new_username.empty() || new_password.empty()) {
        ErrorL << "rtsp authFile missing username/password: " << path;
        return false;
    }

    cache.path = path;
    cache.username = std::move(new_username);
    cache.password = std::move(new_password);
    cache.mtime = mtime;
    cache.size = size;
    cache.loaded = true;
    username = cache.username;
    password = cache.password;
    return true;
}

/**
 * rtsp协议有多种方式传输rtp数据包，目前已支持包括以下4种
 * 1: rtp over udp ,这种方式是rtp通过单独的udp端口传输
 * 2: rtp over udp_multicast,这种方式是rtp通过共享udp组播端口传输
 * 3: rtp over tcp,这种方式是通过rtsp信令tcp通道完成传输
 * 4: rtp over http，下面着重讲解：rtp over http
 *
 * rtp over http 是把rtsp协议伪装成http协议以达到穿透防火墙的目的，
 * 此时播放器会发送两次http请求至rtsp服务器，第一次是http get请求，
 * 第二次是http post请求。
 *
 * 这两次请求通过http请求头中的x-sessioncookie键完成绑定
 *
 * 第一次http get请求用于接收rtp、rtcp和rtsp回复，后续该链接不再发送其他请求
 * 第二次http post请求用于发送rtsp请求，rtsp握手结束后可能会断开连接，此时我们还要维持rtp发送
 * 需要指出的是http post请求中的content负载就是base64编码后的rtsp请求包，
 * 播放器会把rtsp请求伪装成http content负载发送至rtsp服务器，然后rtsp服务器又把回复发送给第一次http get请求的tcp链接
 * 这样，对防火墙而言，本次rtsp会话就是两次http请求，防火墙就会放行数据
 *
 * zlmediakit在处理rtsp over http的请求时，会把http poster中的content数据base64解码后转发给http getter处理
 */


//rtsp over http 情况下get请求实例，在请求实例用于接收rtp数据包
static unordered_map<string, weak_ptr<RtspSession> > g_mapGetter;
//对g_mapGetter上锁保护
static recursive_mutex g_mtxGetter;

// 对两个等长的十六进制摘要做常量时间、忽略大小写的比较，避免 strcasecmp 提前返回
// 而通过比较耗时泄漏「已匹配前缀长度」的时序侧信道。
static bool digestEquals(const string &a, const string &b) {
    if (a.size() != b.size()) {
        return false;
    }
    unsigned char diff = 0;
    for (size_t i = 0; i < a.size(); ++i) {
        diff |= (unsigned char)(tolower((unsigned char)a[i]) ^ tolower((unsigned char)b[i]));
    }
    return diff == 0;
}

#ifdef ENABLE_OPENSSL
static string sha256Hex(const string &input) {
    unsigned char out[SHA256_DIGEST_LENGTH];
    if (!SHA256((const unsigned char *)input.data(), input.size(), out)) {
        return "";
    }
    static constexpr char kHex[] = "0123456789abcdef";
    string ret;
    ret.resize(SHA256_DIGEST_LENGTH * 2);
    for (size_t i = 0; i < SHA256_DIGEST_LENGTH; ++i) {
        ret[i * 2] = kHex[out[i] >> 4];
        ret[i * 2 + 1] = kHex[out[i] & 0x0F];
    }
    return ret;
}
#endif

// 真实 active 播放会话配额：only 进入 PLAY 的会话才计数（DESCRIBE/SETUP 不算）。
// g_play_total 为 live+replay 总数，replay 另受 g_play_replay 约束；0 表示不限制。
// 两个计数需原子事务（replay 要同时满足两个上限）
static std::mutex g_mtxPlayQuota;
static size_t g_play_total = 0;
static size_t g_play_replay = 0;

static bool acquirePlayQuota(bool is_replay) {
    GET_CONFIG(int, max_total, Rtsp::kMaxSessionCount);
    GET_CONFIG(int, max_replay, Rtsp::kMaxReplaySessionCount);
    std::lock_guard<std::mutex> lck(g_mtxPlayQuota);
    if (max_total > 0 && g_play_total >= (size_t)max_total) {
        return false;
    }
    if (is_replay && max_replay > 0 && g_play_replay >= (size_t)max_replay) {
        return false;
    }
    ++g_play_total;
    if (is_replay) {
        ++g_play_replay;
    }
    return true;
}

static void releasePlayQuota(bool is_replay) {
    std::lock_guard<std::mutex> lck(g_mtxPlayQuota);
    if (g_play_total > 0) {
        --g_play_total;
    }
    if (is_replay && g_play_replay > 0) {
        --g_play_replay;
    }
}

RtspSession::RtspSession(const Socket::Ptr &sock) : Session(sock) {
    GET_CONFIG(uint32_t,keep_alive_sec,Rtsp::kKeepAliveSecond);
    sock->setSendTimeOutSecond(keep_alive_sec);
}

RtspSession::~RtspSession() {
    // 析构是唯一能覆盖所有退出路径（TEARDOWN/断连/异常）的释放点，保证配额不泄漏。
    if (_play_quota_acquired) {
        releasePlayQuota(_play_quota_is_replay);
    }
}

void RtspSession::onError(const SockException &err) {
    bool is_player = !_push_src_ownership;
    uint64_t duration = _alive_ticker.createdTime() / 1000;
    WarnP(this) << (is_player ? "RTSP播放器(" : "RTSP推流器(")
                << _media_info.shortUrl()
                << ")断开:" << err.what()
                << ",耗时(s):" << duration;

    if (_rtp_type == Rtsp::RTP_MULTICAST) {
        //取消UDP端口监听
        UDPServer::Instance().stopListenPeer(get_peer_ip().data(), this);
    }

    if (_http_x_sessioncookie.size() != 0) {
        //移除http getter的弱引用记录
        lock_guard<recursive_mutex> lock(g_mtxGetter);
        g_mapGetter.erase(_http_x_sessioncookie);
    }

    //流量统计事件广播
    GET_CONFIG(uint32_t, iFlowThreshold, General::kFlowThreshold);
    if (_bytes_usage >= iFlowThreshold * 1024) {
        NOTICE_EMIT(BroadcastFlowReportArgs, Broadcast::kBroadcastFlowReport, _media_info, _bytes_usage, duration, is_player, *this);
    }

    //如果是主动关闭的，那么不延迟注销
    if (_push_src && _continue_push_ms  && err.getErrCode() != Err_shutdown) {
        //取消所有权
        _push_src_ownership = nullptr;
        //延时10秒注销流
        auto push_src = std::move(_push_src);
        getPoller()->doDelayTask(_continue_push_ms, [push_src]() { return 0; });
    }
}

void RtspSession::onManager() {
    GET_CONFIG(uint32_t, handshake_sec, Rtsp::kHandshakeSecond);
    GET_CONFIG(uint32_t, keep_alive_sec, Rtsp::kKeepAliveSecond);

    if (_alive_ticker.createdTime() > handshake_sec * 1000) {
        if (_sessionid.size() == 0) {
            shutdown(SockException(Err_timeout,"illegal connection"));
            return;
        }
    }

    if (_push_src && _alive_ticker.elapsedTime() > keep_alive_sec * 1000) {
        //推流超时
        shutdown(SockException(Err_timeout, "pusher session timeout"));
        return;
    }

    if (!_push_src && _rtp_type == Rtsp::RTP_UDP && _alive_ticker.elapsedTime() > keep_alive_sec * 4000) {
        //rtp over udp播放器超时
        shutdown(SockException(Err_timeout, "rtp over udp player timeout"));
    }
}

void RtspSession::onRecv(const Buffer::Ptr &buf) {
    _alive_ticker.resetTime();
    _bytes_usage += buf->size();
    if (_on_recv) {
        //http poster的请求数据转发给http getter处理
        _on_recv(buf);
    } else {
        input(buf->data(), buf->size());
    }
}

void RtspSession::onWholeRtspPacket(Parser &parser) {
    string method = parser.method(); //提取出请求命令字
    _cseq = atoi(parser["CSeq"].data());
    if (_content_base.empty() && method != "GET" && method != "POST" ) {
        RtspUrl rtsp;
        rtsp.parse(parser.url());
        _content_base = rtsp._url;
        _media_info.parse(parser.fullUrl());
        _media_info.schema = RTSP_SCHEMA;
        _media_info.protocol = overSsl() ? "rtsps" : "rtsp";
    }

    using rtsp_request_handler = void (RtspSession::*)(const Parser &parser);
    static unordered_map<string, rtsp_request_handler> s_cmd_functions;
    static onceToken token([]() {
        s_cmd_functions.emplace("OPTIONS", &RtspSession::handleReq_Options);
        s_cmd_functions.emplace("DESCRIBE", &RtspSession::handleReq_Describe);
        s_cmd_functions.emplace("ANNOUNCE", &RtspSession::handleReq_ANNOUNCE);
        s_cmd_functions.emplace("RECORD", &RtspSession::handleReq_RECORD);
        s_cmd_functions.emplace("SETUP", &RtspSession::handleReq_Setup);
        s_cmd_functions.emplace("PLAY", &RtspSession::handleReq_Play);
        s_cmd_functions.emplace("PAUSE", &RtspSession::handleReq_Pause);
        s_cmd_functions.emplace("TEARDOWN", &RtspSession::handleReq_Teardown);
        s_cmd_functions.emplace("GET", &RtspSession::handleReq_Get);
        s_cmd_functions.emplace("POST", &RtspSession::handleReq_Post);
        s_cmd_functions.emplace("SET_PARAMETER", &RtspSession::handleReq_SET_PARAMETER);
        s_cmd_functions.emplace("GET_PARAMETER", &RtspSession::handleReq_SET_PARAMETER);
    });

    auto it = s_cmd_functions.find(method);
    if (it == s_cmd_functions.end()) {
        sendRtspResponse("403 Forbidden");
        throw SockException(Err_shutdown, StrPrinter << "403 Forbidden:" << method);
    }

    (this->*(it->second))(parser);
    parser.clear();
}

void RtspSession::onRtpPacket(const char *data, size_t len) {
    uint8_t interleaved = data[1];
    if (interleaved % 2 == 0) {
        CHECK(len > RtpPacket::kRtpHeaderSize + RtpPacket::kRtpTcpHeaderSize);
        RtpHeader *header = (RtpHeader *)(data + RtpPacket::kRtpTcpHeaderSize);
        auto track_idx = getTrackIndexByPT(header->pt);
        handleOneRtp(track_idx, _sdp_track[track_idx]->_type, _sdp_track[track_idx]->_samplerate, (uint8_t *) data + RtpPacket::kRtpTcpHeaderSize, len - RtpPacket::kRtpTcpHeaderSize);
    } else {
        auto track_idx = getTrackIndexByInterleaved(interleaved - 1);
        onRtcpPacket(track_idx, _sdp_track[track_idx], data + RtpPacket::kRtpTcpHeaderSize, len - RtpPacket::kRtpTcpHeaderSize);
    }
}

void RtspSession::onRtcpPacket(int track_idx, SdpTrack::Ptr &track, const char *data, size_t len){
    auto rtcp_arr = RtcpHeader::loadFromBytes((char *) data, len);
    for (auto &rtcp : rtcp_arr) {
        _rtcp_context[track_idx]->onRtcp(rtcp);
        if ((RtcpType) rtcp->pt == RtcpType::RTCP_SR) {
            auto sr = (RtcpSR *) (rtcp);
            //设置rtp时间戳与ntp时间戳的对应关系
            setNtpStamp(track_idx, sr->rtpts, sr->getNtpUnixStampMS());
        }
    }
}

ssize_t RtspSession::getContentLength(Parser &parser) {
    if(parser.method() == "POST"){
        //http post请求的content数据部分是base64编码后的rtsp请求信令包
        return remainDataSize();
    }
    return RtspSplitter::getContentLength(parser);
}

void RtspSession::handleReq_Options(const Parser &parser) {
    //支持这些命令
    sendRtspResponse("200 OK",{"Public" , "OPTIONS, DESCRIBE, SETUP, TEARDOWN, PLAY, PAUSE, ANNOUNCE, RECORD, SET_PARAMETER, GET_PARAMETER"});
}

void RtspSession::handleReq_ANNOUNCE(const Parser &parser) {
    auto full_url = parser.fullUrl();
    _content_base = full_url;
    if (end_with(full_url, ".sdp")) {
        //去除.sdp后缀，防止EasyDarwin推流器强制添加.sdp后缀
        full_url = full_url.substr(0, full_url.length() - 4);
        _media_info.parse(full_url);
        _media_info.protocol = overSsl() ? "rtsps" : "rtsp";
    }

    if (_media_info.app.empty() || _media_info.stream.empty()) {
        //推流rtsp url必须最少两级(rtsp://host/app/stream_id)，不允许莫名其妙的推流url
        static constexpr auto err = "rtsp推流url非法,最少确保两级rtsp url";
        sendRtspResponse("403 Forbidden", {"Content-Type", "text/plain"}, err);
        throw SockException(Err_shutdown, StrPrinter << err << ":" << full_url);
    }

    auto onRes = [this, parser, full_url](const string &err, const ProtocolOption &option) {
        if (!err.empty()) {
            sendRtspResponse("401 Unauthorized", { "Content-Type", "text/plain" }, err);
            shutdown(SockException(Err_shutdown, StrPrinter << "401 Unauthorized:" << err));
            return;
        }

        assert(!_push_src);
        auto src = MediaSource::find(RTSP_SCHEMA, _media_info.vhost, _media_info.app, _media_info.stream);
        auto push_failed = (bool)src;

        while (src) {
            //尝试断连后继续推流
            auto rtsp_src = dynamic_pointer_cast<RtspMediaSourceImp>(src);
            if (!rtsp_src) {
                //源不是rtsp推流产生的
                break;
            }
            auto ownership = rtsp_src->getOwnership();
            if (!ownership) {
                //获取推流源所有权失败
                break;
            }
            _push_src = std::move(rtsp_src);
            _push_src_ownership = std::move(ownership);
            push_failed = false;
            break;
        }

        if (push_failed) {
            sendRtspResponse("406 Not Acceptable", { "Content-Type", "text/plain" }, "Already publishing.");
            string err = StrPrinter << "ANNOUNCE: Already publishing:" << _media_info.shortUrl() << endl;
            throw SockException(Err_shutdown, err);
        }

        SdpParser sdpParser(parser.content());
        _sessionid = makeRandStr(12);
        _sdp_track = sdpParser.getAvailableTrack();
        if (_sdp_track.empty()) {
            // sdp无效
            static constexpr auto err = "sdp中无有效track";
            sendRtspResponse("403 Forbidden", { "Content-Type", "text/plain" }, err);
            shutdown(SockException(Err_shutdown, StrPrinter << err << ":" << full_url));
            return;
        }
        _rtcp_context.clear();
        for (auto &track : _sdp_track) {
            _rtcp_context.emplace_back(std::make_shared<RtcpContextForRecv>());
        }

        if (!_push_src) {
            _push_src = std::make_shared<RtspMediaSourceImp>(_media_info);
            //获取所有权
            _push_src_ownership = _push_src->getOwnership();
            _push_src->setProtocolOption(option);
            _push_src->setSdp(parser.content());
        }

        _push_src->setListener(static_pointer_cast<RtspSession>(shared_from_this()));
        _continue_push_ms = option.continue_push_ms;
        sendRtspResponse("200 OK");
    };

    weak_ptr<RtspSession> weak_self = static_pointer_cast<RtspSession>(shared_from_this());
    Broadcast::PublishAuthInvoker invoker = [weak_self, onRes](const string &err, const ProtocolOption &option) {
        auto strong_self = weak_self.lock();
        if (!strong_self) {
            return;
        }
        strong_self->async([weak_self, onRes, err, option]() {
            auto strong_self = weak_self.lock();
            if (!strong_self) {
                return;
            }
            onRes(err, option);
        });
    };

    //rtsp推流需要鉴权
    auto flag = NOTICE_EMIT(BroadcastMediaPublishArgs, Broadcast::kBroadcastMediaPublish, MediaOriginType::rtsp_push, _media_info, invoker, *this);
    if (!flag) {
        //该事件无人监听,默认不鉴权
        onRes("", ProtocolOption());
    }
}

void RtspSession::handleReq_RECORD(const Parser &parser){
    if (_sdp_track.empty() || parser["Session"] != _sessionid) {
        send_SessionNotFound();
        throw SockException(Err_shutdown, _sdp_track.empty() ? "can not find any available track when record" : "session not found when record");
    }

    _StrPrinter rtp_info;
    for (auto &track : _sdp_track) {
        if (track->_inited == false) {
            //还有track没有setup
            shutdown(SockException(Err_shutdown, "track not setuped"));
            return;
        }
        rtp_info << "url=" << track->getControlUrl(_content_base) << ",";
    }
    rtp_info.pop_back();
    sendRtspResponse("200 OK", {"RTP-Info", rtp_info});
    if (_rtp_type == Rtsp::RTP_TCP) {
        //如果是rtsp推流服务器，并且是TCP推流，设置socket flags,，这样能提升接收性能
        setSocketFlags();
    }
}

void RtspSession::emitOnPlay(){
    weak_ptr<RtspSession> weak_self = static_pointer_cast<RtspSession>(shared_from_this());
    //url鉴权回调
    auto onRes = [weak_self](const string &err) {
        auto strong_self = weak_self.lock();
        if (!strong_self) {
            return;
        }
        if (!err.empty()) {
            //播放url鉴权失败
            strong_self->sendRtspResponse("401 Unauthorized", {"Content-Type", "text/plain"}, err);
            strong_self->shutdown(SockException(Err_shutdown, StrPrinter << "401 Unauthorized:" << err));
            return;
        }
        strong_self->onAuthSuccess();
    };

    Broadcast::AuthInvoker invoker = [weak_self, onRes](const string &err) {
        auto strong_self = weak_self.lock();
        if (!strong_self) {
            return;
        }
        strong_self->async([onRes, err, weak_self]() {
            onRes(err);
        });
    };

    //广播通用播放url鉴权事件
    auto flag = _emit_on_play ? false : NOTICE_EMIT(BroadcastMediaPlayedArgs, Broadcast::kBroadcastMediaPlayed, _media_info, invoker, *this);
    if (!flag) {
        //该事件无人监听,默认不鉴权
        onRes("");
    }
    //已经鉴权过了
    _emit_on_play = true;
}

void RtspSession::handleReq_Describe(const Parser &parser) {
    //该请求中的认证信息
    auto authorization = parser["Authorization"];
    weak_ptr<RtspSession> weak_self = static_pointer_cast<RtspSession>(shared_from_this());
    //rtsp专属鉴权是否开启事件回调
    onGetRealm invoker = [weak_self, authorization](const string &realm) {
        auto strong_self = weak_self.lock();
        if (!strong_self) {
            //本对象已经销毁
            return;
        }
        //切换到自己的线程然后执行
        strong_self->async([weak_self, realm, authorization]() {
            auto strong_self = weak_self.lock();
            if (!strong_self) {
                //本对象已经销毁
                return;
            }
            if (realm.empty()) {
                //realm为空，鉴权失败
                strong_self->onAuthFailed("unknown", "realm is empty, auth rejected");
                return;
            }
            strong_self->_rtsp_realm = realm;
            strong_self->onAuthUser(realm, authorization);
        });
    };

    if(_rtsp_realm.empty()){
        GET_CONFIG(string, auth_realm, Rtsp::kAuthRealm);
        //realm配置为空时使用默认值
        invoker(auth_realm.empty() ? "tinynvr" : auth_realm);
    }else{
        invoker(_rtsp_realm);
    }
}

void RtspSession::onAuthSuccess() {
    weak_ptr<RtspSession> weak_self = static_pointer_cast<RtspSession>(shared_from_this());
    MediaSource::findAsync(_media_info, weak_self.lock(), [weak_self](const MediaSource::Ptr &src){
        auto strong_self = weak_self.lock();
        if(!strong_self){
            return;
        }
        auto rtsp_src = dynamic_pointer_cast<RtspMediaSource>(src);
        if (!rtsp_src) {
            //未找到相应的MediaSource
            string err = StrPrinter << "no such stream:" << strong_self->_media_info.shortUrl();
            strong_self->send_StreamNotFound();
            strong_self->shutdown(SockException(Err_shutdown,err));
            return;
        }
        //找到了相应的rtsp流
        SdpParser sdp_parser(rtsp_src->getSdp());
        strong_self->_sdp_track = sdp_parser.getAvailableTrack();
        if (strong_self->_sdp_track.empty()) {
            //该流无效
            WarnL << "sdp中无有效track，该流无效:" << rtsp_src->getSdp();
            strong_self->send_StreamNotFound();
            strong_self->shutdown(SockException(Err_shutdown,"can not find any available track in sdp"));
            return;
        }

        auto base_url = strong_self->_content_base;
        for (auto &track : strong_self->_sdp_track) {
            auto control = track->_type == TrackVideo ? "video" : "audio";
            track->_control = base_url + "/" + control;
            auto range = track->_attr.equal_range("control");
            if (range.first != range.second) {
                track->_attr.erase(range.first, range.second);
            }
            track->_attr.emplace("control", track->_control);
        }

        strong_self->_rtcp_context.clear();
        for (auto &track : strong_self->_sdp_track) {
            strong_self->_rtcp_context.emplace_back(std::make_shared<RtcpContextForSend>());
        }
        strong_self->_sessionid = makeRandStr(12);
        strong_self->_play_src = rtsp_src;
        for(auto &track : strong_self->_sdp_track){
            track->_ssrc = rtsp_src->getSsrc(track->_type);
            track->_seq = rtsp_src->getSequence(track->_type);
            track->_time_stamp = rtsp_src->getTimeStamp(track->_type);
        }

        strong_self->sendRtspResponse("200 OK",
                                     {"Content-Base", strong_self->_content_base + "/",
                                      "x-Accept-Retransmit","our-retransmit",
                                      "x-Accept-Dynamic-Rate","1"
                                     },sdp_parser.toString());
    });
}

void RtspSession::onAuthFailed(const string &realm,const string &why,bool close) {
    GET_CONFIG(bool, strict_sha256, Rtsp::kAuthStrictSha256);
    //只使用digest模式；严格模式只发SHA-256 challenge，非严格模式也发SHA-256 challenge但接受MD5 fallback
    _auth_nonce = encodeBase64(makeRandStr(16, false));
    _auth_opaque = encodeBase64(makeRandStr(16, false));
    if (strict_sha256) {
        sendRtspResponse("401 Unauthorized", { "WWW-Authenticate", StrPrinter << "Digest realm=\"" << realm << "\",nonce=\"" << _auth_nonce << "\",algorithm=SHA-256,qop=\"auth\",opaque=\"" << _auth_opaque << "\"" });
    } else {
        //非严格模式：发SHA-256 challenge，客户端不支持SHA-256时会fallback到MD5响应，onAuthUser中接受两种
        sendRtspResponse("401 Unauthorized", { "WWW-Authenticate", StrPrinter << "Digest realm=\"" << realm << "\",nonce=\"" << _auth_nonce << "\",qop=\"auth\",opaque=\"" << _auth_opaque << "\"" });
    }
    if (close) {
        shutdown(SockException(Err_shutdown, StrPrinter << "401 Unauthorized:" << why));
    }
}

void RtspSession::onAuthBasic(const string &realm, const string &auth_base64) {
    //base64认证
    auto user_passwd = decodeBase64(auth_base64);
    auto user_pwd_vec = split(user_passwd, ":");
    if (user_pwd_vec.size() < 2) {
        // 认证信息格式不合法，回复401 Unauthorized
        onAuthFailed(realm, "can not find user and passwd when basic64 auth");
        return;
    }
    auto user = user_pwd_vec[0];
    auto pwd = user_pwd_vec[1];
    weak_ptr<RtspSession> weak_self = static_pointer_cast<RtspSession>(shared_from_this());
    onAuth invoker = [pwd, realm, weak_self](bool encrypted, const string &good_pwd) {
        auto strong_self = weak_self.lock();
        if (!strong_self) {
            //本对象已经销毁
            return;
        }
        //切换到自己的线程执行
        strong_self->async([weak_self, good_pwd, pwd, realm]() {
            auto strong_self = weak_self.lock();
            if (!strong_self) {
                //本对象已经销毁
                return;
            }
            //base64忽略encrypted参数，上层必须传入明文密码
            if (pwd == good_pwd) {
                //提供的密码且匹配正确
                strong_self->onAuthSuccess();
                return;
            }
            //密码错误
            strong_self->onAuthFailed(realm, StrPrinter << "password mismatch when base64 auth:" << pwd << " != " << good_pwd);
        });
    };

    //此时必须提供明文密码
    if (!NOTICE_EMIT(BroadcastOnRtspAuthArgs, Broadcast::kBroadcastOnRtspAuth, _media_info, realm, user, true, invoker, *this)) {
        //表明该流需要认证却没监听请求密码事件，这一般是大意的程序所为，警告之
        WarnP(this) << "请监听kBroadcastOnRtspAuth事件！";
        //但是我们还是忽略认证以便完成播放
        //我们输入的密码是明文
        invoker(false, pwd);
    }
}

void RtspSession::onAuthDigest(const string &realm,const string &auth_md5){
    DebugP(this) << auth_md5;
    auto mapTmp = Parser::parseArgs(auth_md5, ",", "=");
    decltype(mapTmp) map;
    for(auto &pr : mapTmp){
        map[trim(string(pr.first)," \"")] = trim(pr.second," \"");
    }
    //check realm
    if(realm != map["realm"]){
        onAuthFailed(realm,StrPrinter << "realm not mached:" << realm << " != " << map["realm"]);
        return ;
    }
    //check nonce
    auto nonce = map["nonce"];
    if(_auth_nonce != nonce){
        onAuthFailed(realm,StrPrinter << "nonce not mached:" << nonce << " != " << _auth_nonce);
        return ;
    }
    //check username and uri
    auto username = map["username"];
    auto uri = map["uri"];
    auto response = map["response"];
    auto qop = map["qop"];
    auto nc = map["nc"];
    auto cnonce = map["cnonce"];
    if(username.empty() || uri.empty() || response.empty()){
        onAuthFailed(realm,StrPrinter << "username/uri/response empty:" << username << "," << uri << "," << response);
        return ;
    }

    auto realInvoker = [this,realm,nonce,uri,username,response,qop,nc,cnonce](bool ignoreAuth,bool encrypted,const string &good_pwd){
        if(ignoreAuth){
            //忽略认证
            TraceP(this) << "auth ignored";
            onAuthSuccess();
            return;
        }
        /*
        response计算方法如下：
        RTSP客户端应该使用username + password并计算response如下:
        (1)当password为MD5编码,则
            response = md5( password:nonce:md5(public_method:url)  );
        (2)当password为ANSI字符串,则
            response= md5( md5(username:realm:password):nonce:md5(public_method:url) );
         */
        auto encrypted_pwd = good_pwd;
        if(!encrypted){
            //提供的是明文密码
            encrypted_pwd = MD5(username+ ":" + realm + ":" + good_pwd).hexdigest();
        }

        auto ha2 = MD5(string("DESCRIBE") + ":" + uri).hexdigest();
        string good_response;
        if (!qop.empty() && !nc.empty() && !cnonce.empty() && strcasecmp(qop.data(), "auth") == 0) {
            // RFC2617: qop=auth 时需包含 nc/cnonce/qop 参与摘要计算
            good_response = MD5(encrypted_pwd + ":" + nonce + ":" + nc + ":" + cnonce + ":" + qop + ":" + ha2).hexdigest();
        } else {
            // 兼容未携带 qop 的老客户端
            good_response = MD5(encrypted_pwd + ":" + nonce + ":" + ha2).hexdigest();
        }
        if(digestEquals(good_response, response)){
            //认证成功！md5摘要为十六进制，比较时容忍大小写
            onAuthSuccess();
        }else{
            //认证失败！
            onAuthFailed(realm, StrPrinter << "password mismatch when md5 auth:" << good_response << " != " << response );
        }
    };

    weak_ptr<RtspSession> weak_self = static_pointer_cast<RtspSession>(shared_from_this());
    onAuth invoker = [realInvoker,weak_self](bool encrypted,const string &good_pwd){
        auto strong_self = weak_self.lock();
        if(!strong_self){
            return;
        }
        //切换到自己的线程确保realInvoker执行时，this指针有效
        strong_self->async([realInvoker,weak_self,encrypted,good_pwd](){
            auto strong_self = weak_self.lock();
            if(!strong_self){
                return;
            }
            realInvoker(false,encrypted,good_pwd);
        });
    };

    //此时可以提供明文或md5加密的密码
    GET_CONFIG(string, auth_file_digest, Rtsp::kAuthFile);
    if (!auth_file_digest.empty()) {
        string file_user, file_pwd;
        if (!loadRtspAuthFile(auth_file_digest, file_user, file_pwd)) {
            onAuthFailed(realm, "rtsp authFile load failed or credentials empty");
            return;
        }
        if (username != file_user) {
            onAuthFailed(realm, StrPrinter << "username mismatch: " << username << " != " << file_user);
            return;
        }
        invoker(false, file_pwd);
        return;
    }
    if(!NOTICE_EMIT(BroadcastOnRtspAuthArgs, Broadcast::kBroadcastOnRtspAuth, _media_info, realm, username, false, invoker, *this)){
        //表明该流需要认证却没监听请求密码事件，这一般是大意的程序所为，警告之
        WarnP(this) << "请监听kBroadcastOnRtspAuth事件！";
        onAuthFailed(realm, "kBroadcastOnRtspAuth listener not found");
    }
}

void RtspSession::onAuthSha256(const string &realm, const string &auth_sha256, const string &method) {
#ifndef ENABLE_OPENSSL
    onAuthFailed(realm, "sha-256 auth requires ENABLE_OPENSSL");
    return;
#else
    DebugP(this) << auth_sha256;
    auto mapTmp = Parser::parseArgs(auth_sha256, ",", "=");
    decltype(mapTmp) map;
    for (auto &pr : mapTmp) {
        map[trim(string(pr.first), " \"")] = trim(pr.second, " \"");
    }
    // check realm
    if (realm != map["realm"]) {
        onAuthFailed(realm, StrPrinter << "realm not mached:" << realm << " != " << map["realm"]);
        return;
    }
    // check nonce
    auto nonce = map["nonce"];
    if (_auth_nonce != nonce) {
        onAuthFailed(realm, StrPrinter << "nonce not mached:" << nonce << " != " << _auth_nonce);
        return;
    }
    // check opaque
    auto opaque = map["opaque"];
    if (!_auth_opaque.empty() && _auth_opaque != opaque) {
        onAuthFailed(realm, StrPrinter << "opaque not mached:" << opaque << " != " << _auth_opaque);
        return;
    }
    // check username and uri
    auto username = map["username"];
    auto uri = map["uri"];
    auto response = map["response"];
    auto qop = map["qop"];
    auto nc = map["nc"];
    auto cnonce = map["cnonce"];
    if (!response.empty() && response.size() != 64) {
        WarnP(this) << "client digest response length is invalid for sha-256, len=" << response.size() << ", auth=" << auth_sha256;
        onAuthFailed(realm, StrPrinter << "invalid sha-256 digest response length:" << response.size());
        return;
    }
    if (username.empty() || uri.empty() || response.empty()) {
        WarnP(this) << "client digest fields incomplete for sha-256, auth=" << auth_sha256;
        onAuthFailed(realm, StrPrinter << "username/uri/response empty:" << username << "," << uri << "," << response);
        return;
    }

    auto realInvoker = [this, realm, nonce, uri, username, response, qop, nc, cnonce, method](bool ignoreAuth, bool encrypted, const string &good_pwd) {
        if (ignoreAuth) {
            // 忽略认证
            TraceP(this) << "auth ignored";
            onAuthSuccess();
            return;
        }

        // Digest SHA-256与MD5流程一致，仅哈希算法替换为SHA-256
        auto encrypted_pwd = good_pwd;
        if (!encrypted) {
            // 提供的是明文密码
            encrypted_pwd = sha256Hex(username + ":" + realm + ":" + good_pwd);
        }

        auto ha2 = sha256Hex(method + ":" + uri);
        string good_response;
        if (!qop.empty() && !nc.empty() && !cnonce.empty() && strcasecmp(qop.data(), "auth") == 0) {
            good_response = sha256Hex(encrypted_pwd + ":" + nonce + ":" + nc + ":" + cnonce + ":" + qop + ":" + ha2);
        } else {
            // 宽松模式：兼容未携带qop/nc/cnonce的客户端
            good_response = sha256Hex(encrypted_pwd + ":" + nonce + ":" + ha2);
        }
        if (digestEquals(good_response, response)) {
            // 认证成功！SHA-256 摘要输出为小写十六进制，比较时容忍客户端大写
            onAuthSuccess();
        } else {
            // 认证失败！
            onAuthFailed(realm, StrPrinter << "password mismatch when sha256 auth:" << good_response << " != " << response);
        }
    };

    weak_ptr<RtspSession> weak_self = static_pointer_cast<RtspSession>(shared_from_this());
    onAuth invoker = [realInvoker, weak_self](bool encrypted, const string &good_pwd) {
        auto strong_self = weak_self.lock();
        if (!strong_self) {
            return;
        }
        // 切换到自己的线程确保realInvoker执行时，this指针有效
        strong_self->async([realInvoker, weak_self, encrypted, good_pwd]() {
            auto strong_self = weak_self.lock();
            if (!strong_self) {
                return;
            }
            realInvoker(false, encrypted, good_pwd);
        });
    };

    // 此时可以提供明文或sha256加密的密码
    GET_CONFIG(string, auth_file_sha256, Rtsp::kAuthFile);
    if (!auth_file_sha256.empty()) {
        string file_user, file_pwd;
        if (!loadRtspAuthFile(auth_file_sha256, file_user, file_pwd)) {
            onAuthFailed(realm, "rtsp authFile load failed or credentials empty");
            return;
        }
        if (username != file_user) {
            onAuthFailed(realm, StrPrinter << "username mismatch: " << username << " != " << file_user);
            return;
        }
        invoker(false, file_pwd);
        return;
    }
    if (!NOTICE_EMIT(BroadcastOnRtspAuthArgs, Broadcast::kBroadcastOnRtspAuth, _media_info, realm, username, false, invoker, *this)) {
        WarnP(this) << "请监听kBroadcastOnRtspAuth事件！";
        onAuthFailed(realm, "kBroadcastOnRtspAuth listener not found");
    }
#endif
}

void RtspSession::onAuthUser(const string &realm,const string &authorization){
    if(authorization.empty()){
        onAuthFailed(realm,"", false);
        return;
    }
    //请求中包含认证信息
    auto authType = findSubString(authorization.data(), NULL, " ");
    auto authStr = findSubString(authorization.data(), " ", NULL);
    if(authType.empty() || authStr.empty()){
        //认证信息格式不合法，回复401 Unauthorized
        onAuthFailed(realm,"can not find auth type or auth string");
        return;
    }
    if(authType == "Basic"){
        //只允许digest模式，拒绝basic
        WarnP(this) << "client uses basic auth but server only accepts digest";
        onAuthFailed(realm, "basic auth is disabled, only digest is allowed");
    }else if(authType == "Digest"){
        GET_CONFIG(bool, strict_sha256, Rtsp::kAuthStrictSha256);
        auto mapTmp = Parser::parseArgs(authStr, ",", "=");
        auto algorithm = trim(string(mapTmp["algorithm"]), " \"");
        auto response = trim(string(mapTmp["response"]), " \"");
        if (strict_sha256) {
            //严格模式：只允许SHA-256
            if (algorithm.empty() || strcasecmp(algorithm.data(), "SHA-256") == 0) {
                onAuthSha256(realm, authStr, "DESCRIBE");
            } else {
                onAuthFailed(realm, StrPrinter << "unsupported digest algorithm:" << algorithm << ", strict SHA-256 required");
            }
        } else {
            //非严格模式：优先SHA-256，客户端不支持时fallback到MD5
            if (strcasecmp(algorithm.data(), "SHA-256") == 0 && response.size() == 64) {
                onAuthSha256(realm, authStr, "DESCRIBE");
            } else if (algorithm.empty() || strcasecmp(algorithm.data(), "MD5") == 0 || response.size() == 32) {
                onAuthDigest(realm, authStr);
            } else {
                //未知算法默认按md5尝试
                onAuthDigest(realm, authStr);
            }
        }
    }else{
        //其他认证方式？不支持！
        onAuthFailed(realm,StrPrinter << "unsupported auth type:" << authType);
    }
}

void RtspSession::send_StreamNotFound() {
    sendRtspResponse("404 Stream Not Found",{"Connection","Close"});
}

void RtspSession::send_UnsupportedTransport() {
    sendRtspResponse("461 Unsupported Transport",{"Connection","Close"});
}

void RtspSession::send_SessionNotFound() {
    sendRtspResponse("454 Session Not Found",{"Connection","Close"});
}

void RtspSession::handleReq_Setup(const Parser &parser) {
    //处理setup命令，该函数可能进入多次
    int trackIdx = getTrackIndexByControlUrl(parser.fullUrl());
    SdpTrack::Ptr &trackRef = _sdp_track[trackIdx];
    if (trackRef->_inited) {
        //已经初始化过该Track
        throw SockException(Err_shutdown, "can not setup one track twice");
    }

    static auto getRtpTypeStr = [](const int type) {
        switch (type)
        {
        case Rtsp::RTP_TCP:
            return "TCP";
        case Rtsp::RTP_UDP:
            return "UDP";
        case Rtsp::RTP_MULTICAST:
            return "MULTICAST";
        default:
            return "Invalid";
        }
    };

    if (_rtp_type == Rtsp::RTP_Invalid) {
        auto &strTransport = parser["Transport"];
        auto rtpType = Rtsp::RTP_Invalid;
        if (strTransport.find("TCP") != string::npos) {
            rtpType = Rtsp::RTP_TCP;
        } else if (strTransport.find("multicast") != string::npos) {
            rtpType = Rtsp::RTP_MULTICAST;
        } else {
            rtpType = Rtsp::RTP_UDP;
        }
        //检查RTP传输类型限制
        GET_CONFIG(int, transport, Rtsp::kRtpTransportType);
        if (transport != Rtsp::RTP_Invalid && transport != rtpType) {
            WarnL << "rtsp client setup transport " << getRtpTypeStr(rtpType) << " but config force transport " << getRtpTypeStr(transport);
            //配置限定RTSP传输方式，但是客户端握手方式不一致，返回461
            sendRtspResponse("461 Unsupported transport");
            return;
        }
        _rtp_type = rtpType;
    }

    trackRef->_inited = true; //现在初始化

    //允许接收rtp、rtcp包
    RtspSplitter::enableRecvRtp(_rtp_type == Rtsp::RTP_TCP);

    switch (_rtp_type) {
    case Rtsp::RTP_TCP: {
        if (_push_src) {
            // rtsp推流时，interleaved由推流者决定
            auto key_values = Parser::parseArgs(parser["Transport"], ";", "=");
            int interleaved_rtp = -1, interleaved_rtcp = -1;
            if (2 == sscanf(key_values["interleaved"].data(), "%d-%d", &interleaved_rtp, &interleaved_rtcp)) {
                trackRef->_interleaved = interleaved_rtp;
            } else {
                throw SockException(Err_shutdown, "can not find interleaved when setup of rtp over tcp");
            }
        } else {
            // rtsp播放时，由于数据共享分发，所以interleaved必须由服务器决定
            trackRef->_interleaved = 2 * trackRef->_type;
        }
        sendRtspResponse("200 OK",
                         {"Transport", StrPrinter << "RTP/AVP/TCP;unicast;"
                                                  << "interleaved=" << (int) trackRef->_interleaved << "-"
                                                  << (int) trackRef->_interleaved + 1 << ";"
                                                  << "ssrc=" << printSSRC(trackRef->_ssrc),
                          "x-Transport-Options", "late-tolerance=1.400000",
                          "x-Dynamic-Rate", "1"
                         });
    }
        break;

    case Rtsp::RTP_UDP: {
        std::pair<Socket::Ptr, Socket::Ptr> pr = std::make_pair(createSocket(),createSocket());
        try {
            makeSockPair(pr, get_local_ip());
        } catch (std::exception &ex) {
            //分配端口失败
            send_NotAcceptable();
            throw SockException(Err_shutdown, ex.what());
        }

        _rtp_socks[trackIdx] = pr.first;
        _rtcp_socks[trackIdx] = pr.second;

        //设置客户端内网端口信息
        string strClientPort = findSubString(parser["Transport"].data(), "client_port=", NULL);
        uint16_t ui16RtpPort = atoi(findSubString(strClientPort.data(), NULL, "-").data());
        uint16_t ui16RtcpPort = atoi(findSubString(strClientPort.data(), "-", NULL).data());

        auto peerAddr = SockUtil::make_sockaddr(get_peer_ip().data(), ui16RtpPort);
        //设置rtp发送目标地址
        pr.first->bindPeerAddr((struct sockaddr *) (&peerAddr), 0, true);

        //设置rtcp发送目标地址
        peerAddr = SockUtil::make_sockaddr(get_peer_ip().data(), ui16RtcpPort);
        pr.second->bindPeerAddr((struct sockaddr *) (&peerAddr), 0, true);

        //尝试获取客户端nat映射地址
        startListenPeerUdpData(trackIdx);
        //InfoP(this) << "分配端口:" << srv_port;

        sendRtspResponse("200 OK",
                         {"Transport", StrPrinter << "RTP/AVP/UDP;unicast;"
                                                  << "client_port=" << strClientPort << ";"
                                                  << "server_port=" << pr.first->get_local_port() << "-"
                                                  << pr.second->get_local_port() << ";"
                                                  << "ssrc=" << printSSRC(trackRef->_ssrc)
                         });
    }
        break;
    case Rtsp::RTP_MULTICAST: {
        if(!_multicaster){
            _multicaster = RtpMultiCaster::get(*this, get_local_ip(), _media_info, _multicast_ip, _multicast_video_port, _multicast_audio_port);
            if (!_multicaster) {
                send_NotAcceptable();
                throw SockException(Err_shutdown, "can not get a available udp multicast socket");
            }
            weak_ptr<RtspSession> weak_self = static_pointer_cast<RtspSession>(shared_from_this());
            _multicaster->setDetachCB(this, [weak_self]() {
                auto strong_self = weak_self.lock();
                if(!strong_self) {
                    return;
                }
                strong_self->safeShutdown(SockException(Err_shutdown,"ring buffer detached"));
            });
        }
        int iSrvPort = _multicaster->getMultiCasterPort(trackRef->_type);
        //我们用trackIdx区分rtp和rtcp包
        //由于组播udp端口是共享的，而rtcp端口为组播udp端口+1，所以rtcp端口需要改成共享端口
        auto pSockRtcp = UDPServer::Instance().getSock(*this, get_local_ip().data(), 2 * trackIdx + 1, iSrvPort + 1);
        if (!pSockRtcp) {
            //分配端口失败
            send_NotAcceptable();
            throw SockException(Err_shutdown, "open shared rtcp socket failed");
        }
        startListenPeerUdpData(trackIdx);
        GET_CONFIG(uint32_t,udpTTL,MultiCast::kUdpTTL);

        sendRtspResponse("200 OK",
                         {"Transport", StrPrinter << "RTP/AVP;multicast;"
                                                  << "destination=" << _multicaster->getMultiCasterIP() << ";"
                                                  << "source=" << get_local_ip() << ";"
                                                  << "port=" << iSrvPort << "-" << pSockRtcp->get_local_port() << ";"
                                                  << "ttl=" << udpTTL << ";"
                                                  << "ssrc=" << printSSRC(trackRef->_ssrc)
                         });
    }
        break;
    default:
        break;
    }
}

void RtspSession::handleReq_Play(const Parser &parser) {
    if (_sdp_track.empty() || parser["Session"] != _sessionid) {
        send_SessionNotFound();
        throw SockException(Err_shutdown, _sdp_track.empty() ? "can not find any available track when play" : "session not found when play");
    }
    auto play_src = _play_src.lock();
    if(!play_src){
        send_StreamNotFound();
        shutdown(SockException(Err_shutdown,"rtsp stream released"));
        return;
    }

    // 真实 active 播放会话配额：仅在首次进入 PLAY 时占用（seek/暂停恢复会重入本函数，用标记防重）。
    if (!_play_quota_acquired) {
        GET_CONFIG(string, replay_app, Rtsp::kReplayAppName);
        auto is_replay = play_src->getMediaTuple().app == replay_app;
        if (!acquirePlayQuota(is_replay)) {
            sendRtspResponse("503 Service Unavailable", {"Connection", "Close"});
            shutdown(SockException(Err_shutdown, "play session limit reached"));
            return;
        }
        _play_quota_acquired = true;
        _play_quota_is_replay = is_replay;
    }

    // seek 会重建 reader，必须始终从 GOP 缓存起播，否则会错过 seek 期间已写入的关键帧。
    const bool use_gop = true;
    auto &strScale = parser["Scale"];
    auto &strSpeed = parser["Speed"];
    auto &strRange = parser["Range"];
    StrCaseMap res_header;
    string speed_header_name;
    string speed_value;
    if (!strScale.empty()) {
        speed_header_name = "Scale";
        speed_value = strScale;
    } else if (!strSpeed.empty()) {
        speed_header_name = "Speed";
        speed_value = strSpeed;
    }
    if (!speed_header_name.empty()) {
        //播放速度，兼容Scale与Speed两种头
        auto speed = atof(speed_value.data());
        play_src->speed(speed);
        res_header.emplace(speed_header_name, speed_value);
        InfoP(this) << "rtsp set play speed via " << speed_header_name << ":" << speed;
    }

    if (!strRange.empty()) {
        //这是seek操作
        auto strStart = findSubString(strRange.data(), "npt=", "-");
        if (strStart == "now") {
            strStart = "0";
        }
        auto iStartTime = 1000 * (float) atof(strStart.data());
        auto seek_ok = play_src->seekTo((uint32_t) iStartTime);
        auto actual_ms = play_src->getTimeStamp(TrackInvalid);
        _seek_probe_req_ms = (uint32_t)iStartTime;
        _seek_probe_actual_ms = actual_ms;
        _seek_probe_pending = seek_ok;
        InfoP(this) << "rtsp seekTo(ms):" << iStartTime
                    << ", seek_ok:" << seek_ok
                    << ", actual_ms:" << actual_ms
                    << ", delta_ms:" << ((int64_t)actual_ms - (int64_t)iStartTime);
    }

    vector<TrackType> inited_tracks;
    _StrPrinter rtp_info;
    for (auto &track : _sdp_track) {
        if (track->_inited == false) {
            //为支持播放器播放单一track, 不校验没有发setup的track
            continue;
        }
        inited_tracks.emplace_back(track->_type);
        track->_ssrc = play_src->getSsrc(track->_type);
        track->_seq = play_src->getSequence(track->_type);
        track->_time_stamp = play_src->getTimeStamp(track->_type);

        rtp_info << "url=" << track->getControlUrl(_content_base) << ";"
                 << "seq=" << track->_seq << ";"
                 << "rtptime=" << (int64_t)(track->_time_stamp) * (int64_t)(track->_samplerate/ 1000) << ",";
    }

    rtp_info.pop_back();

    res_header.emplace("RTP-Info", rtp_info);
    //PLAY响应中的Range统一回写为实际播放起点，避免透传请求值导致协议语义不一致
    res_header["Range"] = StrPrinter << "npt=" << setiosflags(ios::fixed) << setprecision(2) << play_src->getTimeStamp(TrackInvalid) / 1000.0;
    sendRtspResponse("200 OK", res_header);

    //设置播放track
    if (inited_tracks.size() == 1) {
        _target_play_track = inited_tracks[0];
        InfoP(this) << "指定播放track:" << _target_play_track;
    }

    //在回复rtsp信令后再恢复播放
    play_src->pause(false);

    setSocketFlags();

    if (!strRange.empty() && _play_reader && _rtp_type != Rtsp::RTP_MULTICAST) {
        // seek后重建ring reader，避免旧读游标/缓存导致客户端画面不推进。
        _play_reader = nullptr;
    }

    if (!_play_reader && _rtp_type != Rtsp::RTP_MULTICAST) {
        weak_ptr<RtspSession> weak_self = static_pointer_cast<RtspSession>(shared_from_this());
        _play_reader = play_src->getRing()->attach(getPoller(), use_gop);
        _play_reader->setGetInfoCB([weak_self]() {
            Any ret;
            ret.set(static_pointer_cast<Session>(weak_self.lock()));
            return ret;
        });
        _play_reader->setDetachCB([weak_self]() {
            auto strong_self = weak_self.lock();
            if (!strong_self) {
                return;
            }
            strong_self->shutdown(SockException(Err_shutdown, "rtsp ring buffer detached"));
        });
        _play_reader->setReadCB([weak_self](const RtspMediaSource::RingDataType &pack) {
            auto strong_self = weak_self.lock();
            if (!strong_self) {
                return;
            }
            strong_self->sendRtpPacket(pack);
        });
    }
}

void RtspSession::handleReq_Pause(const Parser &parser) {
    if (parser["Session"] != _sessionid) {
        send_SessionNotFound();
        throw SockException(Err_shutdown, "session not found when pause");
    }

    sendRtspResponse("200 OK");
    auto play_src = _play_src.lock();
    if (play_src) {
        play_src->pause(true);
    }
}

void RtspSession::handleReq_Teardown(const Parser &parser) {
    _push_src = nullptr;
    //此时回复可能触发broken pipe事件，从而直接触发onError回调；所以需要先把_push_src置空，防止触发断流续推功能
    sendRtspResponse("200 OK");
    throw SockException(Err_shutdown,"recv teardown request");
}

void RtspSession::handleReq_Get(const Parser &parser) {
    _http_x_sessioncookie = parser["x-sessioncookie"];
    sendRtspResponse("200 OK",
                     {"Cache-Control","no-store",
                      "Pragma","no-store",
                      "Content-Type","application/x-rtsp-tunnelled",
                     },"","HTTP/1.0");

    //注册http getter，以便http poster绑定
    lock_guard<recursive_mutex> lock(g_mtxGetter);
    g_mapGetter[_http_x_sessioncookie] = static_pointer_cast<RtspSession>(shared_from_this());
}

void RtspSession::handleReq_Post(const Parser &parser) {
    lock_guard<recursive_mutex> lock(g_mtxGetter);
    string sessioncookie = parser["x-sessioncookie"];
    //Poster 找到 Getter
    auto it = g_mapGetter.find(sessioncookie);
    if (it == g_mapGetter.end()) {
        throw SockException(Err_shutdown,"can not find http getter by x-sessioncookie");
    }

    //Poster 找到Getter的SOCK
    auto httpGetterWeak = it->second;
    //移除http getter的弱引用记录
    g_mapGetter.erase(sessioncookie);

    //http poster收到请求后转发给http getter处理
    _on_recv = [this,httpGetterWeak](const Buffer::Ptr &buf){
        auto httpGetterStrong = httpGetterWeak.lock();
        if(!httpGetterStrong){
            shutdown(SockException(Err_shutdown,"http getter released"));
            return;
        }

        //切换到http getter的线程
        httpGetterStrong->async([buf,httpGetterWeak](){
            auto httpGetterStrong = httpGetterWeak.lock();
            if(!httpGetterStrong){
                return;
            }
            httpGetterStrong->onRecv(std::make_shared<BufferString>(decodeBase64(string(buf->data(), buf->size()))));
        });
    };

    if(!parser.content().empty()){
        //http poster后面的粘包
        _on_recv(std::make_shared<BufferString>(parser.content()));
    }

    sendRtspResponse("200 OK",
                     {"Cache-Control","no-store",
                      "Pragma","no-store",
                      "Content-Type","application/x-rtsp-tunnelled",
                     },"","HTTP/1.0");
}

void RtspSession::handleReq_SET_PARAMETER(const Parser &parser) {
    //TraceP(this) <<endl;
    sendRtspResponse("200 OK");
}

void RtspSession::send_NotAcceptable() {
    sendRtspResponse("406 Not Acceptable",{"Connection","Close"});
}

void RtspSession::onRtpSorted(RtpPacket::Ptr rtp, int track_idx) {
    if (_push_src) {
        _push_src->onWrite(std::move(rtp), false);
    } else {
        WarnL << "Not a rtsp push!";
    }
}

void RtspSession::onRcvPeerUdpData(int interleaved, const Buffer::Ptr &buf, const struct sockaddr_storage &addr) {
    //这是rtcp心跳包，说明播放器还存活
    _alive_ticker.resetTime();

    if (interleaved % 2 == 0) {
        if (_push_src) {
            //这是rtsp推流上来的rtp包
            auto &ref = _sdp_track[interleaved / 2];
            handleOneRtp(interleaved / 2, ref->_type, ref->_samplerate, (uint8_t *) buf->data(), buf->size());
        } else if (!_udp_connected_flags.count(interleaved)) {
            //这是rtsp播放器的rtp打洞包
            _udp_connected_flags.emplace(interleaved);
            if (_rtp_socks[interleaved / 2]) {
                _rtp_socks[interleaved / 2]->bindPeerAddr((struct sockaddr *)&addr);
            }
        }
    } else {
        //rtcp包
        if (!_udp_connected_flags.count(interleaved)) {
            _udp_connected_flags.emplace(interleaved);
            if (_rtcp_socks[(interleaved - 1) / 2]) {
                _rtcp_socks[(interleaved - 1) / 2]->bindPeerAddr((struct sockaddr *)&addr);
            }
        }
        onRtcpPacket((interleaved - 1) / 2, _sdp_track[(interleaved - 1) / 2], buf->data(), buf->size());
    }
}

void RtspSession::startListenPeerUdpData(int track_idx) {
    weak_ptr<RtspSession> weak_self = static_pointer_cast<RtspSession>(shared_from_this());
    auto peer_ip = get_peer_ip();
    auto onUdpData = [weak_self,peer_ip](const Buffer::Ptr &buf, struct sockaddr *peer_addr, int interleaved){
        auto strong_self = weak_self.lock();
        if (!strong_self) {
            return false;
        }

        if (SockUtil::inet_ntoa(peer_addr) != peer_ip) {
            WarnP(strong_self.get()) << ((interleaved % 2 == 0) ? "收到其他地址的rtp数据:" : "收到其他地址的rtcp数据:")
                                    << SockUtil::inet_ntoa(peer_addr);
            return true;
        }

        struct sockaddr_storage addr = *((struct sockaddr_storage *)peer_addr);
        strong_self->async([weak_self, buf, addr, interleaved]() {
            auto strong_self = weak_self.lock();
            if (!strong_self) {
                return;
            }
            try {
                strong_self->onRcvPeerUdpData(interleaved, buf, addr);
            } catch (SockException &ex) {
                strong_self->shutdown(ex);
            } catch (std::exception &ex) {
                strong_self->shutdown(SockException(Err_other, ex.what()));
            }
        });
        return true;
    };

    switch (_rtp_type){
        case Rtsp::RTP_MULTICAST:{
            //组播使用的共享rtcp端口
            UDPServer::Instance().listenPeer(get_peer_ip().data(), this,
                    [onUdpData]( int interleaved, const Buffer::Ptr &buf, struct sockaddr *peer_addr) {
                return onUdpData(buf, peer_addr, interleaved);
            });
        }
            break;
        case Rtsp::RTP_UDP:{
            auto setEvent = [&](Socket::Ptr &sock,int interleaved){
                if(!sock){
                    WarnP(this) << "udp端口为空:" << interleaved;
                    return;
                }
                sock->setOnRead([onUdpData,interleaved](const Buffer::Ptr &pBuf, struct sockaddr *pPeerAddr , int addr_len){
                    onUdpData(pBuf, pPeerAddr, interleaved);
                });
            };
            setEvent(_rtp_socks[track_idx], 2 * track_idx );
            setEvent(_rtcp_socks[track_idx], 2 * track_idx + 1 );
        }
            break;

        default:
            break;
    }

}

static string dateStr(){
    char buf[64];
    time_t tt = time(NULL);
    strftime(buf, sizeof buf, "%a, %b %d %Y %H:%M:%S GMT", gmtime(&tt));
    return buf;
}

bool RtspSession::sendRtspResponse(const string &res_code, const StrCaseMap &header_const, const string &sdp, const char *protocol){
    auto header = header_const;
    header.emplace("CSeq",StrPrinter << _cseq);
    if(!_sessionid.empty()){
        header.emplace("Session", _sessionid);
    }

    header.emplace("Server",kServerName);
    header.emplace("Date",dateStr());

    if(!sdp.empty()){
        header.emplace("Content-Length",StrPrinter << sdp.size());
        header.emplace("Content-Type","application/sdp");
    }

    _StrPrinter printer;
    printer << protocol << " " << res_code << "\r\n";
    for (auto &pr : header){
        printer << pr.first << ": " << pr.second << "\r\n";
    }

    printer << "\r\n";

    if(!sdp.empty()){
        printer << sdp;
    }
//	DebugP(this) << printer;
    return send(std::make_shared<BufferString>(std::move(printer))) > 0 ;
}

ssize_t RtspSession::send(Buffer::Ptr pkt){
//	if(!_enableSendRtp){
//		DebugP(this) << pkt->data();
//	}
    _bytes_usage += pkt->size();
    return Session::send(std::move(pkt));
}

bool RtspSession::sendRtspResponse(const string &res_code, const std::initializer_list<string> &header, const string &sdp, const char *protocol) {
    string key;
    StrCaseMap header_map;
    int i = 0;
    for(auto &val : header){
        if(++i % 2 == 0){
            header_map.emplace(key,val);
        }else{
            key = val;
        }
    }
    return sendRtspResponse(res_code,header_map,sdp,protocol);
}

int RtspSession::getTrackIndexByPT(int pt) const {
    for (size_t i = 0; i < _sdp_track.size(); ++i) {
        if (pt == _sdp_track[i]->_pt) {
            return i;
        }
    }
    if (_sdp_track.size() == 1) {
        return 0;
    }
    throw SockException(Err_shutdown, StrPrinter << "no such track with pt:" << pt);
}

int RtspSession::getTrackIndexByTrackType(TrackType type) {
    for (size_t i = 0; i < _sdp_track.size(); ++i) {
        if (type == _sdp_track[i]->_type) {
            return i;
        }
    }
    if (_sdp_track.size() == 1) {
        return 0;
    }
    throw SockException(Err_shutdown, StrPrinter << "no such track with type:" << getTrackString(type));
}

int RtspSession::getTrackIndexByControlUrl(const string &control_url) {
    for (size_t i = 0; i < _sdp_track.size(); ++i) {
        if (control_url.find(_sdp_track[i]->getControlUrl(_content_base)) == 0) {
            return i;
        }
    }
    if (_sdp_track.size() == 1) {
        return 0;
    }
    throw SockException(Err_shutdown, StrPrinter << "no such track with control url:" << control_url);
}

int RtspSession::getTrackIndexByInterleaved(int interleaved) {
    for (size_t i = 0; i < _sdp_track.size(); ++i) {
        if (_sdp_track[i]->_interleaved == interleaved) {
            return i;
        }
    }
    if (_sdp_track.size() == 1) {
        return 0;
    }
    throw SockException(Err_shutdown, StrPrinter << "no such track with interleaved:" << interleaved);
}

bool RtspSession::close(MediaSource &sender) {
    shutdown(SockException(Err_shutdown,"close media: " + sender.getUrl()));
    return true;
}

int RtspSession::totalReaderCount(MediaSource &sender) {
    return _push_src ? _push_src->totalReaderCount() : sender.readerCount();
}

MediaOriginType RtspSession::getOriginType(MediaSource &sender) const{
    return MediaOriginType::rtsp_push;
}

string RtspSession::getOriginUrl(MediaSource &sender) const {
    return _media_info.full_url;
}

std::shared_ptr<SockInfo> RtspSession::getOriginSock(MediaSource &sender) const {
    return const_cast<RtspSession *>(this)->shared_from_this();
}

toolkit::EventPoller::Ptr RtspSession::getOwnerPoller(MediaSource &sender) {
    return getPoller();
}

void RtspSession::onBeforeRtpSorted(const RtpPacket::Ptr &rtp, int track_index){
    updateRtcpContext(rtp);
}

void RtspSession::updateRtcpContext(const RtpPacket::Ptr &rtp){
    int track_index = getTrackIndexByTrackType(rtp->type);
    auto &rtcp_ctx = _rtcp_context[track_index];
    rtcp_ctx->onRtp(rtp->getSeq(), rtp->getStamp(), rtp->ntp_stamp, rtp->sample_rate, rtp->size() - RtpPacket::kRtpTcpHeaderSize);
    if (!rtp->ntp_stamp && !rtp->getStamp()) {
        // 忽略时间戳都为0的rtp
        return;
    }

    auto &ticker = _rtcp_send_tickers[track_index];
    //send rtcp every 5 second
    if (ticker.elapsedTime() > 5 * 1000 || (_send_sr_rtcp[track_index] && !_push_src)) {
        //确保在发送rtp前，先发送一次sender report rtcp(用于播放器同步音视频)
        ticker.resetTime();
        _send_sr_rtcp[track_index] = false;

        static auto send_rtcp = [](RtspSession *thiz, int index, Buffer::Ptr ptr) {
            if (thiz->_rtp_type == Rtsp::RTP_TCP) {
                auto &track = thiz->_sdp_track[index];
                thiz->send(makeRtpOverTcpPrefix((uint16_t)(ptr->size()), track->_interleaved + 1));
                thiz->send(std::move(ptr));
            } else {
                thiz->_rtcp_socks[index]->send(std::move(ptr));
            }
        };

        auto ssrc = rtp->getSSRC();
        auto rtcp = _push_src ?  rtcp_ctx->createRtcpRR(ssrc + 1, ssrc) : rtcp_ctx->createRtcpSR(ssrc);
        auto rtcp_sdes = RtcpSdes::create({kServerName});
        rtcp_sdes->chunks.type = (uint8_t)SdesType::RTCP_SDES_CNAME;
        rtcp_sdes->chunks.ssrc = htonl(ssrc);
        send_rtcp(this, track_index, std::move(rtcp));
        send_rtcp(this, track_index, RtcpHeader::toBuffer(rtcp_sdes));
    }
}

void RtspSession::sendRtpPacket(const RtspMediaSource::RingDataType &pkt) {
    switch (_rtp_type) {
        case Rtsp::RTP_TCP: {
            setSendFlushFlag(false);
            pkt->for_each([&](const RtpPacket::Ptr &rtp) {
                if (_target_play_track == TrackInvalid || _target_play_track == rtp->type) {
                    if (_seek_probe_pending) {
                        auto rtp_ms = rtp->sample_rate ? (rtp->getStamp() * uint64_t(1000) / rtp->sample_rate) : 0;
                        InfoP(this) << "rtsp seek probe first RTP(tcp):"
                                    << " req_ms=" << _seek_probe_req_ms
                                    << ", actual_ms=" << _seek_probe_actual_ms
                                    << ", seq=" << rtp->getSeq()
                                    << ", rtp_ms=" << rtp_ms
                                    << ", track=" << rtp->type;
                        _seek_probe_pending = false;
                    }
                    updateRtcpContext(rtp);
                    send(rtp);
                }
            });
            flushAll();
            setSendFlushFlag(true);
        }
            break;
        case Rtsp::RTP_UDP: {
            //下标0表示视频，1表示音频
            Socket::Ptr rtp_socks[2];
            rtp_socks[TrackVideo] = _rtp_socks[getTrackIndexByTrackType(TrackVideo)];
            rtp_socks[TrackAudio] = _rtp_socks[getTrackIndexByTrackType(TrackAudio)];
            pkt->for_each([&](const RtpPacket::Ptr &rtp) {
                if (_target_play_track == TrackInvalid || _target_play_track == rtp->type) {
                    if (_seek_probe_pending) {
                        auto rtp_ms = rtp->sample_rate ? (rtp->getStamp() * uint64_t(1000) / rtp->sample_rate) : 0;
                        InfoP(this) << "rtsp seek probe first RTP(udp):"
                                    << " req_ms=" << _seek_probe_req_ms
                                    << ", actual_ms=" << _seek_probe_actual_ms
                                    << ", seq=" << rtp->getSeq()
                                    << ", rtp_ms=" << rtp_ms
                                    << ", track=" << rtp->type;
                        _seek_probe_pending = false;
                    }
                    updateRtcpContext(rtp);
                    auto &sock = rtp_socks[rtp->type];
                    if (!sock) {
                        shutdown(SockException(Err_shutdown, "udp sock not opened yet"));
                        return;
                    }
                    _bytes_usage += rtp->size() - RtpPacket::kRtpTcpHeaderSize;
                    sock->send(std::make_shared<BufferRtp>(rtp, RtpPacket::kRtpTcpHeaderSize), nullptr, 0, false);
                }
            });
            for (auto &sock : rtp_socks) {
                if (sock) {
                    sock->flushAll();
                }
            }
        }
            break;
        default:
            break;
    }
}

void RtspSession::setSocketFlags(){
    GET_CONFIG(int, mergeWriteMS, General::kMergeWriteMS);
    if(mergeWriteMS > 0) {
        //推流模式下，关闭TCP_NODELAY会增加推流端的延时，但是服务器性能将提高
        SockUtil::setNoDelay(getSock()->rawFD(), false);
        //播放模式下，开启MSG_MORE会增加延时，但是能提高发送性能
        setSendFlags(SOCKET_DEFAULE_FLAGS | FLAG_MORE);
    }
}

}
/* namespace mediakit */
