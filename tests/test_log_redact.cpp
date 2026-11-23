/*
 * Checks the credential masking in server/LogRedact.h. Exits non-zero if any check fails.
 */

#include <cstdlib>
#include <iostream>
#include <map>
#include <string>
#include "../server/LogRedact.h"

using namespace std;
using namespace mediakit;

static int s_failed = 0;

static void expectEq(const string &name, const string &got, const string &want) {
    if (got != want) {
        cerr << "FAIL " << name << ": got \"" << got << "\", want \"" << want << "\"" << endl;
        ++s_failed;
    }
}

static void expectTrue(const string &name, bool value) {
    if (!value) {
        cerr << "FAIL " << name << endl;
        ++s_failed;
    }
}

static Json::Value parse(const string &str) {
    Json::Value root;
    Json::Reader reader;
    if (!reader.parse(str, root)) {
        cerr << "FAIL parse: " << str << endl;
        ++s_failed;
    }
    return root;
}

// JSON bodies and responses: masked at any depth, in objects and in arrays.
static void testJson() {
    auto nested = parse(R"({"app":"live","auth":{"token":"t1","user":"u"}})");
    redactJsonForLog(nested);
    expectEq("nested token", nested["auth"]["token"].asString(), kRedacted);
    expectEq("nested other", nested["auth"]["user"].asString(), "u");
    expectEq("top other", nested["app"].asString(), "live");

    // The getServerConfig shape: an array of config objects.
    auto config = parse(R"({"code":0,"data":[{"api.secret":"s","api.apiDebug":"1","hook.enable":"1"}]})");
    redactJsonForLog(config);
    expectEq("config api.secret", config["data"][0]["api.secret"].asString(), kRedacted);
    expectEq("config other", config["data"][0]["api.apiDebug"].asString(), "1");

    auto deep = parse(R"({"a":{"b":{"Authorization":"Bearer x","Cookie":"c"}}})");
    redactJsonForLog(deep);
    expectEq("deep authorization", deep["a"]["b"]["Authorization"].asString(), kRedacted);
    expectEq("deep cookie", deep["a"]["b"]["Cookie"].asString(), kRedacted);

    // Query credentials inside string values, at any depth; other strings are kept as they are.
    auto urls = parse(R"({"data":[{"originUrl":"rtsp://h/live/s1?token=t&vhost=v","app":"a=b","sdp":"v=0\r\na=rtpmap:96 H264/90000","url":"http://h/x?app=live"}]})");
    redactJsonForLog(urls);
    expectEq("url token", urls["data"][0]["originUrl"].asString(), "rtsp://h/live/s1?token=***&vhost=v");
    expectEq("plain pair", urls["data"][0]["app"].asString(), "a=b");
    expectEq("sdp", urls["data"][0]["sdp"].asString(), "v=0\r\na=rtpmap:96 H264/90000");
    expectEq("url without token", urls["data"][0]["url"].asString(), "http://h/x?app=live");

    auto response = redactResponseForLog(R"({"code":0,"data":[{"api.secret":"s"}]})");
    expectTrue("response masked", response.find("\"s\"") == string::npos && response.find(kRedacted) != string::npos);
    expectEq("response not json", redactResponseForLog("plain text"), "plain text");
    expectEq("response empty", redactResponseForLog(""), "");
}

// Request bodies by content type.
static void testBody() {
    auto json = redactBodyForLog("application/json; charset=utf-8", R"({"secret":"s","app":"live"})");
    expectTrue("json body masked", json.find("\"s\"") == string::npos && json.find("live") != string::npos);
    expectEq("json not object", redactBodyForLog("application/json", "[1,2]"), "<json, 5 bytes>");
    expectEq("json unparsable", redactBodyForLog("application/json", "{bad"), "<json, 4 bytes>");

    expectEq("form body", redactBodyForLog("application/x-www-form-urlencoded", "secret=s&app=live"), "secret=***&app=live");
    // A long form body is cut to kMaxLogBody before masking; a secret near the start is still masked.
    auto long_form = "secret=s&pad=" + string(2 * kMaxLogBody, 'x');
    auto cut = redactBodyForLog("application/x-www-form-urlencoded", long_form);
    expectTrue("long form cut", cut.size() <= kMaxLogBody + 3);
    expectTrue("long form masked", cut.compare(0, 11, "secret=***&") == 0);

    expectEq("other body", redactBodyForLog("text/plain", "hello"), "hello");
    expectTrue("other body cut", redactBodyForLog("text/plain", string(2 * kMaxLogBody, 'x')).size() == kMaxLogBody);
}

// URL passwords, nested query credentials and password-like keys.
static void testUrlCredentials() {
    expectEq("userinfo", redactUrlUserInfo("rtsp://admin:pw@10.0.0.5:554/ch1"), "rtsp://admin:***@10.0.0.5:554/ch1");
    expectEq("userinfo two urls", redactUrlUserInfo("a rtsp://u:p@h/x b rtmp://v:q@g/y"), "a rtsp://u:***@h/x b rtmp://v:***@g/y");
    expectEq("userinfo @ in password", redactUrlUserInfo("rtsp://u:p@ss@h/x"), "rtsp://u:***@h/x");
    expectEq("no userinfo", redactUrlUserInfo("rtsp://h:554/live/a@b"), "rtsp://h:554/live/a@b");
    expectEq("user only", redactUrlUserInfo("rtsp://u@h/x"), "rtsp://u@h/x");
    expectEq("not a url", redactUrlUserInfo("a:b@c"), "a:b@c");

    // Passwords holding unencoded '/', '?', '#' or '&'; a port before an '@' in the path is not a password.
    expectEq("password slash", redactUrlUserInfo("rtsp://u:p/w@h/x"), "rtsp://u:***@h/x");
    expectEq("password question", redactUrlUserInfo("rtsp://u:p?w@h/x"), "rtsp://u:***@h/x");
    expectEq("password hash", redactUrlUserInfo("rtsp://u:p#w@h/x"), "rtsp://u:***@h/x");
    expectEq("password amp", redactUrlUserInfo("rtsp://u:p&w@h/x"), "rtsp://u:***@h/x");
    expectEq("port then path @", redactUrlUserInfo("rtsp://h:554/live/a@b"), "rtsp://h:554/live/a@b");
    expectEq("userinfo then path @", redactUrlUserInfo("rtsp://u:p@h/live/a@b"), "rtsp://u:***@h/live/a@b");
    expectEq("url ends at next url", redactUrlUserInfo("rtsp://h:554/a?cb=http://u:p@g/x"), "rtsp://h:554/a?cb=http://u:***@g/x");
    expectEq("url ends at quote", redactUrlUserInfo("'rtsp://h/x' u:p@g"), "'rtsp://h/x' u:p@g");
    // Many URLs without '@' stay linear: a quadratic scan of this would take minutes.
    string many;
    for (int i = 0; i < 200000; ++i) {
        many += "a://b/";
    }
    expectTrue("many urls", redactUrlUserInfo(many) == many);

    expectEq("query url userinfo", redactQuery("/index/api/addStreamProxy?url=rtsp://u:p@h/x&app=a"),
             "/index/api/addStreamProxy?url=rtsp://u:***@h/x&app=a");
    expectEq("query nested raw token", redactQuery("/index/api/addStreamProxy?url=rtsp://h/live?token=t&app=a"),
             "/index/api/addStreamProxy?url=rtsp://h/live?token=***&app=a");
    expectEq("query nested raw token first", redactQuery("/index/api/addStreamProxy?app=a&url=rtsp://h/live?token=t&stream=s"),
             "/index/api/addStreamProxy?app=a&url=rtsp://h/live?token=***&stream=s");
    expectEq("query nested encoded token", redactQuery("/a?url=rtsp%3A%2F%2Fh%2Fs%3Ftoken%3Dt&app=a"),
             "/a?url=rtsp://h/s?token=***&app=a");
    expectEq("query nested encoded userinfo", redactQuery("/a?dst_url=rtmp%3A%2F%2Fu%3Ap%40h%2Fs"), "/a?dst_url=rtmp://u:***@h/s");
    expectEq("query encoded no secret", redactQuery("/a?url=rtsp%3A%2F%2Fh%2Fs&app=a"), "/a?url=rtsp%3A%2F%2Fh%2Fs&app=a");
    expectEq("form url userinfo", redactBodyForLog("application/x-www-form-urlencoded", "url=rtsp%3A%2F%2Fu%3Ap%40h%2Fx&app=a"),
             "url=rtsp://u:***@h/x&app=a");

    expectEq("query key before =", redactQuery("rtsp://u:p@h/x;transport=tcp"), "rtsp://u:***@h/x;transport=tcp");
    expectEq("query encoded bare query", redactQuery("/a?params=a%3D1%26token%3Dt&app=a"), "/a?params=a=1&token=***&app=a");
    // A form body is masked before it is cut, so a URL password at the cut is not revealed.
    auto pad = string(kMaxLogBody - 20, 'x');
    auto cut_form = redactBodyForLog("application/x-www-form-urlencoded", "pad=" + pad + "&url=rtsp://admin:secretpw@h/x");
    expectTrue("form cut after mask", cut_form.find("secr") == string::npos && cut_form.size() <= kMaxLogBody);

    expectTrue("header prefixed cookie", isSecretHeader("header.Cookie"));
    expectTrue("header prefixed proxy", isSecretHeader("header.Proxy-Authorization"));
    expectTrue("header set-cookie", isSecretHeader("Set-Cookie"));
    expectTrue("header prefixed host", !isSecretHeader("header.Host"));
    auto access = parse(R"({"header.Cookie":"session=abc; sid=xyz","header.Host":"h"})");
    redactHookBody(access);
    expectEq("hook header cookie", access["header.Cookie"].asString(), kRedacted);
    expectEq("hook header host", access["header.Host"].asString(), "h");

    expectTrue("key passPhrase", isSecretQueryKey("srt.passPhrase"));
    expectTrue("key icePwd", isSecretQueryKey("rtc.icePwd"));
    expectTrue("key password", isSecretQueryKey("password"));
    expectTrue("key passwd", isSecretQueryKey("passwd"));
    expectTrue("key pwd", isSecretQueryKey("PWD"));
    expectTrue("key port", !isSecretQueryKey("rtc.port"));
    expectEq("setServerConfig passwords", redactQuery("/index/api/setServerConfig?srt.passPhrase=k&rtc.icePwd=p&password=x&rtc.port=8000"),
             "/index/api/setServerConfig?srt.passPhrase=***&rtc.icePwd=***&password=***&rtc.port=8000");

    auto json = parse(R"({"data":[{"originUrl":"rtsp://admin:pw@h/ch1","srt.passPhrase":"k","rtc.icePwd":"p","name":"cam"}]})");
    redactJsonForLog(json);
    expectEq("json userinfo", json["data"][0]["originUrl"].asString(), "rtsp://admin:***@h/ch1");
    expectEq("json passPhrase", json["data"][0]["srt.passPhrase"].asString(), kRedacted);
    expectEq("json icePwd", json["data"][0]["rtc.icePwd"].asString(), kRedacted);
    expectEq("json other", json["data"][0]["name"].asString(), "cam");

    auto body = redactBodyForLog("application/json", R"({"url":"rtsp://u:p@h/x","password":"x"})");
    expectTrue("json body userinfo", body.find(":p@") == string::npos && body.find("rtsp://u:***@h/x") != string::npos);
    expectTrue("json body password", body.find("\"x\"") == string::npos);
}

// JSON nested deeper than jsoncpp allows makes its reader throw; the log helpers fall back instead.
static void testDeepJson() {
    string deep_object;
    for (int i = 0; i < 2000; ++i) {
        deep_object += "{\"a\":";
    }
    deep_object += "1" + string(2000, '}');
    expectEq("deep json body", redactBodyForLog("application/json", deep_object), "<json, " + to_string(deep_object.size()) + " bytes>");
    auto deep_array = string(2000, '[') + string(2000, ']');
    expectEq("deep json response", redactResponseForLog(deep_array), deep_array);
}

// Hook bodies: authorization and api.secret masked whole, params keeps everything but its token.
static void testHook() {
    auto body = parse(R"({"authorization":"Bearer jwt","api.secret":"s","params":"a=1&token=t","app":"live"})");
    redactHookBody(body);
    expectEq("hook authorization", body["authorization"].asString(), kRedacted);
    expectEq("hook api.secret", body["api.secret"].asString(), kRedacted);
    expectEq("hook params", body["params"].asString(), "a=1&token=***");
    expectEq("hook other", body["app"].asString(), "live");

    // Nested media info in a hook body: its originUrl token is masked too.
    auto nested = parse(R"({"mediaInfo":{"originUrl":"webrtc://h/live/s1?token=t"},"schema":"rtsp"})");
    redactHookBody(nested);
    expectEq("hook nested url", nested["mediaInfo"]["originUrl"].asString(), "webrtc://h/live/s1?token=***");
    expectEq("hook nested other", nested["schema"].asString(), "rtsp");

    // A missing params is not added.
    auto plain = parse(R"({"app":"live"})");
    redactHookBody(plain);
    expectTrue("hook no params", !plain.isMember("params") && plain.size() == 1);

    map<string, string> args { { "authorization", "Bearer jwt" }, { "params", "token=t&b=2" }, { "app", "live" },
                               { "originUrl", "rtsp://u:p@h/x" }, { "srt.passPhrase", "k" } };
    redactHookArgs(args);
    expectEq("hook args authorization", args["authorization"], kRedacted);
    expectEq("hook args params", args["params"], "token=***&b=2");
    expectEq("hook args other", args["app"], "live");
    expectEq("hook args userinfo", args["originUrl"], "rtsp://u:***@h/x");
    expectEq("hook args passPhrase", args["srt.passPhrase"], kRedacted);
    expectTrue("hook args no secret added", args.find("api.secret") == args.end());
}

int main() {
    // Secret query keys, case-insensitive.
    expectTrue("query key secret", isSecretQueryKey("secret"));
    expectTrue("query key token", isSecretQueryKey("token"));
    expectTrue("query key upper", isSecretQueryKey("SECRET"));
    expectTrue("query key authorization", isSecretQueryKey("authorization"));
    expectTrue("query key app", !isSecretQueryKey("app"));
    expectTrue("query key stream", !isSecretQueryKey("stream"));

    // Secret headers, case-insensitive.
    expectTrue("header authorization", isSecretHeader("Authorization"));
    expectTrue("header authorization lower", isSecretHeader("authorization"));
    expectTrue("header proxy-authorization", isSecretHeader("Proxy-Authorization"));
    expectTrue("header cookie", isSecretHeader("Cookie"));
    expectTrue("header host", !isSecretHeader("Host"));

    // Path with a query: only the secret values are masked, order and other pairs are kept.
    expectEq("url with secret", redactQuery("/index/api/getMediaList?secret=abc&app=live"),
             "/index/api/getMediaList?secret=***&app=live");
    expectEq("url with token last", redactQuery("/index/api/whep?app=live&stream=s1&token=jwt.a.b"),
             "/index/api/whep?app=live&stream=s1&token=***");
    expectEq("url upper key", redactQuery("/a?Secret=x"), "/a?Secret=***");
    // A bare query string (no '?') is masked the same way.
    expectEq("bare query", redactQuery("secret=abc&app=live"), "secret=***&app=live");
    // Nothing to mask.
    expectEq("no secret", redactQuery("/a?app=live&stream=s1"), "/a?app=live&stream=s1");
    expectEq("no query", redactQuery("/index/api/getServerConfig"), "/index/api/getServerConfig");
    // A key without '=' and an empty value are kept as they are; an empty secret is still masked.
    expectEq("flag and empty", redactQuery("/a?debug&secret="), "/a?debug&secret=***");
    // A key that only contains "secret" is not a secret key.
    expectEq("similar key", redactQuery("/a?secretary=1"), "/a?secretary=1");

    // A config key in section.key form is matched by its key: setServerConfig's api.secret.
    expectTrue("query key api.secret", isSecretQueryKey("api.secret"));
    expectTrue("query key API.Secret", isSecretQueryKey("API.Secret"));
    expectTrue("query key api.apiDebug", !isSecretQueryKey("api.apiDebug"));
    expectEq("setServerConfig url", redactQuery("/index/api/setServerConfig?secret=a&api.secret=new&api.apiDebug=1"),
             "/index/api/setServerConfig?secret=***&api.secret=***&api.apiDebug=1");

    testJson();
    testBody();
    testHook();
    testUrlCredentials();
    testDeepJson();

    if (s_failed) {
        cerr << s_failed << " check(s) failed" << endl;
        return EXIT_FAILURE;
    }
    cout << "test_log_redact: all checks passed" << endl;
    return EXIT_SUCCESS;
}
