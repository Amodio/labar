#!/usr/bin/env bash
set -euo pipefail

# Ensure git exists
if ! command -v git >/dev/null 2>&1; then
    echo "Error: git not found in PATH."
    exit 1
fi

# Ensure we are inside a git repository
if ! REPO_ROOT="$(git rev-parse --show-toplevel 2>/dev/null)"; then
    echo "Error: Not inside a git repository."
    exit 1
fi

HOOKS_DIR="$REPO_ROOT/scripts/git-hooks"
PRECOMMIT_HOOK="$HOOKS_DIR/pre-commit"

# Verify hook file exists
if [ ! -f "$PRECOMMIT_HOOK" ]; then
    echo "Error: pre-commit hook not found at:"
    echo "  $PRECOMMIT_HOOK"
    exit 1
fi

# Verify hook is executable
if [ ! -x "$PRECOMMIT_HOOK" ]; then
    echo "Error: pre-commit hook is not executable..."
    echo "Fix it with:"
    echo "  chmod +x scripts/git-hooks/pre-commit"
    exit 1
fi

# Check existing hooksPath configuration
EXISTING_HOOKS_PATH="$(git config --get core.hooksPath || true)"

if [ -n "$EXISTING_HOOKS_PATH" ]; then
    # Resolve to absolute path for comparison
    if [ "$EXISTING_HOOKS_PATH" != "$HOOKS_DIR" ] && \
       [ "$EXISTING_HOOKS_PATH" != "scripts/git-hooks" ]; then
        echo "❌ core.hooksPath is already set to:"
        echo "  $EXISTING_HOOKS_PATH"
        echo ""
        echo "This script will NOT override existing configuration."
        echo "If you want to use repository hooks, run:"
        echo "  git config core.hooksPath scripts/git-hooks"
        exit 1
    fi

    echo "Hooks already configured correctly."
    exit 0
fi

# Finally! Set hooksPath (repository-local config)
git config core.hooksPath scripts/git-hooks

echo "✅ Git hooks (pre-commit) installed successfully."
