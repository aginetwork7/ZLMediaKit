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

// The part of a key after its last '.': "api.secret" -> "secret", "header.Cookie" -> "cookie".
inline std::string lowerLeafKey(const std::string &key) {
    auto lower = toLowerCopy(key);
    auto dot = lower.rfind('.');
    return dot == std::string::npos ? lower : lower.substr(dot + 1);
}

// Headers whose values are credentials, also under a prefixed key such as on_http_access's
// "header.Cookie".
inline bool isSecretHeader(const std::string &key) {
    auto lower = lowerLeafKey(key);
    return lower == "authorization" || lower == "proxy-authorization" || lower == "cookie" || lower == "set-cookie";
}

// Keys whose values are credentials, case-insensitive and matched by the part after the last '.'
// (so setServerConfig's "api.secret" and on_http_access's "header.Cookie" are ones): the secret header
// names, jwt, and any key ending in token, secret, passphrase, password, passwd or pwd (access_token,
// refreshToken, client_secret, srt.passPhrase, rtc.icePwd, X-Auth-Token, ...). Used for query
// parameters, JSON members, hook fields and headers alike.
inline bool isSecretQueryKey(const std::string &key) {
    if (isSecretHeader(key)) {
        return true;
    }
    auto lower = lowerLeafKey(key);
    if (lower == "jwt") {
        return true;
    }
    for (auto suffix : { "token", "secret", "passphrase", "password", "passwd", "pwd" }) {
        if (endsWith(lower, suffix)) {
            return true;
        }
    }
    return false;
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

// Masks the userinfo of every scheme://userinfo@host in str: the password of user:password
// ("rtsp://admin:***@10.0.0.5/ch1"), or the whole userinfo when it has no ':' (a token used as the
// user name, "https://***@host").
// A URL runs to the next whitespace, quote or "://", so a password holding whitespace or a quote is
// not recognised. Its userinfo ends at the last '@' before the first '/', '?' or '#'; when there is
// none, a password holding those characters unencoded ("rtsp://u:p/w#d@h") is masked up to the last
// '@' of the URL, unless what follows the ':' is a port ("rtsp://h:554/live/a@b", so an all-digit
// password holding a '/' is not recognised either). Masking too much here only hides log text,
// masking too little leaks. Every scan stays inside the current URL, so the cost is linear.
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
        if (at != std::string::npos) {
            auto mask_begin = colon < at ? colon + 1 : host_begin;
            out.append(str, pos, mask_begin - pos);
            out += kRedacted;
            pos = at;
        }
        scheme_end = next_scheme;
    }
    out.append(str, pos, std::string::npos);
    return out;
}

// Masks the ICE password of an SDP ("a=ice-pwd:***"), such as a WHIP/WHEP offer or answer.
inline std::string redactSdpIcePwd(const std::string &str) {
    static const std::string kIcePwd = "a=ice-pwd:";
    std::string out;
    size_t pos = 0;
    for (auto found = str.find(kIcePwd); found != std::string::npos; found = str.find(kIcePwd, pos)) {
        auto value_begin = found + kIcePwd.size();
        auto value_end = str.find_first_of("\r\n", value_begin);
        out.append(str, pos, value_begin - pos);
        out += kRedacted;
        pos = value_end == std::string::npos ? str.size() : value_end;
    }
    out.append(str, pos, std::string::npos);
    return out;
}

// Control characters of a decoded value are shown as '.', so decoding cannot inject log lines.
inline std::string escapeControlChars(std::string str) {
    for (auto &ch : str) {
        if ((unsigned char)ch < 0x20 || ch == 0x7F) {
            ch = '.';
        }
    }
    return str;
}

// How many levels of nested queries and percent-encoding redactText looks into.
static constexpr int kMaxRedactDepth = 4;

inline std::string redactQueryAt(const std::string &str, int depth);

// Masks the credentials in text: URL userinfo, query parameters with a credential key (text holding
// a '=' is read as a URL with a query or as a bare query), and the same inside values that are
// themselves URLs or queries, raw (url=rtsp://h/x?token=t) or percent-encoded
// (url=rtsp%3A%2F%2Fh%2Fx%3Ftoken%3Dt, params=a%3D1%26token%3Dt). An encoded value that needed masking
// is logged decoded. Text without credentials comes back unchanged.
inline std::string redactText(const std::string &text, int depth) {
    if (depth >= kMaxRedactDepth) {
        return redactUrlUserInfo(text);
    }
    if (text.find('=') != std::string::npos) {
        return redactQueryAt(text, depth);
    }
    auto out = redactUrlUserInfo(text);
    if (text.find('%') != std::string::npos) {
        auto decoded = strCoding::UrlDecodeComponent(text);
        if (decoded != text) {
            auto masked = redactText(decoded, depth + 1);
            if (masked != decoded) {
                return escapeControlChars(masked);
            }
        }
    }
    return out;
}

inline std::string redactQueryAt(const std::string &in, int depth) {
    // URL userinfo is masked on the whole text first: a password holding '?', '&' or '=' would be split
    // across the pairs below.
    auto str = redactUrlUserInfo(in);
    auto query_pos = str.find('?');
    // A '=' or '&' before the first '?' makes str a bare query (form body, params): a '?' inside one of
    // its values is not a path separator.
    if (query_pos != std::string::npos && str.find_first_of("=&") < query_pos) {
        query_pos = std::string::npos;
    }
    std::string out = query_pos == std::string::npos ? "" : str.substr(0, query_pos + 1);
    size_t pos = query_pos == std::string::npos ? 0 : query_pos + 1;
    while (pos <= str.size()) {
        auto end = str.find('&', pos);
        if (end == std::string::npos) {
            end = str.size();
        }
        auto pair = str.substr(pos, end - pos);
        auto eq = pair.find('=');
        if (eq == std::string::npos) {
            out += pair;
        } else if (isSecretQueryKey(strCoding::UrlDecodeComponent(pair.substr(0, eq)))) {
            out += pair.substr(0, eq + 1) + kRedacted;
        } else {
            out += pair.substr(0, eq + 1) + redactText(pair.substr(eq + 1), depth + 1);
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
// userinfo, and the credentials of URLs and queries nested in query values.
inline std::string redactQuery(const std::string &str) {
    return redactQueryAt(str, 0);
}

// Masks the credentials in one logged string of any kind: URL, query, SDP or plain text.
inline std::string redactStringForLog(const std::string &str) {
    return redactText(redactSdpIcePwd(str), 0);
}

// A request header for the api debug log: credential headers masked whole (Authorization, Cookie,
// X-Auth-Token, ...), any other value masked like a string (a Referer carrying a token).
inline std::string redactHeaderForLog(const std::string &key, const std::string &value) {
    return isSecretQueryKey(key) ? kRedacted : redactStringForLog(value);
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
        if (isSecretQueryKey(key)) {
            value[key] = kRedacted;
        } else {
            redactJsonForLog(value[key]);
        }
    }
}

// Response body for the api debug log: JSON responses are masked like request bodies, anything else
// (an SDP answer, text) like a string.
inline std::string redactResponseForLog(const std::string &response) {
    Json::Value root;
    if (response.empty() || (response[0] != '{' && response[0] != '[') || !parseJsonForLog(response, root)) {
        return redactStringForLog(response);
    }
    redactJsonForLog(root);
    return root.toStyledString();
}

static constexpr size_t kMaxLogBody = 4 * 1024;

// Request body for the api debug log, at most kMaxLogBody bytes, masked before it is cut (a URL
// password cut before its '@' would no longer be recognised). A JSON body is masked member by member
// and one that does not parse as an object is replaced by its size; any other body (form, SDP offer,
// text) is masked like a string.
inline std::string redactBodyForLog(const std::string &content_type_in, const std::string &raw) {
    auto content_type = toLowerCopy(content_type_in);
    if (content_type.find("json") != std::string::npos) {
        Json::Value root;
        if (!parseJsonForLog(raw, root) || !root.isObject()) {
            return "<json, " + std::to_string(raw.size()) + " bytes>";
        }
        redactJsonForLog(root);
        auto content = root.toStyledString();
        return content.size() > kMaxLogBody ? content.substr(0, kMaxLogBody) : content;
    }
    auto masked = redactStringForLog(raw);
    return masked.size() > kMaxLogBody ? masked.substr(0, kMaxLogBody) : masked;
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
        if (isSecretQueryKey(pr.first)) {
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
