@echo off
rem Sets a temporary password for an account (they choose a new one at the next sign-in).
set /p EMAIL=E-mail of the account: 
"C:\InvertedTaskFlow\TaskFlowServer.exe" --config "C:\InvertedTaskFlow\server.ini" --reset-password "%EMAIL%"
pause
