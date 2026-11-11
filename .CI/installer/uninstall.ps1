# SPDX-FileCopyrightText: 2026 Contributors to Chatterino <https://chatterino.com>
#
# SPDX-License-Identifier: MIT

# Removes JilChat Chatterino's program files, shortcuts and Installed apps
# entry. The settings in %APPDATA%\Chatterino2 are kept, so a reinstall picks
# them up.
$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.Windows.Forms
$folder = $PSScriptRoot
$name = 'JilChat Chatterino'
$key = 'Software\Microsoft\Windows\CurrentVersion\Uninstall\JilChatChatterino'

if (-not (Test-Path (Join-Path $folder 'JilChatChatterino.exe'))) {
    [void][System.Windows.Forms.MessageBox]::Show("$name was not found in this folder.", $name)
    exit 1
}

# Installed for all users (e.g. in Program Files): that needs an administrator
$isAdmin = ([Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()).IsInRole(
    [Security.Principal.WindowsBuiltInRole]::Administrator)
$machine = Test-Path "HKLM:\$key"
if ($machine -and -not $isAdmin) {
    Start-Process powershell.exe -Verb RunAs -ArgumentList "-NoProfile -ExecutionPolicy Bypass -File `"$PSCommandPath`""
    exit 0
}

$answer = [System.Windows.Forms.MessageBox]::Show(
    "Remove $name from this computer?`n`nYour settings stay in %APPDATA%\Chatterino2.",
    $name, 'YesNo', 'Question')
if ($answer -ne 'Yes') { exit 0 }

if (Get-Process JilChatChatterino -ErrorAction SilentlyContinue | Where-Object { $_.Path -like "$folder\*" }) {
    [void][System.Windows.Forms.MessageBox]::Show("Exit $name from its tray icon first, then try again.", $name)
    exit 1
}

$places = @(
    [Environment]::GetFolderPath('Desktop'), [Environment]::GetFolderPath('Programs'),
    [Environment]::GetFolderPath('CommonDesktopDirectory'), [Environment]::GetFolderPath('CommonPrograms')
)
foreach ($place in $places) {
    Remove-Item (Join-Path $place "$name.lnk") -Force -ErrorAction SilentlyContinue
}
Remove-Item "HKCU:\$key" -Recurse -Force -ErrorAction SilentlyContinue
Remove-Item "HKLM:\$key" -Recurse -Force -ErrorAction SilentlyContinue

# This script is inside the folder, so a separate process deletes it after exit
Start-Process cmd.exe -WindowStyle Hidden -ArgumentList "/c timeout /t 2 /nobreak >nul & rmdir /s /q `"$folder`""
