# padre-audio-engine

Модульный каркас аудио-движка для ESP32-S3 (Arduino IDE) с акцентом на независимые патчи, которые можно переносить между проектами.

## Что добавлено в репозитории

- `patches/audio/decoder/WavDecoder.h/.cpp` — независимый потоковый WAV-декодер:
  - PCM 8/16/24/32-bit;
  - IEEE float 32/64-bit;
  - A-law (format 6), μ-law (format 7);
  - WAVE_EXTENSIBLE для PCM/float.
- `patches/audio/decoder/Mp3Decoder.h/.cpp` — независимый потоковый MP3-декодер на базе helix-кода из `ESP32-audioI2S`:
  - декодирование в PCM16;
  - работа через `IAudioSource` callbacks;
  - унификация mono/stereo выхода для pipeline.
- `patches/audio/decoder/FlacDecoder.h/.cpp` — независимый потоковый FLAC-декодер на базе встроенного `dr_flac`:
  - декодирование в PCM16;
  - работа через `IAudioSource` callbacks;
  - downmix многоканального потока до stereo (или mono passthrough).
- `patches/audio/decoder/DecoderFacade.h/.cpp` — единая обвязка декодеров WAV/MP3/FLAC:
  - WAV/MP3/FLAC декодируются внутренними patch-модулями из `patches/audio/decoder`;
  - единый цикл `begin/process/stop` с выводом в абстрактный `IAudioSink`.
- `patches/media/playlist/PlaylistManager.h/.cpp` — независимый менеджер плейлистов:
  - sequential/shuffle;
  - пересоздание shuffle после полного прохода;
  - защита от повтора последнего трека старого плейлиста первым треком нового.
- `patches/audio/control/VolumeController.h/.cpp` — управление громкостью 0..20:
  - восстановление сохранённого значения;
  - правило safe boot (`<=15`), иначе fallback;
  - плавное изменение без резких скачков.
- `patches/audio/fade/FadeController.h/.cpp` — fade in/out и crossfade с настраиваемой скоростью:
  - линейный fade по `gain/sec` с ограничением диапазона;
  - `FadeValue` для одиночного канала;
  - `CrossfadeController` для плавного перехода между двумя источниками.
- `patches/input/core/PressDetector.h` — универсальный детектор short/long press для MPR121 и обычных кнопок.
- `patches/input/core/InputEvent.h` — унифицированная модель событий ввода (`PressDown/PressUp/ShortPress/LongPress/ValueChanged`).
- `patches/input/core/PlaybackInputActions.h/.cpp` — mapping `InputEvent -> playback actions`:
  - привязка source-id к действиям (`pause/next/volume +/-`);
  - callback-интерфейс для выполнения действий без хардкода в `.ino`.
- `patches/input/buttons/ButtonInput.h/.cpp` — модуль кнопок с debounce и генерацией унифицированных событий.
- `patches/input/mpr121/Mpr121Input.h/.cpp` — модуль MPR121 (по электродам) с унифицированными событиями касаний.
- `patches/input/mpr121/Mpr121TouchController.h/.cpp` — orchestrator для группы электродов MPR121:
  - единый poll цикла с чтением touch-mask и dispatch `InputEvent`;
  - встроенный debug-лог изменения touch-mask;
  - подключение callback-обработчика действий UI.
- `patches/input/pots/PotInput.h/.cpp` — модуль потенциометров/ADC c deadband и событиями изменения значения.
- `patches/audio/mixer/VoiceMixer.h/.cpp` — многоголосный микшер (N потоков):
  - `global gain` и `voice gain` с ограничением диапазона;
  - `pause/stop` для каждого голоса и глобально;
  - суммирование с saturating-clamp в `int16_t`.
- `patches/media/source/IAudioSource.h` — единый интерфейс источника аудиопотока.
- `patches/media/source/FsAudioSource.h/.cpp` — универсальный файловый источник на базе `FS`:
  - прямое чтение из `SD/SPIFFS/LittleFS` через единый `IAudioSource`;
  - опциональная фильтрация URI по префиксу (например, `sd://`).
- `patches/media/source/SdAudioSource.h/.cpp` и `patches/media/source/EmmcAudioSource.h/.cpp` — файловые провайдеры SD/eMMC через callback-адаптеры.
- `patches/media/source/WiFiAudioSource.h/.cpp` и `patches/media/source/HttpAudioSource.h/.cpp` — сетевые провайдеры WiFi/HTTP(S) через callback-адаптеры.
- `patches/media/source/AudioSourceRouter.h/.cpp` — маршрутизатор `scheme -> IAudioSource` (например, `sd://`, `http://`, `https://`).
- `patches/media/library/AudioFileScanner.h/.cpp` — сканер аудио-файлов в файловой системе (`FS`):
  - рекурсивный обход каталогов с ограничением глубины;
  - фильтрация по расширениям (`.wav/.mp3/.flac` по умолчанию).
- `patches/app/serial/SerialRuntimeConsole.h/.cpp` — однострочные serial-команды для runtime-конфигурации (`set/get/list`) и включения/выключения debug-логов (`debug on/off/toggle`).
  - поддержка расширяемых команд через callback (`RuntimeCommandEntry`) для проект-специфичных runtime действий.
- `patches/app/persistence/SettingsStore.h`, `patches/app/persistence/PreferencesStore.h/.cpp` и `patches/app/persistence/RuntimeSettingsPersistence.h/.cpp` — сохранение runtime-настроек в NVS (`Preferences`):
  - восстановление `volume` c применением safe boot через `VolumeController::restore`;
  - загрузка/сохранение произвольных float-параметров с clamp по диапазону;
  - абстракция `ISettingsStore` для подмены backend (NVS/mock).
- `patches/audio/output/BufferedI2sOutput.h/.cpp` — адаптер вывода I2S/PCM5122:
  - кольцевая очередь PCM-сэмплов для развязки декодера и DMA/I2S;
  - `pump()` для фонового слива очереди в драйвер + ISR-safe `requestPumpFromIsr()`;
  - DMA watermark (`dma_watermark_samples`) для старта/продолжения слива;
  - runtime-обновление watermark через `setDmaWatermarkSamples()`.
  - backpressure через `IAudioSink::writableSamples()` для ограничения декодера по свободному месту.
- `patches/audio/output/Esp32I2sOutputIo.h/.cpp` — реальная интеграция с ESP-IDF/Arduino legacy I2S:
  - `i2s_driver_install / i2s_set_pin / i2s_set_clk / i2s_write`;
  - non-blocking pump (`write_timeout_ms=0`) и оценка заполнения DMA для `availableForWrite()`;
  - runtime-переключение DMA-профиля (`applyDmaProfile`) без изменения API декодера/микшера.
- `patches/audio/output/Esp32StdI2sOutputIo.h/.cpp` — адаптер для ESP-IDF `i2s_std` channel API:
  - `i2s_new_channel / i2s_channel_init_std_mode / i2s_channel_write`;
  - встроенная prebuffer-gate логика (`setPrebuffering`) для связки с `PlaybackController`;
  - опциональный sample-transform callback (например, software volume/gain).
- `patches/app/playback/PlaybackController.h/.cpp` — контроллер playback-сессии:
  - `startTrack/playCurrentTrack/playNextTrack/service/stop`;
  - prebuffer-хуки и телеметрические callbacks для интеграции с loop-метриками.
- `patches/app/playback/PlaybackAutoAdvance.h/.cpp` — state-machine автоперехода воспроизведения:
  - обработка `next`-запросов из UI;
  - автопереход на следующий трек при завершении текущего;
  - retry-таймер на случай временно недоступного следующего трека.
- `patches/app/telemetry/AudioPipelineTelemetry.h/.cpp` — телеметрия audio pipeline:
  - метрики заполнения буфера (текущее/avg/min/max, underrun/overrun);
  - метрики CPU цикла обработки (busy us и load %, avg/max, xrun);
  - минимальная диагностика состояния (`ok/warn/critical`) и периодический serial-отчёт.
- `patches/app/telemetry/PlaybackPerfTelemetry.h/.cpp` — lightweight perf-метрики playback loop:
  - агрегаты `loop/service/decode` (avg/max/slow/budget-hit);
  - события очереди (`qmin/low/empty`) и latency на `next`-команду от UI;
  - периодический serial-отчёт в формате `PERF ...`.
- `patches/app/composites/FsLibraryFacade.h/.cpp` — фасад для FS-источника и сканера библиотеки (`FsAudioSource + AudioFileScanner`).
- `patches/app/composites/PlaybackEngine.h/.cpp` — композит playback-ядра (`PlaybackController + PlaybackAutoAdvance + PlaylistManager`) с единым API `setTracks/start/step`.
- `patches/app/composites/Mpr121InputComposite.h/.cpp` — композит сенсорного ввода (`Mpr121TouchController + PlaybackInputActions`) с единым poll-циклом и обработкой событий.
- `examples/MinimalIntegration/MinimalIntegration.ino` — пример однострочной интеграции основных компонентов, включая runtime-команды по UART и персистентные настройки в NVS.
  - пример custom-команды `i2s profile <loop|balanced|oneshot>` для runtime-переключения DMA-профилей.

## Принцип интеграции

Каждый patch можно подключать отдельно:

```cpp
#include "patches/media/playlist/PlaylistManager.h"
#include "patches/audio/control/VolumeController.h"
#include "patches/input/core/PressDetector.h"
#include "patches/media/source/IAudioSource.h"
```

И использовать без сильной связности с остальной системой.

## Быстрая проверка сборки примера

```bash
arduino-cli compile --fqbn esp32:esp32:esp32s3 --build-path /tmp/padre-audio-engine-build examples/MinimalIntegration
```

## Следующий шаг (рекомендуемая декомпозиция)

После добавления output-патча можно развивать:

1. Параллельный апсемплинг/ресемплинг при несовпадении sample rate источника и DAC.
2. Тонкую настройку DMA/latency профилей (music vs voice/UI) и prefill-стратегий под разные источники.

## Совместимость с железом из ТЗ

- ESP32-S3 N8R2
- PCM5122 (I2S DAC)
- MPR121 (I2C touch)
- microSD 4GB

Текущая версия — архитектурный старт: независимые компоненты (playlist/volume/input/source/mixer/decoder) + рабочая декодерная обвязка WAV/MP3/FLAC через единый фасад и sink-интерфейс, готовый к наращиванию fade/runtime-команд.
