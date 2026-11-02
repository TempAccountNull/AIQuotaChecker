@echo off
rem Builds AIQuotaChecker (x64) into x64\<Config>\AIQuotaChecker.exe.
rem   build.cmd [Release|Debug]
rem Dear ImGui and nlohmann/json download into third_party\ on the first build.
setlocal
set "CONFIG=%~1"
if "%CONFIG%"=="" set "CONFIG=Release"

set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
for /f "usebackq delims=" %%M in (`"%VSWHERE%" -latest -requires Microsoft.Component.MSBuild -find MSBuild\**\Bin\amd64\MSBuild.exe`) do set "MSBUILD=%%M"
if not defined MSBUILD (echo MSBuild was not found. & exit /b 1)

"%MSBUILD%" "%~dp0AIQuotaChecker.vcxproj" /m /nologo /v:minimal /p:Configuration=%CONFIG% /p:Platform=x64
exit /b %ERRORLEVEL%
