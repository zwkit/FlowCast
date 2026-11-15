@echo off
setlocal enabledelayedexpansion
rem ===========================================================================
rem  FlowCast Windows client - single-binary build (MSVC only, no CMake).
rem  Usage:  build.bat [test|client|all|clean]
rem
rem  The environment is assembled by hand instead of calling vcvars64.bat:
rem  that wrapper reaches for reg.exe, which this sandbox blocks. Everything we
rem  need (compiler, headers, libs) is derived from the install layout below.
rem ===========================================================================

if /i "%1"=="clean" ( rd /s /q "%~dp0build" 2>nul & echo [build] cleaned & exit /b 0 )

rem --- locate a Visual Studio with the C++ x64 toolset ----------------------
set VSROOT=
for %%P in ("%ProgramFiles%" "%ProgramFiles(x86)%") do (
    for /f "tokens=*" %%Y in ('dir /b /ad /o-n "%%~P\Microsoft Visual Studio" 2^>nul') do (
        for /f "tokens=*" %%E in ('dir /b /ad /o-n "%%~P\Microsoft Visual Studio\%%Y" 2^>nul') do (
            if exist "%%~P\Microsoft Visual Studio\%%Y\%%E\VC\Auxiliary\Build\vcvars64.bat" set VSROOT=%%~P\Microsoft Visual Studio\%%Y\%%E
        )
    )
)
if not defined VSROOT (
    echo [build] ERROR: no Visual Studio with the C++ x64 toolset found.
    exit /b 1
)

rem --- newest installed MSVC toolset ----------------------------------------
set MSVCVER=
for /f "tokens=*" %%d in ('dir /b /ad /o-n "%VSROOT%\VC\Tools\MSVC" 2^>nul') do if not defined MSVCVER set MSVCVER=%%d
if not defined MSVCVER ( echo [build] ERROR: no MSVC toolset under %VSROOT% & exit /b 1 )

rem --- newest installed Windows SDK -----------------------------------------
set SDKROOT=%ProgramFiles(x86)%\Windows Kits\10
set SDKVER=
for /f "tokens=*" %%d in ('dir /b /ad /o-n "%SDKROOT%\Include" 2^>nul') do if not defined SDKVER set SDKVER=%%d
if not defined SDKVER ( echo [build] ERROR: no Windows SDK found & exit /b 1 )

set MSVC=%VSROOT%\VC\Tools\MSVC\%MSVCVER%
set PATH=%MSVC%\bin\Hostx64\x64;%SDKROOT%\bin\%SDKVER%\x64;%PATH%
set INCLUDE=%MSVC%\include;%SDKROOT%\Include\%SDKVER%\ucrt;%SDKROOT%\Include\%SDKVER%\um;%SDKROOT%\Include\%SDKVER%\shared;%SDKROOT%\Include\%SDKVER%\winrt;%SDKROOT%\Include\%SDKVER%\cppwinrt
set LIB=%MSVC%\lib\x64;%SDKROOT%\Lib\%SDKVER%\ucrt\x64;%SDKROOT%\Lib\%SDKVER%\um\x64
echo [build] VS   : %VSROOT%
echo [build] MSVC : %MSVCVER%
echo [build] SDK  : %SDKVER%

set ROOT=%~dp0
if "%ROOT:~-1%"=="\" set ROOT=%ROOT:~0,-1%
set OBJ=%ROOT%\build\obj
set BIN=%ROOT%\build\bin
if not exist "%OBJ%" md "%OBJ%"
if not exist "%BIN%" md "%BIN%"

rem --- common flags ---------------------------------------------------------
set CFLAGS=/nologo /W3 /c /utf-8 /D_CRT_SECURE_NO_WARNINGS /DWIN32 /D_WIN32_WINNT=0x0A00
rem /utf-8: the sources are UTF-8 without a BOM and hold Chinese literals.
set CPPFLAGS=/nologo /W4 /c /std:c++17 /EHsc /permissive- /Zc:__cplusplus /utf-8 /D_CRT_SECURE_NO_WARNINGS /DWIN32 /D_WIN32_WINNT=0x0A00 /I"%ROOT%" /I"%ROOT%\third_party\playfair" /I"%ROOT%\test"
set LFLAGS=/nologo /DEBUG /INCREMENTAL:NO /OPT:REF /OPT:ICF /SUBSYSTEM:CONSOLE
rem The client itself is a GUI-subsystem exe so double-clicking it opens the
rem window without a console flash; wWinMain still attaches a console when it is
rem handed command-line arguments.
rem /OPT:REF /OPT:ICF: /DEBUG turns both off by default, which keeps every
rem unreferenced function in the image -- visible once the WinRT capture path
rem added ~800 KB of headers-only code.
set LFLAGS_GUI=/nologo /DEBUG /INCREMENTAL:NO /OPT:REF /OPT:ICF /SUBSYSTEM:WINDOWS /ENTRY:wWinMainCRTStartup
set LIBS=bcrypt.lib ws2_32.lib winmm.lib iphlpapi.lib ole32.lib oleaut32.lib avrt.lib mfplat.lib mf.lib mfreadwrite.lib mfuuid.lib d3d11.lib dxgi.lib mmdevapi.lib gdiplus.lib dwmapi.lib uxtheme.lib comctl32.lib shlwapi.lib advapi32.lib shell32.lib windowsapp.lib

rem --- playfair (vendored C, GPL-3.0): /O1 keeps the odd aliasing quiet -------
set PF_OBJ=
for %%f in (hand_garble.c modified_md5.c omg_hax.c playfair.c sap_hash.c) do (
    echo [cl-C] %%f
    cl %CFLAGS% /O1 /TC /Fo"%OBJ%\%%~nf.obj" /Fd"%OBJ%\%%~nf.pdb" "%ROOT%\third_party\playfair\%%f"
    if errorlevel 1 ( echo [build] ERROR compiling %%f & exit /b 1 )
    set PF_OBJ=!PF_OBJ! "%OBJ%\%%~nf.obj"
)

rem --- our sources (main.cpp is linked only into the client, never a test) --
set SRC_OBJ=
for %%f in ("%ROOT%\src\*.cpp") do (
    if /i not "%%~nf"=="main" if /i not "%%~nf"=="gui" (
        echo [cl] %%~nxf
        cl %CPPFLAGS% /O2 /Fo"%OBJ%\%%~nf.obj" /Fd"%OBJ%\%%~nf.pdb" "%%f"
        if errorlevel 1 ( echo [build] ERROR compiling %%~nxf & exit /b 1 )
        set SRC_OBJ=!SRC_OBJ! "%OBJ%\%%~nf.obj"
    )
)

set TARGET=%1
if "%TARGET%"=="" set TARGET=test

rem --- self-tests: one exe per test\*_test.cpp ------------------------------
if /i "%TARGET%"=="test" (
    set FAILED=0
    for %%f in ("%ROOT%\test\*_test.cpp") do (
        echo [cl] %%~nxf
        cl %CPPFLAGS% /O2 /Fo"%OBJ%\%%~nf.obj" /Fd"%OBJ%\%%~nf.pdb" "%%f"
        if errorlevel 1 ( echo [build] ERROR compiling %%~nxf & exit /b 1 )
        link %LFLAGS% /OUT:"%BIN%\%%~nf.exe" "%OBJ%\%%~nf.obj" !SRC_OBJ! !PF_OBJ! %LIBS%
        if errorlevel 1 ( echo [build] ERROR linking %%~nf & exit /b 1 )
        echo.
        echo --- %%~nf ---
        "%BIN%\%%~nf.exe"
        if errorlevel 1 set FAILED=1
        echo.
    )
    if "!FAILED!"=="1" ( echo [build] SOME TESTS FAILED & exit /b 1 )
    echo [build] all self-tests passed
    exit /b 0
)

rem --- capability probes: one-off diagnostics, not part of the self-tests ---
if /i "%TARGET%"=="probe" (
    for %%f in ("%ROOT%\test\*_probe.cpp") do (
        echo [cl] %%~nxf
        cl %CPPFLAGS% /O2 /Fo"%OBJ%\%%~nf.obj" /Fd"%OBJ%\%%~nf.pdb" "%%f"
        if errorlevel 1 ( echo [build] ERROR compiling %%~nxf & exit /b 1 )
        link %LFLAGS% /OUT:"%BIN%\%%~nf.exe" "%OBJ%\%%~nf.obj" %SRC_OBJ% !PF_OBJ! %LIBS%
        if errorlevel 1 ( echo [build] ERROR linking %%~nf & exit /b 1 )
        echo.
        "%BIN%\%%~nf.exe"
        echo.
    )
    exit /b 0
)

rem --- the client itself ----------------------------------------------------
if /i "%TARGET%"=="client" (
    if not exist "%ROOT%\src\main.cpp" (
        echo [build] ERROR: src\main.cpp does not exist yet
        exit /b 1
    )
    cl %CPPFLAGS% /O2 /Fo"%OBJ%\main.obj" /Fd"%OBJ%\main.pdb" "%ROOT%\src\main.cpp"
    if errorlevel 1 exit /b 1
    cl %CPPFLAGS% /O2 /Fo"%OBJ%\gui.obj" /Fd"%OBJ%\gui.pdb" "%ROOT%\src\gui.cpp"
    if errorlevel 1 exit /b 1
    rem The exe icon. res\flowcast.ico is regenerated by
    rem   build\bin\flowcast.exe --make-icon=res\flowcast.ico
    rem whenever the mark changes.
    if exist "%ROOT%\res\flowcast.rc" (
        rc /nologo /fo "%OBJ%\flowcast.res" "%ROOT%\res\flowcast.rc"
        if errorlevel 1 ( echo [build] ERROR compiling flowcast.rc & exit /b 1 )
    )
    set RES_OBJ=
    if exist "%OBJ%\flowcast.res" set RES_OBJ="%OBJ%\flowcast.res"
    link %LFLAGS_GUI% /OUT:"%BIN%\flowcast.exe" "%OBJ%\main.obj" "%OBJ%\gui.obj" !SRC_OBJ! !PF_OBJ! !RES_OBJ! %LIBS%
    if errorlevel 1 exit /b 1
    echo [build] built %BIN%\flowcast.exe
    exit /b 0
)

if /i "%TARGET%"=="all" (
    call "%~f0" test
    if errorlevel 1 exit /b 1
    call "%~f0" client
    exit /b %errorlevel%
)

echo [build] unknown target "%1" ^(expected test, client, all or clean^)
exit /b 1
