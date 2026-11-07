@echo off
REM Double-click to open the server's log file in Notepad. With host= set in
REM server.txt it is copied from that Linux server first (you will be asked for
REM its password) into server-log.txt beside this file. Older logs, once the
REM current one passes 16 MB, are sh_master.log.1 to .5 next to it.
setlocal
set "HOST="
for /f "usebackq tokens=1,* delims==" %%a in (`findstr /b /i /c:"host" "%~dp0server.txt"`) do set "HOST=%%b"
if defined HOST for /f "tokens=* delims= " %%a in ("%HOST%") do set "HOST=%%a"
if not defined HOST goto local
echo %HOST% | findstr "@" >nul || set "HOST=root@%HOST%"
ssh -o StrictHostKeyChecking=accept-new %HOST% "f=$(sh_master --cmd logfile 2>/dev/null || /root/sh_master --cmd logfile); p=\"${f#logfile }\"; [ \"$p\" != - ] && [ -f \"$p\" ] && cat \"$p\"" >"%~dp0server-log.txt"
if errorlevel 1 (
    echo Could not copy the log. Is the server running?
    pause
    goto :eof
)
start "" notepad "%~dp0server-log.txt"
goto :eof
:local
if not exist "%~dp0sh_master.log" (
    echo No sh_master.log here yet. Start the server with Start-Server.bat first.
    pause
    goto :eof
)
start "" notepad "%~dp0sh_master.log"
