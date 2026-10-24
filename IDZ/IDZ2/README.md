# ИДЗ №2, вариант 15 — нелюдимые садовники

В терминале Ubuntu/WSL из этой папки:

```bash
cmake --preset debug
cmake --build --preset debug
./build/cmake-debug/gardeners data/demo.txt
./build/cmake-debug/gardeners_cond data/demo.txt
ctest --preset debug
```

Первая программа использует мьютекс и семафоры, вторая — мьютекс и условные переменные. Два садовника работают в двух потоках. Карта обновляется в терминале, полная история записывается в `garden.log`. Ctrl+C завершает работу.

Из корня `akos` исполняемые файлы находятся в `build/cmake-debug/IDZ/IDZ2/`, входные карты — в `IDZ/IDZ2/data/`.

В PowerShell: `./run.ps1`; другая версия: `./run.ps1 -Cond`. В VS Code откройте эту папку через WSL: Ubuntu, выберите пресет `debug`, нажмите Ctrl+Shift+B для сборки и F5 для отладки.

Формат данных и описание программы — в [отчёте](report/REPORT.md).
