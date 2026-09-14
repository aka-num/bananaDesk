@echo off
setlocal
"%~dp0landesk-helper.exe" --install
set "LANDESK_UNLOCK_RESULT=%errorlevel%"
echo.
if not "%LANDESK_UNLOCK_RESULT%"=="0" echo Windows unlock helper operation failed: %LANDESK_UNLOCK_RESULT%
pause
exit /b %LANDESK_UNLOCK_RESULT%
