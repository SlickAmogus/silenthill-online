@echo off
REM Double-click to run the Silent Hill Online server on this Windows PC, with
REM the command line from server.txt (args=...). Players outside your network
REM need UDP 27888 forwarded to this PC; allow the program through the Windows
REM firewall when asked. Close the window (or Ctrl-C) to stop; markers are
REM saved every 30 seconds and on a clean stop.
REM
REM When a newer server is out, it says so here before it starts and asks:
REM update now, not now, or never ask again (README.md, "Updates").
setlocal
set "ARGS="
for /f "usebackq tokens=1,* delims==" %%a in (`findstr /b /i /c:"args" "%~dp0server.txt"`) do set "ARGS=%%b"
cd /d "%~dp0"
echo Silent Hill Online server. Admin console: Server-Console.bat
:run
"%~dp0sh_master.exe" %ARGS%
REM 75 is the server asking to be started again: after the restart command, or
REM after an update it installed. The server started here does not ask about
REM updates a second time.
if %errorlevel%==75 (
    set "SH_MASTER_RESTARTED=1"
    goto run
)
echo.
pause
