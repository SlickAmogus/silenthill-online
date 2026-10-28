#!/bin/bash
# install-server.sh - put the Silent Hill Online server on this Linux machine
# as a systemd service (sh1pc) that starts with the machine and restarts itself
# two seconds after a crash. Running it again updates the server in place.
#
#   sudo bash install-server.sh [--args "-n Fog -m Welcome"] [--domain server.example.com]
#
# The files come from the folder this script is in (the unzipped release):
# when gcc is installed, sh_master.c + sh_net_proto.h are compiled here, so a
# locally edited server works too; otherwise the prebuilt sh_master-linux-x64
# or sh_master-linux-arm64 matching `uname -m` is used.
#
# --args is only used the first time, when the service is created. After that
# the command line lives in ExecStart= of /etc/systemd/system/sh1pc.service
# (then `systemctl daemon-reload && systemctl restart sh1pc`).
#
# --domain makes browser players possible: Caddy is installed as a TLS proxy so
# the web game can reach the server as wss://DOMAIN. The DNS record must
# already point at this machine, and ports 80 and 443 must be free for Caddy.
set -e

HERE="$(cd "$(dirname "$0")" && pwd)"
ARGS=""
DOMAIN=""
while [ $# -gt 0 ]; do
    case "$1" in
        --args)   ARGS="$2"; shift 2 ;;
        --domain) DOMAIN="$2"; shift 2 ;;
        *) echo "unknown option: $1"; exit 2 ;;
    esac
done

if [ "$(id -u)" != 0 ]; then
    echo "run as root (or with sudo)"; exit 1
fi
if ! command -v systemctl >/dev/null 2>&1; then
    echo "this script needs systemd; without it run sh_master by hand (see README.md)"; exit 1
fi

# An existing service keeps its binary path; a fresh install uses /root/sh_master
# with /root as the working directory (memos.db and bans.txt land there).
UNIT=sh1pc
UNIT_FILE="$(systemctl show -p FragmentPath --value $UNIT 2>/dev/null || true)"
BIN=/root/sh_master
WORKDIR=/root
if [ -n "$UNIT_FILE" ] && [ -f "$UNIT_FILE" ]; then
    P="$(systemctl show -p ExecStart --value $UNIT 2>/dev/null | sed -n 's/.*path=\([^ ;]*\).*/\1/p')"
    [ -n "$P" ] && BIN="$P"
    W="$(systemctl show -p WorkingDirectory --value $UNIT 2>/dev/null)"
    [ -n "$W" ] && [ "$W" != "!" ] && WORKDIR="$W"
else
    UNIT_FILE=/etc/systemd/system/$UNIT.service
fi
mkdir -p "$(dirname "$BIN")" "$WORKDIR"

echo ">> server program ..."
if command -v gcc >/dev/null 2>&1 && [ -f "$HERE/sh_master.c" ] && [ -f "$HERE/sh_net_proto.h" ]; then
    echo "   compiling sh_master.c with gcc"
    gcc -O2 -I"$HERE" -o "$BIN.new" "$HERE/sh_master.c"
else
    case "$(uname -m)" in
        x86_64|amd64)  SRC="$HERE/sh_master-linux-x64" ;;
        aarch64|arm64) SRC="$HERE/sh_master-linux-arm64" ;;
        *) SRC="" ;;
    esac
    if [ -z "$SRC" ] || [ ! -f "$SRC" ]; then
        echo "   no prebuilt server for $(uname -m) and no gcc to build one: apt install gcc, then run this again"
        exit 1
    fi
    echo "   using prebuilt $(basename "$SRC")"
    cp -f "$SRC" "$BIN.new"
    chmod 755 "$BIN.new"
fi
# A wrong architecture or a broken download fails here, before anything is replaced.
"$BIN.new" --help >/dev/null
mv -f "$BIN.new" "$BIN"
# So `sh_master --console` works from any directory (Server-Console.bat relies on it).
ln -sfn "$BIN" /usr/local/bin/sh_master 2>/dev/null || true

echo ">> service $UNIT ..."
if [ ! -f "$UNIT_FILE" ]; then
    [ -n "$ARGS" ] || ARGS='-n "Silent Hill Online"'
    cat > "$UNIT_FILE" <<EOF
[Unit]
Description=Silent Hill Online server
After=network-online.target
Wants=network-online.target
StartLimitIntervalSec=0

[Service]
WorkingDirectory=$WORKDIR
ExecStart=$BIN $ARGS
Restart=always
RestartSec=2

[Install]
WantedBy=multi-user.target
EOF
    echo "   created $UNIT_FILE"
    echo "   command line: $BIN $ARGS"
else
    # Older units predate the restart policy: patch them once.
    CHANGED=0
    if ! grep -q '^Restart=' "$UNIT_FILE"; then
        sed -i '/^\[Service\]/a Restart=always\nRestartSec=2' "$UNIT_FILE"; CHANGED=1
    fi
    if ! grep -q '^StartLimitIntervalSec=' "$UNIT_FILE"; then
        sed -i '/^\[Unit\]/a StartLimitIntervalSec=0' "$UNIT_FILE"; CHANGED=1
    fi
    [ "$CHANGED" = 1 ] && echo "   the service now restarts itself after a crash (Restart=always)"
    [ -n "$ARGS" ] && echo "   (service already exists; --args ignored, edit ExecStart= in $UNIT_FILE)"
fi

# What the command line says, for the firewall and the checks below.
EXEC="$(grep '^ExecStart=' "$UNIT_FILE" | head -1)"
UDP_PORT="$(echo "$EXEC" | sed -n 's/.* -p \([0-9]*\).*/\1/p')";       [ -n "$UDP_PORT" ] || UDP_PORT=27888
ADMIN_PORT="$(echo "$EXEC" | sed -n 's/.* --admin \([0-9]*\).*/\1/p')"; [ -n "$ADMIN_PORT" ] || ADMIN_PORT=27891
WS_PORT="$(echo "$EXEC" | sed -n 's/.* --ws \([0-9]*\).*/\1/p')"

if [ -n "$DOMAIN" ] && [ -z "$WS_PORT" ]; then
    WS_PORT=27890
    sed -i "/^ExecStart=/ s/[[:space:]]*\$/ --ws $WS_PORT/" "$UNIT_FILE"
    echo "   the service now also listens for browser players on TCP $WS_PORT (--ws)"
fi

systemctl daemon-reload
systemctl enable $UNIT >/dev/null 2>&1

if command -v ufw >/dev/null 2>&1 && ufw status 2>/dev/null | grep -q 'Status: active'; then
    ufw allow "$UDP_PORT/udp" >/dev/null 2>&1 || true
fi

# Browser players: a page served over https can only open wss://, so Caddy sits
# in front of the WebSocket port with a certificate it fetches by itself. A
# failure here never stops the UDP server from going live.
web_setup() {
    set +e
    echo ">> browser players (wss://$DOMAIN) ..."
    if ! command -v caddy >/dev/null 2>&1; then
        echo "   installing Caddy ..."
        export DEBIAN_FRONTEND=noninteractive
        if command -v apt-get >/dev/null 2>&1; then
            apt-get update -qq >/dev/null 2>&1
            if ! apt-get install -y -qq caddy >/dev/null 2>&1; then
                apt-get install -y -qq debian-keyring debian-archive-keyring apt-transport-https curl gnupg >/dev/null 2>&1
                curl -1sLf 'https://dl.cloudsmith.io/public/caddy/stable/gpg.key' | gpg --batch --yes --dearmor -o /usr/share/keyrings/caddy-stable-archive-keyring.gpg
                curl -1sLf 'https://dl.cloudsmith.io/public/caddy/stable/debian.deb.txt' > /etc/apt/sources.list.d/caddy-stable.list
                apt-get update -qq >/dev/null 2>&1
                apt-get install -y -qq caddy >/dev/null 2>&1
            fi
        elif command -v dnf >/dev/null 2>&1; then
            dnf install -y -q caddy >/dev/null 2>&1
        fi
    fi
    if ! command -v caddy >/dev/null 2>&1; then
        echo "   WARNING: Caddy could not be installed; browser players cannot connect yet"
        return 0
    fi
    if ! grep -q "reverse_proxy 127.0.0.1:$WS_PORT" /etc/caddy/Caddyfile 2>/dev/null || ! grep -q "^$DOMAIN" /etc/caddy/Caddyfile 2>/dev/null; then
        [ -f /etc/caddy/Caddyfile ] && cp /etc/caddy/Caddyfile /etc/caddy/Caddyfile.before-sh1pc
        printf '%s {\n\treverse_proxy 127.0.0.1:%s\n}\n' "$DOMAIN" "$WS_PORT" > /etc/caddy/Caddyfile
        systemctl enable caddy >/dev/null 2>&1
        systemctl restart caddy
        echo "   Caddy configured for $DOMAIN"
    fi
    if command -v ufw >/dev/null 2>&1 && ufw status 2>/dev/null | grep -q 'Status: active'; then
        ufw allow 80/tcp >/dev/null 2>&1
        ufw allow 443/tcp >/dev/null 2>&1
    fi
    OTHER443="$(ss -ltnpH 'sport = :443' 2>/dev/null | grep -v caddy)"
    [ -n "$OTHER443" ] && echo "   WARNING: something other than Caddy holds port 443: $OTHER443"
    printf '   caddy is: '; systemctl is-active caddy
    return 0
}
[ -n "$DOMAIN" ] && web_setup
set -e

echo ">> starting ..."
systemctl restart $UNIT
sleep 1
printf '>> service is now: '; systemctl is-active $UNIT || true
printf '>> admin console answers: '
if "$BIN" --admin "$ADMIN_PORT" --cmd players >/dev/null 2>&1; then echo yes; else echo "no (check: journalctl -u $UNIT -n 50)"; fi
if [ -n "$WS_PORT" ]; then
    printf '>> WebSocket listener answers: '
    curl -s -o /dev/null --max-time 2 -w '%{http_code}\n' -H 'Connection: Upgrade' -H 'Upgrade: websocket' -H 'Sec-WebSocket-Version: 13' -H 'Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==' "http://127.0.0.1:$WS_PORT/" || echo
    if [ -n "$DOMAIN" ]; then
        printf '>> through Caddy (wss://%s): ' "$DOMAIN"
        curl -s -o /dev/null --max-time 5 -w '%{http_code}\n' -H 'Connection: Upgrade' -H 'Upgrade: websocket' -H 'Sec-WebSocket-Version: 13' -H 'Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==' "https://$DOMAIN/" || echo
        echo "   (101 = working; the first run can take a minute while Caddy gets its certificate)"
    fi
fi
IP="$(curl -4 -s --max-time 3 https://api.ipify.org 2>/dev/null || hostname -I 2>/dev/null | awk '{print $1}')"
[ -n "$IP" ] || IP="this machine's address"
echo ">> done. Players enter $IP (UDP $UDP_PORT) as the Living World IP."
echo "   admin console: $BIN --console      log: journalctl -u $UNIT -f"
