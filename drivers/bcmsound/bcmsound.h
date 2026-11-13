/*++

Module Name:

    bcmsound.h

Abstract:

    The definitions of the sound driver of the Raspberry Pi, a sound card
    driver on NT's sound library (soundlib) as the Windows Sound System's is,
    with its devices: a wave output, a mixer and an FM synthesizer (an
    emulated OPL3, opl3.c), which its multimedia driver (bcmsnd32.dll, on
    drvlib and synthlib, as sndsys32.dll is) makes into the wave, mixer and
    MIDI devices of the system. It plays through the 3.5 mm jack (the PWM),
    HDMI, or nowhere (silent, keeping the time of the sounds, as in QEMU).

    The output (output.c) runs a ring of two halves at 48 kHz, fed by a DMA
    channel, and mixes each half from the wave output (soundlib's buffer,
    read as its DMA would and brought to 48 kHz) and the synthesizer, at the
    gains of the mixer (mixer.c).

--*/

#ifndef _BCMSOUND_
#define _BCMSOUND_

#include <stdarg.h>
#include <soundlib.h>
#include <wave.h>
#include <synth.h>
#include <synthdrv.h>
#include "bcmsoundhw.h"
#include "bcmsndid.h"
#include "opl3.h"

#define BCMSOUND_KEY                    (*(ULONG *)"BcmS")

//
// The registers, and the barrier around what the VideoCore reads or writes
// in shared memory.
//

#define MEMORY_BARRIER()                    __dsb(_ARM_BARRIER_SY)

#define READ_REGISTER(Base, Offset)         READ_REGISTER_ULONG((PULONG)((Base) + (Offset)))
#define WRITE_REGISTER(Base, Offset, Value) WRITE_REGISTER_ULONG((PULONG)((Base) + (Offset)), (Value))

//
// The devices, and the lines and controls of the mixer.
//

#define WAVE_OUT_DEVICE                 0
#define MIXER_DEVICE_INDEX              1
#define SYNTH_DEVICE_INDEX              2
#define NUMBER_OF_DEVICES               3

#define LINE_SPEAKERS                   0
#define LINE_WAVE_OUT                   1
#define LINE_SYNTH                      2
#define MIXER_LINES                     3

#define CONTROL_MASTER_VOLUME           0
#define CONTROL_MASTER_MUTE             1
#define CONTROL_WAVE_VOLUME             2
#define CONTROL_WAVE_MUTE               3
#define CONTROL_SYNTH_VOLUME            4
#define CONTROL_SYNTH_MUTE              5
#define MIXER_CONTROLS                  6

//
// The outputs: none (silent: the time of the sounds alone), the 3.5 mm jack
// (the PWM) and HDMI audio; and the setting that chooses among them,
// sound_output=AUTO (the default).
//

#define OUTPUT_NONE                     0
#define OUTPUT_JACK                     1
#define OUTPUT_HDMI                     2

#define SETTING_AUTO                    0xFF

//
// The formats of the wave output: PCM, 8 or 16 bits, mono or stereo, at
// 8-48 kHz, brought to the output's rate.
//
// The buffers: soundlib's holds an eighth of a second (it is told of each
// half played), at most 48 kHz, 16-bit stereo; a half of the ring holds
// RING_FRAMES frames of the output, each two 32-bit words (21 ms), its time
// in units of 100 ns.
//

#define OUTPUT_RATE                     48000
#define SOUND_BUFFER_SIZE               (48000 * 4 / 8)
#define RING_FRAMES                     1024
#define RING_FRAME_SIZE                 8
#define RING_HALF_SIZE                  (RING_FRAMES * RING_FRAME_SIZE)
#define RING_BUFFER_SIZE                (2 * RING_HALF_SIZE + PAGE_SIZE)
#define RING_HALF_TIME                  (RING_FRAMES * 10000000 / OUTPUT_RATE)

//
// The BCM2835 DMA controller: a channel's registers and its control blocks.
//

#define DMA_OFFSET                      0x7000
#define DMA_CHANNEL_SIZE                0x100
#define DMA_ENABLE                      0xFF0

#define DMA_CS                          0x00
#define DMA_CONBLK_AD                   0x04
#define DMA_TI                          0x08
#define DMA_SOURCE_AD                   0x0C
#define DMA_DEST_AD                     0x10
#define DMA_TXFR_LEN                    0x14
#define DMA_DEBUG                       0x20

#define DMA_CS_ACTIVE                   0x00000001
#define DMA_CS_END                      0x00000002
#define DMA_CS_INT                      0x00000004
#define DMA_CS_ERROR                    0x00000100
#define DMA_CS_PRIORITY(Level)          ((Level) << 16)
#define DMA_CS_PANIC_PRIORITY(Level)    ((Level) << 20)
#define DMA_CS_WAIT_WRITES              0x10000000
#define DMA_CS_ABORT                    0x40000000
#define DMA_CS_RESET                    0x80000000

#define DMA_TI_INTEN                    0x00000001
#define DMA_TI_WAIT_RESP                0x00000008
#define DMA_TI_DEST_DREQ                0x00000040
#define DMA_TI_SRC_INC                  0x00000100
#define DMA_TI_SRC_WIDTH                0x00000200
#define DMA_TI_PERMAP(Peripheral)       ((Peripheral) << 16)

#define DMA_DEBUG_ERRORS                0x00000007

typedef struct _DMA_CONTROL_BLOCK {
    ULONG TransferInformation;
    ULONG SourceAddress;
    ULONG DestinationAddress;
    ULONG TransferLength;
    ULONG Stride;
    ULONG NextControlBlock;
    ULONG Reserved[2];
} DMA_CONTROL_BLOCK, *PDMA_CONTROL_BLOCK;

//
// The VideoCore bus address of a peripheral, which the DMA controller uses.
//

#define BUS_PERIPHERAL_BASE             0x7E000000

//
// The global device information.
//

typedef struct _GLOBAL_DEVICE_INFO {
    ULONG Key;
    PDRIVER_OBJECT DriverObject;
    PWSTR RegistryPathName;
    PDEVICE_OBJECT DeviceObject[NUMBER_OF_DEVICES];
    KMUTEX WaveMutex;
    BOOLEAN DeviceInUse;
    BOOLEAN ShutdownRegistered;
    WAVE_INFO WaveInfo;

    //
    // The hardware: what the firmware described, the board, the output and
    // the registers it drives.
    //

    CM_BCMSOUND_DEVICE_DATA Hardware;
    ULONG InterruptVector;
    ULONG InterruptLevel;
    ULONG BoardType;
    BOOLEAN HasJack;
    ULONG Setting;
    ULONG Output;
    PUCHAR DmaChannel;
    PUCHAR DmaEnable;
    PUCHAR Pwm;
    PUCHAR Clock;
    PUCHAR Gpio;
    ULONG PwmFifoBus;
    ULONG PwmDreq;
    ULONG PwmClock;

    //
    // HDMI (hdmi.c).
    //

    PVOID Hdmi;

    //
    // The ring the DMA channel plays and its two control blocks (one a half).
    //

    PADAPTER_OBJECT RingAdapter;
    PUCHAR Ring;
    ULONG RingBus;
    PDMA_CONTROL_BLOCK ControlBlocks;
    ULONG ControlBlocksBus;

    //
    // The engine: its lock (of the reading of the wave output and of the
    // synthesizer), its deferred routine, the timer of the silent output,
    // whether it runs, the halves played and the halves with nothing to play.
    //

    BOOLEAN EngineInitialized;
    KSPIN_LOCK EngineLock;
    KDPC EngineDpc;
    KTIMER Timer;
    KDPC TimerDpc;
    BOOLEAN Running;
    volatile ULONG HalvesDone;
    ULONG IdleHalves;
    ULONG PwmRange;

    //
    // The wave output: its format, soundlib's buffer (its size, where it is
    // read, the halves passed since its deferred routine last ran), the
    // conversion to the output's rate (16.16) and the frames it
    // interpolates between.
    //

    BOOLEAN WaveActive;
    BOOLEAN InWaveDeferred;
    ULONG Rate;
    ULONG Bits;
    ULONG Channels;
    ULONG FrameSize;
    ULONG BufferSize;
    ULONG WaveOffset;
    ULONG WaveCrossed;
    ULONG WaveStep;
    ULONG WavePhase;
    LONG WavePrevious[2];
    LONG WaveCurrent[2];

    //
    // The synthesizer: open for writing, the frames left of the self-test
    // of debug_sound (output.c), the chip, the register selected and a half
    // of its frames.
    //

    BOOLEAN SynthOpen;
    ULONG SelfTestFrames;
    OPL3 Synth;
    ULONG SynthAddress;
    SHORT SynthFrames[RING_FRAMES * 2];

    //
    // The mixer: soundlib's mixer, the controls (left, right), their
    // notifications and those of the lines, and the gains (0-0x10000) of the
    // sources; Parameters\Device0 of the service, where the devices and the
    // settings of the mixer are kept.
    //

    MIXER_INFO MixerInfo;
    USHORT MixerValue[MIXER_CONTROLS][2];
    MIXER_DATA_ITEM ControlNotification[MIXER_CONTROLS];
    MIXER_DATA_ITEM LineNotification[MIXER_LINES];
    ULONG WaveGain[2];
    ULONG SynthGain[2];
    PWSTR DevicePathName;

    //
    // The log, C:\SOUND.LOG.
    //

    BOOLEAN Debug;
    HANDLE LogFile;
    ULONG LogLines;
} GLOBAL_DEVICE_INFO, *PGLOBAL_DEVICE_INFO;

//
// init.c
//

VOID
BcmSoundLog(
    IN PGLOBAL_DEVICE_INFO GlobalInfo,
    IN PCHAR Format,
    ...
    );

ULONG
WaveOutNameId(
    IN PGLOBAL_DEVICE_INFO GlobalInfo
    );

//
// output.c
//

BOOLEAN
OutputInitialize(
    IN PGLOBAL_DEVICE_INFO GlobalInfo
    );

VOID
OutputShutdown(
    IN PGLOBAL_DEVICE_INFO GlobalInfo
    );

VOID
OutputTerminate(
    IN PGLOBAL_DEVICE_INFO GlobalInfo
    );

VOID
OutputEngineUpdate(
    IN PGLOBAL_DEVICE_INFO GlobalInfo
    );

VOID
OutputLogState(
    IN PGLOBAL_DEVICE_INFO GlobalInfo,
    IN PCHAR Event
    );

VOID
OutputSelfTest(
    IN PGLOBAL_DEVICE_INFO GlobalInfo
    );

ULONG
OutputTakeCrossings(
    IN PGLOBAL_DEVICE_INFO GlobalInfo
    );

WAVE_INTERFACE_ROUTINE OutputSetFormat;
WAVE_INTERFACE_ROUTINE OutputStart;
WAVE_INTERFACE_ROUTINE OutputStop;

BOOLEAN
OutputInterrupt(
    IN PGLOBAL_DEVICE_INFO GlobalInfo
    );

//
// mixer.c
//

VOID
MixerInitialize(
    IN PGLOBAL_DEVICE_INFO GlobalInfo
    );

VOID
MixerShutdown(
    IN PGLOBAL_DEVICE_INFO GlobalInfo
    );

SOUND_DISPATCH_ROUTINE MixerDumpConfiguration;

//
// synth.c
//

SOUND_DISPATCH_ROUTINE SynthDispatch;
SOUND_DISPATCH_ROUTINE SynthGetCaps;

//
// hdmi.c: HDMI audio on the display the firmware brought up.
//

BOOLEAN
HdmiInitialize(
    IN PGLOBAL_DEVICE_INFO GlobalInfo
    );

VOID
HdmiTerminate(
    IN PGLOBAL_DEVICE_INFO GlobalInfo
    );

BOOLEAN
HdmiSinkHasAudio(
    IN PGLOBAL_DEVICE_INFO GlobalInfo
    );

BOOLEAN
HdmiStart(
    IN PGLOBAL_DEVICE_INFO GlobalInfo,
    IN ULONG Rate
    );

VOID
HdmiEnable(
    IN PGLOBAL_DEVICE_INFO GlobalInfo
    );

VOID
HdmiStop(
    IN PGLOBAL_DEVICE_INFO GlobalInfo
    );

ULONG
HdmiFifoBus(
    IN PGLOBAL_DEVICE_INFO GlobalInfo
    );

ULONG
HdmiDreq(
    IN PGLOBAL_DEVICE_INFO GlobalInfo
    );

VOID
HdmiEncode(
    IN PGLOBAL_DEVICE_INFO GlobalInfo,
    OUT PULONG Words,
    IN LONG Left,
    IN LONG Right
    );

ULONG
HdmiReadControl(
    IN PGLOBAL_DEVICE_INFO GlobalInfo
    );

VOID
HdmiResetFrames(
    IN PGLOBAL_DEVICE_INFO GlobalInfo
    );

VOID
HdmiLogState(
    IN PGLOBAL_DEVICE_INFO GlobalInfo
    );

#endif // _BCMSOUND_
