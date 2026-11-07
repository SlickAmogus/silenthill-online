@echo off
REM Double-click to open the running server's admin console: players, kick,
REM ban, prune markers, say, watch... Type help for the commands, quit to leave.
REM With host= set in server.txt this logs in to that Linux server (you will be
REM asked for its password); otherwise it talks to sh_master.exe on this PC.
REM If the connection drops, press a key to reconnect. The server keeps running
REM and keeps logging either way: "watch" shows what happened in between.
setlocal
set "HOST="
for /f "usebackq tokens=1,* delims==" %%a in (`findstr /b /i /c:"host" "%~dp0server.txt"`) do set "HOST=%%b"
if defined HOST for /f "tokens=* delims= " %%a in ("%HOST%") do set "HOST=%%a"
if not defined HOST goto local
echo %HOST% | findstr "@" >nul || set "HOST=root@%HOST%"
:again
REM Keepalives: a console left quiet was dropped by routers along the way
REM ("client_loop: send disconnect: Connection reset").
ssh -t -o StrictHostKeyChecking=accept-new -o ServerAliveInterval=20 -o ServerAliveCountMax=6 -o TCPKeepAlive=yes %HOST% "sh_master --console 2>/dev/null || /root/sh_master --console"
echo.
echo The console closed. Press a key to connect again, or close this window.
pause >nul
goto again
:local
"%~dp0sh_master.exe" --console
echo.
pause
