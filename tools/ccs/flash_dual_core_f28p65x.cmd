@echo off
@powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%~dp0flash_dual_core_f28p65x.ps1" %*
@exit /b %errorlevel%
