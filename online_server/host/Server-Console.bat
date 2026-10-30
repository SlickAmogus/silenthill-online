@echo off
REM Double-click to open the running server's admin console: players, kick,
REM ban, prune markers, say... Type help for the commands, quit to leave.
REM With host= set in server.txt this logs in to that Linux server (you will be
REM asked for its password); otherwise it talks to sh_master.exe on this PC.
setlocal
set "HOST="
for /f "usebackq tokens=1,* delims==" %%a in (`findstr /b /i /c:"host" "%~dp0server.txt"`) do set "HOST=%%b"
if defined HOST for /f "tokens=* delims= " %%a in ("%HOST%") do set "HOST=%%a"
if not defined HOST goto local
echo %HOST% | findstr "@" >nul || set "HOST=root@%HOST%"
REM Keepalives: a console left quiet was dropped by routers along the way
REM ("client_loop: send disconnect: Connection reset"). The server itself
REM keeps running; this only keeps the window's connection to it open.
ssh -t -o StrictHostKeyChecking=accept-new -o ServerAliveInterval=20 -o ServerAliveCountMax=6 -o TCPKeepAlive=yes %HOST% "sh_master --console 2>/dev/null || /root/sh_master --console"
goto end
:local
"%~dp0sh_master.exe" --console
:end
echo.
pause
