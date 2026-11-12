<#
  Builds the release packages in dist\:

    Voyeur-<version>-portable.zip   unzip anywhere and run (also what the
                                    in-game updater downloads)
    Voyeur-<version>-setup.exe      single-file installer (Inno Setup 6)

  The executables are built WITHOUT the translated game code (CMake option
  RECOMP_INTERP_ONLY): they read the game's program from the player's disc
  and run it in the 68000 interpreter, so the packages contain nothing of
  the game. hd_pack_builder.exe is tools\hd_pack.py frozen with PyInstaller
  (in a private venv under build\pyi-venv).

  Needs: CMake + Visual Studio C++ build tools, Python 3, Inno Setup 6
  (winget install JRSoftware.InnoSetup) for the installer.

    powershell -ExecutionPolicy Bypass -File tools\package.ps1 [-SkipBuilder] [-SkipInstaller]
#>
param([switch]$SkipBuilder, [switch]$SkipInstaller)
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
Set-Location $root

$ver = (Select-String -Path runtime\version.h -Pattern '#define APP_VERSION "([^"]+)"').Matches[0].Groups[1].Value
Write-Host "Packaging Voyeur $ver"

# 1. executables (interpreter-only build)
cmake -S . -B build\release -DRECOMP_INTERP_ONLY=ON | Out-Null
if ($LASTEXITCODE) { throw 'cmake configure failed' }
cmake --build build\release --config Release --target game game_hd --parallel
if ($LASTEXITCODE) { throw 'build failed' }

# 2. HD pack builder
$builder = 'build\pyi-dist\hd_pack_builder.exe'
if (-not $SkipBuilder -or -not (Test-Path $builder)) {
    if (-not (Test-Path build\pyi-venv\Scripts\python.exe)) {
        python -m venv build\pyi-venv
        & build\pyi-venv\Scripts\python.exe -m pip install --disable-pip-version-check -q pyinstaller numpy pillow opencv-python-headless
        if ($LASTEXITCODE) { throw 'pip install failed' }
    }
    & build\pyi-venv\Scripts\pyinstaller.exe --noconfirm --onefile --console --name hd_pack_builder `
        --distpath build\pyi-dist --workpath build\pyi-work --specpath build\pyi-work `
        --paths tools --hidden-import rlf_decode tools\hd_pack.py
    if ($LASTEXITCODE) { throw 'pyinstaller failed' }
}

# 3. staging folder: exactly what the user gets (no saves\, no game files)
$stage = 'dist\stage'
if (Test-Path $stage) { Remove-Item -Recurse -Force $stage }
New-Item -ItemType Directory -Force $stage | Out-Null
Copy-Item build\release\Release\voyeur.exe, build\release\Release\voyeur_hd.exe, $builder $stage
Copy-Item installer\README.txt, installer\THIRD-PARTY.txt, 'installer\Build HD video pack.cmd' $stage

# 4. portable zip (files at the top level: the updater unpacks it over the install folder)
$zip = "dist\Voyeur-$ver-portable.zip"
if (Test-Path $zip) { Remove-Item -Force $zip }
Compress-Archive -Path "$stage\*" -DestinationPath $zip -CompressionLevel Optimal
Write-Host "  $zip"

# 5. installer
if (-not $SkipInstaller) {
    $iscc = @("$env:LOCALAPPDATA\Programs\Inno Setup 6\ISCC.exe", "${env:ProgramFiles(x86)}\Inno Setup 6\ISCC.exe",
              "$env:ProgramFiles\Inno Setup 6\ISCC.exe") | Where-Object { Test-Path $_ } | Select-Object -First 1
    if (-not $iscc) { Write-Warning 'Inno Setup 6 not found (winget install JRSoftware.InnoSetup): installer skipped' }
    else {
        & $iscc /Q "/DAppVersion=$ver" "/DDistDir=$root\$stage" "/DOutDir=$root\dist" installer\voyeur.iss
        if ($LASTEXITCODE) { throw 'ISCC failed' }
        Write-Host "  dist\Voyeur-$ver-setup.exe"
    }
}
