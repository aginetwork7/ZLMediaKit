-- The window lengths must match the tumbling windows of the host.netif.1m / host.netif.3m streams
-- created in docker-compose.sfu.yml (fluent-bit command); change both together.
function add_bps(tag, timestamp, record)
    local window_seconds

    if tag == "host.netif.1m" then
        window_seconds = 60
    elseif tag == "host.netif.3m" then
        window_seconds = 180
    else
        return 0, timestamp, record
    end

    record["rx_bps"] = record["rx_bytes"] * 8.0 / window_seconds
    record["tx_bps"] = record["tx_bytes"] * 8.0 / window_seconds

    return 1, timestamp, record
end
