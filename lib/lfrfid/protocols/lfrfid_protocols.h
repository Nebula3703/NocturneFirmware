#pragma once
#include <toolbox/protocols/protocol.h>
#include "../tools/t5577.h"
#include "../tools/em4305.h"

typedef enum {
    LFRFIDFeatureASK = 1 << 0, /** ASK Demodulation */
    LFRFIDFeaturePSK = 1 << 1, /** PSK Demodulation */
} LFRFIDFeature;

typedef enum {
    LFRFIDProtocolEM4100,
    LFRFIDProtocolEM410032,
    LFRFIDProtocolEM410016,
    LFRFIDProtocolElectra,
    LFRFIDProtocolH10301,
    LFRFIDProtocolIdteck,
    LFRFIDProtocolIndala26,
    LFRFIDProtocolIOProxXSF,
    LFRFIDProtocolAwid,
    LFRFIDProtocolFDXA,
    LFRFIDProtocolFDXB,
    LFRFIDProtocolHidGeneric,
    LFRFIDProtocolHidExGeneric,
    LFRFIDProtocolPyramid,
    LFRFIDProtocolViking,
    LFRFIDProtocolJablotron,
    LFRFIDProtocolParadox,
    LFRFIDProtocolPACStanley,
    LFRFIDProtocolKeri,
    LFRFIDProtocolGallagher,
    LFRFIDProtocolNexwatch,
    LFRFIDProtocolSecurakey,
    LFRFIDProtocolGProxII,
    LFRFIDProtocolNoralsy,

    LFRFIDProtocolMax,
} LFRFIDProtocol;

extern const ProtocolBase* const lfrfid_protocols[];

typedef enum {
    LFRFIDWriteTypeT5577,
    LFRFIDWriteTypeEM4305,

    LFRFIDWriteTypeMax,
} LFRFIDWriteType;

typedef struct {
    LFRFIDWriteType write_type;
    union {
        LFRFIDT5577 t5577;
        LFRFIDEM4305 em4305;
    };
} LFRFIDWriteRequest;
