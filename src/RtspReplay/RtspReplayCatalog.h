/*
 * Copyright (c) 2016-present The ZLMediaKit project authors. All Rights Reserved.
 *
 * This file is part of ZLMediaKit(https://github.com/ZLMediaKit/ZLMediaKit).
 *
 * Use of this source code is governed by MIT-like license that can be found in the
 * LICENSE file in the root of the source tree. All contributing project authors
 * may be found in the AUTHORS file in the root of the source tree.
 */

#ifndef SRC_RTSPREPLAY_RTSPREPLAYCATALOG_H_
#define SRC_RTSPREPLAY_RTSPREPLAYCATALOG_H_

#include <string>

#include "RtspReplayTypes.h"

namespace mediakit {

class RtspReplayCatalog {
public:
    static RtspReplayRequest parseRequest(const std::string &schema, const std::string &vhost, const std::string &stream_id);
    static RtspReplayCatalogResult build(const RtspReplayRequest &request);
};

} // namespace mediakit

#endif // SRC_RTSPREPLAY_RTSPREPLAYCATALOG_H_
