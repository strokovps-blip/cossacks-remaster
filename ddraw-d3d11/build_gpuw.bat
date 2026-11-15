@echo off
call "C:\Program Files (x86)\Microsoft Visual Studio\18\BuildTools\VC\Auxiliary\Build\vcvarsall.bat" x86 >nul 2>&1 || exit /b 1
cd /d "%~dp0"
cl /nologo /O2 /LD /EHsc /utf-8 /MT /W3 /DREMASTER_GPUW ddraw.cpp /Fe:ddraw_gpuw.dll /Fo:gpuw_ /link /EXPORT:DirectDrawCreate /EXPORT:DirectDrawCreateEx /EXPORT:DirectDrawEnumerateA /EXPORT:DirectDrawCreateClipper kernel32.lib user32.lib gdi32.lib ole32.lib d3d11.lib dxgi.lib d3dcompiler.lib winmm.lib

