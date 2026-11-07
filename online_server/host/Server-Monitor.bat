@echo off
REM Double-click for a live view of the server's log: everything since the
REM server last started, then each new line as it happens (joins, leaves,
REM chat, messages, deaths, rooms, invites, network trouble). Scroll up for
REM the history. Press Enter to stop.
REM With host= set in server.txt this logs in to that Linux server (you will be
REM asked for its password); otherwise it reads the server on this PC.
REM If the connection drops, press a key to reconnect: the server keeps running
REM and logging, so nothing is missed.
setlocal
set "HOST="
for /f "usebackq tokens=1,* delims==" %%a in (`findstr /b /i /c:"host" "%~dp0server.txt"`) do set "HOST=%%b"
if defined HOST for /f "tokens=* delims= " %%a in ("%HOST%") do set "HOST=%%a"
if not defined HOST goto local
echo %HOST% | findstr "@" >nul || set "HOST=root@%HOST%"
:again
ssh -t -o StrictHostKeyChecking=accept-new -o ServerAliveInterval=20 -o ServerAliveCountMax=6 -o TCPKeepAlive=yes %HOST% "sh_master --watch 2>/dev/null || /root/sh_master --watch"
echo.
echo The monitor closed. Press a key to open it again, or close this window.
pause >nul
goto again
:local
"%~dp0sh_master.exe" --watch
echo.
pause
