#!/bin/sh
set -eu

: "${MEDIA_SERVICE_HOOK_BASE_URL:?MEDIA_SERVICE_HOOK_BASE_URL is required}"
: "${MEDIA_SERVER_SECRET:?MEDIA_SERVER_SECRET is required}"
# CHANGE_ME is the placeholder in the reference compose/env templates; a real secret must replace it.
if [ "$MEDIA_SERVER_SECRET" = "CHANGE_ME" ]; then
  echo "MEDIA_SERVER_SECRET is still the CHANGE_ME placeholder" >&2
  exit 1
fi

media_config_type=${MEDIA_CONFIG_TYPE:-media}
case "$media_config_type" in
  media)
    config_file="/opt/media/conf/config.${media_config_type}.ini"
    ;;
  sfu)
    config_file="/opt/media/conf/config.${media_config_type}.ini"
    # config.sfu.ini 里 externIP=$EXTERN_IP，缺失时 ZLM 会静默退化成容器内网 IP，
    # 导致下发给客户端的 ICE candidate 不可达，故此处强制校验。
    : "${EXTERN_IP:?EXTERN_IP is required when MEDIA_CONFIG_TYPE=sfu}"
    # SFU 节点以 MEDIA_SERVER_ID 向控制面注册，缺失时会以配置文件里的默认 id 启动，多个节点无法区分。
    : "${MEDIA_SERVER_ID:?MEDIA_SERVER_ID is required when MEDIA_CONFIG_TYPE=sfu}"
    ;;
  *)
    echo "unsupported MEDIA_CONFIG_TYPE: $media_config_type" >&2
    exit 1
    ;;
esac

HOOK_BASE_URL=${MEDIA_SERVICE_HOOK_BASE_URL%/}

# 改写容器内的配置副本（保留文件 inode，兼容被挂载的配置文件）。写回前先备份；写回失败时恢复备份并退出，
# 不会留下被截断的配置让下次重启（restart: always）用残缺配置启动。
# Rewrites the container's config copy in place (keeps the inode, so a mounted config file also works).
# The file is backed up first. When writing it back fails (e.g. a full disk), the backup is restored and the
# start fails, so a restart (restart: always) never runs MediaServer on a truncated config.
rewrite_config() {
  backup_file=$(mktemp)
  tmp_file=$(mktemp)
  cat "$config_file" > "$backup_file"
  if ! "$@" "$config_file" > "$tmp_file" || [ ! -s "$tmp_file" ]; then
    rm -f "$tmp_file" "$backup_file"
    echo "rewrite of $config_file failed, config left unchanged" >&2
    exit 1
  fi
  if ! cat "$tmp_file" > "$config_file"; then
    if cat "$backup_file" > "$config_file"; then
      echo "rewrite of $config_file failed, original config restored" >&2
    else
      echo "rewrite of $config_file failed and the original config could not be restored" >&2
    fi
    rm -f "$tmp_file" "$backup_file"
    exit 1
  fi
  rm -f "$tmp_file" "$backup_file"
}

# secret 写入 [api] secret，不再经命令行参数传入：命令行参数对宿主机上的所有用户可见（/proc/<pid>/cmdline）。
# The secret goes into [api] secret instead of a command line argument, which every host user can read
# from /proc/<pid>/cmdline. awk reads it from the environment, so any character is written verbatim.
# The ini parser trims values and splits lines, so a secret with surrounding whitespace or a line break keeps
# the previous --secret argument and its exact value.
secret_trimmed=$(printf '%s' "$MEDIA_SERVER_SECRET" | awk 'NR > 1 { exit 1 } { gsub(/^[ \t]+|[ \t]+$/, ""); printf "%s", $0 }') || secret_trimmed=""
if [ "$secret_trimmed" = "$MEDIA_SERVER_SECRET" ]; then
  rewrite_config awk '
    /^\[/ { section = $0 }
    section == "[api]" && /^secret=/ { print "secret=" ENVIRON["MEDIA_SERVER_SECRET"]; next }
    { print }
  '
else
  echo "MEDIA_SERVER_SECRET has surrounding whitespace or a line break; passing it with --secret" >&2
  set -- "$@" --secret "$MEDIA_SERVER_SECRET"
fi

# MEDIA_ALLOW_IP_RANGE 覆盖 [http] allow_ip_range；未设置或为空时保留配置文件默认值。
# 格式同配置文件：逗号分隔，单个 IP 或 起始-结束，不支持 CIDR。
if [ -n "${MEDIA_ALLOW_IP_RANGE:-}" ]; then
  allow_ip_range=$(printf '%s' "$MEDIA_ALLOW_IP_RANGE" | tr -d ' ')
  case "$allow_ip_range" in
    *[!0-9A-Fa-f.:,-]*)
      echo "invalid MEDIA_ALLOW_IP_RANGE: $MEDIA_ALLOW_IP_RANGE (expected comma-separated IPs or start-end ranges, e.g. 127.0.0.1,10.0.0.0-10.255.255.255; CIDR is not supported)" >&2
      exit 1
      ;;
  esac
  rewrite_config sed "s|^allow_ip_range=.*|allow_ip_range=${allow_ip_range}|"
fi

if [ -n "${MEDIA_SERVER_ID:-}" ]; then
  set -- "$@" --media-server-id "$MEDIA_SERVER_ID"
fi

exec /opt/media/bin/MediaServer \
  -s /opt/media/bin/default.pem \
  -c "$config_file" \
  --hook-base-url "$HOOK_BASE_URL" \
  --log-dir /opt/media/bin/log \
  -l 1 \
  "$@"