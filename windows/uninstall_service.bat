@echo off
rem Removes the Windows service. Your data in C:\InvertedTaskFlow is NOT deleted.
net session >nul 2>&1 || (echo Please run as administrator. & pause & exit /b 1)
"C:\InvertedTaskFlow\TaskFlowServer.exe" --uninstall-service
netsh advfirewall firewall delete rule name="Inverted TaskFlow" >nul 2>&1
echo Data and backups are still in C:\InvertedTaskFlow.
pause
