#!/usr/bin/env bash

set -euo pipefail

usage() {
    cat <<'EOF'
Usage: record_zlm_resources.sh --pid <MediaServer PID> --stream-count <clients> --network-interface <name> [--interval <seconds>]

Records RTSP replay benchmark metrics for the target process. The stream count
must match test_bench_replay --count. CSV files are always written to
/opt/media/bin/log.
EOF
}

pid=""
log_dir="/opt/media/bin/log"
interval=1
client_type="rtsp_replay"
stream_count=""
network_interface=""

while [[ $# -gt 0 ]]; do
    case "$1" in
        --pid)
            pid="${2:-}"
            shift 2
            ;;
        --interval)
            interval="${2:-}"
            shift 2
            ;;
        --stream-count)
            stream_count="${2:-}"
            shift 2
            ;;
        --network-interface)
            network_interface="${2:-}"
            shift 2
            ;;
        -h|--help)
            usage
            exit 0
            ;;
        *)
            echo "Unknown argument: $1" >&2
            usage >&2
            exit 2
            ;;
    esac
done

if [[ ! "$pid" =~ ^[1-9][0-9]*$ ]] || [[ ! "$stream_count" =~ ^[1-9][0-9]*$ ]] || [[ -z "$network_interface" ]]; then
    usage >&2
    exit 2
fi

if [[ ! "$interval" =~ ^[1-9][0-9]*$ ]]; then
    echo "--interval must be a positive integer in seconds" >&2
    exit 2
fi

if ! kill -0 "$pid" 2>/dev/null; then
    echo "Process $pid is not running or cannot be inspected" >&2
    exit 1
fi

mkdir -p "$log_dir"
output_file="$log_dir/zlm-resource-$(date +%Y%m%d-%H%M%S).csv"
printf '本地时间,时间戳,客户端类型,流个数,cpu(%%),内存(MB),网络io(MB/s)\n' > "$output_file"
echo "Writing ZLM resource samples to $output_file"

read_network_bytes() {
    local rx_bytes tx_bytes
    case "$(uname -s)" in
        Linux)
            rx_bytes="$(cat "/sys/class/net/$network_interface/statistics/rx_bytes")"
            tx_bytes="$(cat "/sys/class/net/$network_interface/statistics/tx_bytes")"
            ;;
        Darwin)
            read -r rx_bytes tx_bytes < <(netstat -ibn -I "$network_interface" | awk 'NR > 1 { rx = $7; tx = $10 } END { print rx, tx }')
            ;;
    esac
    printf '%s %s\n' "$rx_bytes" "$tx_bytes"
}

append_sample() {
    local now_seconds cpu_pct rss_mb rx_bytes tx_bytes network_io_mb_s
    now_seconds="$1"
    cpu_pct="$2"
    rss_mb="$3"
    read -r rx_bytes tx_bytes < <(read_network_bytes)

    network_io_mb_s="0.00"
    if [[ -n "${previous_network_rx_bytes:-}" ]] && (( now_seconds > previous_network_seconds )); then
        local elapsed_seconds=$((now_seconds - previous_network_seconds))
        network_io_mb_s="$(awk -v rx_delta="$((rx_bytes - previous_network_rx_bytes))" \
            -v tx_delta="$((tx_bytes - previous_network_tx_bytes))" -v elapsed="$elapsed_seconds" \
            'BEGIN { printf "%.2f", (rx_delta + tx_delta) / 1024 / 1024 / elapsed }')"
    fi

    previous_network_rx_bytes="$rx_bytes"
    previous_network_tx_bytes="$tx_bytes"
    previous_network_seconds="$now_seconds"
    printf '%s,%s,%s,%s,%s,%s,%s\n' "$(date +%Y-%m-%dT%H:%M:%S%z)" "$now_seconds" "$client_type" "$stream_count" "$cpu_pct" "$rss_mb" "$network_io_mb_s" >> "$output_file"
}

sample_linux() {
    local cpu_ticks resident_pages now_seconds elapsed_seconds cpu_pct rss_mb
    read -r cpu_ticks resident_pages < <(awk '{print $14 + $15, $24}' "/proc/$pid/stat")
    now_seconds="$(date +%s)"

    if [[ -n "${previous_cpu_ticks:-}" ]] && (( now_seconds > previous_seconds )); then
        elapsed_seconds=$((now_seconds - previous_seconds))
        cpu_pct="$(awk -v ticks_delta="$((cpu_ticks - previous_cpu_ticks))" \
            -v ticks_per_second="$ticks_per_second" -v elapsed="$elapsed_seconds" \
            'BEGIN { printf "%.2f", ticks_delta * 100 / ticks_per_second / elapsed }')"
    else
        cpu_pct="0.00"
    fi

    rss_mb="$(awk -v pages="$resident_pages" -v page_size="$page_size" \
        'BEGIN { printf "%.2f", pages * page_size / 1024 / 1024 }')"
    previous_cpu_ticks="$cpu_ticks"
    previous_seconds="$now_seconds"
    append_sample "$now_seconds" "$cpu_pct" "$rss_mb"
}

sample_macos() {
    local now_seconds cpu_pct rss_kb rss_mb
    now_seconds="$(date +%s)"
    read -r cpu_pct rss_kb < <(LC_ALL=C ps -p "$pid" -o %cpu= -o rss=)
    rss_mb="$(awk -v rss_kb="$rss_kb" 'BEGIN { printf "%.2f", rss_kb / 1024 }')"
    append_sample "$now_seconds" "$cpu_pct" "$rss_mb"
}

case "$(uname -s)" in
    Linux)
        ticks_per_second="$(getconf CLK_TCK)"
        page_size="$(getconf PAGESIZE)"
        if [[ ! -r "/sys/class/net/$network_interface/statistics/rx_bytes" ]]; then
            echo "Network interface $network_interface cannot be read" >&2
            exit 1
        fi
        ;;
    Darwin)
        if ! netstat -ibn -I "$network_interface" >/dev/null 2>&1; then
            echo "Network interface $network_interface cannot be read" >&2
            exit 1
        fi
        ;;
    *)
        echo "Unsupported platform: $(uname -s)" >&2
        exit 1
        ;;
esac

trap 'echo "Stopped resource recording: $output_file"; exit 0' INT TERM
while kill -0 "$pid" 2>/dev/null; do
    case "$(uname -s)" in
        Linux) sample_linux ;;
        Darwin) sample_macos ;;
    esac
    sleep "$interval"
done

echo "Process $pid exited; resource recording stopped: $output_file"