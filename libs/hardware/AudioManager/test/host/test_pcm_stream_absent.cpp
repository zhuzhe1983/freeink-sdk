#include <AudioManager.h>

#include <cassert>
#include <cstdio>

int main() {
  AudioManager audio;
  const int16_t sample = 1;
  assert(!audio.present() && !audio.begin());
  assert(!audio.beginPcmStream());
  assert(!audio.writePcmStream(&sample, 1));
  audio.endPcmStream();
  audio.stop();
  audio.powerDown();
  assert(!audio.isPlaying());
  std::puts("Absent PCM stream API passed");
}
