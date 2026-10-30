@echo off
rem Build and run the drift check simulator (no game): the decisions of src\drift_judge.h against made-up copies.
call "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars32.bat" >nul || exit /b 1
cd /d "%~dp0"
cl /nologo /O2 /W3 /EHsc drift_sim.cpp /Fe:drift_sim.exe || exit /b 1
.\drift_sim.exe
