# padre-audio-engine

Модульный каркас аудио-движка для ESP32-S3 (Arduino IDE) с акцентом на независимые патчи, которые можно переносить между проектами.

## Что добавлено в репозитории

- `patches/decoder/DecoderFacade.h/.cpp` — реальная обвязка декодеров WAV/MP3/FLAC:
  - WAV PCM (16-bit) декодируется встроенно;
  - MP3/FLAC подключаются как внешние адаптеры (например, AudioTools/Helix/FLAC);
  - единый цикл `begin/process/stop` с выводом в абстрактный `IAudioSink`.
- `patches/playlist/PlaylistManager.h/.cpp` — независимый менеджер плейлистов:
  - sequential/shuffle;
  - пересоздание shuffle после полного прохода;
  - защита от повтора последнего трека старого плейлиста первым треком нового.
- `patches/control/VolumeController.h/.cpp` — управление громкостью 0..20:
  - восстановление сохранённого значения;
  - правило safe boot (`<=15`), иначе fallback;
  - плавное изменение без резких скачков.
- `patches/fade/FadeController.h/.cpp` — fade in/out и crossfade с настраиваемой скоростью:
  - линейный fade по `gain/sec` с ограничением диапазона;
  - `FadeValue` для одиночного канала;
  - `CrossfadeController` для плавного перехода между двумя источниками.
- `patches/input/PressDetector.h` — универсальный детектор short/long press для MPR121 и обычных кнопок.
- `patches/mixer/VoiceMixer.h/.cpp` — многоголосный микшер (N потоков):
  - `global gain` и `voice gain` с ограничением диапазона;
  - `pause/stop` для каждого голоса и глобально;
  - суммирование с saturating-clamp в `int16_t`.
- `patches/source/IAudioSource.h` — единый интерфейс источника аудиопотока.
- `patches/source/SdAudioSource.h/.cpp` и `patches/source/EmmcAudioSource.h/.cpp` — файловые провайдеры SD/eMMC через callback-адаптеры.
- `patches/source/WiFiAudioSource.h/.cpp` и `patches/source/HttpAudioSource.h/.cpp` — сетевые провайдеры WiFi/HTTP(S) через callback-адаптеры.
- `patches/source/AudioSourceRouter.h/.cpp` — маршрутизатор `scheme -> IAudioSource` (например, `sd://`, `http://`, `https://`).
- `examples/MinimalIntegration.ino` — пример однострочной интеграции основных компонентов.

## Принцип интеграции

Каждый patch можно подключать отдельно:

```cpp
#include "patches/playlist/PlaylistManager.h"
#include "patches/control/VolumeController.h"
#include "patches/input/PressDetector.h"
#include "patches/source/IAudioSource.h"
```

И использовать без сильной связности с остальной системой.

## Следующий шаг (рекомендуемая декомпозиция)

Следующими независимыми патчами можно расширять пайплайн:

1. `patches/serial/` — однострочные команды и runtime-конфигурация + вкл/выкл debug-логов.
2. `patches/io_mpr121/`, `patches/io_buttons/`, `patches/io_pots/` — отдельные модули ввода с унифицированными событиями.
3. `patches/persistence/` — сохранение настроек громкости и параметров в NVS/Preferences.

## Совместимость с железом из ТЗ

- ESP32-S3 N8R2
- PCM5122 (I2S DAC)
- MPR121 (I2C touch)
- microSD 4GB

Текущая версия — архитектурный старт: независимые компоненты (playlist/volume/input/source/mixer) + рабочая декодерная обвязка WAV/MP3/FLAC через единый фасад и sink-интерфейс, готовый к наращиванию fade/runtime-команд.
