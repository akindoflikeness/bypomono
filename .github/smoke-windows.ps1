# Starts the staged standalone, captures the whole screen, and fails if the
# process is no longer running when the capture is done.
param(
    [Parameter(Mandatory = $true)][string]$Exe,
    [Parameter(Mandatory = $true)][string]$Shot
)

$ErrorActionPreference = 'Stop'

$p = Start-Process -FilePath $Exe -WorkingDirectory (Split-Path -Parent $Exe) -PassThru
try {
    Start-Sleep -Seconds 6

    Add-Type -AssemblyName System.Windows.Forms
    Add-Type -AssemblyName System.Drawing
    $screen = [System.Windows.Forms.SystemInformation]::VirtualScreen
    $bmp = New-Object System.Drawing.Bitmap $screen.Width, $screen.Height
    $g = [System.Drawing.Graphics]::FromImage($bmp)
    $g.CopyFromScreen($screen.Location, [System.Drawing.Point]::Empty, $screen.Size)
    $bmp.Save($Shot, [System.Drawing.Imaging.ImageFormat]::Png)
    $g.Dispose()
    $bmp.Dispose()

    tasklist /FI "PID eq $($p.Id)"
    if ($p.HasExited) {
        throw "standalone exited with $($p.ExitCode) before termination"
    }
    Get-Process -Id $p.Id | Format-Table Id, ProcessName, Responding
}
finally {
    if (-not $p.HasExited) { Stop-Process -Id $p.Id -Force }
}
