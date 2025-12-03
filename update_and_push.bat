@echo off
setlocal ENABLEDELAYEDEXPANSION

REM Change to the folder where this script is located
cd /d "%~dp0"

REM Check if this is a git repo
if not exist ".git" (
    echo [ERROR] .git folder not found.
    echo Make sure this file is inside the neo_sftp.nro repository folder.
    pause
    exit /b 1
)

REM Get commit message from arguments or ask the user
set COMMIT_MSG=%*
if "%COMMIT_MSG%"=="" (
    set /p COMMIT_MSG=Enter commit message: 
)

if "%COMMIT_MSG%"=="" (
    echo [ERROR] No commit message provided.
    pause
    exit /b 1
)

echo.
echo This script will take the files in this folder and push them to GitHub.
echo It does NOT pull from GitHub first.

REM Ask for GitHub username and token
echo.
set /p GH_USER=GitHub username (without @github.com): 
set /p GH_TOKEN=GitHub personal access token (visible as you type): 

if "%GH_USER%"=="" (
    echo [ERROR] No username provided.
    pause
    exit /b 1
)

if "%GH_TOKEN%"=="" (
    echo [ERROR] No token provided.
    pause
    exit /b 1
)

REM Get current branch name
for /f "usebackq delims=" %%B in (`git rev-parse --abbrev-ref HEAD`) do (
    set BRANCH=%%B
)

echo.
echo Current branch: %BRANCH%

echo.
echo Staging all changes in this folder...
git add -A

echo.
echo Committing with message: "%COMMIT_MSG%"
REM Use --allow-empty so a commit is created even if there are no file diffs,
REM which ensures a full snapshot is pushed every time this script runs.
git commit -m "%COMMIT_MSG%" --allow-empty
if errorlevel 1 (
    echo.
    echo [ERROR] git commit failed.
    pause
    exit /b 1
)

echo.
echo Pushing to your GitHub repo using the username and token you entered...
echo Repo: https://github.com/neo0oen619/neo_sftp_webdav.nro.git
git push "https://%GH_USER%:%GH_TOKEN%@github.com/neo0oen619/neo_sftp_webdav.nro.git" %BRANCH%
REM If you REALLY want to always overwrite remote history, change the line above to:
REM git push "https://%GH_USER%:%GH_TOKEN%@github.com/neo0oen619/neo_sftp_webdav.nro.git" %BRANCH% --force

if errorlevel 1 (
    echo.
    echo [ERROR] git push failed.
    pause
    exit /b 1
)

echo.
echo ==========================================
echo ✅ Done! GitHub now has a commit with the files from this folder.
echo ==========================================
pause
endlocal
exit /b 0
