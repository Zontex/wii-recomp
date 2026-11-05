// One voice of an AX program: decoding through the accelerator, sample-rate conversion,
// the volume envelope, filters and mixing into the buses. Ported from Dolphin's
// HW/DSPHLE/UCodes/AXVoice.h (GPL-2.0-or-later), which, like this file, serves both the
// GameCube and the Wii versions: ax.c includes it twice, with AX_GC or AX_WII defined, and
// VOICE(name) gives each copy its own names.
#if defined(AX_GC)
#define PB_TYPE AxPbGc
#define MOST_SAMPLES 32
#define VOICE(name) name##_gc
#elif defined(AX_WII)
#define PB_TYPE AxPbWii
#define MOST_SAMPLES 96
#define VOICE(name) name##_wii
#else
#error ax_voice.h needs AX_GC or AX_WII
#endif

typedef union {
  struct {
    int32_t *main_left, *main_right, *main_surround;
    int32_t *aux_a_left, *aux_a_right, *aux_a_surround;
    int32_t *aux_b_left, *aux_b_right, *aux_b_surround;
#ifdef AX_WII
    int32_t *aux_c_left, *aux_c_right, *aux_c_surround;
    int32_t *remote_main0, *remote_aux0, *remote_main1, *remote_aux1;
    int32_t *remote_main2, *remote_aux2, *remote_main3, *remote_aux3;
#endif
  };
#ifdef AX_GC
  int32_t *all[9];
#else
  struct { int32_t *regular[12], *remotes[8]; };
#endif
} VOICE(AxBuffers);

static AxUpdates VOICE(load_updates)(const PB_TYPE *pb) {
  AxUpdates updates;
  uint32_t at = HILO(pb->updates.data);
  for (int i = 0; i < AX_UPDATES; i++) {
    updates.entry[i].pb_offset = (uint16_t)rd16(at + i * 4);
    updates.entry[i].new_value = (uint16_t)rd16(at + i * 4 + 2);
  }
  return updates;
}

// The updates for one millisecond of the frame, each a word of the block and its new value.
static void VOICE(apply_updates)(int ms, PB_TYPE *pb, const uint16_t *counts, const AxUpdates *updates) {
  uint32_t start = 0;
  for (int i = 0; i < ms; i++) start += counts[i];
  if (start >= AX_UPDATES) return;
  uint16_t count = counts[ms];
  if (count > AX_UPDATES - start) return;
  for (uint32_t i = start; i < start + count; i++) {
    uint16_t offset = updates->entry[i].pb_offset;
    if (offset < sizeof(PB_TYPE) / 2) ((uint16_t *)pb)[offset] = updates->entry[i].new_value;
  }
}

// What the real program does when the accelerator reaches a sample's end: loop, carrying on
// a stream's decoder state or starting the loop's own, or stop a one-shot voice.
static void VOICE(sample_end)(DspAccelerator *accelerator) {
  PB_TYPE *pb = accelerator->user;
  if (pb->audio_addr.looping) {
    dsp_accelerator_set_pred_scale(accelerator, pb->adpcm_loop_info.pred_scale);
    if (pb->is_stream != 1) {
      dsp_accelerator_set_yn1(accelerator, (int16_t)pb->adpcm_loop_info.yn1);
      dsp_accelerator_set_yn2(accelerator, (int16_t)pb->adpcm_loop_info.yn2);
    } else {
      // Writing them back restarts the reads.
      dsp_accelerator_set_yn1(accelerator, accelerator->yn1);
      dsp_accelerator_set_yn2(accelerator, accelerator->yn2);
#ifdef AX_GC
      pb->loop_counter++;
#endif
    }
  } else {
    pb->running = 0;
  }
}

static void VOICE(accelerator_setup)(DspAccelerator *accelerator, PB_TYPE *pb) {
  memset(accelerator, 0, sizeof *accelerator);
  accelerator->read_memory = dsp_accelerator_wii_read;
  accelerator->write_memory = dsp_accelerator_wii_write;
  accelerator->sample_read_end = VOICE(sample_end);
  accelerator->user = pb;
  dsp_accelerator_set_start_address(accelerator, HILO(pb->audio_addr.loop_addr));
  dsp_accelerator_set_end_address(accelerator, HILO(pb->audio_addr.end_addr));
  dsp_accelerator_set_current_address(accelerator, HILO(pb->audio_addr.cur_addr));
  dsp_accelerator_set_sample_format(accelerator, pb->audio_addr.sample_format);
  dsp_accelerator_set_yn1(accelerator, pb->adpcm.yn1);
  dsp_accelerator_set_yn2(accelerator, pb->adpcm.yn2);
  dsp_accelerator_set_gain(accelerator, (int16_t)pb->adpcm.gain);
  dsp_accelerator_set_pred_scale(accelerator, pb->adpcm.pred_scale);
}

static int16_t VOICE(accelerator_sample)(void *from, uint32_t index) {
  (void)index;
  DspAccelerator *accelerator = from;
  return (int16_t)dsp_accelerator_read_sample(accelerator, ((PB_TYPE *)accelerator->user)->adpcm.coefs);
}

static void VOICE(get_input_samples)(DspAccelerator *accelerator, PB_TYPE *pb, int16_t *samples, uint16_t count,
                                     const int16_t *coefficients) {
  VOICE(accelerator_setup)(accelerator, pb);
  if (coefficients) coefficients += pb->coef_select * 0x200;
  uint32_t position = ax_resample(VOICE(accelerator_sample), accelerator, samples, count, pb->src.last_samples,
                                  pb->src.cur_addr_frac, HILO(pb->src.ratio), pb->src_type, coefficients);
  pb->src.cur_addr_frac = (uint16_t)position;
  pb->audio_addr.cur_addr_hi = (uint16_t)(accelerator->current_address >> 16);
  pb->audio_addr.cur_addr_lo = (uint16_t)accelerator->current_address;
  pb->adpcm.yn1 = accelerator->yn1;
  pb->adpcm.yn2 = accelerator->yn2;
  pb->adpcm.pred_scale = accelerator->pred_scale;
}

// One millisecond of the voice on the GameCube, or all three of a Wii frame.
static void VOICE(process_voice)(DspAccelerator *accelerator, PB_TYPE *pb, const VOICE(AxBuffers) *buffers, uint16_t count,
                                 uint32_t mix, const int16_t *coefficients, int new_filter) {
  (void)new_filter;
  if (pb->running != 1) return;
  int16_t samples[MOST_SAMPLES];
  VOICE(get_input_samples)(accelerator, pb, samples, count, coefficients);
  for (uint32_t i = 0; i < count; i++) {
#ifdef AX_GC
    int32_t volume = (int16_t)pb->vol_env.cur_volume;  // Signed on the GameCube,
#else
    int32_t volume = (uint16_t)pb->vol_env.cur_volume;  // unsigned on the Wii.
#endif
    samples[i] = clamp16(((int32_t)samples[i] * volume) >> 15);
    pb->vol_env.cur_volume += pb->vol_env.cur_volume_delta;
  }
  if (pb->lpf.on) ax_low_pass(samples, count, &pb->lpf);
#ifdef AX_WII
  if (new_filter && pb->biquad.on) ax_biquad(samples, count, &pb->biquad);
#endif

// On the Wii the real program also clears the depop value of a channel the voice isn't
// mixed into, which Dolphin leaves as it was: measured against the interpreter on Mario
// Kart Wii (AXWii 347112ba), where a voice set up again kept its last use's value.
#ifdef AX_WII
#define UNMIXED(field) else pb->dpop.field = 0
#else
#define UNMIXED(field)
#endif
#define MIX(bus, field, flag) \
  if (mix & MIX_##flag) ax_mix_add(buffers->bus, samples, count, &pb->mixer.field, &pb->dpop.field, (mix & MIX_##flag##_RAMP) != 0); \
  UNMIXED(field)
  MIX(main_left, main_left, MAIN_L);
  MIX(main_right, main_right, MAIN_R);
  MIX(main_surround, main_surround, MAIN_S);
  MIX(aux_a_left, aux_a_left, AUXA_L);
  MIX(aux_a_right, aux_a_right, AUXA_R);
  MIX(aux_a_surround, aux_a_surround, AUXA_S);
  MIX(aux_b_left, aux_b_left, AUXB_L);
  MIX(aux_b_right, aux_b_right, AUXB_R);
  MIX(aux_b_surround, aux_b_surround, AUXB_S);
#ifdef AX_WII
  MIX(aux_c_left, aux_c_left, AUXC_L);
  MIX(aux_c_right, aux_c_right, AUXC_R);
  MIX(aux_c_surround, aux_c_surround, AUXC_S);
#endif
#undef MIX
#undef UNMIXED

  // Delaying one side to place a sound: Dolphin doesn't know how either.
  if (pb->initial_time_delay.on) gap_hit("audio.ax_initial_time_delay", pb->initial_time_delay.on);

#ifdef AX_WII
  // The Wii Remote speakers' mix, at a sixth of the rate.
  if (pb->remote) {
    if (new_filter && pb->remote_iir.on) {
      if (pb->remote_iir.on == 2) ax_biquad(samples, count, &pb->remote_iir.biquad);
      else ax_low_pass(samples, count, &pb->remote_iir.lpf);
    }
    uint16_t remote_count = count == 96 ? 18 : 6;  // Older programs go a millisecond at a time.
    int16_t remote_samples[18];
    uint32_t position = ax_resample(ax_array_sample, samples, remote_samples, remote_count, pb->remote_src.last_samples,
                                    pb->remote_src.cur_addr_frac, 0x55555, SRC_POLYPHASE, coefficients);
    pb->remote_src.cur_addr_frac = (uint16_t)position;
#define REMOTE(n, bus, field)                                                                                    \
  if ((pb->remote_mixer_control >> (2 * n)) & 3)                                                                \
    ax_mix_add(buffers->bus, remote_samples, remote_count, &pb->remote_mixer.field, &pb->remote_dpop.field,     \
               ((pb->remote_mixer_control >> (2 * n)) & 2) != 0)
    REMOTE(0, remote_main0, main0);
    REMOTE(1, remote_aux0, aux0);
    REMOTE(2, remote_main1, main1);
    REMOTE(3, remote_aux1, aux1);
    REMOTE(4, remote_main2, main2);
    REMOTE(5, remote_aux2, aux2);
    REMOTE(6, remote_main3, main3);
    REMOTE(7, remote_aux3, aux3);
#undef REMOTE
  }
#endif
}

#undef PB_TYPE
#undef MOST_SAMPLES
#undef VOICE
