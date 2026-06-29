#!/bin/sh
set -eu

repo_root=$(git rev-parse --show-toplevel)
filter_script="$repo_root/tools/git/strip_ccs_agent_header.py"

if [ ! -f "$filter_script" ]; then
    echo "Missing filter script: $filter_script" >&2
    exit 1
fi

git config --local filter.ccs-agent-header.clean "python3 \"$filter_script\""
git config --local filter.ccs-agent-header.smudge cat
git config --local filter.ccs-agent-header.required true

echo "Installed local Git filter: ccs-agent-header"

if git diff --quiet -- AGENTS.md CLAUDE.md; then
    git update-index --assume-unchanged -- AGENTS.md CLAUDE.md
    echo "Marked AGENTS.md and CLAUDE.md assume-unchanged to hide CCS-generated header churn."
    echo "Before intentionally editing these files, run:"
    echo "  git update-index --no-assume-unchanged AGENTS.md CLAUDE.md"
else
    echo "AGENTS.md or CLAUDE.md has a real filtered content diff; not marking assume-unchanged."
    echo "Review the diff, then rerun this setup script after it is clean."
fi
