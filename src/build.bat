@echo off
rem MD98 build script (Microsoft Visual C++ 6.0)
rem Run from a command line where the VC6 environment (vcvars32.bat) is set.

if not exist ..\bin mkdir ..\bin

cl /nologo /O1 /GX /c main.c markdown.c render.c lang.c
if errorlevel 1 goto :fail

rc /fo md98.res md98.rc
if errorlevel 1 goto :fail

link /SUBSYSTEM:WINDOWS /OUT:..\bin\md98.exe main.obj markdown.obj render.obj lang.obj md98.res ^
     comdlg32.lib user32.lib gdi32.lib kernel32.lib
if errorlevel 1 goto :fail

echo Build OK: ..\bin\md98.exe
goto :eof

:fail
echo Build failed.
