#!/usr/bin/env bash
# ==============================================================================
# open-history.sh: what the W key, the history button and a click on an
# account of the DriveSync bar widget open. The widget ships inside the
# DriveSync package, so the app is always there: focus its window when one is
# open, otherwise start it.
#
# Takes one optional argument, the account to open the window on. It is used
# only when it is an account id as the app writes them (account1 to
# account99), and then passed on as one argument; anything else, and every
# other argument, is ignored. Nothing from the shell is ever interpolated.
# ==============================================================================
set -uo pipefail

account=""
if [[ ${1:-} =~ ^account[1-9][0-9]?$ ]]; then
  account=$1
fi

# Address of an open DriveSync window, matched by the exact app_id only:
# omarchy-launch-or-focus also matches titles, so a terminal open in a folder
# called drive-sync would take the focus instead.
app_window() {
  hyprctl clients -j 2>/dev/null \
    | jq -r '.[] | select(.class == "drive-sync") | .address' 2>/dev/null \
    | head -n1
}

address=$(app_window)
if [[ $address =~ ^0x[0-9a-f]+$ ]]; then
  # An open window is only focused, whatever account was asked for: the
  # window keeps the account the user picked there.
  # Same two forms omarchy-launch-or-focus tries (Lua config first).
  hyprctl dispatch "hl.dsp.focus({ window = \"address:$address\" })" >/dev/null 2>&1 \
    || hyprctl dispatch focuswindow "address:$address" >/dev/null
elif [[ -n $account ]]; then
  exec uwsm-app -- drive-sync open --account "$account"
else
  exec uwsm-app -- drive-sync
fi
