/*
 * Copyright (c) 2016-present The ZLMediaKit project authors. All Rights Reserved.
 *
 * This file is part of ZLMediaKit(https://github.com/ZLMediaKit/ZLMediaKit).
 *
 * Use of this source code is governed by MIT-like license that can be found in the
 * LICENSE file in the root of the source tree. All contributing project authors
 * may be found in the AUTHORS file in the root of the source tree.
 */

#ifndef SRC_RTSPREPLAY_RTSPREPLAYSOURCEFACTORY_H_
#define SRC_RTSPREPLAY_RTSPREPLAYSOURCEFACTORY_H_

#include <stdexcept>
#include <string>

namespace mediakit {

// 回放会话超限异常，用于向协议层传播 503
class ReplayLimitException : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

class RtspReplaySourceFactory {
public:
    // 轻量判定请求是否可能是 replay stream id，避免无关业务进入创建流程
    static bool canHandle(const std::string &stream_id);
    static void create(const std::string &schema, const std::string &vhost, const std::string &stream_id, std::string &out_session_stream);
};

// 兼容入口：根据 replay URL 参数创建独立回放会话
void createReplaySession(const std::string &schema, const std::string &vhost, const std::string &stream_id, std::string &out_session_stream);

} // namespace mediakit

#endif // SRC_RTSPREPLAY_RTSPREPLAYSOURCEFACTORY_H_
