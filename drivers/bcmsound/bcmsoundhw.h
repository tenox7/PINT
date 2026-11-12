/*++

Module Name:

    bcmsoundhw.h

Abstract:

    The ARC configuration data of the sound hardware of the Raspberry Pi, which
    the firmware describes as an audio controller of the internal bus and the
    sound driver (bcmsound.sys) reads: the DMA channel that feeds the outputs
    (its interrupt is the controller's), and what the driver needs to drive
    the 3.5 mm jack (the PWM) and HDMI audio on this board.

--*/

#ifndef _BCMSOUNDHW_
#define _BCMSOUNDHW_

#define BCMSOUND_ID             "BCM2835-AUDIO"

//
// The flags: a BCM2711 (Raspberry Pi 4, 400, CM4), and QEMU, whose DMA
// ignores the DREQs.
//

#define BCMSOUND_BCM2711        0x00000001
#define BCMSOUND_EMULATED       0x00000002

//
// The revision code of the board, the ARM physical address of the
// peripherals, the DMA channel of the outputs, the channels the VideoCore
// left to the ARM and the flags.
//

typedef struct _CM_BCMSOUND_DEVICE_DATA {
    USHORT Version;
    USHORT Revision;
    ULONG BoardRevision;
    ULONG PeripheralBase;
    ULONG DmaChannel;
    ULONG DmaChannelMask;
    ULONG Flags;
} CM_BCMSOUND_DEVICE_DATA, *PCM_BCMSOUND_DEVICE_DATA;

#endif // _BCMSOUNDHW_
