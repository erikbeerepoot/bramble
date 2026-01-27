#!/usr/bin/env bash
#
# One-time setup for the bramble development environment.
# Run this after cloning the repository.

set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"

echo "Configuring git hooks..."
git -C "$SCRIPT_DIR" config core.hooksPath .githooks
echo "Done. Git hooks are now active."
