# Packs the staging folder into a single self-contained setup
# (installer/JilChatSetup.cs). Uses only the C# compiler that ships with
# Windows, so it needs no extra tools.
if (Test-Path -PathType Container JilChatChatterino) {
    $stagingDirName = "JilChatChatterino"
}
elseif (Test-Path -PathType Container Chatterino2) {
    $stagingDirName = "Chatterino2"
}
else {
    Write-Error "Couldn't find a staging folder called 'JilChatChatterino' or 'Chatterino2' in the current directory.";
    exit 1
}

# Check if we're on a tag
$OldErrorActionPref = $ErrorActionPreference;
$ErrorActionPreference = 'Continue';
git describe --exact-match --match 'v*' *> $null;
$isTagged = $?;
$ErrorActionPreference = $OldErrorActionPref;

if ($isTagged) {
    # This is a release.
    # Make sure, any existing `modes` file is overwritten for the user,
    # for example when updating from nightly to stable.
    Write-Output "" | Out-File "$stagingDirName/modes" -Encoding ASCII;
    $installerBaseName = "JilChatChatterino.Installer";
}
else {
    Write-Output nightly | Out-File "$stagingDirName/modes" -Encoding ASCII;
    $installerBaseName = "JilChatChatterino.Nightly.Installer";
}

$architecture = [System.Runtime.InteropServices.RuntimeInformation]::OSArchitecture.ToString().ToLower()
if ($architecture -eq 'arm64') {
    $installerBaseName = "Experimental-ARM64-$installerBaseName"
}

if ($Env:GITHUB_OUTPUT) {
    # This is used in CI when creating the artifact
    "C2_INSTALLER_BASE_NAME=$installerBaseName" >> "$Env:GITHUB_OUTPUT"
}

$ErrorActionPreference = 'Stop'
$root = (Resolve-Path "$PSScriptRoot\..").Path
$sources = Join-Path $PSScriptRoot 'installer'
$work = Join-Path $pwd.Path 'installer-work'
$output = Join-Path $pwd.Path "$installerBaseName.exe"

$csc = Join-Path $Env:WINDIR 'Microsoft.NET\Framework64\v4.0.30319\csc.exe'
if (-not (Test-Path $csc)) { $csc = Join-Path $Env:WINDIR 'Microsoft.NET\Framework\v4.0.30319\csc.exe' }
if (-not (Test-Path $csc)) { Write-Error "The C# compiler of Windows wasn't found."; exit 1 }

if (Test-Path $work) { Remove-Item $work -Recurse -Force -Confirm:$false }
New-Item -ItemType Directory -Path $work | Out-Null

# The Visual C++ runtime, installed by the setup when Windows has an older one
if ($null -eq $Env:VCToolsRedistDir) {
    Write-Error "VCToolsRedistDir is not set. Forgot to set Visual Studio environment variables?";
    exit 1
}
$vcRedist = Join-Path $Env:VCToolsRedistDir "vc_redist.$architecture.exe"
$vcrtInfo = Join-Path $work 'vcrt.txt'
"$architecture $((Get-Item $vcRedist).VersionInfo.FileMinorPart)" | Out-File $vcrtInfo -Encoding ASCII -NoNewline

# The folder's contents, not the folder itself
$zip = Join-Path $work 'app.zip'
7z a -tzip -mx=7 $zip ".\$stagingDirName\*" | Out-Null
if ($LASTEXITCODE -ne 0) { Write-Error "Packing $stagingDirName failed."; exit 1 }

if (Test-Path $output) { Remove-Item $output -Force -Confirm:$false }
$arguments = @(
    '/nologo', '/optimize+', '/target:winexe', '/platform:anycpu',
    "/out:$output",
    "/win32icon:$(Join-Path $root 'resources\icon.ico')",
    "/win32manifest:$(Join-Path $sources 'setup.manifest')",
    "/resource:$zip,app.zip",
    "/resource:$(Join-Path $root 'resources\icon.png'),logo.png",
    "/resource:$(Join-Path $sources 'uninstall.ps1'),uninstall.ps1",
    "/resource:$vcRedist,vc_redist.exe",
    "/resource:$vcrtInfo,vcrt.txt",
    '/r:System.dll', '/r:System.Core.dll', '/r:System.Drawing.dll', '/r:System.Windows.Forms.dll',
    '/r:System.IO.Compression.dll', '/r:Microsoft.CSharp.dll',
    (Join-Path $sources 'JilChatSetup.cs')
)
$result = & $csc @arguments 2>&1
if ($LASTEXITCODE -ne 0 -or -not (Test-Path $output)) {
    $result | ForEach-Object { "$_" } | Select-Object -First 30
    Write-Error 'Compiling the setup failed.'
    exit 1
}
Remove-Item $work -Recurse -Force -Confirm:$false
"Created $output ($([int]((Get-Item $output).Length / 1MB)) MB)"
