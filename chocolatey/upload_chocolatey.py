#!/usr/bin/env python3
"""
Chocolatey Package Upload Script
Uploads the .nupkg file to Chocolatey gallery
"""

import requests
import os
import sys

def upload_to_chocolatey(api_key, nupkg_file):
    """Upload .nupkg file to Chocolatey gallery"""
    
    url = "https://push.chocolatey.org/"
    
    headers = {
        'Authorization': f'Bearer {api_key}',
        'Content-Type': 'application/octet-stream'
    }
    
    try:
        with open(nupkg_file, 'rb') as f:
            response = requests.put(url, headers=headers, data=f)
        
        if response.status_code == 200:
            print("✅ Successfully uploaded to Chocolatey!")
            print(f"Response: {response.text}")
            return True
        else:
            print(f"❌ Upload failed with status {response.status_code}")
            print(f"Response: {response.text}")
            return False
            
    except Exception as e:
        print(f"❌ Error uploading: {e}")
        return False

def main():
    if len(sys.argv) != 3:
        print("Usage: python upload_chocolatey.py <api_key> <nupkg_file>")
        print("Get your API key from: https://chocolatey.org/account")
        sys.exit(1)
    
    api_key = sys.argv[1]
    nupkg_file = sys.argv[2]
    
    if not os.path.exists(nupkg_file):
        print(f"❌ File not found: {nupkg_file}")
        sys.exit(1)
    
    print(f"📦 Uploading {nupkg_file} to Chocolatey...")
    success = upload_to_chocolatey(api_key, nupkg_file)
    
    if success:
        print("🎉 SquidGet is now available on Chocolatey!")
        print("Users can install with: choco install squidget")
    else:
        print("❌ Upload failed. Check your API key and try again.")
        sys.exit(1)

if __name__ == "__main__":
    main()
