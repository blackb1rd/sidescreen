// The plug-in's entry points: COM-style interface, factory, and the calls with nothing to do.
#include "driver.h"
#include <CoreFoundation/CoreFoundation.h>

AudioServerPlugInHostRef gHost = NULL;
static _Atomic UInt32 gRefCount = 0;
static AudioServerPlugInDriverRef gDriverRef;

static HRESULT SS_QueryInterface(void *driver, REFIID uuid, LPVOID *out) {
    CFUUIDRef requested = CFUUIDCreateFromUUIDBytes(NULL, uuid);
    Boolean ok = CFEqual(requested, IUnknownUUID) || CFEqual(requested, kAudioServerPlugInDriverInterfaceUUID);
    CFRelease(requested);
    if (!ok) return E_NOINTERFACE;
    atomic_fetch_add(&gRefCount, 1);
    *out = gDriverRef; // the driver ref (a pointer to the interface pointer), as COM expects
    return S_OK;
}

static ULONG SS_AddRef(void *driver) { return atomic_fetch_add(&gRefCount, 1) + 1; }

static ULONG SS_Release(void *driver) {
    UInt32 n = atomic_load(&gRefCount);
    if (n > 0) n = atomic_fetch_sub(&gRefCount, 1) - 1;
    return n;
}

static OSStatus SS_Initialize(AudioServerPlugInDriverRef driver, AudioServerPlugInHostRef host) {
    gHost = host;
    return noErr;
}

static OSStatus SS_CreateDevice(AudioServerPlugInDriverRef d, CFDictionaryRef desc, const AudioServerPlugInClientInfo *c, AudioObjectID *out) {
    return kAudioHardwareUnsupportedOperationError;
}

static OSStatus SS_DestroyDevice(AudioServerPlugInDriverRef d, AudioObjectID id) {
    return kAudioHardwareUnsupportedOperationError;
}

static OSStatus SS_AddDeviceClient(AudioServerPlugInDriverRef d, AudioObjectID id, const AudioServerPlugInClientInfo *c) { return noErr; }
static OSStatus SS_RemoveDeviceClient(AudioServerPlugInDriverRef d, AudioObjectID id, const AudioServerPlugInClientInfo *c) { return noErr; }
static OSStatus SS_PerformConfigChange(AudioServerPlugInDriverRef d, AudioObjectID id, UInt64 action, void *info) { return noErr; }
static OSStatus SS_AbortConfigChange(AudioServerPlugInDriverRef d, AudioObjectID id, UInt64 action, void *info) { return noErr; }

static AudioServerPlugInDriverInterface gInterface = {
    NULL,
    SS_QueryInterface,
    SS_AddRef,
    SS_Release,
    SS_Initialize,
    SS_CreateDevice,
    SS_DestroyDevice,
    SS_AddDeviceClient,
    SS_RemoveDeviceClient,
    SS_PerformConfigChange,
    SS_AbortConfigChange,
    SS_HasProperty,
    SS_IsPropertySettable,
    SS_GetPropertyDataSize,
    SS_GetPropertyData,
    SS_SetPropertyData,
    SS_StartIO,
    SS_StopIO,
    SS_GetZeroTimeStamp,
    SS_WillDoIOOperation,
    SS_BeginIOOperation,
    SS_DoIOOperation,
    SS_EndIOOperation,
};

AudioServerPlugInDriverInterface *gInterfacePtr = &gInterface;
static AudioServerPlugInDriverRef gDriverRef = &gInterfacePtr;

// Named in Info.plist (CFPlugInFactories).
void *Spanly_Create(CFAllocatorRef allocator, CFUUIDRef requestedType) {
    if (!CFEqual(requestedType, kAudioServerPlugInTypeUUID)) return NULL;
    return gDriverRef;
}
