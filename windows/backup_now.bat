@echo off
rem Writes a backup of the database now (safe while the server runs).
"C:\InvertedTaskFlow\TaskFlowServer.exe" --config "C:\InvertedTaskFlow\server.ini" --backup-now
pause
