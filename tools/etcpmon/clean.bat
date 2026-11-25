@echo off
echo Cleaning ETCP Monitor build files...

del /Q *.o 2>nul
del /Q *.exe 2>nul
del /Q build.log 2>nul

echo Clean complete!
build.bat
