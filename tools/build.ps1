# Configure and build from any shell: imports the MSVC environment (headers, libraries,
# linker) and compiles with clang-cl.
#   powershell -ExecutionPolicy Bypass -File tools\build.ps1 [-Disc <game.rvz|.iso>] [-Configure] [-Target red_two]
# -Disc: the user's disc image (or main.dol) to recompile; CMake remembers it (R2_DOL).
# -BuildDir: another build tree (default build\windows, the one the player runs); -CMakeArgs:
# extra -D options when that tree is configured.
param(
    [string]$Disc = "",
    [switch]$Configure,
    [string]$Target = "",
    [string]$Config = "RelWithDebInfo",
    [string]$ClangCl = "",
    [string]$BuildDir = "",
    [string[]]$CMakeArgs = @()
)
$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot
$build = if ($BuildDir) { $BuildDir } else { Join-Path $root "build\windows" }

$vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
$vsPath = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (-not $vsPath) { throw "Visual Studio C++ build tools not found" }
cmd /c "`"$vsPath\VC\Auxiliary\Build\vcvars64.bat`" >nul && set" | ForEach-Object {
    if ($_ -match '^([^=]+)=(.*)$') { Set-Item -Path "env:$($Matches[1])" -Value $Matches[2] }
}

if (-not $ClangCl) {
    foreach ($c in @("$vsPath\VC\Tools\Llvm\x64\bin\clang-cl.exe", "$env:ProgramFiles\LLVM\bin\clang-cl.exe", "C:\msys64\ucrt64\bin\clang-cl.exe", "C:\msys64\clang64\bin\clang-cl.exe")) {
        if (Test-Path $c) { $ClangCl = $c; break }
    }
}
if (-not $ClangCl) { throw "clang-cl not found (install LLVM or pass -ClangCl)" }
$lld = Join-Path (Split-Path $ClangCl) "lld-link.exe"
if (Test-Path "$env:ProgramFiles\CMake\bin") { $env:PATH = "$env:ProgramFiles\CMake\bin;$env:PATH" }
$ninja = (Get-Command ninja -ErrorAction SilentlyContinue).Source
if (-not $ninja -and (Test-Path "C:\msys64\ucrt64\bin\ninja.exe")) { $ninja = "C:\msys64\ucrt64\bin\ninja.exe" }
if (-not $ninja) { throw "ninja not found" }

# Our fixes to Aurora (patches/aurora/*.patch) on top of the upstream submodule: on a clean
# checkout all of them are applied; otherwise each one that is not applied yet (a patch that
# reverse-applies cleanly is already in). Later patches can change the context of earlier ones,
# so on a modified tree a patch that neither reverses nor applies is reported, not fatal.
$auroraClean = -not (git -C "$root\extern\aurora" status --porcelain --untracked-files=no)
foreach ($patch in Get-ChildItem "$root\patches\aurora\*.patch" -ErrorAction SilentlyContinue | Sort-Object Name) {
    if (-not $auroraClean) {
        cmd /c "git -C `"$root\extern\aurora`" apply --reverse --check `"$($patch.FullName)`" 2>nul"
        if ($LASTEXITCODE -eq 0) { continue }
    }
    cmd /c "git -C `"$root\extern\aurora`" apply `"$($patch.FullName)`" 2>nul"
    if ($LASTEXITCODE -eq 0) { Write-Host "applied $($patch.Name)"; continue }
    if ($auroraClean) { throw "could not apply $($patch.Name) to extern/aurora" }
    Write-Host "note: $($patch.Name) neither applies nor reverses on the modified extern/aurora (assumed applied)"
}

if ($Disc) {
    $Disc = (Resolve-Path $Disc).Path
    $Configure = $true
}
if ($Configure -or -not (Test-Path "$build\build.ninja")) {
    # MSYS2/MinGW libraries (fmt, zstd, ...) are not usable from an MSVC-ABI build: keep CMake
    # from finding them through PATH or pkg-config so Aurora fetches its own.
    $ignore = "C:/msys64/ucrt64;C:/msys64/mingw64;C:/msys64/clang64;C:/msys64/usr"
    $args = @("-S", $root, "-B", $build, "-G", "Ninja", "-DCMAKE_BUILD_TYPE=$Config",
        "-DCMAKE_C_COMPILER=$ClangCl", "-DCMAKE_CXX_COMPILER=$ClangCl", "-DCMAKE_MAKE_PROGRAM=$ninja",
        "-DCMAKE_IGNORE_PREFIX_PATH=$ignore", "-DCMAKE_DISABLE_FIND_PACKAGE_PkgConfig=ON")
    if (Test-Path $lld) { $args += "-DCMAKE_LINKER=$lld" }
    if ($Disc) { $args += "-DR2_DOL=$Disc" }
    $args += $CMakeArgs
    cmake @args
    if ($LASTEXITCODE) { throw "configure failed" }
}
if ($Target) { cmake --build $build --target $Target } else { cmake --build $build }
if ($LASTEXITCODE) { throw "build failed" }
