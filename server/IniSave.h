/*
 * Decides what saveIniFile writes back to the config file when values came from the command line.
 */

#ifndef ZLMEDIAKIT_INISAVE_H
#define ZLMEDIAKIT_INISAVE_H

#include <string>
#include "Util/mini.h"

namespace mediakit {

static const std::string kHookBaseUrlPlaceholder = "__HOOK_BASE_URL__";

// Replaces every placeholder in value with hook_base_url (trailing '/' removed); returns false when
// there is nothing to replace.
inline bool replaceHookBaseUrlIn(std::string &value, std::string hook_base_url) {
    if (hook_base_url.empty() || value.find(kHookBaseUrlPlaceholder) == std::string::npos) {
        return false;
    }
    while (hook_base_url.size() > 1 && hook_base_url.back() == '/') {
        hook_base_url.pop_back();
    }
    size_t pos = 0;
    while ((pos = value.find(kHookBaseUrlPlaceholder, pos)) != std::string::npos) {
        value.replace(pos, kHookBaseUrlPlaceholder.size(), hook_base_url);
        pos += hook_base_url.size();
    }
    return true;
}

// Values given on the command line (empty when not given) and the config keys they override.
struct CliOverrides {
    std::string hook_base_url;
    std::string media_server_id;
    std::string api_secret;
    std::string media_server_id_key;
    std::string api_secret_key;

    bool empty() const { return hook_base_url.empty() && media_server_id.empty() && api_secret.empty(); }
};

// The config to write to the file: the in-memory config, except that a key still holding its
// command-line value gets the file's own value back (or is left out when the file does not have it),
// so a --secret, --media-server-id or resolved --hook-base-url never lands in the file. A value set
// through setServerConfig since startup differs from the command-line value and is saved as is.
inline toolkit::mINI mergeIniForSave(const toolkit::mINI &in_memory, const toolkit::mINI &on_disk, const CliOverrides &cli) {
    auto to_save = in_memory;
    if (cli.empty()) {
        return to_save;
    }
    for (auto it = to_save.begin(); it != to_save.end();) {
        auto file_it = on_disk.find(it->first);
        // The value the command line gave this key, or empty when it does not override it.
        std::string cli_value;
        if (!cli.media_server_id.empty() && it->first == cli.media_server_id_key) {
            cli_value = cli.media_server_id;
        } else if (!cli.api_secret.empty() && it->first == cli.api_secret_key) {
            cli_value = cli.api_secret;
        } else if (file_it != on_disk.end()) {
            std::string resolved = file_it->second;
            if (replaceHookBaseUrlIn(resolved, cli.hook_base_url)) {
                cli_value = resolved;
            }
        }
        if (cli_value.empty() || it->second != cli_value) {
            ++it;
        } else if (file_it != on_disk.end()) {
            it->second = file_it->second;
            ++it;
        } else {
            it = to_save.erase(it);
        }
    }
    return to_save;
}

} // namespace mediakit

#endif // ZLMEDIAKIT_INISAVE_H
