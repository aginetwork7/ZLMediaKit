#!/usr/bin/env bash

set -euo pipefail

usage() {
    cat <<'EOF'
Usage:
    bench_seed_zlm_proxies.sh seed --zlm-api <url> --source-base <rtsp-url> --device-count <count> [--secret <secret>|--secret-file <path>] [--source-user <user>] [--source-password <password>|--source-auth-file <path>] [--replace] [--request-delay-ms <ms>] [--request-retries <count>]
  bench_seed_zlm_proxies.sh replay-list --replay-base <rtsp-url> --device-count <count> --begin <unix-sec> --end <unix-sec> --output <file>
  bench_seed_zlm_proxies.sh clean --zlm-api <url> --device-count <count> [--secret <secret>]

seed creates two proxy recordings per virtual device:
  bench/<device-index>/s0 <- <source-base>_main_<device-index>
  bench/<device-index>/s1 <- <source-base>_sub_<device-index>
EOF
}

if [[ $# -lt 1 ]]; then
    usage >&2
    exit 2
fi

case "$1" in
    -h|--help)
        usage
        exit 0
        ;;
esac

mode="$1"
shift

zlm_api=""
source_base=""
source_user=""
source_password=""
source_auth_file="${ZLM_SOURCE_AUTH_FILE:-/opt/media/auth/shared-config.json}"
replay_base=""
secret="${ZLM_SECRET:-}"
script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
secret_file="${ZLM_SECRET_FILE:-${script_dir}/../conf/config.ini}"
device_count=""
begin=""
end=""
output=""
replace=0
request_delay_ms=50
request_retries=5

while [[ $# -gt 0 ]]; do
    case "$1" in
        --zlm-api) zlm_api="${2:-}"; shift 2 ;;
        --source-base) source_base="${2:-}"; shift 2 ;;
        --source-user) source_user="${2:-}"; shift 2 ;;
        --source-password) source_password="${2:-}"; shift 2 ;;
        --source-auth-file) source_auth_file="${2:-}"; shift 2 ;;
        --secret-file) secret_file="${2:-}"; shift 2 ;;
        --replay-base) replay_base="${2:-}"; shift 2 ;;
        --secret) secret="${2:-}"; shift 2 ;;
        --device-count) device_count="${2:-}"; shift 2 ;;
        --begin) begin="${2:-}"; shift 2 ;;
        --end) end="${2:-}"; shift 2 ;;
        --output) output="${2:-}"; shift 2 ;;
        --replace) replace=1; shift ;;
        --request-delay-ms) request_delay_ms="${2:-}"; shift 2 ;;
        --request-retries) request_retries="${2:-}"; shift 2 ;;
        -h|--help) usage; exit 0 ;;
        *) echo "Unknown argument: $1" >&2; usage >&2; exit 2 ;;
    esac
done

if [[ ! "$device_count" =~ ^[1-9][0-9]*$ ]] || [[ ! "$request_delay_ms" =~ ^[0-9]+$ ]] || [[ ! "$request_retries" =~ ^[0-9]+$ ]]; then
    echo "--device-count must be positive; --request-delay-ms and --request-retries must be non-negative integers" >&2
    exit 2
fi

read_secret() {
    if [[ -n "$secret" ]]; then
        return
    fi
    if [[ -r "$secret_file" ]]; then
        secret="$(awk -F= '$1 == "secret" { print substr($0, index($0, "=") + 1); exit }' "$secret_file")"
        if [[ -n "$secret" ]]; then
            return
        fi
    fi
    if [[ ! -t 0 ]]; then
        echo "--secret or ZLM_SECRET is required when no interactive terminal is available" >&2
        exit 2
    fi
    read -r -s -p "ZLM API secret: " secret
    printf '\n'
}

read_source_password() {
    if [[ -r "$source_auth_file" ]]; then
        if [[ -z "$source_user" ]]; then
            source_user="$(jq -r '.username // empty' "$source_auth_file")"
        fi
        if [[ -z "$source_password" ]]; then
            source_password="$(jq -r '.password // empty' "$source_auth_file")"
        fi
    fi
    if [[ -z "$source_user" ]]; then
        if [[ -n "$source_password" ]]; then
            echo "--source-password requires --source-user" >&2
            exit 2
        fi
        return
    fi
    if [[ -n "$source_password" ]]; then
        return
    fi
    if [[ ! -t 0 ]]; then
        echo "--source-password is required when no interactive terminal is available" >&2
        exit 2
    fi
    read -r -s -p "Source RTSP password: " source_password
    printf '\n'
}

source_url_for() {
    local profile="$1"
    local device_index="$2"
    if [[ -z "$source_user" ]]; then
        printf '%s_%s_%s' "$source_base" "$profile" "$device_index"
        return
    fi
    local scheme="${source_base%%://*}"
    local authority_and_path="${source_base#*://}"
    printf '%s://%s:%s@%s_%s_%s' "$scheme" "$source_user" "$source_password" "$authority_and_path" "$profile" "$device_index"
}

call_zlm() {
    local endpoint="$1"
    shift
    curl --fail-with-body --silent --show-error --get "${zlm_api%/}/index/api/${endpoint}" \
        --data-urlencode "secret=${secret}" "$@"
}

call_zlm_with_retry() {
    local attempt=0
    local response
    until response="$(call_zlm "$@")"; do
        if (( attempt >= request_retries )); then
            printf '%s' "$response"
            return 1
        fi
        attempt=$((attempt + 1))
        sleep 1
    done
    printf '%s' "$response"
}

case "$mode" in
    seed)
        if [[ -z "$zlm_api" || -z "$source_base" ]]; then
            echo "seed requires --zlm-api and --source-base" >&2
            exit 2
        fi
        read_secret
        read_source_password
        for ((device_index = 0; device_index < device_count; ++device_index)); do
            for stream_type in s0 s1; do
                if [[ "$stream_type" == "s0" ]]; then
                    source_url="$(source_url_for main "$device_index")"
                else
                    source_url="$(source_url_for sub "$device_index")"
                fi
                stream="bench/${device_index}/${stream_type}"
                if (( replace )); then
                    key="__defaultVhost__/live/${stream}"
                    call_zlm_with_retry delStreamProxy --data-urlencode "key=${key}" >/dev/null
                fi
                if ! response="$(call_zlm_with_retry addStreamProxy \
                    --data-urlencode 'vhost=__defaultVhost__' \
                    --data-urlencode 'app=live' \
                    --data-urlencode "stream=${stream}" \
                    --data-urlencode "url=${source_url}" \
                    --data-urlencode 'enable_rtsp=1' \
                    --data-urlencode 'enable_mp4=1' \
                    --data-urlencode 'enable_hls=0' \
                    --data-urlencode 'enable_hls_fmp4=0' \
                    --data-urlencode 'enable_rtmp=0' \
                    --data-urlencode 'enable_ts=0' \
                    --data-urlencode 'enable_fmp4=0' \
                    --data-urlencode 'rtp_type=0' \
                    --data-urlencode 'force=1')"; then
                    echo "ZLM failed to add ${stream} from ${source_url}: ${response}" >&2
                    exit 1
                fi
                if ! grep -Eq '"code"[[:space:]]*:[[:space:]]*0|"msg"[[:space:]]*:[[:space:]]*"This stream already exists"' <<<"$response"; then
                    echo "ZLM rejected ${stream} from ${source_url}: ${response}" >&2
                    exit 1
                fi
                if (( request_delay_ms > 0 )); then
                    sleep "$(awk -v milliseconds="$request_delay_ms" 'BEGIN { printf "%.3f", milliseconds / 1000 }')"
                fi
            done
            printf 'seeded devices: %d/%d\n' "$((device_index + 1))" "$device_count"
        done
        ;;
    replay-list)
        if [[ -z "$replay_base" || -z "$begin" || -z "$end" || -z "$output" ]]; then
            echo "replay-list requires --replay-base, --begin, --end, and --output" >&2
            exit 2
        fi
        if [[ ! "$begin" =~ ^[1-9][0-9]*$ ]] || [[ ! "$end" =~ ^[1-9][0-9]*$ ]] || (( end <= begin )); then
            echo "--begin and --end must be Unix seconds with end greater than begin" >&2
            exit 2
        fi
        : > "$output"
        for ((device_index = 0; device_index < device_count; ++device_index)); do
            printf '%s/replay/bench/%s/s0/b%s/e%s\n' "${replay_base%/}" "$device_index" "$begin" "$end" >> "$output"
            printf '%s/replay/bench/%s/s1/b%s/e%s\n' "${replay_base%/}" "$device_index" "$begin" "$end" >> "$output"
        done
        printf 'wrote %d replay URLs to %s\n' "$((device_count * 2))" "$output"
        ;;
    clean)
        if [[ -z "$zlm_api" ]]; then
            echo "clean requires --zlm-api" >&2
            exit 2
        fi
        read_secret
        for ((device_index = 0; device_index < device_count; ++device_index)); do
            for stream_type in s0 s1; do
                key="__defaultVhost__/live/bench/${device_index}/${stream_type}"
                call_zlm delStreamProxy --data-urlencode "key=${key}" >/dev/null
            done
            printf 'removed devices: %d/%d\n' "$((device_index + 1))" "$device_count"
        done
        ;;
    *)
        echo "mode must be seed, replay-list, or clean" >&2
        usage >&2
        exit 2
        ;;
esac