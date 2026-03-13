# padre-audio-engine

Модульный каркас аудио-движка для ESP32-S3 (Arduino IDE) с акцентом на независимые патчи, которые можно переносить между проектами.

## Что добавлено в репозитории

- `patches/audio/decoder/WavDecoder.h/.cpp` — независимый потоковый WAV-декодер:
  - PCM 8/16/24/32-bit;
  - IEEE float 32/64-bit;
  - A-law (format 6), μ-law (format 7);
  - WAVE_EXTENSIBLE для PCM/float;
  - основной decode-path теперь выдаёт настоящий PCM32 с сохранением исходной разрядности в старших битах `int32_t`;
  - совместимый decode-path в PCM16 оставлен для старых потребителей.
- `patches/audio/decoder/Mp3Decoder.h/.cpp` — независимый потоковый MP3-декодер на базе helix-кода из `ESP32-audioI2S`:
  - декодирование в PCM16;
  - работа через `IAudioSource` callbacks;
  - унификация mono/stereo выхода для pipeline.
- `patches/audio/decoder/FlacDecoder.h/.cpp` — независимый потоковый FLAC-декодер на базе встроенного `dr_flac`:
  - основной decode-path теперь работает через `drflac_read_pcm_frames_s32` и выдаёт настоящий PCM32;
  - совместимый decode-path в PCM16 оставлен для старых потребителей;
  - работа через `IAudioSource` callbacks;
  - downmix многоканального потока до stereo (или mono passthrough).
- `patches/audio/decoder/DecoderFacade.h/.cpp` — единая обвязка декодеров WAV/MP3/FLAC:
  - WAV/MP3/FLAC декодируются внутренними patch-модулями из `patches/audio/decoder`;
  - при `IAudioSink::supportsPcm32()` выводит WAV/FLAC в PCM32 до sink, а MP3 расширяет из PCM16 в PCM32 без раннего клампа;
  - для sink без PCM32 остаётся совместимый PCM16 path.
- `patches/media/playlist/PlaylistManager.h/.cpp` — независимый менеджер плейлистов:
  - sequential/shuffle;
  - пересоздание shuffle после полного прохода;
  - защита от повтора последнего трека старого плейлиста первым треком нового.
- `patches/audio/control/VolumeController.h/.cpp` — управление громкостью 0..20:
  - восстановление сохранённого значения;
  - правило safe boot (`<=15`), иначе fallback;
  - плавное изменение без резких скачков.
- `patches/input/core/PressDetector.h` — универсальный детектор short/long press для MPR121 и обычных кнопок.
- `patches/input/core/InputEvent.h` — унифицированная модель событий ввода (`PressDown/PressUp/ShortPress/LongPress/ValueChanged`).
- `patches/input/core/PlaybackInputActions.h/.cpp` — mapping `InputEvent -> playback actions`:
  - привязка source-id к действиям (`pause/next/volume +/-`);
  - callback-интерфейс для выполнения действий без хардкода в `.ino`.
- `patches/input/mpr121/Mpr121Input.h/.cpp` — модуль MPR121 (по электродам) с унифицированными событиями касаний.
- `patches/input/mpr121/Mpr121TouchController.h/.cpp` — orchestrator для группы электродов MPR121:
  - единый poll цикла с чтением touch-mask и dispatch `InputEvent`;
  - подключение callback-обработчика действий UI.
- `patches/input/mpr121/Mpr121AdafruitDriver.h/.cpp` — runtime-обвязка реального `Adafruit_MPR121`:
  - инициализация I2C + IRQ;
  - чтение touch-mask через `Adafruit_MPR121`;
  - переинициализация порогов касания/отпускания.
- `patches/audio/mixer/VoiceMixer.h/.cpp` — многоголосный микшер (N потоков):
  - `global gain` и `voice gain` с ограничением диапазона;
  - `pause/stop` для каждого голоса и глобально;
  - чтение голосов и суммирование в `int32_t` с saturating-clamp.
- `patches/media/source/IAudioSource.h` — единый интерфейс источника аудиопотока.
- `patches/media/source/FsAudioSource.h/.cpp` — универсальный файловый источник на базе `FS`:
  - прямое чтение из `SD/SPIFFS/LittleFS` через единый `IAudioSource`;
  - опциональная фильтрация URI по префиксу (например, `sd://`).
- `patches/media/library/AudioFileScanner.h/.cpp` — сканер аудио-файлов в файловой системе (`FS`):
  - рекурсивный обход каталогов с ограничением глубины;
  - фильтрация по расширениям (`.wav/.mp3/.flac` по умолчанию).
- `patches/audio/output/BufferedI2sOutput.h/.cpp` — адаптер вывода I2S/PCM5122:
  - кольцевая очередь PCM-сэмплов для развязки декодера и DMA/I2S;
  - `pump()` для фонового слива очереди в драйвер + ISR-safe `requestPumpFromIsr()`;
  - DMA watermark (`dma_watermark_samples`) для старта/продолжения слива;
  - runtime-обновление watermark через `setDmaWatermarkSamples()`.
  - backpressure через `IAudioSink::writableSamples()` для ограничения декодера по свободному месту.
- `patches/audio/output/Esp32StdI2sOutputIo.h/.cpp` — адаптер для ESP-IDF `i2s_std` channel API:
  - `i2s_new_channel / i2s_channel_init_std_mode / i2s_channel_write`;
  - встроенная prebuffer-gate логика (`setPrebuffering`) для связки с `PlaybackController`;
  - опциональный sample-transform callback (например, software volume/gain).
- `patches/app/playback/PlaybackController.h/.cpp` — контроллер playback-сессии:
  - `startTrack/playCurrentTrack/playNextTrack/service/stop`;
  - prebuffer-хуки для интеграции с output-слоем.
- `patches/app/playback/PlaybackAutoAdvance.h/.cpp` — state-machine автоперехода воспроизведения:
  - обработка `next`-запросов из UI;
  - автопереход на следующий трек при завершении текущего;
  - retry-таймер на случай временно недоступного следующего трека.
- `patches/app/composites/FsLibraryFacade.h/.cpp` — фасад для FS-источника и сканера библиотеки (`FsAudioSource + AudioFileScanner`).
- `patches/app/composites/PlaybackEngine.h/.cpp` — композит playback-ядра (`PlaybackController + PlaybackAutoAdvance + PlaylistManager`) с единым API `setTracks/start/step`.
- `patches/app/composites/Mpr121InputComposite.h/.cpp` — композит сенсорного ввода (`Mpr121TouchController + PlaybackInputActions`) с единым poll-циклом и обработкой событий.
- `examples/DualSdWavLoopI2s/DualSdWavLoopI2s.ino` — пример dual-playlist WAV playback с SD, MPR121 и I2S DAC.
- `examples/DualSdmmc4BitWavLoopI2s/DualSdmmc4BitWavLoopI2s.ino` — тот же dual-playlist WAV playback pipeline, но с `SD_MMC` в 4-bit режиме вместо SPI SD.
  - дефолтные SDMMC GPIO вынесены в начало файла и рассчитаны на ручную подстройку под конкретную разводку платы.
  - при старте печатает pinout и параметры карты, чтобы быстрее диагностировать wiring/pull-up проблемы.
- `examples/test-sd-mpr121/test-sd-mpr121.ino` — SD/I2S пример с `MPR121` и touch-управлением воспроизведением.

## Принцип интеграции

Каждый patch можно подключать отдельно:

```cpp
#include "patches/media/playlist/PlaylistManager.h"
#include "patches/audio/control/VolumeController.h"
#include "patches/input/core/PressDetector.h"
#include "patches/media/source/IAudioSource.h"
```

И использовать без сильной связности с остальной системой.

## Следующий шаг (рекомендуемая декомпозиция)

После добавления output-патча можно развивать:

1. Параллельный апсемплинг/ресемплинг при несовпадении sample rate источника и DAC.
2. Тонкую настройку DMA/latency профилей (music vs voice/UI) и prefill-стратегий под разные источники.

## Совместимость с железом из ТЗ

- ESP32-S3 N8R2
- PCM5122 (I2S DAC)
- MPR121 (I2C touch)
- microSD 4GB

Текущая версия — архитектурный старт: независимые компоненты (playlist/volume/input/source/mixer/decoder) + рабочая декодерная обвязка WAV/MP3/FLAC через единый фасад и sink-интерфейс, готовый к наращиванию сценариев управления.
