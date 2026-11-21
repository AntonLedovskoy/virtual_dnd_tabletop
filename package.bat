@echo off
chcp 65001 >nul
setlocal

if not defined MSYS2_ROOT set "MSYS2_ROOT=C:\msys64"
set "MSYSTEM=UCRT64"
set "MSYS2_PATH_TYPE=inherit"
set "CHERE_INVOKING=1"

set "BASH=%MSYS2_ROOT%\usr\bin\bash.exe"
if not exist "%BASH%" (
  echo Ошибка: bash MSYS2 не найден в "%BASH%"
  echo         задайте MSYS2_ROOT с путём установки MSYS2
  exit /b 1
)
if not exist "%MSYS2_ROOT%\ucrt64\bin" (
  echo Ошибка: окружение UCRT64 не найдено в "%MSYS2_ROOT%\ucrt64"
  exit /b 1
)

cd /d "%~dp0"
"%BASH%" -lc "exec ./tools/package.sh \"$@\"" package.sh %*
exit /b %ERRORLEVEL%
