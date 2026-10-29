#!/bin/bash
set -e
[ "$EUID" -eq 0 ] || { echo "run with sudo"; exit 1; }
D="$(cd "$(dirname "$0")" && pwd)"
L=pro.aarif.awp-rndisd
launchctl bootout system/$L 2>/dev/null || true
mkdir -p /usr/local/sbin
install -o root -g wheel -m 755 "$D/awp_rndisd" /usr/local/sbin/awp_rndisd
install -o root -g wheel -m 644 "$D/$L.plist" /Library/LaunchDaemons/$L.plist
launchctl bootstrap system /Library/LaunchDaemons/$L.plist
echo "installed. log: /var/log/awp_rndisd.log"
