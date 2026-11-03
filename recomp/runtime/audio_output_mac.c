// Sound output on the Mac, through the system's default device. CoreAudio converts
// from the game's sample rate and calls back on a thread of its own.
#include <AudioToolbox/AudioToolbox.h>
#include <stdio.h>

#include "audio.h"

static OSStatus fill(void *context, AudioUnitRenderActionFlags *flags, const AudioTimeStamp *time, UInt32 bus,
                     UInt32 samples, AudioBufferList *buffers) {
  (void)context; (void)flags; (void)time; (void)bus;
  audio_pull(buffers->mBuffers[0].mData, samples);
  return noErr;
}

int audio_output_open(void) {
  AudioComponentDescription wanted = {kAudioUnitType_Output, kAudioUnitSubType_DefaultOutput,
                                      kAudioUnitManufacturer_Apple, 0, 0};
  AudioComponent component = AudioComponentFindNext(0, &wanted);
  AudioUnit unit;
  if (!component || AudioComponentInstanceNew(component, &unit)) return 0;
  AudioStreamBasicDescription format = {
      .mSampleRate = AUDIO_SAMPLE_RATE,
      .mFormatID = kAudioFormatLinearPCM,
      .mFormatFlags = kLinearPCMFormatFlagIsSignedInteger | kLinearPCMFormatFlagIsPacked,
      .mFramesPerPacket = 1,
      .mChannelsPerFrame = AUDIO_CHANNELS,
      .mBitsPerChannel = 16,
      .mBytesPerFrame = AUDIO_CHANNELS * sizeof(int16_t),
      .mBytesPerPacket = AUDIO_CHANNELS * sizeof(int16_t),
  };
  AURenderCallbackStruct callback = {fill, 0};
  if (AudioUnitSetProperty(unit, kAudioUnitProperty_StreamFormat, kAudioUnitScope_Input, 0, &format, sizeof format) ||
      AudioUnitSetProperty(unit, kAudioUnitProperty_SetRenderCallback, kAudioUnitScope_Input, 0, &callback,
                           sizeof callback) ||
      AudioUnitInitialize(unit) || AudioOutputUnitStart(unit)) {
    return 0;
  }
  return 1;
}
