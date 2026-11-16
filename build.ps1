$ErrorActionPreference = 'Stop'

$projectRoot = Split-Path -Parent $MyInvocation.MyCommand.Path
$outputRoot = Join-Path $projectRoot 'build\Release'
$objectRoot = Join-Path $projectRoot 'build\obj'
New-Item -ItemType Directory -Force $outputRoot, $objectRoot | Out-Null

$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
if (-not (Test-Path $vswhere)) { throw 'Visual Studio Build Tools were not found.' }
$vsRoot = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (-not $vsRoot) { throw 'The Visual C++ x64 build tools were not found.' }
$vcvars = Join-Path $vsRoot 'VC\Auxiliary\Build\vcvars64.bat'

$outputDll = Join-Path $outputRoot 'popn_deka_touch.dll'
$outputLib = Join-Path $outputRoot 'popn_deka_touch.lib'
$compile = 'cl /nologo /LD /O2 /W3 /EHsc /std:c++17 /MT /utf-8 /DWIN32 /D_WINDOWS /D_USRDLL "' +
    (Join-Path $projectRoot 'src\plugin.cpp') + '" /Fo"' + $objectRoot + '\\" /link /OUT:"' +
    $outputDll + '" /IMPLIB:"' + $outputLib + '" /DYNAMICBASE /NXCOMPAT user32.lib gdi32.lib gdiplus.lib'
& cmd.exe /d /s /c ('"' + $vcvars + '" >nul && ' + $compile)
if ($LASTEXITCODE -ne 0) { throw "Build failed with exit code $LASTEXITCODE." }

Copy-Item -LiteralPath (Join-Path $projectRoot 'popn_deka_touch.ini') -Destination $outputRoot -Force
Write-Host "Built: $outputDll"
