# Packs a Windows build into a zip: the program, its symbols (crash call stacks in the session
# logs), the DLLs next to it, the license and a short readme.
#   powershell -ExecutionPolicy Bypass -File tools\make_release_win.ps1 [-BuildDir build\windows] [-Out build\RedTwo-windows-x64.zip]
# The program holds the recompiled game: the zip is for the person whose disc it was built from.
param(
    [string]$BuildDir = "",
    [string]$Out = ""
)
$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot
if (-not $BuildDir) { $BuildDir = Join-Path $root "build\windows" }
if (-not $Out) { $Out = Join-Path $root "build\RedTwo-windows-x64.zip" }
# Downloads install newer releases themselves (update.cpp) only when built with -DR2_RELEASE=ON
# (powershell -File tools\build.ps1 -BuildDir build\release-win -Configure -CMakeArgs -DR2_RELEASE=ON).
$cache = Join-Path $BuildDir "CMakeCache.txt"
if (-not (Select-String -Path $cache -Pattern '^R2_RELEASE:BOOL=ON' -Quiet)) {
    throw "$BuildDir is not a release build (-DR2_RELEASE=ON): its downloads could not update themselves"
}
$stage = Join-Path $root "build\release-windows\Red Two"
if (Test-Path (Split-Path $stage)) { Remove-Item -Recurse -Force (Split-Path $stage) }
New-Item -ItemType Directory -Force $stage | Out-Null

Copy-Item (Join-Path $BuildDir "red_two.exe"), (Join-Path $BuildDir "red_two.pdb") $stage
Get-ChildItem $BuildDir -Filter *.dll | Copy-Item -Destination $stage

# The Visual C++ runtime the build was made with, next to the program (Microsoft's app-local
# deployment; the loader takes these before System32's). Without it a PC lacking the runtime got
# "MSVCP140.dll was not found", and one with an older runtime (before 14.40, VS 2022 17.10)
# crashed silently at the first std::mutex lock (issue #14: "it doesn't display anything").
$vswhere = Join-Path ${env:ProgramFiles(x86)} "Microsoft Visual Studio\Installer\vswhere.exe"
$vs = & $vswhere -latest -products * -property installationPath
$redist = Get-ChildItem (Join-Path $vs "VC\Redist\MSVC") -Directory | Where-Object { $_.Name -match '^14\.' } |
    Sort-Object { [version]$_.Name } | Select-Object -Last 1
$crt = Join-Path $redist.FullName "x64\Microsoft.VC143.CRT"
foreach ($dll in "msvcp140.dll", "msvcp140_atomic_wait.dll", "vcruntime140.dll", "vcruntime140_1.dll") {
    Copy-Item (Join-Path $crt $dll) $stage
}
"Visual C++ runtime {0}" -f $redist.Name

# Portable: settings, the memory card, save states, logs, the shader cache, texture packs and
# codes go into user\ next to red_two.exe (mods.cpp portableDir), so the folder can live anywhere
New-Item -ItemType Directory -Force (Join-Path $stage "user") | Out-Null
@"
Red Two keeps everything of yours in this folder: settings.ini, input.json (controls), saves\ (the
memory card), states\ (save states), logs\ (a log per session), textures\ (texture packs, laid
out as in Dolphin's Load\Textures), codes.ini, and the shader cache. Move or copy the whole
Red Two folder (on another drive, a NAS, a USB stick) and it comes along.

Updating: unzip the new version, then copy this folder over the new one's.
"@ | Set-Content -Encoding utf8 (Join-Path $stage "user\README.txt")
Copy-Item (Join-Path $root "LICENSE") (Join-Path $stage "LICENSE.txt")
# The starter shader list (tools\make_shader_seed.py): Aurora merges it into the player's shader
# cache at every start, so a new install compiles those shaders before the game begins.
$seed = Join-Path $root "build\initial_pipeline_cache.db"
if (Test-Path $seed) { Copy-Item $seed $stage }
else { Write-Warning "no build\initial_pipeline_cache.db: the zip has no starter shader list (python tools\make_shader_seed.py)" }
python (Join-Path $root "tools\third_party_notices.py") $BuildDir (Join-Path $stage "THIRD_PARTY_NOTICES.txt")
if ($LASTEXITCODE) { throw "third_party_notices.py failed" }
@"
Red Two - Star Wars: Rogue Squadron II - Rogue Leader, recompiled for PC

Requirements
  64-bit Windows 10 or 11, a CPU with AVX2 (Intel Haswell / AMD Zen or newer), a graphics card
  with Direct3D 12 or Vulkan.

Your own disc image
  Red Two needs your Rogue Leader disc (USA, GSWE64, revision 0) as .iso, .gcm, .rvz or .wia
  (Dolphin's formats), wherever it is: start red_two.exe and choose it on the launcher's disc
  page (type or paste its path, the file dialog, or the built-in browser). Red Two remembers
  it. One next to red_two.exe is found by itself; --disc "<path>" overrides for one start.

Playing
  F1 or the controller's Back/View button opens the menu: display, controls, cheats and the
  game's passcode unlocks, RetroAchievements (log in on the ACHIEVEMENTS page), mods, reset
  and quit (first page). Save states: Back + RB / Back + LB (F5 / F8).

Where things are (portable: everything stays in this folder)
  user\ next to red_two.exe: settings, the memory card (user\saves\GSWE64_A.raw; Dolphin and
  Nintendont .raw cards of any size work there), save states, session logs, the shader cache,
  texture packs (user\textures\GSWE64\) and codes. Coming from 1.0, the first start copies your
  settings and controls into it, and the memory card if you unzipped over the old folder (else
  copy its saves\GSWE64_A.raw to user\saves\); the old files stay where they were.
  The Microsoft Visual C++ runtime DLLs come with it: nothing to install.
"@ | Set-Content -Encoding utf8 (Join-Path $stage "README.txt")

New-Item -ItemType Directory -Force (Split-Path -Parent ([IO.Path]::GetFullPath($Out))) | Out-Null
if (Test-Path $Out) { Remove-Item -Force $Out }
Compress-Archive -Path $stage -DestinationPath $Out -CompressionLevel Optimal
"{0}: {1:N0} MB" -f $Out, ((Get-Item $Out).Length / 1MB)
