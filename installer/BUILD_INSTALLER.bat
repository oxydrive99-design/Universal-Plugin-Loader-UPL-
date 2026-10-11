@echo off
setlocal
cd /d "%~dp0"
if not exist "..\build\installer" mkdir "..\build\installer"
rc /nologo /fo "..\build\installer\loader.res" loader.rc
if errorlevel 1 exit /b 1
cl /nologo /std:c++17 /EHsc /O2 /MT /W4 /DUNICODE /D_UNICODE installer.cpp /Fo"..\build\installer\installer.obj" /Fe"..\build\installer\UniversalPluginLoader-Installer-0.4.2.exe" /link /SUBSYSTEM:WINDOWS /MANIFEST:EMBED /MANIFESTUAC:"level='asInvoker' uiAccess='false'" "..\build\installer\loader.res" user32.lib shell32.lib advapi32.lib comdlg32.lib
exit /b %errorlevel%
