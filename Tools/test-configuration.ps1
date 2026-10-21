#Requires -Version 7.2
param(
    [Parameter(Mandatory)][string]$WdkRoot,
    [Parameter(Mandatory)][string]$BuildOutput
)
$ErrorActionPreference = 'Stop'
$source = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
$output = [IO.Path]::GetFullPath($BuildOutput)
$env:TEMP = $output
$env:TMP = $output
$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio/Installer/vswhere.exe'
$vs = & $vswhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
$msvc = Get-ChildItem -LiteralPath (Join-Path $vs 'VC/Tools/MSVC') -Directory | Sort-Object Name -Descending | Select-Object -First 1
$compiler = Join-Path $msvc.FullName 'bin/Hostx64/x64/cl.exe'
$sdk = Join-Path ${env:ProgramFiles(x86)} 'Windows Kits/10'
$version = '10.0.26100.0'
$include = @((Join-Path $msvc.FullName 'include'), (Join-Path $sdk "Include/$version/ucrt"),
    (Join-Path $sdk "Include/$version/shared"), (Join-Path $sdk "Include/$version/um"),
    (Join-Path $WdkRoot 'c/Include/wdf/kmdf/1.15'), (Join-Path $source 'UsbDk'), (Join-Path $source 'UsbDkHelper'))
$libraries = @((Join-Path $msvc.FullName 'lib/x64'), (Join-Path $sdk "Lib/$version/ucrt/x64"), (Join-Path $sdk "Lib/$version/um/x64"))
foreach ($test in @('configuration','helper-configuration')) {
    $arguments = @('/nologo','/MT','/EHsc','/W4','/std:c++14','/DUNICODE','/D_UNICODE')
    $arguments += @($include | ForEach-Object { '/I'+$_ })
    $arguments += @(('/Fo'+(Join-Path $output "$test-test.obj")), ('/Fe'+(Join-Path $output "$test-test.exe")), (Join-Path $source "Tests/$test.cpp"))
    if ($test -eq 'helper-configuration') {
        $arguments += @('RedirectorAccess','DriverFile','Exception','tstrings','Utils' | ForEach-Object { Join-Path $output "helper/$_.obj" })
    }
    $arguments += '/link'
    $arguments += @($libraries | ForEach-Object { '/LIBPATH:'+$_ })
    $arguments += @('kernel32.lib','user32.lib','advapi32.lib')
    & $compiler @arguments
    if ($LASTEXITCODE) { throw "Fixture compile failed: $test" }
    & (Join-Path $output "$test-test.exe")
    if ($LASTEXITCODE) { throw "Fixture failed: $test" }
}
