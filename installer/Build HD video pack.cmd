@echo off
rem Builds the AI-upscaled video pack (hd_pack\) for voyeur_hd.exe from your disc.
rem Safe to stop (close the window) and run again: finished clips are kept.
cd /d "%~dp0"
echo Building the HD video pack for voyeur_hd.exe.
echo This uses your graphics card for several hours. You can close this window
echo at any time and run it again later; it carries on where it stopped.
echo.
hd_pack_builder.exe %*
echo.
if errorlevel 1 (echo The HD pack build stopped with an error.) else (echo Done. Start voyeur_hd.exe to play with the HD video.)
pause
