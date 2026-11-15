@echo off
rem Developer build (trace hooks active, auto-captures draw calls): never shipped. Output: ddraw_dev.dll
call "C:\Program Files (x86)\Microsoft Visual Studio\18\BuildTools\VC\Auxiliary\Build\vcvarsall.bat" x86 >nul 2>&1 || exit /b 1
cd /d "%~dp0"
cl /nologo /O2 /LD /EHsc /utf-8 /MT /W3 /DREMASTER_DEV ddraw.cpp /Fe:ddraw_dev.dll /Fo:dev_ /link /EXPORT:DirectDrawCreate /EXPORT:DirectDrawCreateEx /EXPORT:DirectDrawEnumerateA /EXPORT:DirectDrawCreateClipper kernel32.lib user32.lib gdi32.lib ole32.lib d3d11.lib dxgi.lib d3dcompiler.lib winmm.lib
