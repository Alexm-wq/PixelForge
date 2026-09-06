@echo off
setlocal EnableExtensions EnableDelayedExpansion

rem PixelForge fallback launcher for Codex on Windows.
rem This file is copied beside PixelForge.exe as codex.cmd. It must never call
rem itself; every candidate below points outside the PixelForge build folder.

if defined PIXELFORGE_CODEX_EXE call :RUN_EXPLICIT "%PIXELFORGE_CODEX_EXE%" %*
if defined CODEX_CLI_PATH call :RUN_EXPLICIT "%CODEX_CLI_PATH%" %*

rem Standard npm / pnpm / bun / scoop shims.
call :TRY "%APPDATA%\npm\codex.cmd" %*
call :TRY "%APPDATA%\npm\codex.exe" %*
call :TRY "%LOCALAPPDATA%\pnpm\codex.exe" %*
call :TRY "%LOCALAPPDATA%\pnpm\codex.cmd" %*
call :TRY "%APPDATA%\pnpm\codex.exe" %*
call :TRY "%APPDATA%\pnpm\codex.cmd" %*
call :TRY "%USERPROFILE%\.bun\bin\codex.exe" %*
call :TRY "%USERPROFILE%\.bun\bin\codex.cmd" %*
call :TRY "%USERPROFILE%\scoop\shims\codex.exe" %*
call :TRY "%USERPROFILE%\scoop\shims\codex.cmd" %*
call :TRY "%LOCALAPPDATA%\Microsoft\WindowsApps\codex.exe" %*

rem Native executable bundled inside common npm installations.
for /d %%D in ("%APPDATA%\fnm\node-versions\*\installation\node_modules\@openai\codex\node_modules\@openai\codex-win32-x64\vendor\x86_64-pc-windows-msvc\bin") do call :TRY "%%~fD\codex.exe" %*
for /d %%D in ("%LOCALAPPDATA%\fnm_multishells\*\node_modules\@openai\codex\node_modules\@openai\codex-win32-x64\vendor\x86_64-pc-windows-msvc\bin") do call :TRY "%%~fD\codex.exe" %*
call :TRY "%APPDATA%\npm\node_modules\@openai\codex\node_modules\@openai\codex-win32-x64\vendor\x86_64-pc-windows-msvc\bin\codex.exe" %*
call :TRY "%ProgramFiles%\nodejs\node_modules\@openai\codex\node_modules\@openai\codex-win32-x64\vendor\x86_64-pc-windows-msvc\bin\codex.exe" %*
if defined NVM_SYMLINK call :TRY "%NVM_SYMLINK%\node_modules\@openai\codex\node_modules\@openai\codex-win32-x64\vendor\x86_64-pc-windows-msvc\bin\codex.exe" %*

rem Ask the active package managers for their global prefix/bin when available.
for /f "usebackq delims=" %%P in (`npm prefix -g 2^>nul`) do (
    call :TRY "%%~fP\codex.cmd" %*
    call :TRY "%%~fP\codex.exe" %*
    call :TRY "%%~fP\node_modules\@openai\codex\node_modules\@openai\codex-win32-x64\vendor\x86_64-pc-windows-msvc\bin\codex.exe" %*
)
for /f "usebackq delims=" %%P in (`pnpm bin -g 2^>nul`) do (
    call :TRY "%%~fP\codex.exe" %*
    call :TRY "%%~fP\codex.cmd" %*
)

rem Final fallback: Codex desktop's bundled native binary from the installed
rem OpenAI.Codex MSIX package. PowerShell is used only for package discovery.
for /f "usebackq delims=" %%P in (`powershell.exe -NoProfile -NonInteractive -Command "$p=Get-AppxPackage -Name OpenAI.Codex -ErrorAction SilentlyContinue ^| Select-Object -First 1; if($p){$c=@((Join-Path $p.InstallLocation 'app\resources\codex.exe'),(Join-Path $p.InstallLocation 'resources\codex.exe')); foreach($x in $c){if(Test-Path $x){Write-Output $x; break}}}" 2^>nul`) do call :TRY "%%~fP" %*

>&2 echo PixelForge: unable to locate Codex CLI.
>&2 echo Set CODEX_CLI_PATH or PIXELFORGE_CODEX_EXE to the full codex.exe/codex.cmd path.
exit /b 9009

:RUN_EXPLICIT
set "PF_CANDIDATE=%~1"
shift
if not exist "%PF_CANDIDATE%" exit /b 0
call :EXEC "%PF_CANDIDATE%" %*
exit /b %ERRORLEVEL%

:TRY
set "PF_CANDIDATE=%~1"
shift
if not exist "%PF_CANDIDATE%" exit /b 0
for %%S in ("%~f0") do set "PF_SELF=%%~fS"
for %%C in ("%PF_CANDIDATE%") do set "PF_REAL=%%~fC"
if /I "%PF_SELF%"=="%PF_REAL%" exit /b 0
call :EXEC "%PF_REAL%" %*
exit /b %ERRORLEVEL%

:EXEC
set "PF_REAL=%~1"
shift
if /I "%~x1"==".cmd" goto EXEC_CMD
if /I "%~x1"==".bat" goto EXEC_CMD
"%PF_REAL%" %*
exit /b %ERRORLEVEL%

:EXEC_CMD
call "%PF_REAL%" %*
exit /b %ERRORLEVEL%
