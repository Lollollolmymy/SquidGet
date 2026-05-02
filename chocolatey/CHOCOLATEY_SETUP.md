# Chocolatey Package Setup Guide

## Files Created
- `squidget.nuspec` - Package metadata and information
- `tools/chocolateyInstall.ps1` - Installation script
- `tools/chocolateyUninstall.ps1` - Uninstallation script

## Setup Steps

### 1. Install Chocolatey (if not already installed)
```powershell
Set-ExecutionPolicy Bypass -Scope Process -Force; [System.Net.ServicePointManager]::SecurityProtocol = [System.Net.ServicePointManager]::SecurityProtocol -bor 3072; iex ((New-Object System.Net.WebClient).DownloadString('https://community.chocolatey.org/install.ps1'))
```

### 2. Install Chocolatey CLI Tools
```powershell
choco install chocolatey-core.extension
```

### 3. Package the Application
```powershell
# Navigate to chocolatey directory
cd chocolatey

# Create the .nupkg file
choco pack
```

### 4. Test the Package Locally
```powershell
# Test installation
choco install squidget --source .

# Test uninstallation
choco uninstall squidget
```

### 5. Push to Chocolatey Gallery
```powershell
# Push to community repository
choco push squidget.1.0.0.nupkg --source https://push.chocolatey.org/
```

## Requirements for Chocolatey Submission

1. **API Key:** Get your API key from https://chocolatey.org/account
2. **Account Setup:** Create a free Chocolatey account
3. **Package Validation:** Ensure package passes all validation rules

## Package Features

- **Portable installation** - No admin rights required
- **Desktop shortcut** - Automatically created
- **Command line access** - Available in PATH
- **Clean uninstall** - Removes all files and shortcuts

## After Approval

Users can install SquidGet with:
```powershell
choco install squidget
```

Or upgrade with:
```powershell
choco upgrade squidget
```

## Automation

You can automate future updates with GitHub Actions:
- Auto-pack on release
- Auto-push to Chocolatey
- Version management
