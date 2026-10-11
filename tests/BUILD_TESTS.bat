@echo off
setlocal
where cl >nul 2>nul
if errorlevel 1 (
  echo Run this from an x64 Developer Command Prompt for Visual Studio.
  exit /b 1
)
set "REPO_ROOT=%~dp0.."
if not exist "%~dp0build" mkdir "%~dp0build"
pushd "%~dp0build"
cl /nologo /EHsc /std:c++20 /W4 /I"%REPO_ROOT%\vendor\minhook\include" "%~dp0test_transport.cpp" "%REPO_ROOT%\vendor\minhook\src\buffer.c" "%REPO_ROOT%\vendor\minhook\src\hook.c" "%REPO_ROOT%\vendor\minhook\src\trampoline.c" "%REPO_ROOT%\vendor\minhook\src\hde\hde64.c" /Fe:test_transport.exe
if errorlevel 1 (
  popd
  exit /b 1
)
test_transport.exe
set "TEST_RESULT=%errorlevel%"
popd
exit /b %TEST_RESULT%
