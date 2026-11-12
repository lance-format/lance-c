@echo off
setlocal

set "SCRIPT=%~dp0limit-windows-exports.ps1"
"%SystemRoot%\System32\WindowsPowerShell\v1.0\powershell.exe" ^
  -NoLogo -NoProfile -NonInteractive -ExecutionPolicy Bypass ^
  -File "%SCRIPT%" %*
exit /b %ERRORLEVEL%
