@echo off
setlocal

if not defined MSYS2_ROOT set "MSYS2_ROOT=C:\msys64"
set "MSYSTEM=UCRT64"
set "MSYS2_PATH_TYPE=inherit"
set "CHERE_INVOKING=1"

set "BASH=%MSYS2_ROOT%\usr\bin\bash.exe"
if not exist "%BASH%" (
  echo error: MSYS2 bash not found at "%BASH%"
  echo        set MSYS2_ROOT to your MSYS2 installation directory
  exit /b 1
)
if not exist "%MSYS2_ROOT%\ucrt64\bin" (
  echo error: UCRT64 environment not found at "%MSYS2_ROOT%\ucrt64"
  exit /b 1
)

cd /d "%~dp0"
"%BASH%" -lc "exec ./build.sh \"$@\"" build.sh %*
exit /b %ERRORLEVEL%
