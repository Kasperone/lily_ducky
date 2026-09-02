#!/usr/bin/env bash
# Wire this clone to the project's vault-authored Claude-Docs.
#
# The private agent notes for this project live ONLY in the Obsidian vault
# (synced across machines), never in this public GitHub repo. This script
# creates a *gitignored* symlink so the notes are readable while developing:
#
#     lily_ducky/Claude-Docs  ->  $VAULT/VMs/archPentestVM/projects/LilyDucky/Claude-Docs
#
# Run once per clone / per machine. Override the vault location with:
#     VAULT=/path/to/vault scripts/link-claude-docs.sh
#
# Optional: also expose this repo inside Obsidian for editing public docs there:
#     scripts/link-claude-docs.sh --with-vault-view
set -euo pipefail

VAULT="${VAULT:-$HOME/Documents/vault}"
DOCS="$VAULT/VMs/archPentestVM/projects/LilyDucky/Claude-Docs"
REPO="$(cd "$(dirname "$0")/.." && pwd)"

if [ ! -d "$DOCS" ]; then
  echo "!! Vault Claude-Docs not found: $DOCS" >&2
  echo "   Set VAULT=... if your vault lives elsewhere, or sync the vault first." >&2
  exit 1
fi

# repo -> vault (private notes visible while developing; gitignored)
ln -sfn "$DOCS" "$REPO/Claude-Docs"
echo "linked  Claude-Docs -> $DOCS"

# optional: vault -> repo (browse/edit the public repo docs inside Obsidian)
if [ "${1:-}" = "--with-vault-view" ]; then
  VIEW="$VAULT/VMs/archPentestVM/projects/LilyDucky/repo"
  ln -sfn "$REPO" "$VIEW"
  echo "linked  $VIEW -> $REPO   (open in Obsidian)"
fi

echo "done."
