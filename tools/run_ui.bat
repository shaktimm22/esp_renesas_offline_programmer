@echo off
rem Launch the programmer UI with a Python that has tkinter.
rem (The PlatformIO terminal puts PlatformIO's own Python first on PATH, which has no tkinter.)
setlocal
cd /d "%~dp0"
set "VIRTUAL_ENV="
set "PY="

py -3 -c "import tkinter" >nul 2>&1 && set "PY=py -3"
if not defined PY if exist "C:\Python313\python.exe" "C:\Python313\python.exe" -c "import tkinter" >nul 2>&1 && set "PY=C:\Python313\python.exe"
if not defined PY python -c "import tkinter" >nul 2>&1 && set "PY=python"
if not defined PY (
  echo No Python with tkinter found. Install Python from python.org with the "tcl/tk and IDLE" option.
  pause
  exit /b 1
)

%PY% -c "import serial" >nul 2>&1 || %PY% -m pip install --quiet --disable-pip-version-check --no-warn-script-location pyserial
%PY% flasher_ui.py
if errorlevel 1 pause
