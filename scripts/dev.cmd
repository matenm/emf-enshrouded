@echo off
rem Runs a command inside the MSVC x64 developer environment: Visual Studio 2022 or its Build Tools, found with
rem vswhere (any edition). Already inside a developer prompt (VCINSTALLDIR set, as on CI): runs the command as it is.
setlocal
if defined VCINSTALLDIR goto run
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
set "VCVARS="
if not exist "%VSWHERE%" goto fallback
for /f "usebackq delims=" %%i in (`call "%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -find VC\Auxiliary\Build\vcvars64.bat`) do set "VCVARS=%%i"
:fallback
if not defined VCVARS set "VCVARS=%ProgramFiles(x86)%\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat"
if exist "%VCVARS%" goto found
echo dev.cmd: no Visual Studio 2022 C++ tools found. Install Visual Studio 2022 or its Build Tools with the
echo "Desktop development with C++" workload, or run this from an "x64 Native Tools" prompt.
exit /b 1
:found
call "%VCVARS%" >nul || exit /b 1
:run
%*
