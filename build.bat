@echo off
for /f "usebackq tokens=*" %%i in (`"%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VS=%%i"
call "%VS%\VC\Auxiliary\Build\vcvarsall.bat" x64 >nul
if exist resource.rc rc /nologo /fo resource.res resource.rc
cl /nologo /W3 /O2 /Oi /Gy /GL /std:c++20 /EHs- /GS- /GR- /MT /D NDEBUG /D _CONSOLE /D UNICODE /D _UNICODE /D _CRT_SECURE_NO_WARNINGS /D _HAS_EXCEPTIONS=0 /permissive- mini-ai.cpp resource.res /Fe:mini-ai.exe /link /nologo /LTCG /OPT:REF /OPT:ICF /SUBSYSTEM:WINDOWS /MANIFEST:EMBED user32.lib gdi32.lib shell32.lib wininet.lib ole32.lib comdlg32.lib
del mini-ai.obj resource.res mini-ai.exe.manifest 2>nul