[CmdletBinding()]
param()

$taskRepoRoot = (Resolve-Path (Join-Path $PSScriptRoot "..\..")).Path
$taskVenvScripts = Join-Path $taskRepoRoot ".venv\Scripts"
$taskZephyrBase = Join-Path $taskRepoRoot "zephyr"
$taskSdkRoot = Join-Path $taskRepoRoot ".toolchains\zephyr-sdk-0.16.8"
$taskArmCompiler = Join-Path $taskSdkRoot "arm-zephyr-eabi\bin\arm-zephyr-eabi-gcc.exe"

foreach ($taskRequiredPath in @(
    (Join-Path $taskVenvScripts "west.exe"),
    (Join-Path $taskVenvScripts "pyocd.exe"),
    (Join-Path $taskVenvScripts "cmake.exe"),
    (Join-Path $taskVenvScripts "ninja.exe"),
    $taskZephyrBase,
    $taskArmCompiler
)) {
    if (-not (Test-Path -LiteralPath $taskRequiredPath)) {
        throw "Mesh flashing environment is incomplete: $taskRequiredPath is missing"
    }
}

$env:PATH = "$taskVenvScripts;$env:PATH"
$env:ZEPHYR_BASE = $taskZephyrBase
$env:ZEPHYR_SDK_INSTALL_DIR = $taskSdkRoot
$env:ZEPHYR_TOOLCHAIN_VARIANT = "zephyr"
$env:CCACHE_DISABLE = "1"

Set-Location -LiteralPath $taskRepoRoot

Write-Host "IMEC mesh build/flash environment is ready."
Write-Host "Build anchor: west build --no-sysbuild -s firmware/app -b nrf52833dk/nrf52833 --build-dir build/mesh-anchor -- -DIMEC_BUILD_PRESET=mesh_anchor"
Write-Host "Stage only through the verified wrapper: python firmware/scripts/flash_verified_mesh.py --build-dir build/mesh-anchor --probe-id <probe-id> --stage-only"
