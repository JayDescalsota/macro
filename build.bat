@echo off
setlocal
call "C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars64.bat" >nul
if errorlevel 1 ( echo vcvars failed & exit /b 1 )
rc /nologo app.rc
cl /nologo /std:c++17 /EHsc /O2 /DUNICODE /D_UNICODE /DNOMINMAX /W3 /wd4312 /wd4996 /Fe:macro_cpp.exe *.cpp app.res user32.lib gdi32.lib comctl32.lib advapi32.lib shell32.lib psapi.lib /link /SUBSYSTEM:WINDOWS /ENTRY:mainCRTStartup
endlocal
