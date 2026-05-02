$ErrorActionPreference = 'Stop'

# Remove desktop shortcut
$desktopPath = [Environment]::GetFolderPath('Desktop')
$shortcutPath = Join-Path $desktopPath 'SquidGet.lnk'

if (Test-Path $shortcutPath) {
  Remove-Item $shortcutPath -Force
  Write-Host "Desktop shortcut removed."
}

# Remove executable
$exePath = Join-Path $env:ChocolateyInstall 'squidget-windows.exe'
if (Test-Path $exePath) {
  Remove-Item $exePath -Force
  Write-Host "SquidGet executable removed."
}

# Remove batch file
$batPath = Join-Path $env:ChocolateyInstall 'squidget.bat'
if (Test-Path $batPath) {
  Remove-Item $batPath -Force
  Write-Host "Batch file removed."
}

# Remove shortcut script
$shortcutScript = Join-Path $env:ChocolateyInstall 'create-shortcut.vbs'
if (Test-Path $shortcutScript) {
  Remove-Item $shortcutScript -Force
  Write-Host "Shortcut script removed."
}

Write-Host "SquidGet uninstalled successfully!"
