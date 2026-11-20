/*
 * Checks mergeIniForSave (server/IniSave.h): which values saveIniFile writes back to the config file
 * when --secret, --media-server-id or --hook-base-url override it. Exits non-zero if any check fails.
 */

#include <cstdlib>
#include <iostream>
#include <string>
#include "../server/IniSave.h"

using namespace std;
using namespace toolkit;
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

static const string kSecretKey = "api.secret";
static const string kServerIdKey = "general.mediaServerId";

static CliOverrides cli(const string &hook_base_url, const string &server_id, const string &secret) {
    return CliOverrides { hook_base_url, server_id, secret, kServerIdKey, kSecretKey };
}

int main() {
    mINI disk;
    disk[kSecretKey] = "file-secret";
    disk[kServerIdKey] = "file-id";
    disk["hook.on_publish"] = "__HOOK_BASE_URL__/on_publish";
    disk["hook.on_play"] = "http://fixed/on_play";
    disk["rtc.port"] = "8000";

    // In memory after startup: the command-line values applied over the file.
    mINI mem = disk;
    mem[kSecretKey] = "cli-secret";
    mem[kServerIdKey] = "cli-id";
    mem["hook.on_publish"] = "http://ctl/on_publish";

    // Unchanged command-line values: the file keeps its own values, placeholder included.
    {
        auto saved = mergeIniForSave(mem, disk, cli("http://ctl/", "cli-id", "cli-secret"));
        expectEq("secret kept", saved[kSecretKey], "file-secret");
        expectEq("server id kept", saved[kServerIdKey], "file-id");
        expectEq("placeholder kept", saved["hook.on_publish"], "__HOOK_BASE_URL__/on_publish");
        expectEq("fixed hook", saved["hook.on_play"], "http://fixed/on_play");
        expectEq("other key", saved["rtc.port"], "8000");
    }

    // Values changed through setServerConfig since startup are saved as they are.
    {
        auto changed = mem;
        changed[kSecretKey] = "rotated";
        changed["hook.on_publish"] = "http://other/on_publish";
        changed["rtc.port"] = "9000";
        auto saved = mergeIniForSave(changed, disk, cli("http://ctl", "cli-id", "cli-secret"));
        expectEq("rotated secret", saved[kSecretKey], "rotated");
        expectEq("changed hook", saved["hook.on_publish"], "http://other/on_publish");
        expectEq("changed key", saved["rtc.port"], "9000");
        expectEq("server id still kept", saved[kServerIdKey], "file-id");
    }

    // A key the file does not have and that still holds the command-line value is left out.
    {
        mINI partial = disk;
        partial.erase(kSecretKey);
        auto saved = mergeIniForSave(mem, partial, cli("", "", "cli-secret"));
        expectTrue("absent secret erased", saved.find(kSecretKey) == saved.end());
        // Without --media-server-id and --hook-base-url their in-memory values are saved as is.
        expectEq("no server id override", saved[kServerIdKey], "cli-id");
        expectEq("no hook override", saved["hook.on_publish"], "http://ctl/on_publish");
    }

    // No command-line overrides: everything is saved as is.
    {
        auto saved = mergeIniForSave(mem, disk, cli("", "", ""));
        expectEq("plain secret", saved[kSecretKey], "cli-secret");
        expectTrue("plain size", saved.size() == mem.size());
    }

    // replaceHookBaseUrlIn: every placeholder, trailing slashes removed, nothing to do otherwise.
    {
        string value = "__HOOK_BASE_URL__/a,__HOOK_BASE_URL__/b";
        expectTrue("replaced", replaceHookBaseUrlIn(value, "http://h//"));
        expectEq("replaced value", value, "http://h/a,http://h/b");
        string plain = "http://x/a";
        expectTrue("no placeholder", !replaceHookBaseUrlIn(plain, "http://h"));
        string empty_base = "__HOOK_BASE_URL__/a";
        expectTrue("empty base", !replaceHookBaseUrlIn(empty_base, ""));
    }

    if (s_failed) {
        cerr << s_failed << " check(s) failed" << endl;
        return EXIT_FAILURE;
    }
    cout << "test_ini_save: all checks passed" << endl;
    return EXIT_SUCCESS;
}
