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

#include <string>

namespace mediakit {

class RtspReplaySourceFactory {
public:
    // replay stream id 校验，避免无关业务进入创建流程；不抛异常。
    // Validate a replay stream id so unrelated streams never enter the creation flow; never throws.
    static bool validateStreamKey(const std::string &stream_id);
    // 按 replay URL 参数创建一个独立的预备回放会话。有副作用：注册 MediaSource、启动 reader 定时器、
    // 挂载超时清理任务。成功时 out_session_stream 为生成的会话流 id；任何失败(id 非法、无录像、
    // reader 启动失败)都不抛异常，只保持 out_session_stream 为空。
    // Create an isolated prepared replay session from replay url parameters. This has side effects: it
    // registers a MediaSource, starts the reader timer and schedules a cleanup task. On success
    // out_session_stream carries the generated session stream id; every failure (invalid id, no
    // recordings, reader start failure) leaves it empty instead of throwing.
    static void create(const std::string &schema, const std::string &vhost, const std::string &stream_id, std::string &out_session_stream);
};

// 根据 replay URL 参数创建独立回放会话
void createReplaySession(const std::string &schema, const std::string &vhost, const std::string &stream_id, std::string &out_session_stream);

} // namespace mediakit

#endif // SRC_RTSPREPLAY_RTSPREPLAYSOURCEFACTORY_H_
