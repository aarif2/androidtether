#!/bin/bash
set -e
[ "$EUID" -eq 0 ] || { echo "run with sudo"; exit 1; }
D="$(cd "$(dirname "$0")" && pwd)"
L=com.github.aarif2.androidtether
launchctl bootout system/$L 2>/dev/null || true
make -C "$D" install PREFIX=/usr/local
install -o root -g wheel -m 644 "$D/$L.plist" /Library/LaunchDaemons/$L.plist
launchctl bootstrap system /Library/LaunchDaemons/$L.plist
echo "installed. log: /var/log/androidtether.log"
