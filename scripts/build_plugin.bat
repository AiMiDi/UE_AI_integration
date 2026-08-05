@echo off
REM Build UE_AI_integration as a standalone plugin binary
REM This creates a pre-compiled version that works in Blueprint-only projects
REM
REM Usage: build_plugin.bat [engine_path] [output_path]
REM   engine_path  - Path to UE5 install (default: auto-detect from Epic launcher)
REM   output_path  - Where to put the built plugin (default: sibling of the source checkout)
REM Set UEAI_RUN_PORTABLE_TESTS=1 to build and run the portable CTest suite.
REM This packaging entry point never runs UE Automation.

setlocal

set ENGINE_PATH=%~1
set OUTPUT_PATH=%~2

REM Keep packaged output outside the plugin source tree. UE 5.3 BuildPlugin
REM creates a host copy before filtering staged files, so an in-tree package
REM can be copied recursively on the next build.
if "%OUTPUT_PATH%"=="" set OUTPUT_PATH=%~dp0..\..\UE_AI_integration-BuiltPlugin

for %%I in ("%~dp0..") do set PLUGIN_ROOT=%%~fI
for %%I in ("%OUTPUT_PATH%") do set OUTPUT_PATH=%%~fI
powershell -NoProfile -ExecutionPolicy Bypass -Command ^
  "$root=[IO.Path]::GetFullPath('%PLUGIN_ROOT%').TrimEnd([IO.Path]::DirectorySeparatorChar); $out=[IO.Path]::GetFullPath('%OUTPUT_PATH%'); if ($out -eq $root -or $out.StartsWith($root + [IO.Path]::DirectorySeparatorChar, [StringComparison]::OrdinalIgnoreCase)) { Write-Error 'BuildPlugin output must be outside the plugin source tree.'; exit 2 }"
if %ERRORLEVEL% neq 0 exit /b 2

REM Auto-detect a supported engine if not provided (newest first).
if "%ENGINE_PATH%"=="" (
    if exist "E:\EpicGames\Games\UE_5.7\Engine" (
        set ENGINE_PATH=E:\EpicGames\Games\UE_5.7
    ) else if exist "C:\Program Files\Epic Games\UE_5.7\Engine" (
        set ENGINE_PATH=C:\Program Files\Epic Games\UE_5.7
    ) else if exist "C:\Program Files\Epic Games\UE_5.6\Engine" (
        set ENGINE_PATH=C:\Program Files\Epic Games\UE_5.6
    ) else if exist "C:\Program Files\Epic Games\UE_5.5\Engine" (
        set ENGINE_PATH=C:\Program Files\Epic Games\UE_5.5
    ) else if exist "C:\Program Files\Epic Games\UE_5.4\Engine" (
        set ENGINE_PATH=C:\Program Files\Epic Games\UE_5.4
    ) else if exist "C:\Program Files\Epic Games\UE_5.3\Engine" (
        set ENGINE_PATH=C:\Program Files\Epic Games\UE_5.3
    ) else (
        echo ERROR: Could not find a supported UE 5.3-5.7 installation.
        echo Pass the engine root as the first argument.
        echo Example: build_plugin.bat "D:\code\D5\d5render-ue5_3"
        exit /b 1
    )
)

set UAT=%ENGINE_PATH%\Engine\Build\BatchFiles\RunUAT.bat
set PLUGIN_PATH=%~dp0..\UE_AI_integration.uplugin
set PLUGIN_ROOT=%PLUGIN_ROOT%

if not exist "%UAT%" (
    echo ERROR: RunUAT.bat not found at %UAT%
    exit /b 1
)

echo.
echo ========================================
echo  Packaging UE_AI_integration Plugin
echo ========================================
echo  Engine: %ENGINE_PATH%
echo  Plugin: %PLUGIN_PATH%
echo  Output: %OUTPUT_PATH%
echo ========================================
echo.

node "%PLUGIN_ROOT%\scripts\validate_capabilities.mjs"
if %ERRORLEVEL% neq 0 (
    echo CAPABILITY VALIDATION FAILED.
    exit /b 1
)
node "%PLUGIN_ROOT%\scripts\validate_skills.mjs"
if %ERRORLEVEL% neq 0 (
    echo AGENT SKILL VALIDATION FAILED.
    exit /b 1
)

echo.
echo Building and testing the MCP bridge from current TypeScript sources...
call npm ci --prefix "%PLUGIN_ROOT%\MCP"
if %ERRORLEVEL% neq 0 exit /b 1
call npm run build --prefix "%PLUGIN_ROOT%\MCP"
if %ERRORLEVEL% neq 0 (
    echo MCP build hit a transient output-write failure; retrying once...
    powershell -NoProfile -Command "Start-Sleep -Milliseconds 750"
    call npm run build --prefix "%PLUGIN_ROOT%\MCP"
)
if %ERRORLEVEL% neq 0 exit /b 1
REM The release gate compiled dist immediately above. Avoid invoking npm test's
REM pretest hook and rewriting the same files while Windows scanners still hold
REM short-lived handles to the freshly generated output.
call npm run test:compiled --prefix "%PLUGIN_ROOT%\MCP"
if %ERRORLEVEL% neq 0 exit /b 1
call npm audit --omit=dev --prefix "%PLUGIN_ROOT%\MCP"
if %ERRORLEVEL% neq 0 exit /b 1

if not "%UEAI_UBT_IDLE_TIMEOUT_SECONDS%"=="" (
    echo Waiting for a stable UnrealBuildTool idle window...
    powershell -NoProfile -ExecutionPolicy Bypass -File "%PLUGIN_ROOT%\scripts\wait_for_ubt_idle.ps1" -EngineRoot "%ENGINE_PATH%" -TimeoutSeconds "%UEAI_UBT_IDLE_TIMEOUT_SECONDS%" -StableSeconds 10
    if errorlevel 1 exit /b 1
)

set "UAT_MODE_ARGS="
if /I "%UEAI_USE_PRECOMPILED_UAT%"=="1" (
    echo Using precompiled AutomationTool and AutomationScript modules.
    set "UAT_MODE_ARGS=-NoCompileUAT -NoCompile"
)
call "%UAT%" %UAT_MODE_ARGS% BuildPlugin -Plugin="%PLUGIN_PATH%" -Package="%OUTPUT_PATH%" -TargetPlatforms=Win64 -Rocket

if %ERRORLEVEL% neq 0 (
    echo.
    echo BUILD FAILED. Check the output above for errors.
    exit /b 1
)

echo.
echo Building and staging UEAITraceWorker...
call "%PLUGIN_ROOT%\scripts\build_trace_worker.bat" "%ENGINE_PATH%" "%PLUGIN_ROOT%" "%OUTPUT_PATH%"
if %ERRORLEVEL% neq 0 (
    echo TRACE WORKER BUILD FAILED.
    exit /b 1
)

echo.
echo Restoring packaged MCP production dependencies...
call npm ci --omit=dev --prefix "%OUTPUT_PATH%\MCP"
if %ERRORLEVEL% neq 0 (
    echo MCP DEPENDENCY INSTALL FAILED.
    exit /b 1
)

set CLI_BUILD_DIR=%TEMP%\UE_AI_integration-cli-%RANDOM%-%RANDOM%
set CLI_TESTS=OFF
if /I "%UEAI_RUN_PORTABLE_TESTS%"=="1" set CLI_TESTS=ON
echo.
echo Building and packaging ue and ue-workflow CLIs ^(portable tests: %CLI_TESTS%^)...
cmake -S "%PLUGIN_ROOT%" -B "%CLI_BUILD_DIR%" -DUE_WORKFLOW_BUILD_TESTS=%CLI_TESTS% -DUE_WORKFLOW_BUILD_CLI=ON
if %ERRORLEVEL% neq 0 (
    if exist "%CLI_BUILD_DIR%" rmdir /s /q "%CLI_BUILD_DIR%"
    echo CLI CONFIGURE FAILED.
    exit /b 1
)
if /I "%UEAI_RUN_PORTABLE_TESTS%"=="1" (
    cmake --build "%CLI_BUILD_DIR%" --config Release
) else (
    cmake --build "%CLI_BUILD_DIR%" --config Release --target ue ue-workflow
)
if %ERRORLEVEL% neq 0 (
    if exist "%CLI_BUILD_DIR%" rmdir /s /q "%CLI_BUILD_DIR%"
    echo CLI BUILD FAILED.
    exit /b 1
)
if /I "%UEAI_RUN_PORTABLE_TESTS%"=="1" (
    ctest --test-dir "%CLI_BUILD_DIR%" -C Release --output-on-failure
    if errorlevel 1 (
        if exist "%CLI_BUILD_DIR%" rmdir /s /q "%CLI_BUILD_DIR%"
        echo PORTABLE CTEST GATE FAILED.
        exit /b 1
    )
)
cmake --install "%CLI_BUILD_DIR%" --config Release --prefix "%OUTPUT_PATH%\CLI"
if %ERRORLEVEL% neq 0 (
    if exist "%CLI_BUILD_DIR%" rmdir /s /q "%CLI_BUILD_DIR%"
    echo CLI INSTALL FAILED.
    exit /b 1
)
if exist "%CLI_BUILD_DIR%" rmdir /s /q "%CLI_BUILD_DIR%"

echo.
echo ========================================
echo  PACKAGE BUILD SUCCESSFUL
echo ========================================
echo.
echo Plugin built to: %OUTPUT_PATH%
echo.
if /I "%UEAI_RUN_PORTABLE_TESTS%"=="1" (
    echo Portable CTest gate: PASSED
) else (
    echo Portable CTest gate: NOT RUN ^(set UEAI_RUN_PORTABLE_TESTS=1 to enable^)
)
echo UE Automation: NOT RUN by this packaging entry point.
echo A successful package build is not, by itself, release qualification.
echo.
echo Install the staged package with scripts\install_plugin.ps1; do not overwrite a loaded DLL.
echo.
