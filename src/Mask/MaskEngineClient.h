/*
 * Copyright (c) 2016-present The ZLMediaKit project authors. All Rights Reserved.
 *
 * This file is part of ZLMediaKit(https://github.com/ZLMediaKit/ZLMediaKit).
 *
 * Use of this source code is governed by MIT-like license that can be found in the
 * LICENSE file in the root of the source tree. All contributing project authors
 * may be found in the AUTHORS file in the root of the source tree.
 */

#ifndef ZLMEDIAKIT_MASKENGINECLIENT_H
#define ZLMEDIAKIT_MASKENGINECLIENT_H

#include <functional>
#include <memory>
#include <string>

#include "json/json.h"
#include "MaskProtocol.h"
#include "Network/Socket.h"
#include "Poller/EventPoller.h"

namespace mediakit {

// 与 edge-mask 的一个打码会话（一条 unix socket 连接）。所有回调都在构造时给的 poller 线程上执行，
// 每个回调最多一次失败通知：open 失败走 open 的回调，open 成功后的失败走 onError。
// One edge-mask session (one unix socket connection). All callbacks run on the poller given at
// construction; a failure is reported once: through the open callback before OPEN_ACK, through
// onError afterwards.
class MaskEngineClient : public std::enable_shared_from_this<MaskEngineClient> {
public:
    using Ptr = std::shared_ptr<MaskEngineClient>;

    struct OpenResult {
        bool ok = false;
        std::string code;     // 引擎回的 reason（busy / engine-unavailable / bad-request）或本地原因（timeout / disconnected / ...）
        std::string message;
        Json::Value ack;      // OPEN_ACK 内容 / OPEN_ACK content
    };
    using OpenCB = std::function<void(const OpenResult &result)>;
    using FrameCB = std::function<void(const MaskProto::FrameInfo &frame)>;
    using ErrorCB = std::function<void(const std::string &reason)>;
    using SpeedCB = std::function<void(bool ok, const std::string &reason)>;
    using DrainCB = std::function<void()>;

    explicit MaskEngineClient(toolkit::EventPoller::Ptr poller);
    ~MaskEngineClient();

    // 连接并发送 OPEN，等到 OPEN_ACK / ERROR / 超时后回调一次 / Connect, send OPEN and report OPEN_ACK / ERROR / timeout once
    void open(const std::string &socket_path, const Json::Value &request, int timeout_ms, OpenCB cb);
    void setOnFrame(FrameCB cb) { _on_frame = std::move(cb); }
    void setOnError(ErrorCB cb) { _on_error = std::move(cb); }

    // pts 为 ZLM 的毫秒时间戳，线上按微秒传（OUT_FRAME 原样带回）/ pts in ZLM milliseconds, sent as microseconds
    void sendFrame(uint32_t epoch, bool key_frame, uint64_t pts_ms, const char *data, size_t size);
    void flush(uint32_t epoch);
    // 改倍速：发 SPEED（新倍速 + 新 epoch），等 SPEED_ACK 后回调一次（ok=false 表示引擎资源不足，会话保持原倍速）；
    // 超时视为会话失败（走 onError，fail-closed）
    // Speed change: send SPEED (speed + new epoch) and report SPEED_ACK once (ok=false: refused, the session keeps the
    // old speed); a timeout fails the session (onError, fail-closed)
    void requestSpeed(float speed, uint32_t epoch, int timeout_ms, SpeedCB cb);
    // 发送缓冲排空时回调（背压恢复用）/ Called when the send buffer drains (to resume after back-pressure)
    void setOnDrain(DrainCB cb);
    // 正常结束：发 CLOSE 后断开，不再回调 / Graceful end: send CLOSE then disconnect, no further callbacks
    void close();

    // 已排队未发出的消息数 / Messages queued but not yet written
    size_t pendingMessages();
    bool opened() const { return _opened; }

private:
    void onMessage(const MaskProto::Message &msg);
    void fail(const std::string &code, const std::string &message);
    void send(std::string data);

private:
    bool _opened = false;
    bool _closed = false;
    toolkit::EventPoller::Ptr _poller;
    toolkit::Socket::Ptr _sock;
    MaskProto::Decoder _decoder;
    OpenCB _on_open;
    FrameCB _on_frame;
    ErrorCB _on_error;
    SpeedCB _on_speed;
    DrainCB _on_drain;
    toolkit::EventPoller::DelayTask::Ptr _open_timer;
    toolkit::EventPoller::DelayTask::Ptr _speed_timer;
};

} // namespace mediakit

#endif // ZLMEDIAKIT_MASKENGINECLIENT_H
