param(
    [Parameter(Mandatory = $true)]
    [string]$OrtRoot
)

$ErrorActionPreference = "Stop"
$Root = Split-Path -Parent $PSScriptRoot
Remove-Item Env:WATCOM -ErrorAction SilentlyContinue
Remove-Item Env:CC -ErrorAction SilentlyContinue
Remove-Item Env:CXX -ErrorAction SilentlyContinue
Remove-Item Env:CFLAGS -ErrorAction SilentlyContinue
Remove-Item Env:CXXFLAGS -ErrorAction SilentlyContinue
$VSRRoots = @(
    "${env:ProgramFiles}\Microsoft Visual Studio",
    "${env:ProgramFiles(x86)}\Microsoft Visual Studio",
    "F:\vs"
) | Where-Object { $_ -and (Test-Path $_) }

$Cmake = (Get-Command cmake -ErrorAction SilentlyContinue).Source
if (-not $Cmake) {
    $Cmake = Get-ChildItem $VSRRoots -Filter cmake.exe -Recurse -ErrorAction Stop |
        Where-Object { $_.FullName -match "CommonExtensions\\Microsoft\\CMake\\CMake\\bin" } |
        Select-Object -First 1 -ExpandProperty FullName
}
$Ninja = (Get-Command ninja -ErrorAction SilentlyContinue).Source
if (-not $Ninja) {
    $Ninja = Get-ChildItem $VSRRoots -Filter ninja.exe -Recurse -ErrorAction Stop |
        Where-Object { $_.FullName -match "CommonExtensions\\Microsoft\\CMake\\Ninja" } |
        Select-Object -First 1 -ExpandProperty FullName
}
if (-not $Cmake -or -not $Ninja) {
    throw "CMake or Ninja was not found in PATH or Visual Studio installations."
}
$MSVCDir = Get-ChildItem $VSRRoots -Filter cl.exe -Recurse -ErrorAction Stop |
    Where-Object { $_.FullName -match "VC\\Tools\\MSVC" -and $_.FullName -match "Hostx64\\x64" } |
    Select-Object -First 1
if (-not $MSVCDir) {
    throw "MSVC x64 compiler was not found."
}
$VSMSVC = Split-Path (Split-Path (Split-Path (Split-Path $MSVCDir.FullName)))
$SDK = "C:\Program Files (x86)\Windows Kits\10"
$SDKVer = Get-ChildItem "$SDK\Include" -Directory | Sort-Object Name -Descending | Select-Object -First 1
if (-not (Test-Path (Join-Path $OrtRoot "lib\onnxruntime.lib"))) {
    throw "ONNX Runtime SDK not found at: $OrtRoot"
}
$env:INCLUDE = "$VSMSVC\include;$SDK\Include\$($SDKVer.Name)\ucrt;$SDK\Include\$($SDKVer.Name)\um;$SDK\Include\$($SDKVer.Name)\shared;$SDK\Include\$($SDKVer.Name)\winrt"
$env:LIB = "$VSMSVC\lib\x64;$SDK\Lib\$($SDKVer.Name)\ucrt\x64;$SDK\Lib\$($SDKVer.Name)\um\x64"
$env:PATH = "$VSMSVC\bin\Hostx64\x64;$SDK\bin\$($SDKVer.Name)\x64;$env:PATH"

$BuildDirectory = Join-Path $Root "cpp-build"
if (Test-Path $BuildDirectory) {
    Remove-Item $BuildDirectory -Recurse -Force
}
& $Cmake -S (Join-Path $Root "cpp") -B $BuildDirectory -G Ninja "-DCMAKE_MAKE_PROGRAM=$Ninja" "-DORT_ROOT=$OrtRoot" "-DCMAKE_CXX_COMPILER=$($MSVCDir.FullName)" -DCMAKE_BUILD_TYPE=Release -DCMAKE_CXX_FLAGS= -DCMAKE_CXX_FLAGS_RELEASE=
if ($LASTEXITCODE -ne 0) {
    throw "CMake configuration failed with exit code $LASTEXITCODE."
}
& $Cmake --build (Join-Path $Root "cpp-build")
if ($LASTEXITCODE -ne 0) {
    throw "C++ build failed with exit code $LASTEXITCODE."
}
Write-Host "Done: $Root\cpp-build\audio_to_midi.exe"
