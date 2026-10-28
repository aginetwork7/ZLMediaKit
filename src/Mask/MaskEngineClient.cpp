/*
 * Copyright (c) 2016-present The ZLMediaKit project authors. All Rights Reserved.
 *
 * This file is part of ZLMediaKit(https://github.com/ZLMediaKit/ZLMediaKit).
 *
 * Use of this source code is governed by MIT-like license that can be found in the
 * LICENSE file in the root of the source tree. All contributing project authors
 * may be found in the AUTHORS file in the root of the source tree.
 */

#include "MaskEngineClient.h"

#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#include <cerrno>
#include <cstring>

#include "Util/logger.h"

using namespace std;
using namespace toolkit;

namespace mediakit {

MaskEngineClient::MaskEngineClient(EventPoller::Ptr poller)
    : _poller(std::move(poller)), _decoder([this](const MaskProto::Message &msg) { onMessage(msg); }) {}

MaskEngineClient::~MaskEngineClient() {
    _closed = true;
    if (_open_timer) {
        _open_timer->cancel();
    }
    if (_speed_timer) {
        _speed_timer->cancel();
    }
    if (_sock) {
        _sock->closeSock();
    }
}

void MaskEngineClient::open(const string &socket_path, const Json::Value &request, int timeout_ms, OpenCB cb) {
    _on_open = std::move(cb);
    // 本机 unix socket 的 connect 立即完成（或立即失败），直接用阻塞 connect
    // connect() on a local unix socket completes (or fails) immediately, so a blocking connect is fine
    int fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) {
        fail("engine-unavailable", string("socket(): ") + strerror(errno));
        return;
    }
    struct sockaddr_un addr {};
    addr.sun_family = AF_UNIX;
    if (socket_path.size() >= sizeof(addr.sun_path)) {
        ::close(fd);
        fail("engine-unavailable", "socket path too long: " + socket_path);
        return;
    }
    strncpy(addr.sun_path, socket_path.data(), sizeof(addr.sun_path) - 1);
    if (::connect(fd, reinterpret_cast<struct sockaddr *>(&addr), sizeof(addr)) != 0) {
        auto err = string("connect ") + socket_path + ": " + strerror(errno);
        ::close(fd);
        fail("engine-unavailable", err);
        return;
    }
    _sock = Socket::createSocket(_poller, false);
    if (!_sock->fromSock(fd, SockNum::Sock_TCP)) {
        fail("engine-unavailable", "attach socket to poller failed");
        return;
    }
    weak_ptr<MaskEngineClient> weak_self = shared_from_this();
    _sock->setOnRead([weak_self](Buffer::Ptr &buf, struct sockaddr *, int) {
        auto strong_self = weak_self.lock();
        if (!strong_self || strong_self->_closed) {
            return;
        }
        if (!strong_self->_decoder.input(buf->data(), buf->size())) {
            strong_self->fail("protocol", "malformed message from edge-mask");
        }
    });
    _sock->setOnErr([weak_self](const SockException &ex) {
        if (auto strong_self = weak_self.lock()) {
            strong_self->fail("disconnected", ex.what());
        }
    });
    _sock->setOnFlush([weak_self]() {
        auto strong_self = weak_self.lock();
        if (!strong_self) {
            return false;
        }
        if (strong_self->_on_drain && !strong_self->_closed) {
            strong_self->_on_drain();
        }
        return true;
    });
    _open_timer = _poller->doDelayTask(timeout_ms, [weak_self]() {
        if (auto strong_self = weak_self.lock()) {
            if (!strong_self->_opened) {
                strong_self->fail("timeout", "no OPEN_ACK from edge-mask");
            }
        }
        return 0;
    });
    Json::FastWriter writer;
    send(MaskProto::encode(MaskProto::kOpen, writer.write(request)));
}

void MaskEngineClient::onMessage(const MaskProto::Message &msg) {
    if (_closed) {
        return;
    }
    switch (msg.type) {
        case MaskProto::kOpenAck: {
            if (_opened) {
                return;
            }
            _opened = true;
            if (_open_timer) {
                _open_timer->cancel();
                _open_timer = nullptr;
            }
            OpenResult result;
            result.ok = true;
            Json::Reader reader;
            reader.parse(msg.payload, result.ack);
            auto cb = std::move(_on_open);
            _on_open = nullptr;
            if (cb) {
                cb(result);
            }
            return;
        }
        case MaskProto::kBusy:
        case MaskProto::kError: {
            // {"reason": busy / engine-unavailable / bad-request / internal, "detail": ...}
            Json::Value err;
            Json::Reader reader;
            reader.parse(msg.payload, err);
            auto reason = err["reason"].asString();
            if (reason.empty()) {
                reason = msg.type == MaskProto::kBusy ? "busy" : "error";
            }
            fail(reason, err["detail"].asString());
            return;
        }
        case MaskProto::kOutFrame: {
            MaskProto::FrameInfo frame;
            if (!_opened || !MaskProto::parseFrame(msg, frame)) {
                fail("protocol", "unexpected OUT_FRAME");
                return;
            }
            if (_on_frame) {
                _on_frame(frame);
            }
            return;
        }
        case MaskProto::kSpeedAck: {
            if (_speed_timer) {
                _speed_timer->cancel();
                _speed_timer = nullptr;
            }
            auto cb = std::move(_on_speed);
            _on_speed = nullptr;
            if (!cb) {
                // 没有在等的 SPEED（超时后才到）：忽略 / No pending SPEED (arrived after the timeout): ignore
                return;
            }
            Json::Value ack;
            Json::Reader reader;
            reader.parse(msg.payload, ack);
            bool ok = ack["ok"].asBool();
            cb(ok, ok ? "" : ack["reason"].asString() + " " + ack["detail"].asString());
            return;
        }
        default:
            fail("protocol", "unexpected message type " + to_string(msg.type));
            return;
    }
}

void MaskEngineClient::fail(const string &code, const string &message) {
    if (_closed) {
        return;
    }
    _closed = true;
    if (_open_timer) {
        _open_timer->cancel();
        _open_timer = nullptr;
    }
    if (_speed_timer) {
        _speed_timer->cancel();
        _speed_timer = nullptr;
    }
    _on_speed = nullptr;
    if (_sock) {
        _sock->closeSock();
    }
    if (!_opened) {
        auto cb = std::move(_on_open);
        _on_open = nullptr;
        if (cb) {
            OpenResult result;
            result.code = code;
            result.message = message;
            cb(result);
        }
        return;
    }
    auto cb = std::move(_on_error);
    _on_error = nullptr;
    if (cb) {
        cb(code + ": " + message);
    }
}

void MaskEngineClient::send(string data) {
    if (_closed || !_sock) {
        return;
    }
    _sock->send(std::move(data));
}

void MaskEngineClient::sendFrame(uint32_t epoch, bool key_frame, uint64_t pts_ms, const char *data, size_t size) {
    send(MaskProto::encodeFrame(MaskProto::kFrame, epoch, key_frame, static_cast<int64_t>(pts_ms) * 1000, data, size));
}

void MaskEngineClient::flush(uint32_t epoch) {
    Json::Value v;
    v["epoch"] = epoch;
    Json::FastWriter writer;
    send(MaskProto::encode(MaskProto::kFlush, writer.write(v)));
}

void MaskEngineClient::requestSpeed(float speed, uint32_t epoch, int timeout_ms, SpeedCB cb) {
    if (_closed) {
        return;
    }
    _on_speed = std::move(cb);
    Json::Value v;
    v["speed"] = speed;
    v["epoch"] = epoch;
    Json::FastWriter writer;
    send(MaskProto::encode(MaskProto::kSpeed, writer.write(v)));
    weak_ptr<MaskEngineClient> weak_self = shared_from_this();
    if (_speed_timer) {
        _speed_timer->cancel();
    }
    _speed_timer = _poller->doDelayTask(timeout_ms, [weak_self]() {
        if (auto strong_self = weak_self.lock()) {
            if (strong_self->_on_speed) {
                // 引擎状态不明：按会话失败处理，绝不在不确定的状态下继续出流 / Engine state unknown: fail the session
                strong_self->fail("timeout", "no SPEED_ACK from edge-mask");
            }
        }
        return 0;
    });
}

void MaskEngineClient::setOnDrain(DrainCB cb) {
    _on_drain = std::move(cb);
}

void MaskEngineClient::close() {
    if (_closed) {
        return;
    }
    send(MaskProto::encode(MaskProto::kClose, ""));
    _closed = true;
    _on_open = nullptr;
    _on_error = nullptr;
    _on_frame = nullptr;
    _on_speed = nullptr;
    _on_drain = nullptr;
    if (_speed_timer) {
        _speed_timer->cancel();
        _speed_timer = nullptr;
    }
    if (_open_timer) {
        _open_timer->cancel();
        _open_timer = nullptr;
    }
    if (_sock) {
        // 留一点时间把 CLOSE 发出去再断开（对端收到 CLOSE 或断连都会结束会话）
        // Give CLOSE a moment to go out before disconnecting (either ends the session on the peer)
        auto sock = std::move(_sock);
        sock->setOnRead(nullptr);
        sock->setOnErr(nullptr);
        sock->setOnFlush(nullptr);
        _poller->doDelayTask(500, [sock]() {
            sock->closeSock();
            return 0;
        });
    }
}

size_t MaskEngineClient::pendingMessages() {
    return _sock ? _sock->getSendBufferCount() : 0;
}

} // namespace mediakit
