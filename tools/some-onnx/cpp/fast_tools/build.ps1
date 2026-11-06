param()

$ErrorActionPreference = "Stop"
$Source = $PSScriptRoot
$BuildDirectory = Join-Path $Source "build"

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

$env:INCLUDE = "$VSMSVC\include;$SDK\Include\$($SDKVer.Name)\ucrt;$SDK\Include\$($SDKVer.Name)\um;$SDK\Include\$($SDKVer.Name)\shared"
$env:LIB = "$VSMSVC\lib\x64;$SDK\Lib\$($SDKVer.Name)\ucrt\x64;$SDK\Lib\$($SDKVer.Name)\um\x64"
$env:PATH = "$VSMSVC\bin\Hostx64\x64;$SDK\bin\$($SDKVer.Name)\x64;$env:PATH"

if (Test-Path $BuildDirectory) {
    Remove-Item $BuildDirectory -Recurse -Force
}
& $Cmake -S $Source -B $BuildDirectory -G Ninja "-DCMAKE_MAKE_PROGRAM=$Ninja" "-DCMAKE_CXX_COMPILER=$($MSVCDir.FullName)" -DCMAKE_BUILD_TYPE=Release
if ($LASTEXITCODE -ne 0) {
    throw "CMake configuration failed with exit code $LASTEXITCODE."
}
& $Cmake --build $BuildDirectory
if ($LASTEXITCODE -ne 0) {
    throw "C++ build failed with exit code $LASTEXITCODE."
}
Write-Host "Built: $BuildDirectory\fast_midi_to_nbt.exe"
Write-Host "Built: $BuildDirectory\fast_arrange_multilane.exe"
Write-Host "Built: $BuildDirectory\fast_merge_midis.exe"
