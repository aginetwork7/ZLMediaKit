/*
 * Masks credentials before request data is written to logs.
 */

#ifndef ZLMEDIAKIT_LOGREDACT_H
#define ZLMEDIAKIT_LOGREDACT_H

#include <algorithm>
#include <cctype>
#include <exception>
#include <string>
#include "json/json.h"
#include "Common/strCoding.h"

namespace mediakit {

static constexpr char kRedacted[] = "***";

inline std::string toLowerCopy(std::string str) {
    std::transform(str.begin(), str.end(), str.begin(), [](unsigned char c) { return (char)std::tolower(c); });
    return str;
}

inline bool endsWith(const std::string &str, const std::string &suffix) {
    return str.size() >= suffix.size() && str.compare(str.size() - suffix.size(), suffix.size(), suffix) == 0;
}

// Keys whose values are credentials: media tokens, the API secret, a JWT passed as a parameter and
// passwords (srt.passPhrase, rtc.icePwd, password, passwd, pwd). A config key in section.key form is
// matched by its key, so setServerConfig's "api.secret" is one.
inline bool isSecretQueryKey(const std::string &key) {
    auto lower = toLowerCopy(key);
    auto dot = lower.rfind('.');
    if (dot != std::string::npos) {
        lower = lower.substr(dot + 1);
    }
    if (lower == "token" || lower == "secret" || lower == "authorization") {
        return true;
    }
    for (auto suffix : { "passphrase", "password", "passwd", "pwd" }) {
        if (endsWith(lower, suffix)) {
            return true;
        }
    }
    return false;
}

// Headers whose values are credentials, also under a prefixed key such as on_http_access's
// "header.Cookie".
inline bool isSecretHeader(const std::string &key) {
    auto lower = toLowerCopy(key);
    auto dot = lower.rfind('.');
    if (dot != std::string::npos) {
        lower = lower.substr(dot + 1);
    }
    return lower == "authorization" || lower == "proxy-authorization" || lower == "cookie" || lower == "set-cookie";
}

inline bool isAllDigits(const std::string &str, size_t begin, size_t end) {
    if (begin >= end) {
        return false;
    }
    for (auto i = begin; i < end; ++i) {
        if (!std::isdigit((unsigned char)str[i])) {
            return false;
        }
    }
    return true;
}

// Masks the password of every scheme://user:password@host in str, such as the camera credentials of
// an addStreamProxy url: "rtsp://admin:***@10.0.0.5/ch1".
// A URL runs to the next whitespace, quote or "://". Its userinfo ends at the last '@' before the
// first '/', '?' or '#'; when there is none, a password holding those characters unencoded
// ("rtsp://u:p/w#d@h") is masked up to the last '@' of the URL, unless what follows the ':' is a port
// ("rtsp://h:554/live/a@b"). Masking too much here only hides log text, masking too little leaks.
// Every scan stays inside the current URL, so the cost is linear in the size of str.
inline std::string redactUrlUserInfo(const std::string &str) {
    std::string out;
    size_t pos = 0;
    auto scheme_end = str.find("://");
    while (scheme_end != std::string::npos) {
        auto host_begin = scheme_end + 3;
        auto next_scheme = str.find("://", host_begin);
        auto url_end = next_scheme == std::string::npos ? str.size() : next_scheme;
        size_t colon = std::string::npos, authority_end = std::string::npos, at_in_authority = std::string::npos, last_at = std::string::npos;
        for (auto i = host_begin; i < url_end; ++i) {
            auto ch = str[i];
            if (ch == ' ' || ch == '\t' || ch == '\r' || ch == '\n' || ch == '"' || ch == '\'') {
                url_end = i;
                break;
            }
            if (ch == ':' && colon == std::string::npos) {
                colon = i;
            } else if ((ch == '/' || ch == '?' || ch == '#') && authority_end == std::string::npos) {
                authority_end = i;
            } else if (ch == '@') {
                last_at = i;
                if (authority_end == std::string::npos) {
                    at_in_authority = i;
                }
            }
        }
        auto at = at_in_authority;
        if (at == std::string::npos && last_at != std::string::npos && colon < authority_end
            && !isAllDigits(str, colon + 1, authority_end)) {
            at = last_at;
        }
        if (at != std::string::npos && colon < at) {
            out.append(str, pos, colon + 1 - pos);
            out += kRedacted;
            pos = at;
        }
        scheme_end = next_scheme;
    }
    out.append(str, pos, std::string::npos);
    return out;
}

// How deep redactQuery follows URLs nested in query values.
static constexpr int kMaxRedactDepth = 3;

inline std::string redactQueryAt(const std::string &str, int depth);

// A query value: the password of a URL in it is masked, and so are the credentials of a query nested
// in it, whether raw (url=rtsp://h/x?token=t) or percent-encoded (url=rtsp%3A%2F%2Fh%2Fx%3Ftoken%3Dt).
// An encoded value that needed masking is logged decoded.
inline std::string redactQueryValue(const std::string &value, int depth) {
    auto out = redactUrlUserInfo(value);
    if (depth >= kMaxRedactDepth) {
        return out;
    }
    if (out.find('?') != std::string::npos) {
        return redactQueryAt(out, depth + 1);
    }
    if (value.find('%') != std::string::npos) {
        auto decoded = strCoding::UrlDecodeComponent(value);
        if (decoded != value) {
            // A decoded bare query ("a=1&token=t") is masked like a query even without a '?'.
            auto masked = decoded.find('=') != std::string::npos ? redactQueryAt(decoded, depth + 1)
                                                                 : redactQueryValue(decoded, depth + 1);
            if (masked != decoded) {
                return masked;
            }
        }
    }
    return out;
}

inline std::string redactQueryAt(const std::string &str, int depth) {
    auto query_pos = str.find('?');
    std::string out = query_pos == std::string::npos ? "" : redactUrlUserInfo(str.substr(0, query_pos + 1));
    size_t pos = query_pos == std::string::npos ? 0 : query_pos + 1;
    while (pos <= str.size()) {
        auto end = str.find('&', pos);
        if (end == std::string::npos) {
            end = str.size();
        }
        auto pair = str.substr(pos, end - pos);
        auto eq = pair.find('=');
        if (eq == std::string::npos) {
            out += redactUrlUserInfo(pair);
        } else if (isSecretQueryKey(pair.substr(0, eq))) {
            out += pair.substr(0, eq + 1) + kRedacted;
        } else {
            // The key side may be a URL too, as in "rtsp://u:p@h/x;transport=tcp".
            out += redactUrlUserInfo(pair.substr(0, eq + 1)) + redactQueryValue(pair.substr(eq + 1), depth);
        }
        if (end == str.size()) {
            break;
        }
        out += '&';
        pos = end + 1;
    }
    return out;
}

// Masks secret values in a URL ("/path?a=1&token=x") or a bare query string ("a=1&token=x"), URL
// passwords, and the credentials of URLs nested in query values.
inline std::string redactQuery(const std::string &str) {
    return redactQueryAt(str, 0);
}

// Masks the credentials in one logged string value: a query string or URL goes through redactQuery,
// any other string only has its URL passwords masked.
inline std::string redactStringForLog(const std::string &str) {
    return str.find('=') != std::string::npos ? redactQuery(str) : redactUrlUserInfo(str);
}

// Parses str as JSON; false when it is not valid JSON or nests deeper than jsoncpp allows, in which
// case the reader throws instead of returning false.
inline bool parseJsonForLog(const std::string &str, Json::Value &root) {
    try {
        Json::Reader reader;
        return reader.parse(str, root);
    } catch (std::exception &) {
        return false;
    }
}

// Masks credentials at any depth: members with a credential key (also as section.key, such as the
// "api.secret" or "srt.passPhrase" of getServerConfig) or a secret header name such as cookie, and the
// credentials inside string values: URL passwords and query credentials, such as an originUrl carrying
// "?token=..." or "rtsp://user:password@...". Strings without credentials are kept unchanged.
inline void redactJsonForLog(Json::Value &value) {
    if (value.isString()) {
        auto str = value.asString();
        auto masked = redactStringForLog(str);
        if (masked != str) {
            value = masked;
        }
        return;
    }
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
    if (response.empty() || (response[0] != '{' && response[0] != '[') || !parseJsonForLog(response, root)) {
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
        // Mask before truncating: a URL password cut before its '@' would no longer be recognised.
        auto masked = redactQuery(raw);
        return masked.size() > kMaxLogBody ? masked.substr(0, kMaxLogBody) : masked;
    }
    if (content_type.find("json") != std::string::npos) {
        Json::Value root;
        if (!parseJsonForLog(raw, root) || !root.isObject()) {
            return "<json, " + std::to_string(raw.size()) + " bytes>";
        }
        redactJsonForLog(root);
        auto content = root.toStyledString();
        return content.size() > kMaxLogBody ? content.substr(0, kMaxLogBody) : content;
    }
    return raw.size() > kMaxLogBody ? raw.substr(0, kMaxLogBody) : raw;
}

// A JSON hook body is masked like an API body: credential members at any depth (authorization,
// api.secret, ...) and the credentials in any string value (params, originUrl, ...).
inline void redactHookBody(Json::Value &value) {
    redactJsonForLog(value);
}

// The same for a form hook body (HttpArgs or any map of string-convertible values).
template <typename Args>
inline void redactHookArgs(Args &args) {
    for (auto &pr : args) {
        if (isSecretQueryKey(pr.first) || isSecretHeader(pr.first)) {
            pr.second = kRedacted;
            continue;
        }
        std::string value = pr.second;
        auto masked = redactStringForLog(value);
        if (masked != value) {
            pr.second = masked;
        }
    }
}

} // namespace mediakit

#endif // ZLMEDIAKIT_LOGREDACT_H
