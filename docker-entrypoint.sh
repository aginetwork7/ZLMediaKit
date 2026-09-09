#!/bin/sh
set -eu

: "${MEDIA_SERVICE_HOOK_BASE_URL:?MEDIA_SERVICE_HOOK_BASE_URL is required}"
: "${MEDIA_SERVER_SECRET:?MEDIA_SERVER_SECRET is required}"

media_config_type=${MEDIA_CONFIG_TYPE:-media}
case "$media_config_type" in
  media|sfu)
    config_file="/opt/media/conf/config.${media_config_type}.ini"
    ;;
  *)
    echo "unsupported MEDIA_CONFIG_TYPE: $media_config_type" >&2
    exit 1
    ;;
esac

HOOK_BASE_URL=${MEDIA_SERVICE_HOOK_BASE_URL%/}

if [ -n "${MEDIA_SERVER_ID:-}" ]; then
  set -- --media-server-id "$MEDIA_SERVER_ID"
fi

exec /opt/media/bin/MediaServer \
  -s /opt/media/bin/default.pem \
  -c "$config_file" \
  --hook-base-url "$HOOK_BASE_URL" \
  --secret "$MEDIA_SERVER_SECRET" \
  --log-dir /opt/media/bin/log \
  -l 0 \
  "$@"