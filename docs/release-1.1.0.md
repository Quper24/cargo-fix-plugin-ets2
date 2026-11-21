# Выпуск 1.1.0

Главная ветка: `main`. Создание заказов включено по умолчанию (`CARGO_CONTROL_CUSTOM_JOBS=ON`), имя единой DLL для ATS/ETS2 — `CargoControl.dll`. Исследование Road Trip и Coaches остаётся в `dev`.

Проверки 21.11.2026: сборка Release, CTest 34/34, отдельный DX11-предпросмотр четырёх состояний интерфейса; просмотр формы создания заказа. Новая игровая проверка после перевода в main не проводилась.

Сборка: `build-release/bin/Release/CargoControl.dll`. Архив: `release/CargoControl-1.1.0.zip` (локальные артефакты, не входят в Git).

SHA-256 DLL: `F9A9F39C679FCCD5089B5E3BCB1A68FA6CE2507C999C758673D69ABD6EA43C9B`.

DLL установлена в ATS; `CargoControl-dev.dll` заменена на `CargoControl.dll`. Предыдущая DLL сохранена в `diagnostics/plugin-backups`. Установка ETS2 в этом шаге не изменялась.
