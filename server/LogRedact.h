/*
 * Masks credentials before request data is written to logs.
 */

#ifndef ZLMEDIAKIT_LOGREDACT_H
#define ZLMEDIAKIT_LOGREDACT_H

#include <algorithm>
#include <cctype>
#include <string>

namespace mediakit {

static constexpr char kRedacted[] = "***";

inline std::string toLowerCopy(std::string str) {
    std::transform(str.begin(), str.end(), str.begin(), [](unsigned char c) { return (char)std::tolower(c); });
    return str;
}

// Query keys whose values are credentials: media tokens and the API secret.
inline bool isSecretQueryKey(const std::string &key) {
    auto lower = toLowerCopy(key);
    return lower == "token" || lower == "secret";
}

// Headers whose values are credentials.
inline bool isSecretHeader(const std::string &key) {
    auto lower = toLowerCopy(key);
    return lower == "authorization" || lower == "proxy-authorization" || lower == "cookie";
}

// Masks secret values in a URL ("/path?a=1&token=x") or a bare query string ("a=1&token=x").
inline std::string redactQuery(const std::string &str) {
    auto query_pos = str.find('?');
    std::string out = query_pos == std::string::npos ? "" : str.substr(0, query_pos + 1);
    size_t pos = query_pos == std::string::npos ? 0 : query_pos + 1;
    while (pos <= str.size()) {
        auto end = str.find('&', pos);
        if (end == std::string::npos) {
            end = str.size();
        }
        auto pair = str.substr(pos, end - pos);
        auto eq = pair.find('=');
        if (eq != std::string::npos && isSecretQueryKey(pair.substr(0, eq))) {
            out += pair.substr(0, eq + 1) + kRedacted;
        } else {
            out += pair;
        }
        if (end == str.size()) {
            break;
        }
        out += '&';
        pos = end + 1;
    }
    return out;
}

} // namespace mediakit

#endif // ZLMEDIAKIT_LOGREDACT_H
