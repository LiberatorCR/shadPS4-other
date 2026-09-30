@echo off
setlocal
rem Build and run every shadPS4 test in an already configured ENABLE_TESTS build.
set "TEST_BUILD=%~1"
if not defined TEST_BUILD set "TEST_BUILD=%~dp0..\Build\review-tests"
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "%VSWHERE%" exit /b 2
for /f "usebackq delims=" %%V in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "TEST_VS=%%V"
if not defined TEST_VS exit /b 2
call "%TEST_VS%\Common7\Tools\VsDevCmd.bat" -arch=x64 -host_arch=x64 >nul
if not "%errorlevel%"=="0" exit /b %errorlevel%
cmake --build "%TEST_BUILD%" --target shadps4_settings_test shadps4_ngs2_test shadps4_gcn_test shadps4_http_test -j 6
if not "%errorlevel%"=="0" exit /b %errorlevel%
ctest --test-dir "%TEST_BUILD%" --output-on-failure
exit /b %errorlevel%
