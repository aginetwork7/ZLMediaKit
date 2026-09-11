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