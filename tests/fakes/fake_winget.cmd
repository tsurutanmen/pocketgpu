@echo off
rem Test double for winget.exe. It never installs anything.
rem   --version                  prints a version
rem   install --id <ID> ...      appends "install <ID> <source>" to %PGPU_TEST_WINGET_LOG%
rem                              and creates %PGPU_TEST_STATE_DIR%\<ID>.installed (the "installed" mark the setup tests read).
rem                              It exits 1 when <ID> is the value of PGPU_TEST_WINGET_FAIL.
setlocal EnableDelayedExpansion
if "%~1"=="--version" (
  echo v1.7.10861
  exit /b 0
)
if /i not "%~1"=="install" (
  echo fake winget: unsupported command %~1
  exit /b 2
)
set "ID="
set "SRC="
:loop
shift
if "%~1"=="" goto done
if /i "%~1"=="--id" (
  set "ID=%~2"
  shift
  goto loop
)
if /i "%~1"=="--source" (
  set "SRC=%~2"
  shift
  goto loop
)
goto loop
:done
if "%ID%"=="" (
  echo fake winget: no --id
  exit /b 2
)
if defined PGPU_TEST_WINGET_LOG >>"%PGPU_TEST_WINGET_LOG%" echo install %ID% %SRC%
if /i "%ID%"=="%PGPU_TEST_WINGET_FAIL%" (
  echo fake winget: the install of %ID% failed on purpose
  exit /b 1
)
if defined PGPU_TEST_STATE_DIR >"%PGPU_TEST_STATE_DIR%\%ID%.installed" echo installed
echo fake winget: installed %ID%
exit /b 0
