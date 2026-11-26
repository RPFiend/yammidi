#!/bin/sh
# Install yammidi-bridge as a per-user LaunchAgent.
#
# Per-user on purpose: no sudo, no system-wide files, no security changes.
# Removing it is `launchctl bootout` plus deleting two paths.
set -eu

LABEL="com.rpfiend.yamahamx61bridge"
BIN_NAME="yammidi-bridge"
INSTALL_DIR="$HOME/Library/Application Support/YamahaMX61Bridge"
LOG_PATH="$HOME/Library/Logs/yamahamx61bridge.log"
PLIST="$HOME/Library/LaunchAgents/$LABEL.plist"

command -v clang >/dev/null || { echo "clang not found; install the Xcode command line tools"; exit 1; }

mkdir -p "$INSTALL_DIR" "$HOME/Library/LaunchAgents" "$(dirname "$LOG_PATH")"

echo "== build =="
make clean >/dev/null 2>&1 || true
make || { echo "build failed"; exit 1; }

echo
echo "== selftest =="
./yammidi-bridge --selftest || { echo "selftest failed — refusing to install"; exit 1; }

echo
echo "== install binary =="
cp "$BIN_NAME" "$INSTALL_DIR/$BIN_NAME"
chmod 755 "$INSTALL_DIR/$BIN_NAME"
echo "  $INSTALL_DIR/$BIN_NAME"

echo
echo "== write LaunchAgent =="
sed -e "s|__INSTALL_DIR__|$INSTALL_DIR|g" \
    -e "s|__LOG_PATH__|$LOG_PATH|g" \
    launchd/$LABEL.plist.template > "$PLIST"
plutil -lint "$PLIST" >/dev/null || { echo "plist failed to parse"; exit 1; }
echo "  $PLIST"

echo
echo "== load =="
launchctl bootout "gui/$(id -u)/$LABEL" 2>/dev/null || true
launchctl bootstrap "gui/$(id -u)" "$PLIST"
launchctl print "gui/$(id -u)/$LABEL" 2>/dev/null | grep -E "state =|pid =" | head -2 || true

echo
echo "Done. Log: $LOG_PATH"
echo "If the keyboard was not attached at login the log says"
echo "  'no matching device, waiting...'  — plug it in and it attaches."
echo "Check what the bridge would find, without claiming it:"
echo "  \"$INSTALL_DIR/$BIN_NAME\" --probe"
echo
echo "Uninstall: launchctl bootout gui/$(id -u)/$LABEL"
echo "           rm -rf \"$INSTALL_DIR\" \"$PLIST\""
