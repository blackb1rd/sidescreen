// Real-time I/O: a ring buffer that the output side (Spanly playing the tablet's
// microphone) writes and the input side (apps recording) reads, on one shared clock.
#include "driver.h"
#include <mach/mach_time.h>
#include <string.h>

void SS_SetRunning(int running);

_Atomic UInt32 gIOCount = 0;
static Float32 gRing[kRingFrames * kChannels];
static Float64 gTicksPerFrame = 0;
static UInt64 gAnchorHostTime = 0;
static UInt64 gPeriods = 0;

OSStatus SS_StartIO(AudioServerPlugInDriverRef d, AudioObjectID id, UInt32 client) {
    if (atomic_fetch_add(&gIOCount, 1) == 0) {
        mach_timebase_info_data_t tb;
        mach_timebase_info(&tb);
        Float64 hostTicksPerSecond = 1e9 * (Float64)tb.denom / (Float64)tb.numer;
        gTicksPerFrame = hostTicksPerSecond / kSampleRate;
        gAnchorHostTime = mach_absolute_time();
        gPeriods = 0;
        memset(gRing, 0, sizeof(gRing));
        SS_SetRunning(1);
        AudioObjectPropertyAddress running = {kAudioDevicePropertyDeviceIsRunning, kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain};
        if (gHost) gHost->PropertiesChanged(gHost, kObjectDevice, 1, &running);
    }
    return noErr;
}

OSStatus SS_StopIO(AudioServerPlugInDriverRef d, AudioObjectID id, UInt32 client) {
    UInt32 n = atomic_load(&gIOCount);
    if (n > 0 && atomic_fetch_sub(&gIOCount, 1) == 1) {
        SS_SetRunning(0);
        AudioObjectPropertyAddress running = {kAudioDevicePropertyDeviceIsRunning, kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain};
        if (gHost) gHost->PropertiesChanged(gHost, kObjectDevice, 1, &running);
    }
    return noErr;
}

// The device's clock: one "zero time stamp" per trip around the ring, anchored at StartIO.
OSStatus SS_GetZeroTimeStamp(AudioServerPlugInDriverRef d, AudioObjectID id, UInt32 client, Float64 *sampleTime, UInt64 *hostTime, UInt64 *seed) {
    Float64 ticksPerRing = gTicksPerFrame * kRingFrames;
    UInt64 now = mach_absolute_time();
    UInt64 next = gAnchorHostTime + (UInt64)((gPeriods + 1) * ticksPerRing);
    if (next <= now) gPeriods++;
    *sampleTime = (Float64)gPeriods * kRingFrames;
    *hostTime = gAnchorHostTime + (UInt64)(gPeriods * ticksPerRing);
    *seed = 1;
    return noErr;
}

OSStatus SS_WillDoIOOperation(AudioServerPlugInDriverRef d, AudioObjectID id, UInt32 client, UInt32 op, Boolean *willDo, Boolean *inPlace) {
    *willDo = op == kAudioServerPlugInIOOperationReadInput || op == kAudioServerPlugInIOOperationWriteMix;
    *inPlace = true;
    return noErr;
}

OSStatus SS_BeginIOOperation(AudioServerPlugInDriverRef d, AudioObjectID id, UInt32 client, UInt32 op, UInt32 frames, const AudioServerPlugInIOCycleInfo *info) {
    return noErr;
}

OSStatus SS_EndIOOperation(AudioServerPlugInDriverRef d, AudioObjectID id, UInt32 client, UInt32 op, UInt32 frames, const AudioServerPlugInIOCycleInfo *info) {
    return noErr;
}

OSStatus SS_DoIOOperation(AudioServerPlugInDriverRef d, AudioObjectID id, AudioObjectID stream, UInt32 client, UInt32 op, UInt32 frames, const AudioServerPlugInIOCycleInfo *info, void *main, void *secondary) {
    Float32 *buf = (Float32 *)main;
    if (op == kAudioServerPlugInIOOperationWriteMix && stream == kObjectStreamOutput) {
        UInt64 start = (UInt64)info->mOutputTime.mSampleTime;
        for (UInt32 i = 0; i < frames * kChannels; i++) gRing[(start * kChannels + i) % (kRingFrames * kChannels)] = buf[i];
    } else if (op == kAudioServerPlugInIOOperationReadInput && stream == kObjectStreamInput) {
        UInt64 start = (UInt64)info->mInputTime.mSampleTime;
        for (UInt32 i = 0; i < frames * kChannels; i++) {
            UInt64 at = (start * kChannels + i) % (kRingFrames * kChannels);
            buf[i] = gRing[at];
            gRing[at] = 0; // consumed: silence rather than an echo if the writer stops
        }
    }
    return noErr;
}
