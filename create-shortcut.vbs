Set shell = CreateObject("WScript.Shell")
Set shortcut = shell.CreateShortcut(shell.SpecialFolders("Desktop") & "\SquidGet.lnk")
shortcut.TargetPath = shell.ExpandEnvironmentStrings("%LOCALAPPDATA%\Microsoft\WinGet\packages\Lollollolmymy.SquidGet_Microsoft.Winget.Source_1.0.0\bin\squidget-windows.exe")
shortcut.WorkingDirectory = shell.ExpandEnvironmentStrings("%LOCALAPPDATA%\Microsoft\WinGet\packages\Lollollolmymy.SquidGet_Microsoft.Winget.Source_1.0.0\bin")
shortcut.IconLocation = shell.ExpandEnvironmentStrings("%LOCALAPPDATA%\Microsoft\WinGet\packages\Lollollolmymy.SquidGet_Microsoft.Winget.Source_1.0.0\bin\squidget-windows.exe")
shortcut.Description = "SquidGet - Music Downloader"
shortcut.Save
