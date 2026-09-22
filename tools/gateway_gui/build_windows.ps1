param(
    [string]$Python = ""
)

$ErrorActionPreference = "Stop"
$repoRoot = (Resolve-Path (Join-Path $PSScriptRoot "../..")).Path
if (-not $Python) {
    $Python = Join-Path $repoRoot ".venv/Scripts/python.exe"
}
$outputRoot = Join-Path $repoRoot "build/gateway-gui-windows"
New-Item -ItemType Directory -Force -Path $outputRoot | Out-Null

& $Python -m PyInstaller --noconfirm --onefile --windowed --noupx `
    --name IMEC2-Gateway-GUI-Windows-x64 `
    --paths $repoRoot `
    --distpath (Join-Path $outputRoot "dist") `
    --workpath (Join-Path $outputRoot "work") `
    --specpath $outputRoot `
    (Join-Path $PSScriptRoot "standalone.py")
if ($LASTEXITCODE -ne 0) {
    throw "Standalone GUI build failed with exit code $LASTEXITCODE"
}
Write-Output (Join-Path $outputRoot "dist/IMEC2-Gateway-GUI-Windows-x64.exe")
