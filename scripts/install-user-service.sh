#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
set -eu
ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
install -Dm755 "$ROOT/build/g7motion" "$HOME/.local/bin/g7motion"
install -Dm644 "$ROOT/systemd/g7motion.user.service" "$HOME/.config/systemd/user/g7motion.service"
systemctl --user daemon-reload
systemctl --user enable --now g7motion
systemctl --user --no-pager status g7motion | head -15
