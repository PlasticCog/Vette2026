@echo off
rem Puts the VETTE! 2026 relay server on your Cloudflare account and prints its address (deploy.mjs).
rem Double-click it, or run it from a terminal. Needs Node.js 20.3 or newer: https://nodejs.org/
setlocal
where node >nul 2>nul
if errorlevel 1 (
    echo Node.js isn't installed. Get it from https://nodejs.org/ ^(the LTS version^), then run this again.
    pause
    exit /b 1
)
node "%~dp0deploy.mjs"
set RESULT=%ERRORLEVEL%
echo.
pause
exit /b %RESULT%
