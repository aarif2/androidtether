#!/bin/bash
[ "$EUID" -eq 0 ] || { echo "run with sudo"; exit 1; }
L=com.github.aarif2.androidtether
launchctl bootout system/$L 2>/dev/null
rm -f /Library/LaunchDaemons/$L.plist /usr/local/sbin/androidtetherd
echo removed
