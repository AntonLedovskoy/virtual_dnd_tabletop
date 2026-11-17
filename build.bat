@echo off
chcp 65001 >nul
setlocal

if not defined MSYS2_ROOT set "MSYS2_ROOT=C:\msys64"
set "MSYSTEM=UCRT64"
set "MSYS2_PATH_TYPE=inherit"
set "CHERE_INVOKING=1"

set "BASH=%MSYS2_ROOT%\usr\bin\bash.exe"
if not exist "%BASH%" (
  echo РћС€РёР±РєР°: bash MSYS2 РЅРµ РЅР°Р№РґРµРЅ РІ "%BASH%"
  echo         Р·Р°РґР°Р№С‚Рµ MSYS2_ROOT СЃ РїСѓС‚С‘Рј СѓСЃС‚Р°РЅРѕРІРєРё MSYS2
  exit /b 1
)
if not exist "%MSYS2_ROOT%\ucrt64\bin" (
  echo РћС€РёР±РєР°: РѕРєСЂСѓР¶РµРЅРёРµ UCRT64 РЅРµ РЅР°Р№РґРµРЅРѕ РІ "%MSYS2_ROOT%\ucrt64"
  exit /b 1
)

cd /d "%~dp0"
"%BASH%" -lc "exec ./build.sh \"$@\"" build.sh %*
exit /b %ERRORLEVEL%
