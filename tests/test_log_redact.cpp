/*
 * Checks the credential masking in server/LogRedact.h. Exits non-zero on the first failure.
 */

#include <cstdlib>
#include <iostream>
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

    if (s_failed) {
        cerr << s_failed << " check(s) failed" << endl;
        return EXIT_FAILURE;
    }
    cout << "test_log_redact: all checks passed" << endl;
    return EXIT_SUCCESS;
}
