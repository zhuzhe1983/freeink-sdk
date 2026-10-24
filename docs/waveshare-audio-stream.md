# Waveshare 3.97 PCM output

This describes `FREEINK_DEVICE_WAVESHARE_EPAPER_397`, the CrossMux profile.
The separate upstream `FREEINK_DEVICE_WS397` profile is unchanged.

`AudioManager::beginPcmStream(24000)` exposes synchronous signed 16-bit mono
output to an existing decoder worker. Each source sample is sent once to both
16-bit Philips I2S slots at 24 kHz. There is no resampling, speed change, queue,
or extra playback task in this API. Existing WAV/`playPcm()` tasks remain
available for short sounds.

## Clock and gain evidence

The reference is Espressif esp-adf commit
[`1c7aa251b937742f1308c4f3b9e7587034bc5fb7`](https://github.com/espressif/esp-adf/blob/1c7aa251b937742f1308c4f3b9e7587034bc5fb7/components/esp_codec_dev/device/es8311/es8311.c),
`components/esp_codec_dev/device/es8311/es8311.c`, inspected 2026-10-24.
Its clock coefficient table has the following 256-fs entries:

| PCM rate | MCLK | Pre divider/multiplier | ADC/DAC divider | LRCK H/L | BCLK divider | ADC OSR | DAC OSR |
| --- | --- | --- | --- | --- | --- | --- | --- |
| 16 kHz | 4.096 MHz | 1 / 1 | 1 / 1 | 0 / 255 | 4 | 16 | 32 |
| 24 kHz | 6.144 MHz | 1 / 1 | 1 / 1 | 0 / 255 | 4 | 16 | 16 |

The driver maps DAC OSR to register `0x04`: `0x20` for 16 kHz and `0x10` for
24 kHz. CrossMux already used the 16 kHz divider sequence; the new stream
changes the I2S clock and DAC OSR together. Switching back to a 16 kHz short
sound restores `0x20`. The `I2sEs8311Mclk16k` enum spelling and value remain
unchanged for compatibility, but the profile now accepts these two rates.

The same reference maps register `0x32` from -95.5 dB at 0 to +32 dB at 255
(0.5 dB steps), making `0xBF` unity/0 dB. Streams hold that codec gain fixed
and apply `sample * min(volume,100) / 100` during mono-to-stereo conversion.
The legacy short-sound `setVolume()` mapping is unchanged; its `0xCF` maximum
is +8 dB. No I2C operations occur in `writePcmStream()`.

## Ownership and failure handling

The caller owns serialization across **all** `AudioManager` methods, including
short sounds, volume, shutdown, and stream begin/write/end. It must also
serialize the codec's shared I2C bus against other peripherals. A stream does
not claim the bus or spawn a task itself. One existing worker owns the stream;
foreground code passes volume changes to that worker.

`beginPcmStream()` refuses any live asynchronous playback. Before starting it,
the caller may request `stop()` and must then check `isPlaying()`. If a source
callback outlives the existing two-second stop wait, `powerDown()` and new
playback leave its channel intact. They cannot forcibly destroy its task or
source. The source callback must eventually return so the worker can clean up.

Each write accepts 1..1024 samples. Positive partial writes advance only the
accepted prefix, including a partial `ESP_ERR_TIMEOUT` result. Invalid byte
counts, zero progress, hardware errors, or exhausting the 100 ms DMA budget
make the stream terminal and immediately turn off the amplifier. The producer
must stop sending and call `endPcmStream()`; it must never replay a failed block.
That final call releases I2S and suspends the codec even after a write failure.

Healthy end appends a 64-frame release ramp and 512 frames of silence; it never
removes or stretches submitted source samples. Stream DMA is four descriptors
of 120 stereo frames, totaling 1920 sample bytes and 20 ms at 24 kHz. More than
one full ring of appended silence drains the preceding samples and ramp before
the amplifier is disabled. Individual I2S waits are at most 20 ms, writes share
a 100 ms total budget, and end's ramp/drain shares 40 ms. GPIO, I2C bus locks,
codec writes, and IDF allocation/deallocation latency are outside those DMA
budgets; this is not a hard-real-time guarantee for the entire call.

Wave has no local codec rail-enable GPIO. End first drops the amplifier and
mutes the DAC, then suspends the DAC (`0x12=0x02`), analog path (`0x0D=0xFA`),
and codec clocks (`0x01=0`). These values follow the reference driver's suspend
sequence. Shared board power rails are left to the board owner. Next begin
reinitializes the codec. I2C failure makes shutdown best-effort; the amplifier
and I2S are still disabled and released.

Production conversion and drain reuse a 256-byte stack array; the only new
per-instance sample state is one `int16_t` last sample. There is no new C++ heap
allocation. IDF still allocates its channel, DMA descriptors, and the 1920-byte
DMA sample storage; these are necessary for hardware output and are released
on end/setup failure. Async playback retains its existing DMA configuration.

## Host regression and hardware acceptance

From the SDK root:

```sh
sh libs/hardware/AudioManager/test/host/run_pcm_stream.sh
CXXFLAGS='-fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer' \
  sh libs/hardware/AudioManager/test/host/run_pcm_stream.sh
```

The harness compiles the real `AudioManager.cpp` with the real Wave board
profile and substitutes only Arduino, task scheduling, I2C, and I2S hardware.
It covers extremes/volume/odd sample counts, partial writes and errors, terminal
failure, 100/20/40 ms budgets with clock rollover, signed release ramp, codec
rate changes, rollback/retry, preserved asynchronous playback, stalled-task
ownership, and capability-disabled builds. The stub uses a 10 ms RTOS tick to
catch accidental tick conversion of IDF's millisecond timeout parameter.

Host tests establish data and lifecycle behavior; they cannot establish analog
gain, speaker quality, clock accuracy, or I2C concurrency on the device. On
Wave, play a known clip at 50/100%, verify its original duration and pitch, stop
and resume repeatedly, then play a 16 kHz short sound and resume the stream.
Check silence when muted/stopped, no repeated tail audio, no task/heap growth,
and correct behavior while the display/RTC/PMIC share I2C.
