# Run the game, capture its window after N seconds to a PNG, then stop it.
#   powershell -ExecutionPolicy Bypass -File tools\capture_window.ps1 -Seconds 15 -Out build\shot.png [-Exe build\windows\red_two.exe] [-ExeArgs "--disc orig\GSWE64.iso"] [-Shots 3 -Interval 2]
param(
    [string]$Exe = "build\windows\red_two.exe",
    [string]$ExeArgs = "--disc orig\GSWE64.iso",
    [double]$Seconds = 15,
    [string]$Out = "build\shot.png",
    [int]$Shots = 1,
    [double]$Interval = 2,
    [string]$Log = "build\capture_run.log",
    [ValidateSet("print", "screen")][string]$Method = "print",
    [switch]$Sound
)
$ErrorActionPreference = "Stop"
Add-Type -AssemblyName System.Drawing
Add-Type @"
using System;
using System.Runtime.InteropServices;
public static class R2Win {
    [StructLayout(LayoutKind.Sequential)] public struct RECT { public int Left, Top, Right, Bottom; }
    [DllImport("user32.dll")] public static extern bool GetClientRect(IntPtr h, out RECT r);
    [DllImport("user32.dll")] public static extern bool PrintWindow(IntPtr h, IntPtr hdc, uint flags);
    [StructLayout(LayoutKind.Sequential)] public struct POINT { public int X, Y; }
    [DllImport("user32.dll")] public static extern bool ClientToScreen(IntPtr h, ref POINT p);
    [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr h);
    [DllImport("user32.dll")] public static extern bool SetProcessDPIAware();
}
"@

[void][R2Win]::SetProcessDPIAware()
# Background mode: no console window of its own (-NoNewWindow), and the game window opens
# behind everything without taking focus (R2_BACKGROUND). PrintWindow still captures it.
$env:R2_BACKGROUND = "1"
# Silent unless asked: the user works meanwhile (R2_NO_AUDIO); pass -Sound to hear it.
if (-not $Sound -and -not $env:R2_NO_AUDIO) { $env:R2_NO_AUDIO = "1"; $setNoAudio = $true }
$p = Start-Process -FilePath $Exe -ArgumentList $ExeArgs -PassThru -NoNewWindow -RedirectStandardError $Log
Remove-Item Env:R2_BACKGROUND
if ($setNoAudio) { Remove-Item Env:R2_NO_AUDIO }
try {
    Start-Sleep -Milliseconds ([int]($Seconds * 1000))
    for ($i = 0; $i -lt $Shots; $i++) {
        $p.Refresh()
        if ($p.HasExited) { throw "the game exited (code $($p.ExitCode)); see $Log" }
        $h = $p.MainWindowHandle
        if ($h -eq [IntPtr]::Zero) { throw "no window" }
        $r = New-Object R2Win+RECT
        [void][R2Win]::GetClientRect($h, [ref]$r)
        $w = [Math]::Max(1, $r.Right - $r.Left); $hgt = [Math]::Max(1, $r.Bottom - $r.Top)
        $bmp = New-Object System.Drawing.Bitmap $w, $hgt
        $g = [System.Drawing.Graphics]::FromImage($bmp)
        if ($Method -eq "print") {
            $hdc = $g.GetHdc()
            [void][R2Win]::PrintWindow($h, $hdc, 3)  # PW_CLIENTONLY | PW_RENDERFULLCONTENT
            $g.ReleaseHdc($hdc)
        } else {
            # copy from the screen: brings the window to the front and can catch other windows
            # (avoid; kept for debugging PrintWindow problems)
            [void][R2Win]::SetForegroundWindow($h)
            Start-Sleep -Milliseconds 150
            $pt = New-Object R2Win+POINT
            [void][R2Win]::ClientToScreen($h, [ref]$pt)
            $g.CopyFromScreen($pt.X, $pt.Y, 0, 0, (New-Object System.Drawing.Size $w, $hgt))
        }
        $name = if ($Shots -gt 1) { [IO.Path]::ChangeExtension($Out, $null).TrimEnd('.') + "_$i.png" } else { $Out }
        $bmp.Save($name, [System.Drawing.Imaging.ImageFormat]::Png)
        $g.Dispose(); $bmp.Dispose()
        Write-Output "saved $name (${w}x$hgt)"
        if ($i + 1 -lt $Shots) { Start-Sleep -Milliseconds ([int]($Interval * 1000)) }
    }
} finally {
    if (-not $p.HasExited) { Stop-Process -Id $p.Id -Force }
}
