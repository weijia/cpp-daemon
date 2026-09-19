Add-Type -AssemblyName System.Drawing
$path = "C:\codes\tray-app\cpp-daemon\build\Release\cpp-daemon.exe"
$icon = [System.Drawing.Icon]::ExtractAssociatedIcon($path)
if ($icon -ne $null) {
    Write-Host ("OK icon embedded, size=" + $icon.Width + "x" + $icon.Height)
} else {
    Write-Host "NO ICON RESOURCE"
}
