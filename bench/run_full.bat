@echo off
rem Put cso_full.exe and this file next to your ISOs and double-click.
rem Full-ISO test at 256K: zopfli5 with splits 15 / 20, each with and without the libdeflate12 pre-check.
rem Results go to bench_full\ (summary.txt + one small .csv.gz per ISO). ISOs are only read.
rem Options: --threshold 0.98, --threads N, --block 262144
cd /d "%~dp0"
"%~dp0cso_full.exe" %* "%~dp0."
pause
