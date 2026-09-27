// Properties of the plug-in, its device and the device's two streams (input = what apps
// record, output = what Spanly plays in).
#include "driver.h"

static _Atomic int gRunning = 0; // set by io.c through SS_SetRunning

void SS_SetRunning(int running) { atomic_store(&gRunning, running); }

static AudioStreamBasicDescription Format(void) {
    AudioStreamBasicDescription f = {0};
    f.mSampleRate = kSampleRate;
    f.mFormatID = kAudioFormatLinearPCM;
    f.mFormatFlags = kAudioFormatFlagIsFloat | kAudioFormatFlagsNativeEndian | kAudioFormatFlagIsPacked;
    f.mBytesPerPacket = 4 * kChannels;
    f.mFramesPerPacket = 1;
    f.mBytesPerFrame = 4 * kChannels;
    f.mChannelsPerFrame = kChannels;
    f.mBitsPerChannel = 32;
    return f;
}

static Boolean IsStream(AudioObjectID id) { return id == kObjectStreamInput || id == kObjectStreamOutput; }

Boolean SS_HasProperty(AudioServerPlugInDriverRef d, AudioObjectID id, pid_t pid, const AudioObjectPropertyAddress *a) {
    switch (a->mSelector) {
    case kAudioObjectPropertyBaseClass:
    case kAudioObjectPropertyClass:
    case kAudioObjectPropertyOwner:
    case kAudioObjectPropertyOwnedObjects:
        return true;
    case kAudioDevicePropertyLatency: // same selector as kAudioStreamPropertyLatency
        return id == kObjectDevice || IsStream(id);
    case kAudioObjectPropertyManufacturer:
    case kAudioPlugInPropertyDeviceList:
    case kAudioPlugInPropertyTranslateUIDToDevice:
    case kAudioPlugInPropertyResourceBundle:
        return id == kObjectPlugIn || (a->mSelector == kAudioObjectPropertyManufacturer && id == kObjectDevice);
    case kAudioObjectPropertyName:
    case kAudioDevicePropertyDeviceUID:
    case kAudioDevicePropertyModelUID:
    case kAudioDevicePropertyTransportType:
    case kAudioDevicePropertyRelatedDevices:
    case kAudioDevicePropertyClockDomain:
    case kAudioDevicePropertyDeviceIsAlive:
    case kAudioDevicePropertyDeviceIsRunning:
    case kAudioDevicePropertyDeviceCanBeDefaultDevice:
    case kAudioDevicePropertyDeviceCanBeDefaultSystemDevice:
    case kAudioDevicePropertyStreams:
    case kAudioObjectPropertyControlList:
    case kAudioDevicePropertySafetyOffset:
    case kAudioDevicePropertyNominalSampleRate:
    case kAudioDevicePropertyAvailableNominalSampleRates:
    case kAudioDevicePropertyIsHidden:
    case kAudioDevicePropertyZeroTimeStampPeriod:
    case kAudioDevicePropertyPreferredChannelsForStereo:
        return id == kObjectDevice;
    case kAudioStreamPropertyIsActive:
    case kAudioStreamPropertyDirection:
    case kAudioStreamPropertyTerminalType:
    case kAudioStreamPropertyStartingChannel:
    case kAudioStreamPropertyVirtualFormat:
    case kAudioStreamPropertyPhysicalFormat:
    case kAudioStreamPropertyAvailableVirtualFormats:
    case kAudioStreamPropertyAvailablePhysicalFormats:
        return IsStream(id);
    }
    return false;
}

OSStatus SS_IsPropertySettable(AudioServerPlugInDriverRef d, AudioObjectID id, pid_t pid, const AudioObjectPropertyAddress *a, Boolean *out) {
    if (!SS_HasProperty(d, id, pid, a)) return kAudioHardwareUnknownPropertyError;
    // The format and rate are fixed; accept "setting" them to the only value (some apps insist).
    *out = a->mSelector == kAudioDevicePropertyNominalSampleRate || a->mSelector == kAudioStreamPropertyVirtualFormat ||
           a->mSelector == kAudioStreamPropertyPhysicalFormat;
    return noErr;
}

// Number of streams visible in the given scope.
static UInt32 StreamCount(AudioObjectPropertyScope scope) {
    return scope == kAudioObjectPropertyScopeGlobal ? 2 : 1;
}

OSStatus SS_GetPropertyDataSize(AudioServerPlugInDriverRef d, AudioObjectID id, pid_t pid, const AudioObjectPropertyAddress *a, UInt32 qs, const void *q, UInt32 *out) {
    if (!SS_HasProperty(d, id, pid, a)) return kAudioHardwareUnknownPropertyError;
    switch (a->mSelector) {
    case kAudioObjectPropertyName:
    case kAudioObjectPropertyManufacturer:
    case kAudioDevicePropertyDeviceUID:
    case kAudioDevicePropertyModelUID:
    case kAudioPlugInPropertyResourceBundle:
        *out = sizeof(CFStringRef);
        break;
    case kAudioObjectPropertyOwnedObjects:
        *out = (UInt32)sizeof(AudioObjectID) * (id == kObjectPlugIn ? 1 : id == kObjectDevice ? StreamCount(a->mScope) : 0);
        break;
    case kAudioDevicePropertyStreams:
        *out = (UInt32)sizeof(AudioObjectID) * StreamCount(a->mScope);
        break;
    case kAudioPlugInPropertyDeviceList:
    case kAudioDevicePropertyRelatedDevices:
        *out = sizeof(AudioObjectID);
        break;
    case kAudioObjectPropertyControlList:
        *out = 0;
        break;
    case kAudioDevicePropertyNominalSampleRate:
        *out = sizeof(Float64);
        break;
    case kAudioDevicePropertyAvailableNominalSampleRates:
        *out = sizeof(AudioValueRange);
        break;
    case kAudioDevicePropertyPreferredChannelsForStereo:
        *out = 2 * sizeof(UInt32);
        break;
    case kAudioStreamPropertyVirtualFormat:
    case kAudioStreamPropertyPhysicalFormat:
        *out = sizeof(AudioStreamBasicDescription);
        break;
    case kAudioStreamPropertyAvailableVirtualFormats:
    case kAudioStreamPropertyAvailablePhysicalFormats:
        *out = sizeof(AudioStreamRangedDescription);
        break;
    default:
        *out = sizeof(UInt32); // AudioClassID, AudioObjectID and UInt32 properties
    }
    return noErr;
}

#define PUT(type, value)                                          \
    do {                                                          \
        if (size < sizeof(type)) return kAudioHardwareBadPropertySizeError; \
        *(type *)out = (value);                                   \
        *used = sizeof(type);                                     \
    } while (0)

static OSStatus PutIDs(const AudioObjectID *ids, UInt32 count, UInt32 size, UInt32 *used, void *out) {
    UInt32 n = size / sizeof(AudioObjectID);
    if (n > count) n = count;
    for (UInt32 i = 0; i < n; i++) ((AudioObjectID *)out)[i] = ids[i];
    *used = n * sizeof(AudioObjectID);
    return noErr;
}

static OSStatus Streams(AudioObjectPropertyScope scope, UInt32 size, UInt32 *used, void *out) {
    static const AudioObjectID both[] = {kObjectStreamInput, kObjectStreamOutput};
    static const AudioObjectID input[] = {kObjectStreamInput};
    static const AudioObjectID output[] = {kObjectStreamOutput};
    if (scope == kAudioObjectPropertyScopeInput) return PutIDs(input, 1, size, used, out);
    if (scope == kAudioObjectPropertyScopeOutput) return PutIDs(output, 1, size, used, out);
    return PutIDs(both, 2, size, used, out);
}

static OSStatus PlugInProperty(const AudioObjectPropertyAddress *a, UInt32 qs, const void *q, UInt32 size, UInt32 *used, void *out) {
    static const AudioObjectID device[] = {kObjectDevice};
    switch (a->mSelector) {
    case kAudioObjectPropertyBaseClass: PUT(AudioClassID, kAudioObjectClassID); break;
    case kAudioObjectPropertyClass: PUT(AudioClassID, kAudioPlugInClassID); break;
    case kAudioObjectPropertyOwner: PUT(AudioObjectID, kAudioObjectUnknown); break;
    case kAudioObjectPropertyManufacturer: PUT(CFStringRef, CFSTR(kManufacturer)); break;
    case kAudioPlugInPropertyResourceBundle: PUT(CFStringRef, CFSTR("")); break;
    case kAudioObjectPropertyOwnedObjects:
    case kAudioPlugInPropertyDeviceList:
        return PutIDs(device, 1, size, used, out);
    case kAudioPlugInPropertyTranslateUIDToDevice: {
        Boolean match = qs == sizeof(CFStringRef) && CFStringCompare(*(CFStringRef *)q, CFSTR(kDeviceUID), 0) == kCFCompareEqualTo;
        PUT(AudioObjectID, match ? kObjectDevice : kAudioObjectUnknown);
        break;
    }
    default: return kAudioHardwareUnknownPropertyError;
    }
    return noErr;
}

static OSStatus DeviceProperty(const AudioObjectPropertyAddress *a, UInt32 size, UInt32 *used, void *out) {
    static const AudioObjectID device[] = {kObjectDevice};
    switch (a->mSelector) {
    case kAudioObjectPropertyBaseClass: PUT(AudioClassID, kAudioObjectClassID); break;
    case kAudioObjectPropertyClass: PUT(AudioClassID, kAudioDeviceClassID); break;
    case kAudioObjectPropertyOwner: PUT(AudioObjectID, kObjectPlugIn); break;
    case kAudioObjectPropertyName: PUT(CFStringRef, CFSTR(kDeviceName)); break;
    case kAudioObjectPropertyManufacturer: PUT(CFStringRef, CFSTR(kManufacturer)); break;
    case kAudioDevicePropertyDeviceUID: PUT(CFStringRef, CFSTR(kDeviceUID)); break;
    case kAudioDevicePropertyModelUID: PUT(CFStringRef, CFSTR(kModelUID)); break;
    case kAudioDevicePropertyTransportType: PUT(UInt32, kAudioDeviceTransportTypeVirtual); break;
    case kAudioDevicePropertyRelatedDevices: return PutIDs(device, 1, size, used, out);
    case kAudioDevicePropertyClockDomain: PUT(UInt32, 0); break;
    case kAudioDevicePropertyDeviceIsAlive: PUT(UInt32, 1); break;
    case kAudioDevicePropertyDeviceIsRunning: PUT(UInt32, atomic_load(&gRunning) ? 1 : 0); break;
    // A microphone: it may be the default input, but never the default output.
    case kAudioDevicePropertyDeviceCanBeDefaultDevice: PUT(UInt32, a->mScope == kAudioObjectPropertyScopeInput ? 1 : 0); break;
    case kAudioDevicePropertyDeviceCanBeDefaultSystemDevice: PUT(UInt32, 0); break;
    case kAudioDevicePropertyLatency: PUT(UInt32, 0); break;
    case kAudioDevicePropertySafetyOffset: PUT(UInt32, 0); break;
    case kAudioDevicePropertyIsHidden: PUT(UInt32, 0); break;
    case kAudioDevicePropertyZeroTimeStampPeriod: PUT(UInt32, kRingFrames); break;
    case kAudioDevicePropertyNominalSampleRate: PUT(Float64, kSampleRate); break;
    case kAudioDevicePropertyAvailableNominalSampleRates: {
        AudioValueRange r = {kSampleRate, kSampleRate};
        PUT(AudioValueRange, r);
        break;
    }
    case kAudioDevicePropertyPreferredChannelsForStereo:
        if (size < 2 * sizeof(UInt32)) return kAudioHardwareBadPropertySizeError;
        ((UInt32 *)out)[0] = 1;
        ((UInt32 *)out)[1] = 1;
        *used = 2 * sizeof(UInt32);
        break;
    case kAudioObjectPropertyControlList: *used = 0; break;
    case kAudioObjectPropertyOwnedObjects:
    case kAudioDevicePropertyStreams:
        return Streams(a->mScope, size, used, out);
    default: return kAudioHardwareUnknownPropertyError;
    }
    return noErr;
}

static OSStatus StreamProperty(AudioObjectID id, const AudioObjectPropertyAddress *a, UInt32 size, UInt32 *used, void *out) {
    Boolean input = id == kObjectStreamInput;
    switch (a->mSelector) {
    case kAudioObjectPropertyBaseClass: PUT(AudioClassID, kAudioObjectClassID); break;
    case kAudioObjectPropertyClass: PUT(AudioClassID, kAudioStreamClassID); break;
    case kAudioObjectPropertyOwner: PUT(AudioObjectID, kObjectDevice); break;
    case kAudioObjectPropertyOwnedObjects: *used = 0; break;
    case kAudioStreamPropertyIsActive: PUT(UInt32, 1); break;
    case kAudioStreamPropertyDirection: PUT(UInt32, input ? 1 : 0); break;
    case kAudioStreamPropertyTerminalType:
        PUT(UInt32, input ? kAudioStreamTerminalTypeMicrophone : kAudioStreamTerminalTypeSpeaker);
        break;
    case kAudioStreamPropertyStartingChannel: PUT(UInt32, 1); break;
    case kAudioStreamPropertyLatency: PUT(UInt32, 0); break;
    case kAudioStreamPropertyVirtualFormat:
    case kAudioStreamPropertyPhysicalFormat: PUT(AudioStreamBasicDescription, Format()); break;
    case kAudioStreamPropertyAvailableVirtualFormats:
    case kAudioStreamPropertyAvailablePhysicalFormats: {
        AudioStreamRangedDescription r = {Format(), {kSampleRate, kSampleRate}};
        PUT(AudioStreamRangedDescription, r);
        break;
    }
    default: return kAudioHardwareUnknownPropertyError;
    }
    return noErr;
}

OSStatus SS_GetPropertyData(AudioServerPlugInDriverRef d, AudioObjectID id, pid_t pid, const AudioObjectPropertyAddress *a, UInt32 qs, const void *q, UInt32 size, UInt32 *used, void *out) {
    if (!SS_HasProperty(d, id, pid, a)) return kAudioHardwareUnknownPropertyError;
    if (id == kObjectPlugIn) return PlugInProperty(a, qs, q, size, used, out);
    if (id == kObjectDevice) return DeviceProperty(a, size, used, out);
    if (IsStream(id)) return StreamProperty(id, a, size, used, out);
    return kAudioHardwareBadObjectError;
}

OSStatus SS_SetPropertyData(AudioServerPlugInDriverRef d, AudioObjectID id, pid_t pid, const AudioObjectPropertyAddress *a, UInt32 qs, const void *q, UInt32 size, const void *data) {
    if (!SS_HasProperty(d, id, pid, a)) return kAudioHardwareUnknownPropertyError;
    switch (a->mSelector) {
    case kAudioDevicePropertyNominalSampleRate:
        return size == sizeof(Float64) && *(const Float64 *)data == kSampleRate ? noErr : kAudioDeviceUnsupportedFormatError;
    case kAudioStreamPropertyVirtualFormat:
    case kAudioStreamPropertyPhysicalFormat:
        return size == sizeof(AudioStreamBasicDescription) && ((const AudioStreamBasicDescription *)data)->mSampleRate == kSampleRate
                   ? noErr
                   : kAudioDeviceUnsupportedFormatError;
    }
    return kAudioHardwareUnsupportedOperationError;
}
