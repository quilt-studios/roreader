@echo off
setlocal
title ROREADER Installer
powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%~dp0Start-Installer.ps1"
if errorlevel 1 (
  echo Der ROREADER Installer konnte nicht gestartet werden.
  pause
)
