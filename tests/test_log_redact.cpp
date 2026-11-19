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

// Hook bodies: authorization and api.secret masked whole, params keeps everything but its token.
static void testHook() {
    auto body = parse(R"({"authorization":"Bearer jwt","api.secret":"s","params":"a=1&token=t","app":"live"})");
    redactHookBody(body);
    expectEq("hook authorization", body["authorization"].asString(), kRedacted);
    expectEq("hook api.secret", body["api.secret"].asString(), kRedacted);
    expectEq("hook params", body["params"].asString(), "a=1&token=***");
    expectEq("hook other", body["app"].asString(), "live");

    // A missing params is not added.
    auto plain = parse(R"({"app":"live"})");
    redactHookBody(plain);
    expectTrue("hook no params", !plain.isMember("params") && plain.size() == 1);

    map<string, string> args { { "authorization", "Bearer jwt" }, { "params", "token=t&b=2" }, { "app", "live" } };
    redactHookArgs(args);
    expectEq("hook args authorization", args["authorization"], kRedacted);
    expectEq("hook args params", args["params"], "token=***&b=2");
    expectEq("hook args other", args["app"], "live");
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

    if (s_failed) {
        cerr << s_failed << " check(s) failed" << endl;
        return EXIT_FAILURE;
    }
    cout << "test_log_redact: all checks passed" << endl;
    return EXIT_SUCCESS;
}
