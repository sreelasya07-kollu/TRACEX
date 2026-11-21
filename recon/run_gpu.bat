@echo off
call "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat"
set "PATH=F:\3d\roomscan-recon\.venv\Scripts;C:\Program Files\NVIDIA GPU Computing Toolkit\CUDA\v12.8\bin;%PATH%"
cd /d F:\3d\roomscan-recon
F:\3d\roomscan-recon\.venv\Scripts\python.exe -m pytest %*
