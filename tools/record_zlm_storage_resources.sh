#!/usr/bin/env bash

set -euo pipefail

usage() {
    cat <<'EOF'
Usage: record_zlm_storage_resources.sh --pid <MediaServer PID|auto> --stream-count <clients> --block-device <device> [--interval <seconds>] [--log-dir <path>]

Run this on the Linux host that owns the ZLM container. --block-device is the
whole block device backing mp4_save_path, for example nvme0n1 or sdb.
EOF
}

pid=""
stream_count=""
block_device=""
interval=1
log_dir="./log"

while [[ $# -gt 0 ]]; do
    case "$1" in
        --pid) pid="${2:-}"; shift 2 ;;
        --stream-count) stream_count="${2:-}"; shift 2 ;;
        --block-device) block_device="${2:-}"; shift 2 ;;
        --interval) interval="${2:-}"; shift 2 ;;
        --log-dir) log_dir="${2:-}"; shift 2 ;;
        -h|--help) usage; exit 0 ;;
        *) echo "Unknown argument: $1" >&2; usage >&2; exit 2 ;;
    esac
done

if [[ "$(uname -s)" != "Linux" ]]; then
    echo "This collector runs only on the Linux ZLM host" >&2
    exit 2
fi
if [[ -z "$pid" || ! "$stream_count" =~ ^[1-9][0-9]*$ || -z "$block_device" || ! "$interval" =~ ^[1-9][0-9]*$ ]]; then
    usage >&2
    exit 2
fi
if [[ "$pid" == "auto" ]]; then
    pid="$(pgrep -n MediaServer || true)"
fi
if [[ ! "$pid" =~ ^[1-9][0-9]*$ ]] || ! kill -0 "$pid" 2>/dev/null; then
    echo "MediaServer PID is not running or cannot be inspected" >&2
    exit 1
fi
if [[ ! -r "/sys/class/block/$block_device/stat" ]]; then
    echo "Cannot read /sys/class/block/$block_device/stat" >&2
    exit 1
fi

mkdir -p "$log_dir"
output_file="$log_dir/zlm-storage-resource-$(date +%Y%m%d-%H%M%S).csv"
header='record_type,timestamp,stream_count,cpu_pct,rss_mb,proc_read_mb_s,proc_write_mb_s,disk_read_mb_s,disk_write_mb_s,disk_read_iops,disk_write_iops'
printf '%s\n' "$header" > "$output_file"
printf '%s\n' "$header"
echo "Writing samples to $output_file"

read_proc_bytes() {
    awk '/^read_bytes:/ { read_bytes = $2 } /^write_bytes:/ { write_bytes = $2 } END { print read_bytes + 0, write_bytes + 0 }' "/proc/$pid/io"
}

read_disk_stats() {
    local reads sectors_read writes sectors_written
    read -r reads _ sectors_read _ writes _ sectors_written _ < "/sys/class/block/$block_device/stat"
    printf '%s %s %s %s\n' "$reads" "$sectors_read" "$writes" "$sectors_written"
}

previous_cpu_ticks=0
previous_proc_read=0
previous_proc_write=0
previous_disk_reads=0
previous_disk_read_sectors=0
previous_disk_writes=0
previous_disk_write_sectors=0
previous_timestamp=0
ticks_per_second="$(getconf CLK_TCK)"
page_size="$(getconf PAGESIZE)"

while kill -0 "$pid" 2>/dev/null; do
    now="$(date +%s)"
    read -r cpu_ticks resident_pages < <(awk '{print $14 + $15, $24}' "/proc/$pid/stat")
    read -r proc_read proc_write < <(read_proc_bytes)
    read -r disk_reads disk_read_sectors disk_writes disk_write_sectors < <(read_disk_stats)

    cpu_pct=0
    rss_mb="$(awk -v pages="$resident_pages" -v page_size="$page_size" 'BEGIN { printf "%.2f", pages * page_size / 1024 / 1024 }')"
    proc_read_mb_s=0
    proc_write_mb_s=0
    disk_read_mb_s=0
    disk_write_mb_s=0
    disk_read_iops=0
    disk_write_iops=0
    if (( previous_timestamp > 0 && now > previous_timestamp )); then
        elapsed=$((now - previous_timestamp))
        cpu_pct="$(awk -v delta="$((cpu_ticks - previous_cpu_ticks))" -v hz="$ticks_per_second" -v elapsed="$elapsed" 'BEGIN { printf "%.2f", delta * 100 / hz / elapsed }')"
        proc_read_mb_s="$(awk -v delta="$((proc_read - previous_proc_read))" -v elapsed="$elapsed" 'BEGIN { printf "%.2f", delta / 1024 / 1024 / elapsed }')"
        proc_write_mb_s="$(awk -v delta="$((proc_write - previous_proc_write))" -v elapsed="$elapsed" 'BEGIN { printf "%.2f", delta / 1024 / 1024 / elapsed }')"
        disk_read_mb_s="$(awk -v delta="$((disk_read_sectors - previous_disk_read_sectors))" -v elapsed="$elapsed" 'BEGIN { printf "%.2f", delta * 512 / 1024 / 1024 / elapsed }')"
        disk_write_mb_s="$(awk -v delta="$((disk_write_sectors - previous_disk_write_sectors))" -v elapsed="$elapsed" 'BEGIN { printf "%.2f", delta * 512 / 1024 / 1024 / elapsed }')"
        disk_read_iops="$(awk -v delta="$((disk_reads - previous_disk_reads))" -v elapsed="$elapsed" 'BEGIN { printf "%.2f", delta / elapsed }')"
        disk_write_iops="$(awk -v delta="$((disk_writes - previous_disk_writes))" -v elapsed="$elapsed" 'BEGIN { printf "%.2f", delta / elapsed }')"
    fi

    printf 'sample,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s\n' \
        "$now" "$stream_count" "$cpu_pct" "$rss_mb" "$proc_read_mb_s" "$proc_write_mb_s" \
        "$disk_read_mb_s" "$disk_write_mb_s" "$disk_read_iops" "$disk_write_iops" | tee -a "$output_file"

    previous_timestamp="$now"
    previous_cpu_ticks="$cpu_ticks"
    previous_proc_read="$proc_read"
    previous_proc_write="$proc_write"
    previous_disk_reads="$disk_reads"
    previous_disk_read_sectors="$disk_read_sectors"
    previous_disk_writes="$disk_writes"
    previous_disk_write_sectors="$disk_write_sectors"
    sleep "$interval"
done