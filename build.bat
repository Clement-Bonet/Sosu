@echo off
rem Builds sosuc with MSVC. Usage: build.bat [test]
setlocal
cd /d "%~dp0"

where cl >nul 2>nul && goto :have_cl
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "%VSWHERE%" goto :no_cl
for /f "usebackq tokens=*" %%i in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VSDIR=%%i"
if not defined VSDIR goto :no_cl
set "PATH=%PATH%;%ProgramFiles(x86)%\Microsoft Visual Studio\Installer"
call "%VSDIR%\VC\Auxiliary\Build\vcvars64.bat" >nul || goto :no_cl

:have_cl
if not exist build\obj\sosuc mkdir build\obj\sosuc
if not exist build\obj\tests mkdir build\obj\tests
set CFLAGS=/nologo /std:c11 /W4 /WX /utf-8 /Zi /Od /D_CRT_SECURE_NO_WARNINGS /Isrc\lib

cl %CFLAGS% /Fobuild\obj\sosuc\ /Fdbuild\obj\sosuc\ /Febuild\sosuc.exe src\lib\*.c src\sosuc\main.c || exit /b 1
cl %CFLAGS% /Fobuild\obj\tests\ /Fdbuild\obj\tests\ /Febuild\lexer_tests.exe src\lib\*.c tests\lexer_tests.c || exit /b 1
cl %CFLAGS% /Fobuild\obj\tests\ /Fdbuild\obj\tests\ /Febuild\parser_tests.exe src\lib\*.c tests\parser_tests.c || exit /b 1
cl %CFLAGS% /Fobuild\obj\tests\ /Fdbuild\obj\tests\ /Febuild\package_tests.exe src\lib\*.c tests\package_tests.c || exit /b 1
cl %CFLAGS% /Fobuild\obj\tests\ /Fdbuild\obj\tests\ /Febuild\sema_tests.exe src\lib\*.c tests\sema_tests.c || exit /b 1
cl %CFLAGS% /Fobuild\obj\tests\ /Fdbuild\obj\tests\ /Febuild\fmt_tests.exe src\lib\*.c tests\fmt_tests.c || exit /b 1
cl %CFLAGS% /Fobuild\obj\tests\ /Fdbuild\obj\tests\ /Febuild\program_tests.exe tests\program_tests.c || exit /b 1

if /i not "%1"=="test" exit /b 0

build\lexer_tests.exe || exit /b 1
build\parser_tests.exe || exit /b 1
build\package_tests.exe || exit /b 1
build\sema_tests.exe || exit /b 1
build\fmt_tests.exe || exit /b 1
build\program_tests.exe build\sosuc.exe || exit /b 1
build\sosuc.exe check tests\packages\app || exit /b 1
build\sosuc.exe check packages\platform || exit /b 1
build\sosuc.exe check examples\window || exit /b 1
build\sosuc.exe check tests\window || exit /b 1
build\sosuc.exe test packages\render || exit /b 1
build\sosuc.exe test packages\ui || exit /b 1
build\sosuc.exe check examples\form || exit /b 1
for %%f in (tests\samples\*.sosu) do (
    build\sosuc.exe parse "%%f" >nul || (echo FAIL %%f & exit /b 1)
)
build\sosuc.exe parse tests\samples\ui\login.ui.sosu >nul || exit /b 1
for %%f in (tests\samples\ui\*.sosu) do (
    build\sosuc.exe lex "%%f" >nul || (echo FAIL %%f & exit /b 1)
)
echo samples: ok
exit /b 0

:no_cl
echo error: MSVC (cl.exe) not found; install the "Desktop development with C++" workload 1>&2
exit /b 2
