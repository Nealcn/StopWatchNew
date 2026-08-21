@echo off
set MSYSTEM=
set IDF_PATH=D:\esp-idf
set IDF_TOOLS_PATH=D:\Espressif
set IDF_PYTHON_ENV_PATH=D:\Espressif\python_env\idf5.5_py3.10_env
set PATH=D:\Espressif\tools\xtensa-esp-elf\esp-14.2.0_20260121\xtensa-esp-elf\bin;D:\Espressif\tools\ninja\1.12.1;D:\Espressif\tools\cmake\3.30.2\bin;D:\Espressif\tools\idf-exe\1.0.3;D:\Espressif\python_env\idf5.5_py3.10_env\Scripts;%PATH%
cd /d %~dp0
python D:\esp-idf\tools\idf.py -p COM6 flash
echo.
echo === Flash complete. ===
pause
