param([switch]$LauncherOnly)

$ErrorActionPreference = 'Stop'
$exe = Join-Path $PSScriptRoot 'pc-port\out\local-play\inFAMOUS Second Son Launcher.exe'

if (-not (Test-Path -LiteralPath $exe)) {
    Add-Type -AssemblyName System.Windows.Forms
    [System.Windows.Forms.MessageBox]::Show("Launcher not found:`n$exe", 'inFAMOUS Second Son') | Out-Null
    exit 1
}

if (Get-Process iss-probe -ErrorAction SilentlyContinue) {
    exit 0
}

Start-Process -FilePath $exe -WorkingDirectory (Split-Path -Parent $exe)
if ($LauncherOnly) {
    exit 0
}

Add-Type -TypeDefinition @'
using System;
using System.Runtime.InteropServices;
public static class IssInput {
    [DllImport("user32.dll")]
    public static extern bool PostMessage(IntPtr hWnd, uint msg, IntPtr wParam, IntPtr lParam);
}
'@

$deadline = (Get-Date).AddSeconds(20)
$launcher = $null
while ((Get-Date) -lt $deadline) {
    Start-Sleep -Milliseconds 300
    $candidate = Get-Process | Where-Object {
        $_.ProcessName -like 'inFAMOUS*' -and $_.MainWindowHandle -ne [IntPtr]::Zero
    } | Select-Object -First 1
    if (-not $candidate) {
        continue
    }
    Start-Sleep -Milliseconds 1200
    $candidate.Refresh()
    if ($candidate.HasExited -or $candidate.MainWindowHandle -eq [IntPtr]::Zero) {
        continue
    }
    $launcher = $candidate
    break
}
if (-not $launcher) {
    exit 0
}

[IssInput]::PostMessage($launcher.MainWindowHandle, 0x0100, [IntPtr]0x0D, [IntPtr]::Zero) | Out-Null
Start-Sleep -Milliseconds 150
[IssInput]::PostMessage($launcher.MainWindowHandle, 0x0101, [IntPtr]0x0D, [IntPtr]::Zero) | Out-Null
