@echo off
rem Abre el panel local de vigilancia (cerrar esta ventana para apagarlo)
cd /d "%~dp0"
python servidor.py
pause
