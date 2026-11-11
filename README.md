# TBOI Better Coop

[Русский](#русский) · [English](#english)

## Русский

**TL;DR** — Экспериментальный мод для приватного онлайн-кооператива вдвоём в **The Binding of Isaac: Repentance+** на Windows (Steam). Мод сделан с помощью Codex, чтобы после вылета было проще вернуться в забег и продолжить играть вместе.

- **Возврат на том же этаже.** Второй игрок может войти в текущий забег при переходе хоста в подходящую комнату, без ожидания следующего этажа.
- **Сохранение и продолжение онлайн-забега.** Хост может сохранить состояние обоих игроков и позднее продолжить забег с тем же напарником.

### Установка и запуск

1. Закройте игру и положите `winmm.dll` из архива `TBOI-Better-Coop-win32.zip` в разделе **Releases** или из папки `build` рядом с `isaac-ng.exe`. Если в папке игры уже есть `winmm.dll` от другого мода, сначала сделайте копию существующего файла.
2. В Steam откройте свойства игры → **Общие** → **Параметры запуска**. Укажите нужную строку из таблицы и запустите игру через Steam.

| Кто и когда запускает игру | Параметры запуска |
| --- | --- |
| Хост начинает новый забег с возвратом через дверь и сохранением | `-RoomRejoin -SaveOnline` |
| Второй игрок с возвратом через дверь | `-RoomRejoin` |
| Хост продолжает сохранённый забег | `-RoomRejoin -ResumeOnline` |

Для входа через дверь у обоих игроков должна стоять одна версия мода. Мод рассчитан на конкретную сборку игры. Если после обновления Steam изменится `isaac-ng.exe`, мод покажет предупреждение и отключит свои функции. Для удаления мода уберите его `winmm.dll` из папки игры.

### Как вернуться на том же этаже

1. Второй игрок снова входит в приватное лобби или лобби для друзей и ждёт.
2. Хост проходит через обычную дверь из одной зачищенной комнаты в другую. Перед переходом проверьте [условия спавна](#известные-ограничения).
3. Как только хост пересёк дверь, обоим нужно отпустить клавиши и стики. Не трогайте управление, пока второй игрок не загрузится. Если случился рассинхрон (`DESYNCED`), пусть второй игрок снова войдёт в лобби, а хост попробует другую комнату.

### Как сохранить и продолжить забег

1. Чтобы сохранить забег, хост штатно выходит первым из стабильной зачищенной комнаты, пока оба игрока ещё находятся в забеге.
2. Чтобы продолжить, хост перезапускает игру с `-RoomRejoin -ResumeOnline`, создаёт приватное лобби или лобби для друзей, приглашает того же игрока и запускает забег через меню лобби.
3. `-ResumeOnline` также включает сохранение при следующем выходе. Для нового забега замените его на `-SaveOnline`.

У хоста есть один слот чекпоинта в `tools/IsaacNetDiag/saves/online-run.ind` относительно папки игры. Новое успешное сохранение заменяет предыдущее.

### Известные ограничения

Используйте мод **на свой страх и риск**. Возможны рассинхрон, зависание и потеря прогресса.

Для входа на том же этаже хост должен пройти через обычную дверь между двумя зачищенными комнатами. В комнате назначения не должно быть пикапов (например, монет, бомб, ключей или предметов на пьедесталах) и автоматов. Стартовая комната этажа всегда пропускается.

Для спавна подойдёт обычная зачищенная комната, где вы уже подобрали награду. Проверьте, что центр свободен и от него можно пройти к двери. При загрузке игроки могут появиться в центре, а камни или ямы вокруг могут перекрыть выход.

### Сборка

Установите Visual Studio с инструментами C++ для x86 и Windows SDK. Выполните `Build.cmd` в корне репозитория. Готовый файл появится в `build/winmm.dll`.

## English

**TL;DR** — An experimental mod for two-player private online co-op in **The Binding of Isaac: Repentance+** on Windows (Steam). It was built with help from Codex to make it easier to get back into a run after a disconnect and keep playing together.

- **Rejoin on the same floor.** The second player can enter the active run when the host moves to a suitable room, without waiting for the next floor.
- **Save and resume online runs.** The host can save both players' progress and continue the run later with the same partner.

### Installation and launch

1. Close the game and place `winmm.dll` from `TBOI-Better-Coop-win32.zip` under **Releases**, or from the `build` directory, next to `isaac-ng.exe`. If another mod already provides a `winmm.dll` in the game folder, make a copy of that file first.
2. In Steam, open the game's properties → **General** → **Launch Options**. Enter the appropriate line below and launch the game through Steam.

| Who is launching and when | Launch options |
| --- | --- |
| Host starting a new run with room rejoin and checkpoints | `-RoomRejoin -SaveOnline` |
| Second player using room rejoin | `-RoomRejoin` |
| Host resuming a saved run | `-RoomRejoin -ResumeOnline` |

Both players need the same version of the mod for room rejoin. The mod targets a specific game build. If a Steam update changes `isaac-ng.exe`, the mod displays a warning and disables its features. To uninstall the mod, remove its `winmm.dll` from the game folder.

### Rejoining on the same floor

1. The second player rejoins the private or friends-only lobby and waits.
2. The host uses a regular door to move between two cleared rooms. Check the [spawn requirements](#known-limitations) before going through.
3. As soon as the host crosses the door, both players should release their keys and sticks. Keep the controls untouched until the second player finishes loading. If you see `DESYNCED`, have the second player rejoin the lobby and try a different room.

### Saving and resuming a run

1. To save, the host exits normally from a stable, cleared room while both players are still in the run.
2. To resume, the host restarts the game with `-RoomRejoin -ResumeOnline`, opens a private or friends-only lobby, invites the same player, and starts the run from the lobby.
3. `-ResumeOnline` also enables saving the next time the host exits. Switch to `-SaveOnline` to start a new run.

The host has one checkpoint slot at `tools/IsaacNetDiag/saves/online-run.ind` inside the game folder. Each successful save replaces the previous checkpoint.

### Known limitations

Use this mod **at your own risk**. Desyncs, freezes, and lost progress are possible.

To rejoin on the same floor, the host must use a regular door between two cleared rooms. The destination must be free of pickups (such as coins, bombs, keys, or pedestal items) and machines. The floor's starting room is always skipped.

A regular cleared room where you've already collected the reward is a good choice. Check that the center is open and there is a clear path from it to the door. Players can spawn in the center, where surrounding rocks or pits may trap them.

### Building

Install Visual Studio with the x86 C++ toolset and the Windows SDK. Run `Build.cmd` from the repository root. The output is `build/winmm.dll`.
