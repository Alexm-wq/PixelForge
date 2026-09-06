@echo off
setlocal EnableExtensions EnableDelayedExpansion

rem PixelForge fallback launcher for Codex on Windows.
rem Copied beside PixelForge.exe as codex.cmd. It resolves common Codex installs
rem without requiring PixelForge to inherit the user's interactive-shell PATH.

if defined PIXELFORGE_CODEX_EXE (
    call :TRY "%PIXELFORGE_CODEX_EXE%" %*
    if defined PF_FOUND exit /b !PF_EXIT!
)
if defined CODEX_CLI_PATH (
    call :TRY "%CODEX_CLI_PATH%" %*
    if defined PF_FOUND exit /b !PF_EXIT!
)

rem Standard npm / pnpm / bun / scoop shims.
call :TRY "%APPDATA%\npm\codex.cmd" %*
if defined PF_FOUND exit /b !PF_EXIT!
call :TRY "%APPDATA%\npm\codex.exe" %*
if defined PF_FOUND exit /b !PF_EXIT!
call :TRY "%LOCALAPPDATA%\pnpm\codex.exe" %*
if defined PF_FOUND exit /b !PF_EXIT!
call :TRY "%LOCALAPPDATA%\pnpm\codex.cmd" %*
if defined PF_FOUND exit /b !PF_EXIT!
call :TRY "%APPDATA%\pnpm\codex.exe" %*
if defined PF_FOUND exit /b !PF_EXIT!
call :TRY "%APPDATA%\pnpm\codex.cmd" %*
if defined PF_FOUND exit /b !PF_EXIT!
call :TRY "%USERPROFILE%\.bun\bin\codex.exe" %*
if defined PF_FOUND exit /b !PF_EXIT!
call :TRY "%USERPROFILE%\.bun\bin\codex.cmd" %*
if defined PF_FOUND exit /b !PF_EXIT!
call :TRY "%USERPROFILE%\scoop\shims\codex.exe" %*
if defined PF_FOUND exit /b !PF_EXIT!
call :TRY "%USERPROFILE%\scoop\shims\codex.cmd" %*
if defined PF_FOUND exit /b !PF_EXIT!
call :TRY "%LOCALAPPDATA%\Microsoft\WindowsApps\codex.exe" %*
if defined PF_FOUND exit /b !PF_EXIT!

rem Native executable bundled inside common npm/FNM installations.
for /d %%D in ("%APPDATA%\fnm\node-versions\*\installation\node_modules\@openai\codex\node_modules\@openai\codex-win32-x64\vendor\x86_64-pc-windows-msvc\bin") do (
    call :TRY "%%~fD\codex.exe" %*
    if defined PF_FOUND exit /b !PF_EXIT!
)
for /d %%D in ("%LOCALAPPDATA%\fnm_multishells\*\node_modules\@openai\codex\node_modules\@openai\codex-win32-x64\vendor\x86_64-pc-windows-msvc\bin") do (
    call :TRY "%%~fD\codex.exe" %*
    if defined PF_FOUND exit /b !PF_EXIT!
)
call :TRY "%APPDATA%\npm\node_modules\@openai\codex\node_modules\@openai\codex-win32-x64\vendor\x86_64-pc-windows-msvc\bin\codex.exe" %*
if defined PF_FOUND exit /b !PF_EXIT!
call :TRY "%ProgramFiles%\nodejs\node_modules\@openai\codex\node_modules\@openai\codex-win32-x64\vendor\x86_64-pc-windows-msvc\bin\codex.exe" %*
if defined PF_FOUND exit /b !PF_EXIT!
if defined NVM_SYMLINK (
    call :TRY "%NVM_SYMLINK%\node_modules\@openai\codex\node_modules\@openai\codex-win32-x64\vendor\x86_64-pc-windows-msvc\bin\codex.exe" %*
    if defined PF_FOUND exit /b !PF_EXIT!
)

rem Ask active package managers for their global locations when available.
for /f "usebackq delims=" %%P in (`npm prefix -g 2^>nul`) do (
    call :TRY "%%~fP\codex.cmd" %*
    if defined PF_FOUND exit /b !PF_EXIT!
    call :TRY "%%~fP\codex.exe" %*
    if defined PF_FOUND exit /b !PF_EXIT!
    call :TRY "%%~fP\node_modules\@openai\codex\node_modules\@openai\codex-win32-x64\vendor\x86_64-pc-windows-msvc\bin\codex.exe" %*
    if defined PF_FOUND exit /b !PF_EXIT!
)
for /f "usebackq delims=" %%P in (`pnpm bin -g 2^>nul`) do (
    call :TRY "%%~fP\codex.exe" %*
    if defined PF_FOUND exit /b !PF_EXIT!
    call :TRY "%%~fP\codex.cmd" %*
    if defined PF_FOUND exit /b !PF_EXIT!
)

rem Final fallback: Codex desktop's bundled native binary from an installed
rem OpenAI.Codex package. PowerShell is only used for package discovery.
for /f "usebackq delims=" %%P in (`powershell.exe -NoProfile -NonInteractive -Command "$p=Get-AppxPackage -Name OpenAI.Codex -ErrorAction SilentlyContinue ^| Select-Object -First 1; if($p){$roots=@($p.InstallLocation); foreach($r in $roots){Get-ChildItem -Path $r -Filter codex.exe -Recurse -ErrorAction SilentlyContinue ^| Select-Object -First 1 -ExpandProperty FullName}}" 2^>nul`) do (
    call :TRY "%%~fP" %*
    if defined PF_FOUND exit /b !PF_EXIT!
)

>&2 echo PixelForge: unable to locate Codex CLI.
>&2 echo Set CODEX_CLI_PATH or PIXELFORGE_CODEX_EXE to the full codex.exe/codex.cmd path.
exit /b 9009

:TRY
set "PF_CANDIDATE=%~1"
shift
if not exist "%PF_CANDIDATE%" exit /b 0
for %%S in ("%~f0") do set "PF_SELF=%%~fS"
for %%C in ("%PF_CANDIDATE%") do (
    set "PF_REAL=%%~fC"
    set "PF_EXT=%%~xC"
)
if /I "!PF_SELF!"=="!PF_REAL!" exit /b 0
set "PF_FOUND=1"
if /I "!PF_EXT!"==".cmd" (
    call "!PF_REAL!" %*
) else if /I "!PF_EXT!"==".bat" (
    call "!PF_REAL!" %*
) else (
    "!PF_REAL!" %*
)
set "PF_EXIT=!ERRORLEVEL!"
exit /b 0
