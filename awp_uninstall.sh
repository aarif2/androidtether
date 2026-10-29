#!/bin/bash
[ "$EUID" -eq 0 ] || { echo "run with sudo"; exit 1; }
L=pro.aarif.awp-rndisd
launchctl bootout system/$L 2>/dev/null
rm -f /Library/LaunchDaemons/$L.plist /usr/local/sbin/awp_rndisd
echo removed
