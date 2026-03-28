@echo off
echo [sandbox] Launching bro...
echo ============================================
cd /d C:\bro\build\src\Release
bro.exe C:\bro\apps\hello 2>&1
echo.
echo ============================================
echo [sandbox] bro exited with code %ERRORLEVEL%
echo.
echo If the app closed unexpectedly, check the output above for errors.
pause
