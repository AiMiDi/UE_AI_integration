@echo off
REM Compatibility entry point. The PowerShell implementation owns the
REM isolated Host lifecycle so no recursive file operation crosses shells.
powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%~dp0build_trace_worker.ps1" ^
    -EngineRoot "%~1" -PluginRoot "%~2" -StagingPluginRoot "%~3"
exit /b %ERRORLEVEL%
