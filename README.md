# padre-audio-engine

Модульный каркас аудио-движка для ESP32-S3 (Arduino IDE) с акцентом на независимые патчи, которые можно переносить между проектами.

## Что добавлено в репозитории

- `patches/wav_decoder/WavDecoder.h/.cpp` — независимый потоковый WAV-декодер:
  - PCM 8/16/24/32-bit;
  - IEEE float 32/64-bit;
  - A-law (format 6), μ-law (format 7);
  - WAVE_EXTENSIBLE для PCM/float.
- `patches/decoder/DecoderFacade.h/.cpp` — единая обвязка декодеров WAV/MP3/FLAC:
  - WAV декодируется через отдельный patch `wav_decoder`;
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
- `patches/input/InputEvent.h` — унифицированная модель событий ввода (`PressDown/PressUp/ShortPress/LongPress/ValueChanged`).
- `patches/io_buttons/ButtonInput.h/.cpp` — модуль кнопок с debounce и генерацией унифицированных событий.
- `patches/io_mpr121/Mpr121Input.h/.cpp` — модуль MPR121 (по электродам) с унифицированными событиями касаний.
- `patches/io_pots/PotInput.h/.cpp` — модуль потенциометров/ADC c deadband и событиями изменения значения.
- `patches/mixer/VoiceMixer.h/.cpp` — многоголосный микшер (N потоков):
  - `global gain` и `voice gain` с ограничением диапазона;
  - `pause/stop` для каждого голоса и глобально;
  - суммирование с saturating-clamp в `int16_t`.
- `patches/source/IAudioSource.h` — единый интерфейс источника аудиопотока.
- `patches/source/SdAudioSource.h/.cpp` и `patches/source/EmmcAudioSource.h/.cpp` — файловые провайдеры SD/eMMC через callback-адаптеры.
- `patches/source/WiFiAudioSource.h/.cpp` и `patches/source/HttpAudioSource.h/.cpp` — сетевые провайдеры WiFi/HTTP(S) через callback-адаптеры.
- `patches/source/AudioSourceRouter.h/.cpp` — маршрутизатор `scheme -> IAudioSource` (например, `sd://`, `http://`, `https://`).
- `patches/serial/SerialRuntimeConsole.h/.cpp` — однострочные serial-команды для runtime-конфигурации (`set/get/list`) и включения/выключения debug-логов (`debug on/off/toggle`).
- `patches/persistence/SettingsStore.h`, `patches/persistence/PreferencesStore.h/.cpp` и `patches/persistence/RuntimeSettingsPersistence.h/.cpp` — сохранение runtime-настроек в NVS (`Preferences`):
  - восстановление `volume` c применением safe boot через `VolumeController::restore`;
  - загрузка/сохранение произвольных float-параметров с clamp по диапазону;
  - абстракция `ISettingsStore` для подмены backend (NVS/mock).
- `patches/output/I2sPcm5122Output.h/.cpp` — адаптер вывода I2S/PCM5122:
  - кольцевая очередь PCM-сэмплов для развязки декодера и DMA/I2S;
  - `pump()` для фонового слива очереди в драйвер;
  - backpressure через `IAudioSink::writableSamples()` для ограничения декодера по свободному месту.
- `patches/telemetry/AudioPipelineTelemetry.h/.cpp` — телеметрия audio pipeline:
  - метрики заполнения буфера (текущее/avg/min/max, underrun/overrun);
  - метрики CPU цикла обработки (busy us и load %, avg/max, xrun);
  - минимальная диагностика состояния (`ok/warn/critical`) и периодический serial-отчёт.
- `examples/MinimalIntegration/MinimalIntegration.ino` — пример однострочной интеграции основных компонентов, включая runtime-команды по UART и персистентные настройки в NVS.

## Принцип интеграции

Каждый patch можно подключать отдельно:

```cpp
#include "patches/playlist/PlaylistManager.h"
#include "patches/control/VolumeController.h"
#include "patches/input/PressDetector.h"
#include "patches/source/IAudioSource.h"
```

И использовать без сильной связности с остальной системой.

## Быстрая проверка сборки примера

```bash
arduino-cli compile --fqbn esp32:esp32:esp32s3 --build-path /tmp/padre-audio-engine-build examples/MinimalIntegration
```

## Следующий шаг (рекомендуемая декомпозиция)

После добавления output-патча можно развивать:

1. Интеграцию реального I2S-драйвера ESP-IDF/Arduino (`i2s_write`, DMA watermark, IRQ-safe pump).
2. Параллельный апсемплинг/ресемплинг при несовпадении sample rate источника и DAC.

## Совместимость с железом из ТЗ

- ESP32-S3 N8R2
- PCM5122 (I2S DAC)
- MPR121 (I2C touch)
- microSD 4GB

Текущая версия — архитектурный старт: независимые компоненты (playlist/volume/input/source/mixer/wav_decoder) + рабочая декодерная обвязка WAV/MP3/FLAC через единый фасад и sink-интерфейс, готовый к наращиванию fade/runtime-команд.
