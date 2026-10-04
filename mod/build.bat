@echo off
rem Builds AC1RTX.asi (x86). Usage: build.bat [deploy]
setlocal

set ROOT=%~dp0
set VSWHERE="%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
for /f "usebackq tokens=*" %%i in (`%VSWHERE% -latest -products * -property installationPath`) do set VSPATH=%%i
call "%VSPATH%\VC\Auxiliary\Build\vcvarsall.bat" x86 >nul || exit /b 1

set REMIX_INC=%ROOT%..\..\dxvk-remix\public\include
set MINHOOK=%ROOT%..\..\Vibe-Reverse-Engineering\rtx_remix_tools\dx\remix-comp-proxy\deps\minhook
set OUT=%ROOT%build
if not exist "%OUT%\minhook" mkdir "%OUT%\minhook"

rem Third-party MinHook: built without warnings-as-errors.
cl /nologo /c /O2 /MT /W1 /I"%MINHOOK%\include" /Fo"%OUT%\minhook\\" ^
  "%MINHOOK%\src\buffer.c" "%MINHOOK%\src\hook.c" "%MINHOOK%\src\trampoline.c" "%MINHOOK%\src\hde\hde32.c" || exit /b 1

cl /nologo /LD /EHsc /std:c++17 /O2 /MT /W4 /WX /DWIN32_LEAN_AND_MEAN /DNOMINMAX /D_CRT_SECURE_NO_WARNINGS ^
  /I"%REMIX_INC%" /I"%MINHOOK%\include" /Fo"%OUT%\\" ^
  "%ROOT%src\main.cpp" "%ROOT%src\camera.cpp" "%ROOT%src\remix.cpp" "%ROOT%src\log.cpp" "%ROOT%src\hook.cpp" "%ROOT%src\static_geometry.cpp" "%ROOT%src\screenshot.cpp" "%ROOT%src\materials.cpp" "%ROOT%src\skinned.cpp" "%ROOT%src\lights.cpp" "%ROOT%src\dynamic_mesh.cpp" "%ROOT%src\anticull.cpp" "%ROOT%src\hud.cpp" "%ROOT%src\pbr.cpp" "%ROOT%src\pbr_edit.cpp" ^
  "%OUT%\minhook\buffer.obj" "%OUT%\minhook\hook.obj" "%OUT%\minhook\trampoline.obj" "%OUT%\minhook\hde32.obj" ^
  /Fe"%OUT%\AC1RTX.asi" /link user32.lib gdi32.lib gdiplus.lib || exit /b 1

if /i "%1"=="deploy" (
  copy /y "%OUT%\AC1RTX.asi" "C:\Program Files\GOG Galaxy\Games\Assassins Creed\scripts\" >nul || exit /b 1
  if not exist "C:\Program Files\GOG Galaxy\Games\Assassins Creed\scripts\AC1RTX.ini" copy /y "%ROOT%AC1RTX.ini" "C:\Program Files\GOG Galaxy\Games\Assassins Creed\scripts\" >nul
  echo Deployed AC1RTX.asi
)
endlocal
