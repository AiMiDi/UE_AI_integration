@echo off
REM Build UE_AI_integration as a standalone plugin binary
REM This creates a pre-compiled version that works in Blueprint-only projects
REM
REM Usage: build_plugin.bat [engine_path] [output_path]
REM   engine_path  - Path to UE5 install (default: auto-detect from Epic launcher)
REM   output_path  - Where to put the built plugin (default: sibling of the source checkout)

setlocal

set ENGINE_PATH=%~1
set OUTPUT_PATH=%~2

REM Keep packaged output outside the plugin source tree. UE 5.3 BuildPlugin
REM creates a host copy before filtering staged files, so an in-tree package
REM can be copied recursively on the next build.
if "%OUTPUT_PATH%"=="" set OUTPUT_PATH=%~dp0..\..\UE_AI_integration-BuiltPlugin

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
set PLUGIN_ROOT=%~dp0..

if not exist "%UAT%" (
    echo ERROR: RunUAT.bat not found at %UAT%
    exit /b 1
)

echo.
echo ========================================
echo  Building UE_AI_integration Plugin
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

call "%UAT%" BuildPlugin -Plugin="%PLUGIN_PATH%" -Package="%OUTPUT_PATH%" -TargetPlatforms=Win64 -Rocket

if %ERRORLEVEL% neq 0 (
    echo.
    echo BUILD FAILED. Check the output above for errors.
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
echo.
echo Building and packaging ue and ue-workflow CLIs...
cmake -S "%PLUGIN_ROOT%" -B "%CLI_BUILD_DIR%" -DUE_WORKFLOW_BUILD_TESTS=OFF -DUE_WORKFLOW_BUILD_CLI=ON
if %ERRORLEVEL% neq 0 (
    if exist "%CLI_BUILD_DIR%" rmdir /s /q "%CLI_BUILD_DIR%"
    echo CLI CONFIGURE FAILED.
    exit /b 1
)
cmake --build "%CLI_BUILD_DIR%" --config Release --target ue ue-workflow
if %ERRORLEVEL% neq 0 (
    if exist "%CLI_BUILD_DIR%" rmdir /s /q "%CLI_BUILD_DIR%"
    echo CLI BUILD FAILED.
    exit /b 1
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
echo  BUILD SUCCESSFUL
echo ========================================
echo.
echo Plugin built to: %OUTPUT_PATH%
echo.
echo To use it:
echo   1. Copy %OUTPUT_PATH% into YourProject\Plugins\UE_AI_integration\
echo   2. Open UE5 -- plugin loads automatically
echo   3. Run: claude mcp add ue_ai_integration -- node Plugins/UE_AI_integration/MCP/dist/index.js
echo   4. Start claude in your project folder
echo   5. Query ue_skills, then ue_context, then use the domain tools
echo   6. Query ue_cli to locate CLI\bin\ue.exe and CLI\bin\ue-workflow.exe
echo.
