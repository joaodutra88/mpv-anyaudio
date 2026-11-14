@echo off
rem Builds the Nuvio shim. Run from an "x64 Native Tools Command Prompt for VS 2022".
cd /d "%~dp0"
cl /nologo /LD /O2 /EHsc /Fe:libmpv-2.dll shim.cpp /link user32.lib
