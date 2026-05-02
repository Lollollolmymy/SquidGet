$ErrorActionPreference = 'Stop'

$packageArgs = @{
  packageName   = $env:ChocolateyPackageName
  fileType      = 'zip'
  url           = 'https://github.com/Lollollolmymy/SquidGet/releases/download/v1.0.0/squidget-windows.zip'
  checksum      = '4e217d33040ce30e8311beec9d79085810322e7224b417d2ff361d7ad68563a3'
  checksumType  = 'sha256'
  unzipLocation = $env:ChocolateyInstall
}

Install-ChocolateyZipPackage @packageArgs

# Create desktop shortcut
$desktopPath = [Environment]::GetFolderPath('Desktop')
$shortcutPath = Join-Path $desktopPath 'SquidGet.lnk'
$exePath = Join-Path $env:ChocolateyInstall 'squidget-windows.exe'

Install-ChocolateyShortcut -ShortcutPath $shortcutPath -TargetPath $exePath -WorkingDirectory $env:ChocolateyInstall

# Add to PATH if not already there
$envPath = [Environment]::GetEnvironmentVariable('PATH', 'Machine')
$installPath = $env:ChocolateyInstall
if ($envPath -notlike "*$installPath*") {
  [Environment]::SetEnvironmentVariable('PATH', "$envPath;$installPath", 'Machine')
}

Write-Host "SquidGet installed successfully!"
Write-Host "Run 'squidget-windows' from command line or use the desktop shortcut."
