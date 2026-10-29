@echo off
rem ============================================================================
rem  Inverted TaskFlow Server - install as a Windows service (run as Administrator)
rem  Installs to C:\InvertedTaskFlow, opens the firewall port, starts automatically.
rem ============================================================================
setlocal
net session >nul 2>&1 || (echo Please right-click this file and choose "Run as administrator". & pause & exit /b 1)
set "DEST=C:\InvertedTaskFlow"
set "PORT=8080"
cd /d "%~dp0"
if not exist "TaskFlowServer.exe" (echo TaskFlowServer.exe not found next to this file. & pause & exit /b 1)
echo.
echo  Installing Inverted TaskFlow Server to %DEST%
if not exist "%DEST%" mkdir "%DEST%"
if not exist "%DEST%\backups" mkdir "%DEST%\backups"
if not exist "%DEST%\logs" mkdir "%DEST%\logs"
sc query InvertedTaskFlow >nul 2>&1 && (echo  Stopping the running service... & sc stop InvertedTaskFlow >nul & timeout /t 5 /nobreak >nul)
copy /y "TaskFlowServer.exe" "%DEST%\" >nul || (echo Copy failed & pause & exit /b 1)
if not exist "%DEST%\server.ini" copy /y "server.ini" "%DEST%\server.ini" >nul
for /f "tokens=2 delims==" %%P in ('findstr /r /c:"^port *=" "%DEST%\server.ini"') do set "PORT=%%P"
set "PORT=%PORT: =%"
rem the service account (LocalService) may only change the data folders
icacls "%DEST%" /grant "NT AUTHORITY\LocalService:(OI)(CI)M" >nul
netsh advfirewall firewall delete rule name="Inverted TaskFlow" >nul 2>&1
netsh advfirewall firewall add rule name="Inverted TaskFlow" dir=in action=allow protocol=TCP localport=%PORT% profile=domain,private >nul
"%DEST%\TaskFlowServer.exe" --install-service || (pause & exit /b 1)
sc start InvertedTaskFlow >nul
timeout /t 4 /nobreak >nul
sc query InvertedTaskFlow | findstr /i "RUNNING" >nul && (echo  Service is running.) || (echo  The service did not start - see %DEST%\logs\server.log)
echo.
echo  Server address for the desktop app:  http://%COMPUTERNAME%:%PORT%
if exist "%DEST%\initial_admin_password.txt" (
  echo  First administrator password:  %DEST%\initial_admin_password.txt
)
echo  Daily backups: %DEST%\backups      Log: %DEST%\logs\server.log
echo.
pause
