# padre-audio-engine

Модульный каркас аудио-движка для ESP32-S3 (Arduino IDE) с акцентом на независимые патчи, которые можно переносить между проектами.

## Что добавлено в репозитории

- `patches/audio_decoder/DecoderFacade.h` — фасад для автоопределения формата WAV/MP3/FLAC.
- `patches/audio_decoder/WavDecoder.h/.cpp` — реальный PCM/float WAV декодер:
  - mono/stereo;
  - 44100/48000 Hz;
  - 16/24/32/32float.
- `patches/audio_decoder/Mp3Decoder.h/.cpp` — MP3 декодер через `minimp3` backend.
- `patches/audio_decoder/FlacDecoder.h/.cpp` — FLAC декодер через `dr_flac` backend.
- `patches/audio_decoder/AudioTypes.h` — общий контракт `DecodedAudio` + `DecodeResult`.
- `patches/playlist/PlaylistManager.h/.cpp` — независимый менеджер плейлистов.
- `patches/control/VolumeController.h/.cpp` — управление громкостью 0..20.
- `patches/input/PressDetector.h` — универсальный детектор short/long press.
- `examples/MinimalIntegration.ino` — пример интеграции (декодер + контролы).

## Быстрый старт (1-2 строки)

```cpp
padre::DecoderFacade decoder;
padre::DecodedAudio pcm;
const auto result = decoder.decode_file("/music/track.flac", pcm);
```

`pcm.samples` содержит interleaved float32 сэмплы в диапазоне `[-1..1]`.

## Подключение backend-ов MP3/FLAC

Для MP3/FLAC нужны single-header зависимости рядом с кодом:

- `patches/audio_decoder/third_party/minimp3.h`
- `patches/audio_decoder/third_party/minimp3_ex.h`
- `patches/audio_decoder/third_party/dr_flac.h`

Если они отсутствуют, WAV продолжит работать, а MP3/FLAC вернут понятную ошибку с подсказкой.

## Принцип интеграции

Каждый patch можно подключать отдельно:

```cpp
#include "patches/audio_decoder/DecoderFacade.h"
#include "patches/playlist/PlaylistManager.h"
#include "patches/control/VolumeController.h"
#include "patches/input/PressDetector.h"
```

И использовать без сильной связности с остальной системой.

## Совместимость с железом из ТЗ

- ESP32-S3 N8R2
- PCM5122 (I2S DAC)
- MPR121 (I2C touch)
- microSD 4GB
