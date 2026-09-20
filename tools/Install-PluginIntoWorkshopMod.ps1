param(
    [Parameter(Mandatory=$true)][string]$ModDirectory,
    [Parameter(Mandatory=$true)][string]$PluginDll,
    [Parameter(Mandatory=$true)][string]$PluginId,
    [Parameter(Mandatory=$true)][string]$PluginName,
    [Parameter(Mandatory=$true)][string]$PluginVersion,
    [string]$RuntimeLua
)

$ErrorActionPreference = 'Stop'
$mod = (Resolve-Path $ModDirectory).Path
$dll = (Resolve-Path $PluginDll).Path
$native = Join-Path $mod 'Native'
New-Item -ItemType Directory -Path $native -Force | Out-Null

$dllName = [IO.Path]::GetFileName($dll)
Copy-Item $dll (Join-Path $native $dllName) -Force
if ($RuntimeLua) {
    Copy-Item (Resolve-Path $RuntimeLua).Path (Join-Path $native 'KeybindRuntime.lua') -Force
}

$manifest = [ordered]@{
    schemaVersion = 1
    id = $PluginId
    name = $PluginName
    version = $PluginVersion
    dll = $dllName
}
$manifest | ConvertTo-Json | Set-Content (Join-Path $native 'plugin.json') -Encoding UTF8

Write-Host "Installed native plugin into: $native"
Write-Host "Manifest: $(Join-Path $native 'plugin.json')"
