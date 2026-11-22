@echo off
setlocal
pushd "%~dp0"
where cl.exe >nul 2>&1
if errorlevel 1 (
    echo Run this from a Visual Studio Developer Command Prompt with C++ tools installed.
    popd
    exit /b 1
)
if not exist build mkdir build
pushd src
rc /nologo /fo ..\build\app.res app.rc
if not errorlevel 1 cl /nologo /std:c++17 /EHsc /O1 /MT /W4 /DUNICODE /D_UNICODE /D_WIN32_WINNT=0x0A00 main.cpp /Fo..\build\main.obj /Fe..\build\windows-lockscreen-mute.exe /link /SUBSYSTEM:WINDOWS ..\build\app.res user32.lib gdi32.lib shell32.lib ole32.lib advapi32.lib wtsapi32.lib
set "build_status=%errorlevel%"
popd
popd
exit /b %build_status%
