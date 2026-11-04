/*
 * Copyright (c) 2016-present The ZLMediaKit project authors. All Rights Reserved.
 *
 * This file is part of ZLMediaKit(https://github.com/ZLMediaKit/ZLMediaKit).
 *
 * Use of this source code is governed by MIT-like license that can be found in the
 * LICENSE file in the root of the source tree. All contributing project authors
 * may be found in the AUTHORS file in the root of the source tree.
 */

#include "BatchPublishSessionManager.h"

using namespace std;

namespace mediakit {

BatchPublishSessionManager &BatchPublishSessionManager::Instance() {
    static BatchPublishSessionManager s_instance;
    return s_instance;
}

BatchPublishSession::Ptr BatchPublishSessionManager::replaceSession(const BatchPublishSession::Ptr &session) {
    lock_guard<recursive_mutex> lck(_mtx);
    auto &slot = _session_map[makeKey(session->getApp(), session->getNvrId())];
    auto old = std::move(slot);
    slot = session;
    if (old) {
        // Release the old sources before the new session can register the same app/sourceId.
        old->clearAllSource();
    }
    return old;
}

void BatchPublishSessionManager::removeSession(const string &app, const string &nvr_id, const BatchPublishSession::Ptr &session) {
    lock_guard<recursive_mutex> lck(_mtx);
    auto it = _session_map.find(makeKey(app, nvr_id));
    if (it != _session_map.end() && it->second == session) {
        _session_map.erase(it);
    }
}

BatchPublishSession::Ptr BatchPublishSessionManager::getSession(const string &app, const string &nvr_id) const {
    lock_guard<recursive_mutex> lck(_mtx);
    auto it = _session_map.find(makeKey(app, nvr_id));
    if (it == _session_map.end()) {
        return nullptr;
    }
    return it->second;
}

bool BatchPublishSessionManager::getSourceBinding(const string &source_id, BatchPublishSession::SourceBinding &binding, string &session_key) const {
    lock_guard<recursive_mutex> lck(_mtx);
    for (auto &it : _session_map) {
        if (it.second && it.second->getSourceBinding(source_id, binding)) {
            session_key = it.first;
            return true;
        }
    }
    return false;
}

Json::Value BatchPublishSessionManager::getSessionInfo(const string &app, const string &nvr_id) const {
    auto session = getSession(app, nvr_id);
    Json::Value data;
    data["exists"] = !!session;
    if (session) {
        data["session"] = session->getInfo();
    }
    return data;
}

Json::Value BatchPublishSessionManager::getSessionSummary(const string &app, const string &nvr_id) const {
    auto session = getSession(app, nvr_id);
    Json::Value data;
    data["exists"] = !!session;
    if (session) {
        data["session"] = session->getSummary();
    }
    return data;
}

string BatchPublishSessionManager::makeKey(const string &app, const string &nvr_id) {
    return app + "/" + nvr_id;
}

} // namespace mediakit
