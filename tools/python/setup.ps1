# Creates the agent's dedicated Python virtualenv (tools/python/venv) and
# installs tools/python/requirements.txt into it. Safe to re-run: an existing
# venv is reused, packages are refreshed. See tools_plan.md section 6.3.

$ErrorActionPreference = "Stop"
$here = Split-Path -Parent $MyInvocation.MyCommand.Path
$venv = Join-Path $here "venv"

if (-not (Test-Path (Join-Path $venv "Scripts\python.exe"))) {
    Write-Host "creating venv: $venv"
    python -m venv $venv
} else {
    Write-Host "venv already present: $venv"
}

$py = Join-Path $venv "Scripts\python.exe"
& $py -m pip install --upgrade pip
& $py -m pip install -r (Join-Path $here "requirements.txt")

Write-Host "done: $py"
