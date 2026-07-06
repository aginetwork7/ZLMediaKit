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

bool BatchPublishSessionManager::addSession(const BatchPublishSession::Ptr &session) {
    lock_guard<recursive_mutex> lck(_mtx);
    auto key = makeKey(session->getApp(), session->getNvrId());
    auto it = _session_map.find(key);
    if (it != _session_map.end()) {
        return false;
    }
    _session_map.emplace(std::move(key), session);
    return true;
}

void BatchPublishSessionManager::removeSession(const string &app, const string &nvr_id) {
    lock_guard<recursive_mutex> lck(_mtx);
    _session_map.erase(makeKey(app, nvr_id));
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

string BatchPublishSessionManager::makeKey(const string &app, const string &nvr_id) {
    return app + "/" + nvr_id;
}

} // namespace mediakit
