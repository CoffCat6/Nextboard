@echo off
call "D:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat" >nul
cd /d "D:\3project\rust\Nextboard\app\build"
"D:\Qt\Tools\Ninja\ninja.exe"
