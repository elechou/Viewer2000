$ErrorActionPreference = "Stop"

try {
    $repoRoot = (Resolve-Path (Join-Path $PSScriptRoot "..\..")).Path
    $ccsRoot = if ($env:CCS_ROOT) { $env:CCS_ROOT } else { "C:\ti\ccs2100" }
    $dss = Join-Path $ccsRoot "ccs\ccs_base\scripting\bin\dss.bat"
    $script = Join-Path $PSScriptRoot "flash_dual_core_f28p65x.js"

    if (-not (Test-Path -LiteralPath $dss -PathType Leaf)) {
        throw "DSS runner not found: $dss"
    }
    if (-not (Test-Path -LiteralPath $script -PathType Leaf)) {
        throw "DSS script not found: $script"
    }

    $env:CCS_ROOT = $ccsRoot
    if (-not $env:TI_APPDATA_DIR) {
        $env:TI_APPDATA_DIR = Join-Path ([IO.Path]::GetTempPath()) "v2k-ti-appdata"
    }
    New-Item -ItemType Directory -Force -Path $env:TI_APPDATA_DIR | Out-Null

    $statusFile = Join-Path (
        [IO.Path]::GetTempPath()
    ) ("viewer2000-flash-status-{0}.txt" -f [guid]::NewGuid())
    $previousStatusFile = $env:V2K_FLASH_STATUS_FILE
    $env:V2K_FLASH_STATUS_FILE = $statusFile

    try {
        Push-Location $repoRoot
        try {
            & $dss $script @args
            $dssExitCode = $LASTEXITCODE
        }
        finally {
            Pop-Location
        }

        if (Test-Path -LiteralPath $statusFile -PathType Leaf) {
            $scriptExitCode = [int](Get-Content -Raw -LiteralPath $statusFile)
        }
        elseif ($dssExitCode -ne 0) {
            $scriptExitCode = $dssExitCode
        }
        else {
            throw "DSS exited without reporting the Flash script result"
        }
    }
    finally {
        Remove-Item -Force -LiteralPath $statusFile -ErrorAction SilentlyContinue
        if ($null -eq $previousStatusFile) {
            Remove-Item Env:V2K_FLASH_STATUS_FILE -ErrorAction SilentlyContinue
        }
        else {
            $env:V2K_FLASH_STATUS_FILE = $previousStatusFile
        }
    }
    exit $scriptExitCode
}
catch {
    [Console]::Error.WriteLine("Flash launcher error: $($_.Exception.Message)")
    exit 2
}
