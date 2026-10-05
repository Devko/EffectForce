#pragma once
// The slice of the VST2 ABI that MPC OS's JUCE host uses, written out by hand (no Steinberg SDK).
// Layout and opcode values follow sd88me/mpc-vst-plugins wrapper/vst2_wrap.c (MIT, Copyright (c)
// 2026 sd88me), which is device-verified on a Force, as in PolyForce and SubForce; the effect
// opcodes and host queries below are the probe's additions (VST 2.4 values). On 32-bit ARM
// intptr_t is 4 bytes.
#include <cstdint>

extern "C" {

struct AEffect;
typedef intptr_t (*audioMasterCallback)(AEffect*, int32_t, int32_t, intptr_t, void*, float);

struct AEffect {
    int32_t magic;
    intptr_t (*dispatcher)(AEffect*, int32_t, int32_t, intptr_t, void*, float);
    void (*process)(AEffect*, float**, float**, int32_t);
    void (*setParameter)(AEffect*, int32_t, float);
    float (*getParameter)(AEffect*, int32_t);
    int32_t numPrograms, numParams, numInputs, numOutputs, flags;
    intptr_t resvd1, resvd2;
    int32_t initialDelay, realQualities, offQualities;
    float ioRatio;
    void *object, *user;
    int32_t uniqueID, version;
    void (*processReplacing)(AEffect*, float**, float**, int32_t);
    void (*processDoubleReplacing)(AEffect*, double**, double**, int32_t);
    char future[56];
};

struct VstEvent { int32_t type, byteSize, deltaFrames, flags; char data[16]; };
struct VstEvents { int32_t numEvents; intptr_t reserved; VstEvent* events[2]; };  // events[] is variable length

struct VstTimeInfo {
    double samplePos, sampleRate, nanoSeconds, ppqPos, tempo, barStartPos, cycleStartPos, cycleEndPos;
    int32_t timeSigNumerator, timeSigDenominator, smpteOffset, smpteFrameRate, samplesToNextClock, flags;
};

// effSetSpeakerArrangement's two arguments start like this (the speakers follow).
struct VstSpeakerArrangementHead { int32_t type, numChannels; };

} // extern "C"

namespace vst {

constexpr int32_t kMagic = 0x56737450;   // 'VstP'

enum : int32_t {
    effOpen = 0, effClose = 1, effSetProgram = 2, effGetProgram = 3, effSetProgramName = 4, effGetProgramName = 5,
    effGetParamLabel = 6, effGetParamDisplay = 7, effGetParamName = 8, effSetSampleRate = 10, effSetBlockSize = 11,
    effMainsChanged = 12, effEditGetRect = 13, effEditOpen = 14, effEditClose = 15, effEditIdle = 19,
    effIdentify = 22, effGetChunk = 23, effSetChunk = 24, effProcessEvents = 25, effCanBeAutomated = 26,
    effString2Parameter = 27, effGetProgramNameIndexed = 29, effConnectInput = 31, effConnectOutput = 32,
    effGetInputProperties = 33, effGetOutputProperties = 34,
    effGetPlugCategory = 35, effSetSpeakerArrangement = 42, effSetBypass = 44, effGetEffectName = 45,
    effGetVendorString = 47, effGetProductString = 48, effGetVendorVersion = 49, effVendorSpecific = 50,
    effCanDo = 51, effGetTailSize = 52, effGetParameterProperties = 56, effGetVstVersion = 58,
    effGetMidiKeyName = 66, effBeginSetProgram = 67, effEndSetProgram = 68, effGetSpeakerArrangement = 69,
    effStartProcess = 71, effStopProcess = 72, effSetProcessPrecision = 77,
};

enum : int32_t {
    audioMasterAutomate = 0, audioMasterVersion = 1, audioMasterGetTime = 7, audioMasterIOChanged = 13,
    audioMasterGetSampleRate = 16, audioMasterGetBlockSize = 17, audioMasterGetCurrentProcessLevel = 23,
    audioMasterGetVendorString = 32, audioMasterGetProductString = 33, audioMasterGetVendorVersion = 34,
    audioMasterCanDo = 37, audioMasterUpdateDisplay = 42,
};

enum : int32_t {
    kVstTransportChanged = 1, kVstTransportPlaying = 1 << 1, kVstTransportCycleActive = 1 << 2,
    kVstTransportRecording = 1 << 3, kVstNanosValid = 1 << 8, kVstPpqPosValid = 1 << 9,
    kVstTempoValid = 1 << 10, kVstBarsValid = 1 << 11, kVstCyclePosValid = 1 << 12,
    kVstTimeSigValid = 1 << 13,
};

enum : int32_t {
    effFlagsHasEditor = 1, effFlagsCanReplacing = 1 << 4, effFlagsProgramChunks = 1 << 5,
    effFlagsIsSynth = 1 << 8, effFlagsNoSoundInStop = 1 << 9,
};

constexpr int32_t kPlugCategEffect = 1;

constexpr int32_t fourcc(const char (&s)[5]) {
    return (int32_t(uint8_t(s[0])) << 24) | (int32_t(uint8_t(s[1])) << 16) |
           (int32_t(uint8_t(s[2])) << 8) | int32_t(uint8_t(s[3]));
}

// For the probe's log: the dispatcher opcode's name, or nullptr.
inline const char* opcodeName(int32_t op) {
    switch (op) {
        case effOpen: return "effOpen";
        case effClose: return "effClose";
        case effSetProgram: return "effSetProgram";
        case effGetProgram: return "effGetProgram";
        case effSetProgramName: return "effSetProgramName";
        case effGetProgramName: return "effGetProgramName";
        case effGetParamLabel: return "effGetParamLabel";
        case effGetParamDisplay: return "effGetParamDisplay";
        case effGetParamName: return "effGetParamName";
        case effSetSampleRate: return "effSetSampleRate";
        case effSetBlockSize: return "effSetBlockSize";
        case effMainsChanged: return "effMainsChanged";
        case effEditGetRect: return "effEditGetRect";
        case effEditOpen: return "effEditOpen";
        case effEditClose: return "effEditClose";
        case effEditIdle: return "effEditIdle";
        case effIdentify: return "effIdentify";
        case effGetChunk: return "effGetChunk";
        case effSetChunk: return "effSetChunk";
        case effProcessEvents: return "effProcessEvents";
        case effCanBeAutomated: return "effCanBeAutomated";
        case effString2Parameter: return "effString2Parameter";
        case effGetProgramNameIndexed: return "effGetProgramNameIndexed";
        case effConnectInput: return "effConnectInput";
        case effConnectOutput: return "effConnectOutput";
        case effGetInputProperties: return "effGetInputProperties";
        case effGetOutputProperties: return "effGetOutputProperties";
        case effGetPlugCategory: return "effGetPlugCategory";
        case effSetSpeakerArrangement: return "effSetSpeakerArrangement";
        case effSetBypass: return "effSetBypass";
        case effGetEffectName: return "effGetEffectName";
        case effGetVendorString: return "effGetVendorString";
        case effGetProductString: return "effGetProductString";
        case effGetVendorVersion: return "effGetVendorVersion";
        case effVendorSpecific: return "effVendorSpecific";
        case effCanDo: return "effCanDo";
        case effGetTailSize: return "effGetTailSize";
        case effGetParameterProperties: return "effGetParameterProperties";
        case effGetVstVersion: return "effGetVstVersion";
        case effGetMidiKeyName: return "effGetMidiKeyName";
        case effBeginSetProgram: return "effBeginSetProgram";
        case effEndSetProgram: return "effEndSetProgram";
        case effGetSpeakerArrangement: return "effGetSpeakerArrangement";
        case effStartProcess: return "effStartProcess";
        case effStopProcess: return "effStopProcess";
        case effSetProcessPrecision: return "effSetProcessPrecision";
        default: return nullptr;
    }
}

} // namespace vst
