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

git diff --quiet -- AGENTS.md CLAUDE.md
if ($LASTEXITCODE -eq 0) {
    git update-index --assume-unchanged -- AGENTS.md CLAUDE.md
    Write-Host "Marked AGENTS.md and CLAUDE.md assume-unchanged to hide CCS-generated header churn."
    Write-Host "Before intentionally editing these files, run:"
    Write-Host "  git update-index --no-assume-unchanged AGENTS.md CLAUDE.md"
} elseif ($LASTEXITCODE -eq 1) {
    Write-Host "AGENTS.md or CLAUDE.md has a real filtered content diff; not marking assume-unchanged."
    Write-Host "Review the diff, then rerun this setup script after it is clean."
} else {
    Write-Error "Failed to inspect AGENTS.md and CLAUDE.md."
    exit $LASTEXITCODE
}
