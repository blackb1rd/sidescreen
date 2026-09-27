// SideScreen Microphone: a CoreAudio server plug-in with one loopback device. SideScreen plays
// the tablet's microphone into the device's output; apps record it from the device's input.
// Loaded by coreaudiod from /Library/Audio/Plug-Ins/HAL; no kernel extension.
#pragma once

#include <CoreAudio/AudioServerPlugIn.h>
#include <stdatomic.h>

enum {
    kObjectPlugIn = kAudioObjectPlugInObject, // 1
    kObjectDevice = 2,
    kObjectStreamInput = 3,
    kObjectStreamOutput = 4,
};

#define kSampleRate 48000.0
#define kChannels 1
#define kRingFrames 16384 // ~340 ms of loopback buffer; also the zero-time-stamp period

#define kDeviceName "SideScreen Microphone"
#define kDeviceUID "dev.blackb1rd.sidescreen.microphone"
#define kModelUID "dev.blackb1rd.sidescreen.microphone.model"
#define kManufacturer "SideScreen"

extern AudioServerPlugInDriverInterface *gInterfacePtr;
extern AudioServerPlugInHostRef gHost;

// properties.c
Boolean SS_HasProperty(AudioServerPlugInDriverRef, AudioObjectID, pid_t, const AudioObjectPropertyAddress *);
OSStatus SS_IsPropertySettable(AudioServerPlugInDriverRef, AudioObjectID, pid_t, const AudioObjectPropertyAddress *, Boolean *);
OSStatus SS_GetPropertyDataSize(AudioServerPlugInDriverRef, AudioObjectID, pid_t, const AudioObjectPropertyAddress *, UInt32, const void *, UInt32 *);
OSStatus SS_GetPropertyData(AudioServerPlugInDriverRef, AudioObjectID, pid_t, const AudioObjectPropertyAddress *, UInt32, const void *, UInt32, UInt32 *, void *);
OSStatus SS_SetPropertyData(AudioServerPlugInDriverRef, AudioObjectID, pid_t, const AudioObjectPropertyAddress *, UInt32, const void *, UInt32, const void *);

// io.c
extern _Atomic UInt32 gIOCount;
OSStatus SS_StartIO(AudioServerPlugInDriverRef, AudioObjectID, UInt32);
OSStatus SS_StopIO(AudioServerPlugInDriverRef, AudioObjectID, UInt32);
OSStatus SS_GetZeroTimeStamp(AudioServerPlugInDriverRef, AudioObjectID, UInt32, Float64 *, UInt64 *, UInt64 *);
OSStatus SS_WillDoIOOperation(AudioServerPlugInDriverRef, AudioObjectID, UInt32, UInt32, Boolean *, Boolean *);
OSStatus SS_BeginIOOperation(AudioServerPlugInDriverRef, AudioObjectID, UInt32, UInt32, UInt32, const AudioServerPlugInIOCycleInfo *);
OSStatus SS_DoIOOperation(AudioServerPlugInDriverRef, AudioObjectID, AudioObjectID, UInt32, UInt32, UInt32, const AudioServerPlugInIOCycleInfo *, void *, void *);
OSStatus SS_EndIOOperation(AudioServerPlugInDriverRef, AudioObjectID, UInt32, UInt32, UInt32, const AudioServerPlugInIOCycleInfo *);
