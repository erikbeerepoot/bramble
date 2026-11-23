#!/usr/bin/env bash
# Reconnects the hub's WiFi when the Cloudflare tunnel has been down for a while.
#
# The hub has been seen to stay up but lose its route to the Cloudflare edge
# (stuck on a bad access point), which previously needed a power cycle. This
# runs once a minute from bramble-tunnel-watchdog.timer, asks cloudflared's
# readiness endpoint whether any edge connection is alive, and after enough
# consecutive failures forces a fresh WiFi association.
#
# Settings can be overridden in /etc/default/bramble-tunnel-watchdog.

set -u

TUNNEL_CONTAINER="${TUNNEL_CONTAINER:-cloudflared}"
WIFI_INTERFACE="${WIFI_INTERFACE:-wlan0}"
FAILURES_BEFORE_RECONNECT="${FAILURES_BEFORE_RECONNECT:-5}"
# cloudflared picks the first free port in this range for its metrics server.
METRICS_PORTS="${METRICS_PORTS:-20241 20242 20243 20244 20245}"
FAILURE_COUNT_FILE="${RUNTIME_DIRECTORY:-/run/bramble-tunnel-watchdog}/failures"

tunnel_container_address() {
    docker inspect "$TUNNEL_CONTAINER" \
        --format '{{if .State.Running}}{{range .NetworkSettings.Networks}}{{.IPAddress}}{{end}}{{end}}' 2>/dev/null
}

tunnel_is_ready() {
    local address="$1"
    local port
    for port in $METRICS_PORTS; do
        # /ready returns 200 only while at least one edge connection is registered.
        if curl --silent --fail --max-time 5 --output /dev/null "http://${address}:${port}/ready"; then
            return 0
        fi
    done
    return 1
}

reconnect_wifi() {
    local connection
    connection="$(nmcli --get-values GENERAL.CONNECTION device show "$WIFI_INTERFACE")"

    nmcli device disconnect "$WIFI_INTERFACE"
    if nmcli device connect "$WIFI_INTERFACE"; then
        return 0
    fi

    # Could not associate at all. If the profile is pinned to one access point
    # and that access point is gone, the pin would keep the hub offline forever,
    # so drop it and let NetworkManager pick any access point for the network.
    local pinned_access_point
    pinned_access_point="$(nmcli --get-values 802-11-wireless.bssid connection show "$connection")"
    if [ -n "$connection" ] && [ -n "$pinned_access_point" ]; then
        echo "Cannot reach pinned access point ${pinned_access_point}; removing pin from '${connection}'"
        nmcli connection modify "$connection" 802-11-wireless.bssid ""
        nmcli connection up "$connection"
    fi
}

address="$(tunnel_container_address)"
if [ -z "$address" ]; then
    # Container stopped or being replaced: not a network problem, leave WiFi alone.
    echo "Tunnel container '${TUNNEL_CONTAINER}' is not running; skipping"
    exit 0
fi

if tunnel_is_ready "$address"; then
    rm -f "$FAILURE_COUNT_FILE"
    exit 0
fi

failures=$(( $(cat "$FAILURE_COUNT_FILE" 2>/dev/null || echo 0) + 1 ))
echo "Tunnel not ready (${failures}/${FAILURES_BEFORE_RECONNECT})"

if [ "$failures" -lt "$FAILURES_BEFORE_RECONNECT" ]; then
    echo "$failures" > "$FAILURE_COUNT_FILE"
    exit 0
fi

echo "Tunnel down for ${failures} checks; reconnecting ${WIFI_INTERFACE}"
rm -f "$FAILURE_COUNT_FILE"
reconnect_wifi
