#Requires -Version 7.2
param(
    [Parameter(Mandatory)][string]$WdkRoot,
    [Parameter(Mandatory)][string]$Output
)
$ErrorActionPreference='Stop'
$source=[IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
$outputRoot=[IO.Path]::GetFullPath($Output)
if ($outputRoot.Equals($source,[StringComparison]::OrdinalIgnoreCase) -or
    $outputRoot.StartsWith($source+[IO.Path]::DirectorySeparatorChar,[StringComparison]::OrdinalIgnoreCase)) { throw 'Keep build output outside the source checkout.' }
[void][IO.Directory]::CreateDirectory($outputRoot)
$env:TEMP=$outputRoot;$env:TMP=$outputRoot
$vswhere=Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio/Installer/vswhere.exe'
$vs=(& $vswhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath)
if (!$vs) { throw 'Existing Visual C++ tools are required; this script installs nothing.' }
$msvc=Get-ChildItem -LiteralPath (Join-Path $vs 'VC/Tools/MSVC') -Directory | Sort-Object Name -Descending | Select-Object -First 1
$compiler=Join-Path $msvc.FullName 'bin/Hostx64/x64/cl.exe'
$linker=Join-Path $msvc.FullName 'bin/Hostx64/x64/link.exe'
$sdk=Join-Path ${env:ProgramFiles(x86)} 'Windows Kits/10'
$version='10.0.26100.0'
$wdk=Join-Path ([IO.Path]::GetFullPath($WdkRoot)) 'c'
$trace=Join-Path $sdk "bin/$version/x64/tracewpp.exe"
$include=@((Join-Path $wdk "Include/$version/km"),(Join-Path $wdk "Include/$version/shared"),
    (Join-Path $wdk 'Include/wdf/kmdf/1.15'),(Join-Path $sdk "Include/$version/shared"),(Join-Path $sdk "Include/$version/ucrt"),
    (Join-Path $msvc.FullName 'include'),(Join-Path $source 'UsbDk'),$outputRoot)
foreach ($path in @($compiler,$linker,$trace)+$include) { if (!(Test-Path -LiteralPath $path)) { throw "Missing build dependency: $path" } }
$project=[xml](Get-Content -LiteralPath (Join-Path $source 'UsbDk/UsbDk.vcxproj') -Raw)
$files=@($project.Project.ItemGroup.ClCompile | Where-Object Include | ForEach-Object { Join-Path $source "UsbDk/$($_.Include)" })
$files+=@(Get-ChildItem -LiteralPath (Join-Path $source 'UsbDk') -Filter '*.cpp' | Where-Object FullName -NotIn $files | ForEach-Object FullName)
$objects=@()
Push-Location (Join-Path $source 'UsbDk')
try {
    foreach ($file in $files) {
        & $trace '-km' "-cfgdir:$(Join-Path $sdk "bin/$version/WppConfig/Rev1")" "-scan:$(Join-Path $source 'UsbDk/Trace.h')" "-odir:$outputRoot" $file
        if ($LASTEXITCODE) { throw "Trace preprocessing failed: $file" }
        $object=Join-Path $outputRoot ([IO.Path]::GetFileNameWithoutExtension($file)+'.obj')
        $arguments=@('/nologo','/c','/kernel','/Zl','/GR-','/EHs-c-','/GS','/Oi','/W3','/std:c++14',
            '/D_WIN64','/D_AMD64_','/DAMD64','/DWINNT=1','/D_WIN32_WINNT=0x0A00','/DWINVER=0x0A00',
            '/DNTDDI_VERSION=0x0A000000','/DUNICODE','/D_UNICODE','/DWDF_MAJOR_VERSION=1','/DWDF_MINOR_VERSION=15')
        $arguments+=@($include | ForEach-Object { '/I'+$_ })
        & $compiler @arguments "/Fo$object" $file
        if ($LASTEXITCODE) { throw "Kernel compilation failed: $file" }
        $objects+=$object
    }
    $libraries=@((Join-Path $wdk "Lib/$version/km/x64"),(Join-Path $wdk 'Lib/wdf/kmdf/x64/1.15'))
    $arguments=@('/nologo','/DRIVER','/SUBSYSTEM:NATIVE,10.00','/ENTRY:FxDriverEntry','/NODEFAULTLIB',
        '/MACHINE:X64',('/OUT:'+(Join-Path $outputRoot 'UsbDk.sys')))
    $arguments+=@($libraries | ForEach-Object { '/LIBPATH:'+$_ })
    & $linker @arguments @objects ntoskrnl.lib hal.lib wmilib.lib WdfLdr.lib WdfDriverEntry.lib usbdex.lib usbd.lib ntstrsafe.lib wdmsec.lib BufferOverflowFastFailK.lib libcntpr.lib
    if ($LASTEXITCODE) { throw 'Unsigned driver link failed.' }
    $helperOutput=Join-Path $outputRoot 'helper'
    [void][IO.Directory]::CreateDirectory($helperOutput)
    $helperInclude=@((Join-Path $source 'UsbDk'),(Join-Path $source 'UsbDkHelper'),
        (Join-Path $wdk 'Include/wdf/kmdf/1.15'),(Join-Path $sdk "Include/$version/um"),
        (Join-Path $sdk "Include/$version/shared"),(Join-Path $sdk "Include/$version/ucrt"),(Join-Path $msvc.FullName 'include'))
    $project=[xml](Get-Content -LiteralPath (Join-Path $source 'UsbDkHelper/UsbDkHelper.vcxproj') -Raw)
    $objects=@()
    foreach ($item in @($project.Project.ItemGroup.ClCompile | Where-Object Include)) {
        $file=Join-Path $source "UsbDkHelper/$($item.Include)"
        $object=Join-Path $helperOutput ([IO.Path]::GetFileNameWithoutExtension($file)+'.obj')
        $arguments=@('/nologo','/c','/MT','/EHsc','/W3','/std:c++14','/DBUILD_DLL',
            '/DUNICODE','/D_UNICODE','/DKMDF_VERSION_MAJOR=1','/DKMDF_VERSION_MINOR=15')
        $arguments+=@($helperInclude | ForEach-Object { '/I'+$_ })
        & $compiler @arguments "/Fo$object" $file
        if ($LASTEXITCODE) { throw "Helper compilation failed: $file" }
        $objects+=$object
    }
    $libraries=@((Join-Path $sdk "Lib/$version/um/x64"),(Join-Path $sdk "Lib/$version/ucrt/x64"),(Join-Path $msvc.FullName 'lib/x64'))
    $arguments=@('/nologo','/DLL','/MACHINE:X64',('/OUT:'+(Join-Path $outputRoot 'UsbDkHelper.dll')))
    $arguments+=@($libraries | ForEach-Object { '/LIBPATH:'+$_ })
    & $linker @arguments @objects kernel32.lib user32.lib advapi32.lib setupapi.lib rpcrt4.lib cfgmgr32.lib
    if ($LASTEXITCODE) { throw 'Unsigned helper link failed.' }
    Get-FileHash -LiteralPath (Join-Path $outputRoot 'UsbDk.sys') -Algorithm SHA256
    Get-FileHash -LiteralPath (Join-Path $outputRoot 'UsbDkHelper.dll') -Algorithm SHA256
    'UNSIGNED build only. No driver was signed, installed or loaded.'
} finally { Pop-Location }
