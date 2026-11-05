#!/bin/sh
set -eu

: "${MEDIA_SERVICE_HOOK_BASE_URL:?MEDIA_SERVICE_HOOK_BASE_URL is required}"
: "${MEDIA_SERVER_SECRET:?MEDIA_SERVER_SECRET is required}"

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
    ;;
  *)
    echo "unsupported MEDIA_CONFIG_TYPE: $media_config_type" >&2
    exit 1
    ;;
esac

HOOK_BASE_URL=${MEDIA_SERVICE_HOOK_BASE_URL%/}

# MEDIA_ALLOW_IP_RANGE 覆盖 [http] allow_ip_range；未设置或为空时保留配置文件默认值。
# 格式同配置文件：逗号分隔，单个 IP 或 起始-结束，不支持 CIDR。
if [ -n "${MEDIA_ALLOW_IP_RANGE:-}" ]; then
  allow_ip_range=$(printf '%s' "$MEDIA_ALLOW_IP_RANGE" | tr -d ' ')
  case "$allow_ip_range" in
    *[!0-9A-Fa-f.:,-]*)
      echo "invalid MEDIA_ALLOW_IP_RANGE: $MEDIA_ALLOW_IP_RANGE" >&2
      exit 1
      ;;
  esac
  tmp_file=$(mktemp)
  sed "s|^allow_ip_range=.*|allow_ip_range=${allow_ip_range}|" "$config_file" > "$tmp_file"
  cat "$tmp_file" > "$config_file"
  rm -f "$tmp_file"
fi

if [ -n "${MEDIA_SERVER_ID:-}" ]; then
  set -- "$@" --media-server-id "$MEDIA_SERVER_ID"
fi

exec /opt/media/bin/MediaServer \
  -s /opt/media/bin/default.pem \
  -c "$config_file" \
  --hook-base-url "$HOOK_BASE_URL" \
  --secret "$MEDIA_SERVER_SECRET" \
  --log-dir /opt/media/bin/log \
  -l 0 \
  "$@"