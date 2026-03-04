# padre-audio-engine

Модульный каркас аудио-движка для ESP32-S3 (Arduino IDE) с акцентом на независимые патчи, которые можно переносить между проектами.

## Что добавлено в репозитории

- `patches/audio_decoder/DecoderFacade.h` — минимальная фасад-обвязка для определения формата (WAV/MP3/FLAC) и общей конфигурации выхода 48kHz/24bit.
- `patches/playlist/PlaylistManager.h/.cpp` — независимый менеджер плейлистов:
  - sequential/shuffle;
  - пересоздание shuffle после полного прохода;
  - защита от повтора последнего трека старого плейлиста первым треком нового.
- `patches/control/VolumeController.h/.cpp` — управление громкостью 0..20:
  - восстановление сохранённого значения;
  - правило safe boot (`<=15`), иначе fallback;
  - плавное изменение без резких скачков.
- `patches/input/PressDetector.h` — универсальный детектор short/long press для MPR121 и обычных кнопок.
- `examples/MinimalIntegration.ino` — пример однострочной интеграции основных компонентов.

## Принцип интеграции

Каждый patch можно подключать отдельно:

```cpp
#include "patches/playlist/PlaylistManager.h"
#include "patches/control/VolumeController.h"
#include "patches/input/PressDetector.h"
```

И использовать без сильной связности с остальной системой.

## Следующий шаг (рекомендуемая декомпозиция)

Для полного покрытия вашего ТЗ логично добавить следующими независимыми патчами:

1. `patches/source/` — SD/eMMC/WiFi/HTTP провайдеры потоков (единый интерфейс `IAudioSource`).
2. `patches/decoder/` — реальная обвязка декодеров под wav/mp3/flac (например, через AudioTools/Helix/FLAC).
3. `patches/mixer/` — многоголосный микшер (N потоков) + global/voice gain + pause/stop.
4. `patches/fade/` — fade in/out и crossfade с настраиваемой скоростью.
5. `patches/serial/` — однострочные команды и runtime-конфигурация + вкл/выкл debug-логов.
6. `patches/io_mpr121/`, `patches/io_buttons/`, `patches/io_pots/` — отдельные модули ввода с унифицированными событиями.
7. `patches/persistence/` — сохранение настроек громкости и параметров в NVS/Preferences.

## Совместимость с железом из ТЗ

- ESP32-S3 N8R2
- PCM5122 (I2S DAC)
- MPR121 (I2C touch)
- microSD 4GB

Текущая версия — архитектурный старт: независимые компоненты и базовая логика плейлист/громкость/нажатия, готовая к наращиванию декодеров и аудио-пайплайна.
