@echo off
setlocal
where msbuild >nul 2>nul
if errorlevel 1 (
  echo MSBuild was not found. Run this from "Developer Command Prompt for VS 2022".
  exit /b 1
)
msbuild UniversalPluginLoader.sln /m /p:Configuration=Release /p:Platform=x64
if errorlevel 1 exit /b %errorlevel%
echo.
echo Built: bin\x64\Release\vcruntime140_1.dll
