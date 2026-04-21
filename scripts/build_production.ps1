param(
    [string]$BuildDir = "build-production",
    [string]$Sdkconfig = "sdkconfig.production",
    [string]$IdfPath = $(if ($env:IDF_PATH) { $env:IDF_PATH } else { "C:\esp\v6.0\esp-idf" }),
    [string]$ToolsRoot = "C:\Espressif\tools"
)

$ErrorActionPreference = "Stop"

function Find-ToolDir {
    param([string]$Filter)

    $tool = Get-ChildItem -Path $ToolsRoot -Recurse -Filter $Filter -ErrorAction SilentlyContinue |
        Select-Object -First 1
    if (-not $tool) {
        throw "Could not find $Filter under $ToolsRoot"
    }

    return $tool.Directory.FullName
}

$sourceDir = Resolve-Path (Join-Path $PSScriptRoot "..")
$buildPath = Join-Path $sourceDir $BuildDir
$sdkconfigPath = Join-Path $sourceDir $Sdkconfig
$defaults = @(
    (Join-Path $sourceDir "sdkconfig"),
    (Join-Path $sourceDir "sdkconfig.defaults.production")
) -join ";"

$env:IDF_PATH = $IdfPath
$pythonDir = Join-Path $ToolsRoot "python\v6.0\venv\Scripts"
$pythonExe = Join-Path $pythonDir "python.exe"
$toolDirs = @(
    (Find-ToolDir "riscv32-esp-elf-gcc.exe"),
    (Find-ToolDir "ninja.exe"),
    $pythonDir
)
$env:PATH = ($toolDirs + $env:PATH) -join [IO.Path]::PathSeparator
$env:PYTHON = $pythonExe
$env:IDF_PYTHON_ENV_PATH = Join-Path $ToolsRoot "python\v6.0\venv"

cmake -S $sourceDir -B $buildPath -G Ninja `
    "-DIDF_TARGET=esp32c5" `
    "-DPYTHON=$pythonExe" `
    "-DPYTHON_DEPS_CHECKED=1" `
    "-DSDKCONFIG=$sdkconfigPath" `
    "-DSDKCONFIG_DEFAULTS=$defaults"
if ($LASTEXITCODE -ne 0) {
    exit $LASTEXITCODE
}

cmake --build $buildPath
if ($LASTEXITCODE -ne 0) {
    exit $LASTEXITCODE
}
