@echo off
rem Open Watcom 2.0 ring-3 firmware-loader build.
if "%WATCOM%" == "" goto nowatcom
if not exist build mkdir build
wcl386 -q -bt=os2 -i=include -fe=build\iwmfwld.exe tools\iwmfwld.c core\iwm_crc32.c
if errorlevel 1 goto failed
echo Built build\iwmfwld.exe
goto end
:nowatcom
echo WATCOM is not set.
goto end
:failed
echo Build failed.
:end

