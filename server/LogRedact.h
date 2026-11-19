/*
 * Masks credentials before request data is written to logs.
 */

#ifndef ZLMEDIAKIT_LOGREDACT_H
#define ZLMEDIAKIT_LOGREDACT_H

#include <algorithm>
#include <cctype>
#include <string>
#include "json/json.h"

namespace mediakit {

static constexpr char kRedacted[] = "***";

inline std::string toLowerCopy(std::string str) {
    std::transform(str.begin(), str.end(), str.begin(), [](unsigned char c) { return (char)std::tolower(c); });
    return str;
}

// Keys whose values are credentials: media tokens, the API secret and a JWT passed as a parameter.
// A config key in section.key form is matched by its key, so setServerConfig's "api.secret" is one.
inline bool isSecretQueryKey(const std::string &key) {
    auto lower = toLowerCopy(key);
    auto dot = lower.rfind('.');
    if (dot != std::string::npos) {
        lower = lower.substr(dot + 1);
    }
    return lower == "token" || lower == "secret" || lower == "authorization";
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

// Masks credential members at any depth: secret/token/authorization keys (also as section.key, such as
// the "api.secret" of getServerConfig) and secret headers such as cookie.
inline void redactJsonForLog(Json::Value &value) {
    if (value.isArray()) {
        for (auto &item : value) {
            redactJsonForLog(item);
        }
        return;
    }
    if (!value.isObject()) {
        return;
    }
    for (auto &key : value.getMemberNames()) {
        if (isSecretQueryKey(key) || isSecretHeader(key)) {
            value[key] = kRedacted;
        } else {
            redactJsonForLog(value[key]);
        }
    }
}

// Response body for the api debug log: JSON responses are masked like request bodies, anything else
// is kept as is.
inline std::string redactResponseForLog(const std::string &response) {
    Json::Value root;
    Json::Reader reader;
    if (response.empty() || (response[0] != '{' && response[0] != '[') || !reader.parse(response, root)) {
        return response;
    }
    redactJsonForLog(root);
    return root.toStyledString();
}

static constexpr size_t kMaxLogBody = 4 * 1024;

// Request body for the api debug log, at most kMaxLogBody bytes, with credentials masked in form and
// JSON bodies. A JSON body that does not parse as an object is replaced by its size.
inline std::string redactBodyForLog(const std::string &content_type_in, const std::string &raw) {
    auto content_type = toLowerCopy(content_type_in);
    if (content_type.find("urlencoded") != std::string::npos) {
        // Truncating first cannot reveal a value: a key cut before its '=' carries none.
        return redactQuery(raw.size() > kMaxLogBody ? raw.substr(0, kMaxLogBody) : raw);
    }
    if (content_type.find("json") != std::string::npos) {
        Json::Value root;
        Json::Reader reader;
        if (!reader.parse(raw, root) || !root.isObject()) {
            return "<json, " + std::to_string(raw.size()) + " bytes>";
        }
        redactJsonForLog(root);
        auto content = root.toStyledString();
        return content.size() > kMaxLogBody ? content.substr(0, kMaxLogBody) : content;
    }
    return raw.size() > kMaxLogBody ? raw.substr(0, kMaxLogBody) : raw;
}

// Hook body fields masked whole: the player/device JWT ("authorization") and the API secret that
// on_server_started/on_server_keepalive carry with the server config ("api.secret"). "params" (the
// play/publish url query) keeps its other values, only its media token is masked.
static constexpr const char *kHookSecretFields[] = { "authorization", "api.secret" };
static constexpr char kHookParamsField[] = "params";

inline void redactHookBody(Json::Value &value) {
    if (!value.isObject()) {
        return;
    }
    for (auto field : kHookSecretFields) {
        if (value.isMember(field)) {
            value[field] = kRedacted;
        }
    }
    // isMember first: operator[] on a missing key would add "params": null to the logged body.
    if (value.isMember(kHookParamsField) && value[kHookParamsField].isString()) {
        value[kHookParamsField] = redactQuery(value[kHookParamsField].asString());
    }
}

// The same for a form hook body (HttpArgs or any map of string-convertible values).
template <typename Args>
inline void redactHookArgs(Args &args) {
    for (auto field : kHookSecretFields) {
        auto it = args.find(field);
        if (it != args.end()) {
            it->second = kRedacted;
        }
    }
    auto it = args.find(kHookParamsField);
    if (it != args.end()) {
        std::string params = it->second;
        it->second = redactQuery(params);
    }
}

} // namespace mediakit

#endif // ZLMEDIAKIT_LOGREDACT_H
