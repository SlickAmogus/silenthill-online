@echo off
REM Double-click to run the Silent Hill Online server on this Windows PC, with
REM the command line from server.txt (args=...). Players outside your network
REM need UDP 27888 forwarded to this PC; allow the program through the Windows
REM firewall when asked. Close the window (or Ctrl-C) to stop; markers are
REM saved every 30 seconds and on a clean stop.
setlocal
set "ARGS="
for /f "usebackq tokens=1,* delims==" %%a in (`findstr /b /i /c:"args" "%~dp0server.txt"`) do set "ARGS=%%b"
cd /d "%~dp0"
echo Silent Hill Online server. Admin console: Server-Console.bat
"%~dp0sh_master.exe" %ARGS%
echo.
pause
