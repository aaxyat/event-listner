#!/usr/bin/env bash
# gluetun-watchdog.sh — Re-resolves Cloudflare Anycast endpoint and restores VPN
set -euo pipefail

COMPOSE_FILE="/docker/compose/gluetun/docker-compose.yml"
WG_CONF="/docker/data/gluetun/wireguard/wg0.conf"
LOG_FILE="/docker/mounts/logs/gluetun-watchdog.log"
DOMAIN="engage.cloudflareclient.com"
PORT=2408

FALLBACK_IPS=("162.159.192.1" "162.159.193.1" "162.159.195.1" "188.114.96.1" "188.114.97.1")

mkdir -p "$(dirname "$LOG_FILE")"

log() {
    local msg="[$(date '+%Y-%m-%d %H:%M:%S')] $1"
    echo "$msg"
    echo "$msg" >> "$LOG_FILE"
}

get_current_ip() {
    if [ -f "$WG_CONF" ]; then
        grep -E '^\s*Endpoint\s*=' "$WG_CONF" | awk -F'=' '{print $2}' | tr -d ' ' | cut -d: -f1 || echo ""
    else
        echo ""
    fi
}

resolve_domain_ips() {
    local ips=()
    for resolver in "1.1.1.1" "8.8.8.8" ""; do
        local res_arg=""
        [ -n "$resolver" ] && res_arg="@$resolver"
        local found
        found=$(dig +short +time=2 +tries=2 $res_arg "$DOMAIN" 2>/dev/null | grep -E '^[0-9]+\.[0-9]+\.[0-9]+\.[0-9]+$' || true)
        for ip in $found; do
            ips+=("$ip")
        done
        [ ${#ips[@]} -gt 0 ] && break
    done

    if [ ${#ips[@]} -eq 0 ]; then
        local getent_ips
        getent_ips=$(getent ahostsv4 "$DOMAIN" 2>/dev/null | awk '{print $1}' | sort -u || true)
        for ip in $getent_ips; do
            ips+=("$ip")
        done
    fi

    echo "${ips[@]}" | tr ' ' '\n' | sort -u | tr '\n' ' '
}

check_gluetun_healthy() {
    local status
    status=$(docker inspect gluetun --format '{{.State.Health.Status}}' 2>/dev/null || echo "missing")
    [ "$status" = "healthy" ] || return 1
    docker exec gluetun wget -qO- --timeout=5 https://api.ipify.org >/dev/null 2>&1 || return 1
    return 0
}

update_endpoint_ip() {
    local new_ip="$1"
    local current_ip
    current_ip=$(get_current_ip)

    if [ "$current_ip" = "$new_ip" ]; then
        log "Endpoint IP in wg0.conf is already $new_ip. Restarting container to force re-handshake..."
    else
        log "Updating WireGuard endpoint IP from '$current_ip' to '$new_ip'..."
        sed -i -E "s/^\s*Endpoint\s*=.*/Endpoint = ${new_ip}:${PORT}/" "$WG_CONF"
    fi

    docker compose -f "$COMPOSE_FILE" restart gluetun

    log "Waiting for Gluetun to recover..."
    for i in $(seq 1 15); do
        sleep 1
        if check_gluetun_healthy; then
            local new_pub_ip
            new_pub_ip=$(docker exec gluetun wget -qO- --timeout=4 https://api.ipify.org 2>/dev/null || echo "unknown")
            log "SUCCESS: Gluetun is healthy! Active VPN Public IP: $new_pub_ip"
            return 0
        fi
    done

    log "WARNING: Gluetun did not become healthy with IP $new_ip within 15s."
    return 1
}

CURRENT_IP=$(get_current_ip)
RESOLVED_IPS=($(resolve_domain_ips))
PRIMARY_RESOLVED="${RESOLVED_IPS[0]:-${FALLBACK_IPS[0]}}"

log "ALERT: Gluetun is unhealthy! Attempting auto-healing recovery..."
CANDIDATES=("$PRIMARY_RESOLVED")
for fb in "${FALLBACK_IPS[@]}"; do
    [ "$fb" != "$PRIMARY_RESOLVED" ] && CANDIDATES+=("$fb")
done

for cand in "${CANDIDATES[@]}"; do
    log "Testing recovery with endpoint candidate: $cand..."
    if update_endpoint_ip "$cand"; then
        log "Recovery completed successfully."
        exit 0
    fi
done

log "ERROR: All endpoint candidates failed to restore Gluetun health."
exit 1
