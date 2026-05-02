# Winget (Windows Package Manager) Setup Guide

## Overview
Winget is Microsoft's Windows Package Manager that allows users to install applications with a single command: `winget install SquidGet`

## Files Created
- `Lollollolmymy.SquidGet.yaml` - Version and installer manifest
- `Lollollolmymy.SquidGet.locale.en-US.yaml` - Package metadata and description

## Setup Steps

### 1. Create Windows Release ZIP
First, create a ZIP file with your Windows binaries:

```bash
# Create ZIP with Windows files
zip squidget-windows.zip squidget.exe squidget.bat
```

### 2. Calculate SHA256
```bash
# Calculate SHA256 of the ZIP file
shasum -a 256 squidget-windows.zip
```

### 3. Update Manifest
Replace `placeholder_sha256` in `Lollollolmymy.SquidGet.yaml` with the actual SHA256

### 4. Submit to Winget Repository

**Option A: GitHub Pull Request (Recommended)**
1. Fork: https://github.com/microsoft/winget-pkgs
2. Create folder structure: `manifests/l/Lollollolmymy/SquidGet/1.0.0/`
3. Add your YAML files to that folder
4. Submit pull request

**Option B: Microsoft Community**
1. Submit to: https://github.com/microsoft/winget-pkgs/issues/new?assignees=&labels=&template=add-new-package.yml
2. Fill out the package request form

## Required Folder Structure
```
winget-pkgs/
└── manifests/
    └── l/
        └── Lollollolmymy/
            └── SquidGet/
                └── 1.0.0/
                    ├── Lollollolmymy.SquidGet.yaml
                    └── Lollollolmymy.SquidGet.locale.en-US.yaml
```

## After Approval
Once approved, users can install SquidGet with:
```cmd
winget install SquidGet
```

## Updates
For future releases:
1. Update version numbers in manifests
2. Create new release ZIP
3. Update SHA256
4. Submit new pull request

## Automation
You can automate this process with GitHub Actions:
- Auto-generate ZIP on release
- Calculate SHA256
- Update manifests
- Submit pull request automatically
