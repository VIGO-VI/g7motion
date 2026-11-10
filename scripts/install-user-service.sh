#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
set -eu
ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)

install -Dm755 "$ROOT/build/g7motion" "$HOME/.local/bin/g7motion"
install -Dm755 "$ROOT/build/g7bt" "$HOME/.local/bin/g7bt"

install -Dm644 "$ROOT/systemd/g7motion.user.service" \
	"$HOME/.config/systemd/user/g7motion.service"
install -Dm644 "$ROOT/systemd/g7bt.user.service" \
	"$HOME/.config/systemd/user/g7bt.service"

systemctl --user daemon-reload
systemctl --user enable --now g7motion
systemctl --user enable --now g7bt

systemctl --user --no-pager status g7motion | head -12
systemctl --user --no-pager status g7bt | head -12
