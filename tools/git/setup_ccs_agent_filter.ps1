$ErrorActionPreference = "Stop"

git rev-parse --show-toplevel *> $null
if ($LASTEXITCODE -ne 0) {
    Write-Error "This script must be run inside the Viewer2000 Git repository."
    exit 1
}

$pythonFilter = $null

if (Get-Command py -ErrorAction SilentlyContinue) {
    $pythonFilter = "py -3 tools/git/strip_ccs_agent_header.py"
} elseif (Get-Command python -ErrorAction SilentlyContinue) {
    $pythonFilter = "python tools/git/strip_ccs_agent_header.py"
} else {
    Write-Error "Python was not found. Install Python 3 or add it to PATH."
    exit 1
}

git config --local filter.ccs-agent-header.clean $pythonFilter
git config --local filter.ccs-agent-header.smudge cat
git config --local filter.ccs-agent-header.required true

Write-Host "Installed local Git filter: ccs-agent-header"
Write-Host "If Git still reports header-only changes, run: git add --renormalize AGENTS.md CLAUDE.md"
