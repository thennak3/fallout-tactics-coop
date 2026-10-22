@echo off
rem Build netlog.dll (x86) with the VS 2022 Build Tools.
call "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars32.bat" >nul || exit /b 1
cd /d "%~dp0"
cl /nologo /O2 /MT /W3 /EHa /LD netlog.cpp /Fe:netlog.dll user32.lib ws2_32.lib || exit /b 1
