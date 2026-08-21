#!/bin/sh
# Install repo git hooks for every fresh clone of lily_ducky.
# Run once after cloning:  scripts/install-hooks.sh
#
# Uses core.hooksPath (git >= 2.9) to point at the committed .githooks/
# directory, so the hooks live in the repo and stay up to date — no
# copying into .git/hooks where they'd silently drift.

set -e
cd "$(git rev-parse --show-toplevel)"

if [ ! -d .git ]; then
    echo "Not a git repository — run this from a lily_ducky clone." >&2
    exit 1
fi

git config core.hooksPath .githooks
chmod +x .githooks/*

echo "Installed hooks from .githooks/ (core.hooksPath set)."
echo "Active: $(git config core.hooksPath)"
