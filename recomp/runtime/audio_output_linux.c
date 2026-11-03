// Sound output on Linux, through ALSA's default device, which on the Pi leads to
// PipeWire. ALSA has no callback here, so a thread of our own writes each period.
#include <alsa/asoundlib.h>
#include <pthread.h>

#include "audio.h"

#define PERIOD_SAMPLES (AUDIO_FRAME_SAMPLES * 4)
#define LATENCY_MICROSECONDS 40000

static snd_pcm_t *device;

static void *output_thread(void *unused) {
  (void)unused;
  static int16_t period[PERIOD_SAMPLES * AUDIO_CHANNELS];
  while (1) {
    audio_pull(period, PERIOD_SAMPLES);
    snd_pcm_sframes_t wrote = snd_pcm_writei(device, period, PERIOD_SAMPLES);
    if (wrote < 0 && snd_pcm_recover(device, (int)wrote, 1) < 0) return 0;
  }
}

int audio_output_open(void) {
  int error = snd_pcm_open(&device, "default", SND_PCM_STREAM_PLAYBACK, 0);
  if (error >= 0) {
    error = snd_pcm_set_params(device, SND_PCM_FORMAT_S16_LE, SND_PCM_ACCESS_RW_INTERLEAVED, AUDIO_CHANNELS,
                               AUDIO_SAMPLE_RATE, 1, LATENCY_MICROSECONDS);
  }
  if (error < 0) {
    fprintf(stderr, "sound device: %s\n", snd_strerror(error));
    return 0;
  }
  pthread_t thread;
  return pthread_create(&thread, 0, output_thread, 0) == 0;
}
