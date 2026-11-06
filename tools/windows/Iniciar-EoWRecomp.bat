@echo off
rem Starts EoWRecomp from this folder (so the cache and game files stay next to the program).
cd /d "%~dp0"
if not exist "eowrecomp.exe" (
    echo eowrecomp.exe nao encontrado nesta pasta. Extraia o ZIP inteiro antes de abrir.
    pause
    exit /b 1
)
echo "%~dp0" | findstr /i /c:"\Temp\\" /c:".zip" >nul && (
    echo Parece que voce esta rodando de dentro do ZIP. Extraia o ZIP inteiro para uma pasta primeiro.
    pause
    exit /b 1
)
start "" "eowrecomp.exe" %*
