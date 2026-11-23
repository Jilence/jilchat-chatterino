$taskRoot = Split-Path $PSScriptRoot -Parent
$taskExe = Join-Path $taskRoot 'build-canvas/bin/JilChatChatterino.exe'
if (-not (Test-Path -LiteralPath $taskExe)) { throw 'Build the Canvas dev version first.' }
$taskRunning = Get-Process JilChatChatterino -ErrorAction SilentlyContinue | Where-Object { $_.Path -eq $taskExe }
if ($taskRunning) { Write-Output 'Canvas dev is already running.'; return }
$taskPreviousEnable = $env:JILCHAT_ENABLE_CANVAS
$taskPreviousDisable = $env:JILCHAT_DISABLE_CANVAS
try {
    $env:JILCHAT_ENABLE_CANVAS = '1'
    $env:JILCHAT_DISABLE_CANVAS = '0'
    Start-Process -FilePath $taskExe -ArgumentList '--remote-restart' -WorkingDirectory (Split-Path $taskExe)
} finally {
    $env:JILCHAT_ENABLE_CANVAS = $taskPreviousEnable
    $env:JILCHAT_DISABLE_CANVAS = $taskPreviousDisable
}
