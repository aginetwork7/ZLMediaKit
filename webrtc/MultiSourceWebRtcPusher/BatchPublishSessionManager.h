/*
 * Copyright (c) 2016-present The ZLMediaKit project authors. All Rights Reserved.
 *
 * This file is part of ZLMediaKit(https://github.com/ZLMediaKit/ZLMediaKit).
 *
 * Use of this source code is governed by MIT-like license that can be found in the
 * LICENSE file in the root of the source tree. All contributing project authors
 * may be found in the AUTHORS file in the root of the source tree.
 */

#ifndef ZLMEDIAKIT_BATCH_PUBLISH_SESSION_MANAGER_H
#define ZLMEDIAKIT_BATCH_PUBLISH_SESSION_MANAGER_H

#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>

#include "json/json.h"
#include "BatchPublishSession.h"

namespace mediakit {

class BatchPublishSessionManager {
public:
    static BatchPublishSessionManager &Instance();

    bool addSession(const BatchPublishSession::Ptr &session);
    void removeSession(const std::string &app, const std::string &nvr_id);
    BatchPublishSession::Ptr getSession(const std::string &app, const std::string &nvr_id) const;
    bool getSourceBinding(const std::string &source_id, BatchPublishSession::SourceBinding &binding, std::string &session_key) const;

    Json::Value getSessionInfo(const std::string &app, const std::string &nvr_id) const;
    Json::Value getSessionSummary(const std::string &app, const std::string &nvr_id) const;

private:
    static std::string makeKey(const std::string &app, const std::string &nvr_id);

private:
    mutable std::recursive_mutex _mtx;
    std::unordered_map<std::string, BatchPublishSession::Ptr> _session_map;
};

} // namespace mediakit

#endif // ZLMEDIAKIT_BATCH_PUBLISH_SESSION_MANAGER_H
