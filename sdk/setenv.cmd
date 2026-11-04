@echo off
rem setenv.cmd: this command prompt builds programs for PINT 4.0, Windows NT 4.0 on ARM.
rem It runs vcvarsall.bat of the Visual Studio with Microsoft C for ARM (unless a prompt of
rem Visual Studio already runs), then takes the headers, libraries, cl and link of this SDK.
rem Then nmake (win32.mak), cl, link and rc build for PINT.
set PINTSDK=%~dp0
set PINTSDK=%PINTSDK:~0,-1%
set PINTHOST=x64
if /i "%PROCESSOR_ARCHITECTURE%"=="ARM64" set PINTHOST=arm64
if /i "%PROCESSOR_ARCHITEW6432%"=="ARM64" set PINTHOST=arm64
if defined VCToolsInstallDir goto sdk
set PINTVS=
set VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe
if not exist "%VSWHERE%" goto novs
"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.ARM -property installationPath > "%TEMP%\pintvs.txt"
set /p PINTVS=<"%TEMP%\pintvs.txt"
del "%TEMP%\pintvs.txt"
if not defined PINTVS goto novs
if not exist "%PINTVS%\VC\Auxiliary\Build\vcvarsall.bat" goto novs
call "%PINTVS%\VC\Auxiliary\Build\vcvarsall.bat" %PINTHOST%_arm >nul
:sdk
if not exist "%VCToolsInstallDir%bin\Host%PINTHOST%\arm\cl.exe" goto novs
set INCLUDE=%PINTSDK%\include;%PINTSDK%\include\crt
set LIB=%PINTSDK%\lib
set CPU=ARM
set PATH=%PINTSDK%\bin\%PINTHOST%;%VCToolsInstallDir%bin\Host%PINTHOST%\arm;%PATH%
echo PINT 4.0 SDK: nmake, cl, link and rc build for Windows NT 4.0 on ARM.
goto end
:novs
echo setenv: no Microsoft C for ARM. Install Visual Studio 2022 or its Build Tools with the
echo component "MSVC v143 - VS 2022 C++ ARM build tools (Latest)" and a Windows SDK
echo (Visual Studio Installer, Modify, Individual components). Visual Studio 2026 has none.
:end
