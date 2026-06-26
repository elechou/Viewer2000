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
echo "If Git still reports header-only changes, run: git add --renormalize AGENTS.md CLAUDE.md"
