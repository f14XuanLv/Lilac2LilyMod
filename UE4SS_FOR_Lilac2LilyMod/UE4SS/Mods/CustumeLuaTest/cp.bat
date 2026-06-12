@echo off
setlocal

set "SRC=%~dp0"
set "DST=C:\Game\Steam\steamapps\common\ENDER MAGNOLIA\EnderMagnolia\Binaries\Win64Lilac2Lily\Mods\CustumeLuaTest"

if not exist "%DST%\Scripts" mkdir "%DST%\Scripts"

copy /Y "%SRC%Scripts\main.lua" "%DST%\Scripts\main.lua"
copy /Y "%SRC%Scripts\json.lua" "%DST%\Scripts\json.lua"
copy /Y "%SRC%DT_ItemCostumes_Extension.json" "%DST%\DT_ItemCostumes_Extension.json"

echo.
echo Synced CustumeLuaTest to:
echo %DST%

endlocal
