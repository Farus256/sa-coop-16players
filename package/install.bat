@echo off
chcp 65001 >nul
rem SA:COOP: installs the ASI loader when the game has none. Run it from the game folder.
cd /d "%~dp0"
rem the game's own files, not its exe: some builds name it otherwise (gta-sa.exe), issue #54
if not exist models\gta3.img (
    echo Здесь нет файлов игры. Распакуйте архив SA-COOP прямо в папку игры и запустите install.bat оттуда.
    echo The game's files are not here. Unpack the SA-COOP archive right into the game folder and run install.bat from there.
    pause
    exit /b 1
)
if not exist coop.asi (
    echo Не хватает coop.asi. Распакуйте архив SA-COOP в папку игры целиком.
    echo coop.asi is missing. Unpack the whole SA-COOP archive into the game folder.
    pause
    exit /b 1
)
rem gta_sa.exe is a 32-bit program: 2 GB of memory unless its header has the large-address flag (4 GB on a 64-bit
rem Windows). The long draw distance and the other players' cars and people ran the host's game out of memory after
rem a while (malloc gave NULL in an animation, 0x72F4DE). One bit of the header; the size stays, the old exe is kept.
if exist gta_sa.exe (
    powershell -NoProfile -ExecutionPolicy Bypass -Command "$p='gta_sa.exe'; $b=[IO.File]::ReadAllBytes($p); $pe=[BitConverter]::ToInt32($b,0x3C); if ($b[$pe] -ne 0x50 -or $b[$pe+1] -ne 0x45) { exit 1 }; $o=$pe+22; if ($b[$o] -band 0x20) { exit 2 }; if (-not (Test-Path 'gta_sa.exe.bak')) { Copy-Item $p 'gta_sa.exe.bak' }; $b[$o]=$b[$o] -bor 0x20; [IO.File]::WriteAllBytes($p,$b); exit 0"
    if errorlevel 2 (
        rem already has it
    ) else if errorlevel 1 (
        echo Не получилось открыть игре 4 ГБ памяти: gta_sa.exe занят или защищён. Закройте игру или запустите install.bat от имени администратора.
        echo Could not give the game 4 GB of memory: gta_sa.exe is in use or protected. Close the game or run install.bat as administrator.
    ) else (
        echo Игре открыто 4 ГБ памяти вместо 2. Прежний gta_sa.exe сохранён как gta_sa.exe.bak.
        echo The game can use 4 GB of memory instead of 2. The old gta_sa.exe is kept as gta_sa.exe.bak.
    )
)
if exist vorbisHooked.dll (
    echo Загрузчик ASI уже установлен. Ничего менять не нужно.
    echo Можно запускать игру.
    echo The ASI loader is already installed. Nothing needs changing.
    echo You can start the game.
    pause
    exit /b 0
)
if not exist vorbisFile.dll (
    echo В папке игры нет vorbisFile.dll. Похоже, это не GTA San Andreas 1.0.
    echo There is no vorbisFile.dll in the game folder. This does not look like GTA San Andreas 1.0.
    pause
    exit /b 1
)
ren vorbisFile.dll vorbisHooked.dll
copy /y "coop\loader\vorbisFile.dll" vorbisFile.dll >nul
if errorlevel 1 (
    ren vorbisHooked.dll vorbisFile.dll
    echo Не получилось скопировать загрузчик. Если игра лежит в Program Files, запустите install.bat от имени администратора.
    echo Could not copy the loader. If the game is in Program Files, run install.bat as administrator.
    pause
    exit /b 1
)
echo Загрузчик ASI установлен. Оригинальный vorbisFile.dll сохранён как vorbisHooked.dll.
echo Можно запускать игру.
echo The ASI loader is installed. The original vorbisFile.dll is kept as vorbisHooked.dll.
echo You can start the game.
pause
