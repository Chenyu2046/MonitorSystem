@echo off
cd /d "%~dp0"
echo [1/5] Removing old .git...
if exist .git rmdir /s /q .git
echo [2/5] Initializing git...
git init -b main >nul
echo [3/5] Adding files...
git add -A >nul
echo [4/5] Committing...
git commit -m "Initial commit: MonitorSystem" >nul
echo [5/5] Pushing to GitHub...
git remote add origin https://github.com/yu20120707/MonitorSystem.git
git push -u origin main
echo.
if %errorlevel% equ 0 (
    echo [OK] All done! Pushed successfully.
) else (
    echo [FAIL] Push failed. Check error above.
)
echo.
pause
