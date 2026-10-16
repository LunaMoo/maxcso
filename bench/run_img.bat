@echo off
rem Put img_bench.exe, CHDm.dll, iPBP.dll, mCSO.dll and this file next to your ISOs and double-click.
rem Converts each whole ISO to CHD (several codecs and hunk sizes) and to EBOOT.PBP, times it,
rem converts back and checks the image byte for byte. Results go to bench_lib\.
rem Needs free space for one compressed copy plus one ISO-sized temporary file.
rem CSO needs mCSO.dll built from the feature/zopfli-splits-precheck branch. --cso-sets A,B,C,D,L picks the CSO runs.
rem Options: --no-cso  --hunks 2048,4096,16384,65536  --codecs default,lzma,zstd  --no-chd  --no-pbp  --no-verify
cd /d "%~dp0"
"%~dp0img_bench.exe" %* "%~dp0."
pause
