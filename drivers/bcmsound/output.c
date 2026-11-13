/*++

Module Name:

    output.c

Abstract:

    The outputs of the sound driver: the ring of the DMA channel and its
    interrupt, the 3.5 mm jack (the two channels of a PWM, fed by the DMA
    channel), HDMI (hdmi.c), and the silent output (a timer in place of the
    interrupt), and the engine that mixes the wave output and the FM
    synthesizer into the ring.

    The jack (as Circle drives it): the PWM's clock is PLLD divided to
    250 MHz (125 MHz on a BCM2711), and its range is the clock divided by the
    output's rate (5208 levels at 48 kHz); the channels are in mark-space
    mode, fed from the FIFO right then left (channel 1 is the right, GPIO 40;
    channel 2 the left, GPIO 41, or GPIO 45 on a Pi 2), each its level.
    Between sounds the PWM repeats the last level, the middle one, which keeps
    the jack quiet.

--*/

#include "bcmsound.h"

static VOID
OutputEngineDeferred(
    IN PKDPC Dpc,
    IN PVOID Context,
    IN PVOID Argument1,
    IN PVOID Argument2
    );

//
// The PWM, its clock and its pins. The jack's PWM of a BCM2711 is PWM1.
//

#define PWM_OFFSET                  0x20C000
#define PWM1_OFFSET                 0x20C800
#define PWM_SIZE                    0x28
#define PWM_CTL                     0x00
#define PWM_STA                     0x04
#define PWM_DMAC                    0x08
#define PWM_RNG1                    0x10
#define PWM_DAT1                    0x14
#define PWM_FIF1                    0x18
#define PWM_RNG2                    0x20
#define PWM_DAT2                    0x24

#define PWM_CTL_PWEN1               0x00000001
#define PWM_CTL_RPTL1               0x00000004
#define PWM_CTL_USEF1               0x00000020
#define PWM_CTL_MSEN1               0x00000080
#define PWM_CTL_CLRF1               0x00000040
#define PWM_CTL_PWEN2               0x00000100
#define PWM_CTL_RPTL2               0x00000400
#define PWM_CTL_USEF2               0x00002000
#define PWM_CTL_MSEN2               0x00008000
#define PWM_STA_ERRORS              0x0000010C
#define PWM_DMAC_ENAB               0x80000000
#define PWM_DMAC_PANIC(Threshold)   ((Threshold) << 8)
#define PWM_DMAC_DREQ(Threshold)    (Threshold)

#define PWM_RUN                     (PWM_CTL_PWEN1 | PWM_CTL_USEF1 | PWM_CTL_MSEN1 | PWM_CTL_RPTL1 | \
                                     PWM_CTL_PWEN2 | PWM_CTL_USEF2 | PWM_CTL_MSEN2 | PWM_CTL_RPTL2)

//
// The DREQs of the PWM (of a BCM2835-7) and of PWM1 (of a BCM2711).
//

#define PWM_DREQ                    5
#define PWM1_DREQ                   1

#define CLOCK_OFFSET                0x101000
#define CLOCK_SIZE                  0x100
#define CM_PWMCTL                   0xA0
#define CM_PWMDIV                   0xA4
#define CM_PASSWORD                 0x5A000000
#define CM_SRC_PLLD                 6
#define CM_ENAB                     0x00000010
#define CM_BUSY                     0x00000080

//
// The clock of the PWM and the rate of PLLD, of a BCM2835-7 and of a BCM2711.
//

#define PWM_CLOCK                   250000000
#define PWM_CLOCK_BCM2711           125000000
#define PLLD_RATE                   500000000
#define PLLD_RATE_BCM2711           750000000

#define GPIO_OFFSET                 0x200000
#define GPIO_SIZE                   0x100
#define GPIO_GPFSEL4                0x10
#define GPIO_ALT0                   4
#define BOARD_PI2                   0x04

//
// The boards: the type field of a revision code (new style), and those
// with a 3.5 mm jack (2B, 3B, 3B+, 3A+, 4B).
//

#define BOARD_TYPE(Revision)        (((Revision) & 0x00800000) != 0 ? ((Revision) >> 4) & 0xFF : 0)

static BOOLEAN
BoardHasJack(
    IN ULONG Type
    )
{
    return Type == 0x04 || Type == 0x08 || Type == 0x0D || Type == 0x0E || Type == 0x11;
}

static PUCHAR
MapRegisters(
    IN ULONG Physical,
    IN ULONG Length
    )
{
    PHYSICAL_ADDRESS Address;

    Address.QuadPart = Physical;
    return MmMapIoSpace(Address, Length, FALSE);
}

static VOID
StallUntil(
    IN PUCHAR Base,
    IN ULONG Offset,
    IN ULONG Mask,
    IN ULONG Value
    )
{
    ULONG Count;

    for (Count = 0; Count < 1000 && (READ_REGISTER(Base, Offset) & Mask) != Value; Count += 1) {
        KeStallExecutionProcessor(10);
    }
}

//
// The jack.
//

static BOOLEAN
JackPresent(
    IN PGLOBAL_DEVICE_INFO GlobalInfo
    )

/*++

Routine Description:

    This routine tells whether the PWM answers: a range written reads back.
    In QEMU, which has no PWM, it reads 0.

Arguments:

    GlobalInfo - Supplies the global device information.

Return Value:

    TRUE if the PWM answers.

--*/

{
    ULONG Saved = READ_REGISTER(GlobalInfo->Pwm, PWM_RNG2);
    BOOLEAN Present;

    WRITE_REGISTER(GlobalInfo->Pwm, PWM_RNG2, 0x1234);
    Present = READ_REGISTER(GlobalInfo->Pwm, PWM_RNG2) == 0x1234;
    WRITE_REGISTER(GlobalInfo->Pwm, PWM_RNG2, Saved);
    return Present;
}

static VOID
JackInitialize(
    IN PGLOBAL_DEVICE_INFO GlobalInfo
    )

/*++

Routine Description:

    This routine starts the PWM of the jack: its clock (PLLD divided to
    125 MHz), its pins (GPIO 40 and 41, alternate function 0), and the two
    channels from the FIFO at the middle level of a 48 kHz range, which they
    repeat until the DMA channel feeds them.

Arguments:

    GlobalInfo - Supplies the global device information.

Return Value:

    None.

--*/

{
    BOOLEAN Bcm2711 = (GlobalInfo->Hardware.Flags & BCMSOUND_BCM2711) != 0;
    ULONG Plld = Bcm2711 ? PLLD_RATE_BCM2711 : PLLD_RATE;
    ULONG Rate = Bcm2711 ? PWM_CLOCK_BCM2711 : PWM_CLOCK;
    ULONG Select;

    WRITE_REGISTER(GlobalInfo->Pwm, PWM_CTL, 0);
    WRITE_REGISTER(GlobalInfo->Pwm, PWM_DMAC, 0);
    WRITE_REGISTER(GlobalInfo->Clock, CM_PWMCTL, CM_PASSWORD | CM_SRC_PLLD);
    StallUntil(GlobalInfo->Clock, CM_PWMCTL, CM_BUSY, 0);
    WRITE_REGISTER(GlobalInfo->Clock, CM_PWMDIV, CM_PASSWORD | ((Plld / Rate) << 12));
    WRITE_REGISTER(GlobalInfo->Clock, CM_PWMCTL, CM_PASSWORD | CM_SRC_PLLD | CM_ENAB);
    StallUntil(GlobalInfo->Clock, CM_PWMCTL, CM_BUSY, CM_BUSY);
    GlobalInfo->PwmClock = Rate;

    Select = READ_REGISTER(GlobalInfo->Gpio, GPIO_GPFSEL4);
    if (GlobalInfo->BoardType == BOARD_PI2) {
        Select = (Select & ~(7 | (7 << 15))) | GPIO_ALT0 | (GPIO_ALT0 << 15);
    } else {
        Select = (Select & ~0x3F) | GPIO_ALT0 | (GPIO_ALT0 << 3);
    }

    WRITE_REGISTER(GlobalInfo->Gpio, GPIO_GPFSEL4, Select);

    GlobalInfo->PwmRange = (Rate + 24000) / 48000;
    WRITE_REGISTER(GlobalInfo->Pwm, PWM_RNG1, GlobalInfo->PwmRange);
    WRITE_REGISTER(GlobalInfo->Pwm, PWM_RNG2, GlobalInfo->PwmRange);
    WRITE_REGISTER(GlobalInfo->Pwm, PWM_STA, PWM_STA_ERRORS);
    WRITE_REGISTER(GlobalInfo->Pwm, PWM_CTL, PWM_CTL_CLRF1);
    KeStallExecutionProcessor(10);
    WRITE_REGISTER(GlobalInfo->Pwm, PWM_FIF1, GlobalInfo->PwmRange / 2);
    WRITE_REGISTER(GlobalInfo->Pwm, PWM_FIF1, GlobalInfo->PwmRange / 2);
    WRITE_REGISTER(GlobalInfo->Pwm, PWM_DMAC, PWM_DMAC_ENAB | PWM_DMAC_PANIC(7) | PWM_DMAC_DREQ(7));
    WRITE_REGISTER(GlobalInfo->Pwm, PWM_CTL, PWM_RUN);
}

BOOLEAN
OutputInitialize(
    IN PGLOBAL_DEVICE_INFO GlobalInfo
    )

/*++

Routine Description:

    This routine maps the registers, chooses the output and makes the ring.

Arguments:

    GlobalInfo - Supplies the global device information.

Return Value:

    TRUE, or FALSE if the registers cannot be mapped.

--*/

{
    ULONG Base = GlobalInfo->Hardware.PeripheralBase;
    BOOLEAN Bcm2711 = (GlobalInfo->Hardware.Flags & BCMSOUND_BCM2711) != 0;
    BOOLEAN Jack = FALSE;
    BOOLEAN Hdmi = FALSE;
    BOOLEAN HdmiAudio = FALSE;
    DEVICE_DESCRIPTION Description;
    PHYSICAL_ADDRESS Logical;
    ULONG MapRegisterCount;

    GlobalInfo->BoardType = BOARD_TYPE(GlobalInfo->Hardware.BoardRevision);
    GlobalInfo->HasJack = BoardHasJack(GlobalInfo->BoardType);
    GlobalInfo->DmaChannel = MapRegisters(Base + DMA_OFFSET + GlobalInfo->Hardware.DmaChannel * DMA_CHANNEL_SIZE,
                                          DMA_CHANNEL_SIZE);
    GlobalInfo->DmaEnable = MapRegisters(Base + DMA_OFFSET + DMA_ENABLE, sizeof(ULONG));
    GlobalInfo->Pwm = MapRegisters(Base + (Bcm2711 ? PWM1_OFFSET : PWM_OFFSET), PWM_SIZE);
    GlobalInfo->Clock = MapRegisters(Base + CLOCK_OFFSET, CLOCK_SIZE);
    GlobalInfo->Gpio = MapRegisters(Base + GPIO_OFFSET, GPIO_SIZE);
    if (GlobalInfo->DmaChannel == NULL || GlobalInfo->DmaEnable == NULL || GlobalInfo->Pwm == NULL ||
        GlobalInfo->Clock == NULL || GlobalInfo->Gpio == NULL) {
        BcmSoundLog(GlobalInfo, "Cannot map the registers");
        return FALSE;
    }

    GlobalInfo->PwmFifoBus = BUS_PERIPHERAL_BASE + (Bcm2711 ? PWM1_OFFSET : PWM_OFFSET) + PWM_FIF1;
    GlobalInfo->PwmDreq = Bcm2711 ? PWM1_DREQ : PWM_DREQ;

    if ((GlobalInfo->Hardware.Flags & BCMSOUND_EMULATED) != 0) {
        BcmSoundLog(GlobalInfo, "Emulated board (QEMU): no output, silent");
        GlobalInfo->Setting = OUTPUT_NONE;
    } else if (GlobalInfo->HasJack) {
        Jack = JackPresent(GlobalInfo);
        BcmSoundLog(GlobalInfo, "Board type %02X has a jack; the PWM %s", GlobalInfo->BoardType,
                    Jack ? "answers" : "does not answer (QEMU?)");
    } else {
        BcmSoundLog(GlobalInfo, "Board type %02X has no jack", GlobalInfo->BoardType);
    }

    Hdmi = GlobalInfo->Setting != OUTPUT_NONE && HdmiInitialize(GlobalInfo);
    if (Hdmi) {
        HdmiAudio = HdmiSinkHasAudio(GlobalInfo);
        BcmSoundLog(GlobalInfo, "HDMI: the display %s sound", HdmiAudio ? "takes" : "does not say it takes");
    }

    switch (GlobalInfo->Setting) {
    case OUTPUT_HDMI:
        GlobalInfo->Output = Hdmi ? OUTPUT_HDMI : OUTPUT_NONE;
        break;

    case OUTPUT_JACK:
        GlobalInfo->Output = Jack ? OUTPUT_JACK : OUTPUT_NONE;
        break;

    case OUTPUT_NONE:
        GlobalInfo->Output = OUTPUT_NONE;
        break;

    default:
        GlobalInfo->Output = Hdmi && (HdmiAudio || !Jack) ? OUTPUT_HDMI : Jack ? OUTPUT_JACK : OUTPUT_NONE;
        break;
    }

    BcmSoundLog(GlobalInfo, "Setting %s: output %s",
                GlobalInfo->Setting == OUTPUT_HDMI ? "HDMI" : GlobalInfo->Setting == OUTPUT_JACK ? "JACK" :
                GlobalInfo->Setting == OUTPUT_NONE ? "NONE" : "AUTO",
                GlobalInfo->Output == OUTPUT_HDMI ? "HDMI" : GlobalInfo->Output == OUTPUT_JACK ? "jack" :
                "none (silent)");

    //
    // The engine: its lock, its deferred routine (the end of a half), the
    // timer of the silent output and the synthesizer, at the output's rate.
    //

    KeInitializeSpinLock(&GlobalInfo->EngineLock);
    KeInitializeDpc(&GlobalInfo->EngineDpc, OutputEngineDeferred, GlobalInfo);
    KeInitializeTimer(&GlobalInfo->Timer);
    KeInitializeDpc(&GlobalInfo->TimerDpc, OutputEngineDeferred, GlobalInfo);
    Opl3Init(&GlobalInfo->Synth, OUTPUT_RATE);
    GlobalInfo->EngineInitialized = TRUE;

    if (GlobalInfo->Output == OUTPUT_NONE) {
        return TRUE;
    }

    //
    // The ring and its control blocks, which the DMA channel reads through
    // the VideoCore bus.
    //

    RtlZeroMemory(&Description, sizeof(Description));
    Description.Version = DEVICE_DESCRIPTION_VERSION;
    Description.Master = TRUE;
    Description.Dma32BitAddresses = TRUE;
    Description.InterfaceType = Internal;
    Description.MaximumLength = RING_BUFFER_SIZE;
    GlobalInfo->RingAdapter = HalGetAdapter(&Description, &MapRegisterCount);
    if (GlobalInfo->RingAdapter != NULL) {
        GlobalInfo->Ring = HalAllocateCommonBuffer(GlobalInfo->RingAdapter, RING_BUFFER_SIZE, &Logical, FALSE);
    }

    if (GlobalInfo->Ring == NULL) {
        BcmSoundLog(GlobalInfo, "No ring for the DMA channel: silent");
        GlobalInfo->Output = OUTPUT_NONE;
        return TRUE;
    }

    GlobalInfo->RingBus = Logical.LowPart;
    GlobalInfo->ControlBlocks = (PDMA_CONTROL_BLOCK)(GlobalInfo->Ring + 2 * RING_HALF_SIZE);
    GlobalInfo->ControlBlocksBus = GlobalInfo->RingBus + 2 * RING_HALF_SIZE;
    BcmSoundLog(GlobalInfo, "Ring at bus address %08X", GlobalInfo->RingBus);

    if (GlobalInfo->Output == OUTPUT_JACK) {
        JackInitialize(GlobalInfo);
    }

    return TRUE;
}

//
// The engine. The output plays a ring of two halves at OUTPUT_RATE, and runs
// while the wave output plays or the synthesizer is open. At the end of each
// half (the interrupt of the DMA channel, or the timer of the silent output)
// the half that ended is mixed again: the next frames of the wave output,
// read from soundlib's buffer as a DMA channel would and brought to the
// output's rate, and the frames of the synthesizer, each at its gain. Soundlib
// learns where its buffer is read from the counter (OutputGetCounter), and
// is called (its deferred routine) each time the reading passes into the
// other half of its buffer, which it then refills.
//

static VOID
EncodeFrame(
    IN PGLOBAL_DEVICE_INFO GlobalInfo,
    OUT PULONG Words,
    IN LONG Left,
    IN LONG Right
    )
{
    if (GlobalInfo->Output == OUTPUT_JACK) {
        Words[0] = ((ULONG)(Right + 32768) * GlobalInfo->PwmRange) >> 16;
        Words[1] = ((ULONG)(Left + 32768) * GlobalInfo->PwmRange) >> 16;
    } else {
        HdmiEncode(GlobalInfo, Words, Left, Right);
    }
}

static VOID
ReadWaveFrame(
    IN PGLOBAL_DEVICE_INFO GlobalInfo,
    OUT PLONG Frame
    )

/*++

Routine Description:

    This routine reads the next frame of soundlib's buffer, as 16-bit left
    and right, and notes when the reading passes into the other half.

Arguments:

    GlobalInfo - Supplies the global device information.

    Frame - Receives the left and right samples of the frame.

Return Value:

    None.

--*/

{
    PUCHAR Source = (PUCHAR)GlobalInfo->WaveInfo.DMABuf.VirtualAddress + GlobalInfo->WaveOffset;

    if (GlobalInfo->Bits == 8) {
        Frame[0] = ((LONG)Source[0] - 128) << 8;
        Frame[1] = GlobalInfo->Channels == 2 ? ((LONG)Source[1] - 128) << 8 : Frame[0];
    } else {
        Frame[0] = (SHORT)(Source[0] | (Source[1] << 8));
        Frame[1] = GlobalInfo->Channels == 2 ? (SHORT)(Source[2] | (Source[3] << 8)) : Frame[0];
    }

    GlobalInfo->WaveOffset += GlobalInfo->FrameSize;
    if (GlobalInfo->WaveOffset >= GlobalInfo->BufferSize) {
        GlobalInfo->WaveOffset = 0;
        GlobalInfo->WaveCrossed += 1;
    } else if (GlobalInfo->WaveOffset == GlobalInfo->BufferSize / 2) {
        GlobalInfo->WaveCrossed += 1;
    }
}

static VOID
MixHalf(
    IN PGLOBAL_DEVICE_INFO GlobalInfo,
    IN ULONG Half
    )

/*++

Routine Description:

    This routine mixes the next half of the output into a half of the ring
    (nowhere for the silent output, which reads the wave output and runs
    the synthesizer all the same, in time). It is called holding the lock of
    the engine.

Arguments:

    GlobalInfo - Supplies the global device information.

    Half - Supplies the half of the ring to mix, 0 or 1.

Return Value:

    None.

--*/

{
    PULONG Words = GlobalInfo->Ring == NULL ? NULL : (PULONG)(GlobalInfo->Ring + Half * RING_HALF_SIZE);
    BOOLEAN Wave;
    BOOLEAN Synth = GlobalInfo->SynthOpen;
    LONG Mix[2];
    LONG Interpolated;
    ULONG Frame;
    ULONG Channel;
    ULONG Phase;

    //
    // Soundlib may end the playing of its buffer without a stop (the last
    // of the sound played): the reading ends with it.
    //

    if (GlobalInfo->WaveActive && !GlobalInfo->WaveInfo.DMABusy) {
        GlobalInfo->WaveActive = FALSE;
    }

    Wave = GlobalInfo->WaveActive;

    if (Synth) {
        Opl3Render(&GlobalInfo->Synth, GlobalInfo->SynthFrames, RING_FRAMES);
    }

    for (Frame = 0; Frame < RING_FRAMES; Frame += 1) {
        Mix[0] = 0;
        Mix[1] = 0;

        //
        // The wave output: linear interpolation between the frame before and
        // the frame read last, at the phase of the output's frame between
        // them (16.16, advancing by the ratio of the rates).
        //

        if (Wave) {
            GlobalInfo->WavePhase += GlobalInfo->WaveStep;
            while (GlobalInfo->WavePhase >= 0x10000) {
                GlobalInfo->WavePhase -= 0x10000;
                GlobalInfo->WavePrevious[0] = GlobalInfo->WaveCurrent[0];
                GlobalInfo->WavePrevious[1] = GlobalInfo->WaveCurrent[1];
                ReadWaveFrame(GlobalInfo, GlobalInfo->WaveCurrent);
            }

            Phase = GlobalInfo->WavePhase >> 2;
            for (Channel = 0; Channel < 2; Channel += 1) {
                Interpolated = GlobalInfo->WavePrevious[Channel] +
                               (((GlobalInfo->WaveCurrent[Channel] - GlobalInfo->WavePrevious[Channel]) *
                                 (LONG)Phase) >> 14);
                Mix[Channel] += (Interpolated * (LONG)GlobalInfo->WaveGain[Channel]) >> 16;
            }
        }

        if (Synth) {
            for (Channel = 0; Channel < 2; Channel += 1) {
                Mix[Channel] += ((LONG)GlobalInfo->SynthFrames[2 * Frame + Channel] *
                                 (LONG)GlobalInfo->SynthGain[Channel]) >> 16;
            }
        }

        //
        // The self-test (debug_sound) keeps the output running for its time,
        // playing silence.
        //

        if (GlobalInfo->SelfTestFrames != 0) {
            GlobalInfo->SelfTestFrames -= 1;
        }

        if (Words != NULL) {
            for (Channel = 0; Channel < 2; Channel += 1) {
                if (Mix[Channel] > 32767) {
                    Mix[Channel] = 32767;
                } else if (Mix[Channel] < -32768) {
                    Mix[Channel] = -32768;
                }
            }

            EncodeFrame(GlobalInfo, Words, Mix[0], Mix[1]);
            Words += 2;
        }
    }
}

//
// The DMA channel.
//

static ULONG
PlayingHalf(
    IN PGLOBAL_DEVICE_INFO GlobalInfo
    )
{
    return READ_REGISTER(GlobalInfo->DmaChannel, DMA_CONBLK_AD) ==
           GlobalInfo->ControlBlocksBus + sizeof(DMA_CONTROL_BLOCK) ? 1 : 0;
}

static VOID
DmaStop(
    IN PGLOBAL_DEVICE_INFO GlobalInfo
    )
{
    WRITE_REGISTER(GlobalInfo->DmaChannel, DMA_CS, 0);
    StallUntil(GlobalInfo->DmaChannel, DMA_CS, DMA_CS_ACTIVE, 0);
    WRITE_REGISTER(GlobalInfo->DmaChannel, DMA_CS, DMA_CS_RESET);
    KeStallExecutionProcessor(10);
    WRITE_REGISTER(GlobalInfo->DmaChannel, DMA_CS, DMA_CS_INT | DMA_CS_END);
    WRITE_REGISTER(GlobalInfo->DmaChannel, DMA_DEBUG, DMA_DEBUG_ERRORS);
}

static VOID
DmaStart(
    IN PGLOBAL_DEVICE_INFO GlobalInfo,
    IN ULONG TransferInformation,
    IN ULONG Destination
    )

/*++

Routine Description:

    This routine starts the DMA channel on the ring: two control blocks, one
    a half, each ending in an interrupt and leading to the other; it starts
    with half 0.

Arguments:

    GlobalInfo - Supplies the global device information.

    TransferInformation - Supplies the transfer information of the control
        blocks.

    Destination - Supplies the bus address of the FIFO the channel feeds.

Return Value:

    None.

--*/

{
    ULONG Half;

    for (Half = 0; Half < 2; Half += 1) {
        GlobalInfo->ControlBlocks[Half].TransferInformation = TransferInformation;
        GlobalInfo->ControlBlocks[Half].SourceAddress = GlobalInfo->RingBus + Half * RING_HALF_SIZE;
        GlobalInfo->ControlBlocks[Half].DestinationAddress = Destination;
        GlobalInfo->ControlBlocks[Half].TransferLength = RING_HALF_SIZE;
        GlobalInfo->ControlBlocks[Half].Stride = 0;
        GlobalInfo->ControlBlocks[Half].NextControlBlock = GlobalInfo->ControlBlocksBus +
                                                           (1 - Half) * sizeof(DMA_CONTROL_BLOCK);
        GlobalInfo->ControlBlocks[Half].Reserved[0] = 0;
        GlobalInfo->ControlBlocks[Half].Reserved[1] = 0;
    }

    MEMORY_BARRIER();
    WRITE_REGISTER(GlobalInfo->DmaEnable, 0,
                   READ_REGISTER(GlobalInfo->DmaEnable, 0) | (1 << GlobalInfo->Hardware.DmaChannel));
    DmaStop(GlobalInfo);
    WRITE_REGISTER(GlobalInfo->DmaChannel, DMA_CONBLK_AD, GlobalInfo->ControlBlocksBus);
    WRITE_REGISTER(GlobalInfo->DmaChannel, DMA_CS, DMA_CS_ACTIVE | DMA_CS_PRIORITY(8) | DMA_CS_PANIC_PRIORITY(15) |
                                                   DMA_CS_WAIT_WRITES);
}

//
// Starting and stopping the engine (at DISPATCH_LEVEL, holding its lock).
//

static BOOLEAN
EngineStart(
    IN PGLOBAL_DEVICE_INFO GlobalInfo
    )
{
    LARGE_INTEGER Due;
    ULONG TransferInformation;
    ULONG Destination;

    GlobalInfo->IdleHalves = 0;
    GlobalInfo->HalvesDone = 0;

    if (GlobalInfo->Output == OUTPUT_NONE) {
        Due.QuadPart = -(LONGLONG)RING_HALF_TIME;
        KeSetTimer(&GlobalInfo->Timer, Due, &GlobalInfo->TimerDpc);
        GlobalInfo->Running = TRUE;
        return TRUE;
    }

    if (GlobalInfo->Output == OUTPUT_HDMI) {
        HdmiResetFrames(GlobalInfo);
        if (!HdmiStart(GlobalInfo, OUTPUT_RATE)) {
            return FALSE;
        }

        TransferInformation = DMA_TI_INTEN | DMA_TI_WAIT_RESP | DMA_TI_DEST_DREQ | DMA_TI_SRC_INC | DMA_TI_SRC_WIDTH |
                              DMA_TI_PERMAP(HdmiDreq(GlobalInfo));
        Destination = HdmiFifoBus(GlobalInfo);
    } else {
        WRITE_REGISTER(GlobalInfo->Pwm, PWM_STA, PWM_STA_ERRORS);
        TransferInformation = DMA_TI_INTEN | DMA_TI_WAIT_RESP | DMA_TI_DEST_DREQ | DMA_TI_SRC_INC |
                              DMA_TI_PERMAP(GlobalInfo->PwmDreq);
        Destination = GlobalInfo->PwmFifoBus;
    }

    MixHalf(GlobalInfo, 0);
    MixHalf(GlobalInfo, 1);
    GlobalInfo->Running = TRUE;
    DmaStart(GlobalInfo, TransferInformation, Destination);
    if (GlobalInfo->Output == OUTPUT_HDMI) {
        HdmiEnable(GlobalInfo);
    }

    return TRUE;
}

static VOID
EngineStop(
    IN PGLOBAL_DEVICE_INFO GlobalInfo
    )
{
    if (GlobalInfo->Output == OUTPUT_NONE) {
        KeCancelTimer(&GlobalInfo->Timer);
    } else {
        DmaStop(GlobalInfo);
        if (GlobalInfo->Output == OUTPUT_HDMI) {
            HdmiStop(GlobalInfo);
        } else {

            //
            // The jack keeps the last level: the middle one.
            //

            WRITE_REGISTER(GlobalInfo->Pwm, PWM_FIF1, GlobalInfo->PwmRange / 2);
            WRITE_REGISTER(GlobalInfo->Pwm, PWM_FIF1, GlobalInfo->PwmRange / 2);
        }
    }

    GlobalInfo->Running = FALSE;
}

VOID
OutputEngineUpdate(
    IN PGLOBAL_DEVICE_INFO GlobalInfo
    )

/*++

Routine Description:

    This routine starts the engine when it has something to play (the
    synthesizer was opened), and at the start for HDMI, which plays on; the
    others stop by themselves, a ring after their last sound
    (OutputEngineDeferred).

Arguments:

    GlobalInfo - Supplies the global device information.

Return Value:

    None.

--*/

{
    KIRQL Irql;

    KeAcquireSpinLock(&GlobalInfo->EngineLock, &Irql);
    if (!GlobalInfo->Running &&
        (GlobalInfo->WaveActive || GlobalInfo->SynthOpen || GlobalInfo->Output == OUTPUT_HDMI)) {
        EngineStart(GlobalInfo);
    }

    KeReleaseSpinLock(&GlobalInfo->EngineLock, Irql);
}

//
// The end of a half.
//

static BOOLEAN
RequestWaveDeferred(
    IN PVOID Context
    )

/*++

Routine Description:

    This routine queues soundlib's deferred routine for the halves of its
    buffer the reading has passed (BcmSoundDeferred calls it once for each),
    synchronized with the interrupt as soundlib's flag of a queued call is.

Arguments:

    Context - Supplies the global device information.

Return Value:

    TRUE.

--*/

{
    PGLOBAL_DEVICE_INFO GlobalInfo = Context;

    if (GlobalInfo->WaveInfo.DMABusy && !GlobalInfo->WaveInfo.DpcQueued) {
        GlobalInfo->WaveInfo.DpcQueued = TRUE;
        IoRequestDpc(GlobalInfo->WaveInfo.DeviceObject, NULL, NULL);
    }

    return TRUE;
}

static VOID
OutputEngineDeferred(
    IN PKDPC Dpc,
    IN PVOID Context,
    IN PVOID Argument1,
    IN PVOID Argument2
    )

/*++

Routine Description:

    This routine follows the end of a half of the ring: it mixes that half
    again, to play after the current one; it calls soundlib's deferred
    routine when the wave output passed into the other half of its buffer,
    and stops the engine when nothing has played for a whole ring.

Arguments:

    Dpc - Not used.

    Context - Supplies the global device information.

    Argument1, Argument2 - Not used.

Return Value:

    None.

--*/

{
    PGLOBAL_DEVICE_INFO GlobalInfo = Context;
    ULONG Crossed;
    ULONG Half;

    KeAcquireSpinLockAtDpcLevel(&GlobalInfo->EngineLock);
    if (!GlobalInfo->Running) {
        KeReleaseSpinLockFromDpcLevel(&GlobalInfo->EngineLock);
        return;
    }

    //
    // Without an output the timer stands for the end of a half: NT 3.5 has no
    // periodic timer (KeSetTimerEx), so it is set again for the next half
    // (EngineStop cancels it).
    //

    if (GlobalInfo->Output == OUTPUT_NONE) {
        LARGE_INTEGER Due;

        Due.QuadPart = -(LONGLONG)RING_HALF_TIME;
        KeSetTimer(&GlobalInfo->Timer, Due, &GlobalInfo->TimerDpc);
    }

    if (GlobalInfo->WaveActive || GlobalInfo->SynthOpen || GlobalInfo->SelfTestFrames != 0) {
        GlobalInfo->IdleHalves = 0;
    } else {
        GlobalInfo->IdleHalves += 1;
    }

    //
    // HDMI plays on (silence) once started: a display or capture card takes
    // a while to lock on to a new audio stream, and would miss the start of
    // each sound, or a short sound whole.
    //

    if (GlobalInfo->IdleHalves > 2 && GlobalInfo->Output != OUTPUT_HDMI) {
        EngineStop(GlobalInfo);
        KeReleaseSpinLockFromDpcLevel(&GlobalInfo->EngineLock);
        return;
    }

    Half = GlobalInfo->Output == OUTPUT_NONE ? 0 : PlayingHalf(GlobalInfo) ^ 1;
    MixHalf(GlobalInfo, Half);
    Crossed = GlobalInfo->WaveCrossed;
    KeReleaseSpinLockFromDpcLevel(&GlobalInfo->EngineLock);

    if (Crossed != 0 && GlobalInfo->WaveInfo.Interrupt != NULL) {
        KeSynchronizeExecution(GlobalInfo->WaveInfo.Interrupt, RequestWaveDeferred, GlobalInfo);
    }
}

BOOLEAN
OutputInterrupt(
    IN PGLOBAL_DEVICE_INFO GlobalInfo
    )
{
    ULONG Status;

    if (GlobalInfo->Output == OUTPUT_NONE) {
        return FALSE;
    }

    Status = READ_REGISTER(GlobalInfo->DmaChannel, DMA_CS);
    if ((Status & DMA_CS_INT) == 0) {
        return FALSE;
    }

    WRITE_REGISTER(GlobalInfo->DmaChannel, DMA_CS, Status & ~(DMA_CS_ABORT | DMA_CS_RESET));
    GlobalInfo->HalvesDone += 1;
    KeInsertQueueDpc(&GlobalInfo->EngineDpc, NULL, NULL);
    return TRUE;
}

VOID
OutputShutdown(
    IN PGLOBAL_DEVICE_INFO GlobalInfo
    )
{
    KIRQL Irql;

    KeAcquireSpinLock(&GlobalInfo->EngineLock, &Irql);
    GlobalInfo->WaveActive = FALSE;
    GlobalInfo->SynthOpen = FALSE;
    if (GlobalInfo->Running) {
        EngineStop(GlobalInfo);
    }

    KeReleaseSpinLock(&GlobalInfo->EngineLock, Irql);
    if (GlobalInfo->Output == OUTPUT_JACK) {
        WRITE_REGISTER(GlobalInfo->Pwm, PWM_DMAC, 0);
    }
}

VOID
OutputTerminate(
    IN PGLOBAL_DEVICE_INFO GlobalInfo
    )

/*++

Routine Description:

    This routine stops the output and frees what OutputInitialize made: the
    HDMI state, the ring and the mappings of the registers. It also undoes an
    OutputInitialize that failed, or was not reached.

Arguments:

    GlobalInfo - Supplies the global device information.

Return Value:

    None.

--*/

{
    PHYSICAL_ADDRESS Logical;

    if (GlobalInfo->EngineInitialized) {
        OutputShutdown(GlobalInfo);
        KeRemoveQueueDpc(&GlobalInfo->EngineDpc);
        KeRemoveQueueDpc(&GlobalInfo->TimerDpc);
        GlobalInfo->EngineInitialized = FALSE;
    }

    GlobalInfo->Output = OUTPUT_NONE;
    HdmiTerminate(GlobalInfo);
    if (GlobalInfo->Ring != NULL) {
        Logical.QuadPart = GlobalInfo->RingBus;
        HalFreeCommonBuffer(GlobalInfo->RingAdapter, RING_BUFFER_SIZE, Logical, GlobalInfo->Ring, FALSE);
        GlobalInfo->Ring = NULL;
    }

    if (GlobalInfo->DmaChannel != NULL) {
        MmUnmapIoSpace(GlobalInfo->DmaChannel, DMA_CHANNEL_SIZE);
        GlobalInfo->DmaChannel = NULL;
    }

    if (GlobalInfo->DmaEnable != NULL) {
        MmUnmapIoSpace(GlobalInfo->DmaEnable, sizeof(ULONG));
        GlobalInfo->DmaEnable = NULL;
    }

    if (GlobalInfo->Pwm != NULL) {
        MmUnmapIoSpace(GlobalInfo->Pwm, PWM_SIZE);
        GlobalInfo->Pwm = NULL;
    }

    if (GlobalInfo->Clock != NULL) {
        MmUnmapIoSpace(GlobalInfo->Clock, CLOCK_SIZE);
        GlobalInfo->Clock = NULL;
    }

    if (GlobalInfo->Gpio != NULL) {
        MmUnmapIoSpace(GlobalInfo->Gpio, GPIO_SIZE);
        GlobalInfo->Gpio = NULL;
    }
}

//
// Soundlib's hardware routines of the wave output. The device is one of
// soundlib's reprogram-on-interrupt DMA: soundlib starts it on the lower half
// of its buffer (HwSetupDMA), and after each half played (its deferred
// routine, which then refills that half) programs the other half the same
// way. Its position in the buffer is the end of the half last programmed
// (HalReadDmaCounter reads 0 for the DMA of a bus master, which soundlib takes
// as a half played to its end), so its deferred routine must run once for
// each half, when the reading has passed the end of it: the engine counts
// these (WaveCrossed), and BcmSoundDeferred calls soundlib's routine once for
// each. The reading goes on through the whole buffer, whose other half is
// full whenever a half is programmed.
//

BOOLEAN
OutputSetFormat(
    IN PWAVE_INFO WaveInfo
    )
{
    PGLOBAL_DEVICE_INFO GlobalInfo = WaveInfo->HwContext;

    GlobalInfo->Rate = WaveInfo->SamplesPerSec;
    GlobalInfo->Bits = WaveInfo->BitsPerSample;
    GlobalInfo->Channels = WaveInfo->Channels;
    GlobalInfo->FrameSize = GlobalInfo->Bits / 8 * GlobalInfo->Channels;
    GlobalInfo->WaveStep = (ULONG)(((ULONGLONG)GlobalInfo->Rate << 16) / OUTPUT_RATE);
    return TRUE;
}

BOOLEAN
OutputStart(
    IN PWAVE_INFO WaveInfo
    )

/*++

Routine Description:

    This routine starts the reading of soundlib's buffer, from its start, and
    the engine if it is not running. A half programmed from soundlib's
    deferred routine (BcmSoundDeferred) continues the reading.

Arguments:

    WaveInfo - Supplies soundlib's wave information of the device.

Return Value:

    TRUE, or FALSE if the engine could not start.

--*/

{
    PGLOBAL_DEVICE_INFO GlobalInfo = WaveInfo->HwContext;
    BOOLEAN Started = TRUE;
    KIRQL Irql;

    if (GlobalInfo->InWaveDeferred && GlobalInfo->WaveActive) {
        return TRUE;
    }

    KeAcquireSpinLock(&GlobalInfo->EngineLock, &Irql);
    GlobalInfo->BufferSize = WaveInfo->DoubleBuffer.BufferSize;
    GlobalInfo->WaveOffset = 0;
    GlobalInfo->WavePhase = 0;
    GlobalInfo->WaveCrossed = 0;
    GlobalInfo->WavePrevious[0] = GlobalInfo->WavePrevious[1] = 0;
    GlobalInfo->WaveCurrent[0] = GlobalInfo->WaveCurrent[1] = 0;
    GlobalInfo->WaveActive = TRUE;
    if (!GlobalInfo->Running) {
        Started = EngineStart(GlobalInfo);
    }

    KeReleaseSpinLock(&GlobalInfo->EngineLock, Irql);
    return Started;
}

BOOLEAN
OutputStop(
    IN PWAVE_INFO WaveInfo
    )

/*++

Routine Description:

    This routine stops the reading of soundlib's buffer. What was read plays
    on (at most a ring), and the engine stops by itself when nothing more
    plays.

Arguments:

    WaveInfo - Supplies soundlib's wave information of the device.

Return Value:

    TRUE.

--*/

{
    PGLOBAL_DEVICE_INFO GlobalInfo = WaveInfo->HwContext;
    KIRQL Irql;

    KeAcquireSpinLock(&GlobalInfo->EngineLock, &Irql);
    GlobalInfo->WaveActive = FALSE;
    GlobalInfo->WaveCrossed = 0;
    KeReleaseSpinLock(&GlobalInfo->EngineLock, Irql);
    return TRUE;
}

VOID
OutputLogState(
    IN PGLOBAL_DEVICE_INFO GlobalInfo,
    IN PCHAR Event
    )

/*++

Routine Description:

    This routine logs (debug_sound=1) an event of a device and the state of
    the output: running or not, the halves played since it started (the
    interrupts), the format of the wave output and where its buffer is read,
    the gains, and the registers of the DMA channel and of the output.

Arguments:

    GlobalInfo - Supplies the global device information.

    Event - Supplies the text of the event.

Return Value:

    None.

--*/

{
    if (!GlobalInfo->Debug) {
        return;
    }

    BcmSoundLog(GlobalInfo,
                "%s: output %s, %u halves; wave %s %u Hz %u-bit %u ch, read at %u of %u; synth %s; "
                "gains wave %u/%u synth %u/%u",
                Event, GlobalInfo->Running ? "running" : "stopped", GlobalInfo->HalvesDone,
                GlobalInfo->WaveActive ? "active" : "idle", GlobalInfo->Rate, GlobalInfo->Bits, GlobalInfo->Channels,
                GlobalInfo->WaveOffset, GlobalInfo->BufferSize, GlobalInfo->SynthOpen ? "open" : "closed",
                GlobalInfo->WaveGain[0], GlobalInfo->WaveGain[1], GlobalInfo->SynthGain[0], GlobalInfo->SynthGain[1]);
    if (GlobalInfo->Output != OUTPUT_NONE) {
        BcmSoundLog(GlobalInfo, "    DMA CS %08X CONBLK %08X LEN %08X DEBUG %08X; %s %08X",
                    READ_REGISTER(GlobalInfo->DmaChannel, DMA_CS), READ_REGISTER(GlobalInfo->DmaChannel, DMA_CONBLK_AD),
                    READ_REGISTER(GlobalInfo->DmaChannel, DMA_TXFR_LEN),
                    READ_REGISTER(GlobalInfo->DmaChannel, DMA_DEBUG),
                    GlobalInfo->Output == OUTPUT_HDMI ? "MAI_CTL" : "PWM STA",
                    GlobalInfo->Output == OUTPUT_HDMI ? HdmiReadControl(GlobalInfo) :
                    READ_REGISTER(GlobalInfo->Pwm, PWM_STA));
    }
}

VOID
OutputSelfTest(
    IN PGLOBAL_DEVICE_INFO GlobalInfo
    )

/*++

Routine Description:

    This routine (debug_sound=1, at the start) runs the output for two
    seconds, silent, and logs what the hardware did in its first 400 ms: the
    halves played (the interrupts of the DMA channel), the state of the DMA
    channel and, for HDMI, of the MAI. It is called at PASSIVE_LEVEL.

Arguments:

    GlobalInfo - Supplies the global device information.

Return Value:

    None.

--*/

{
    LARGE_INTEGER Delay;
    KIRQL Irql;
    ULONG Step;
    BOOLEAN Started;

    KeAcquireSpinLock(&GlobalInfo->EngineLock, &Irql);
    GlobalInfo->SelfTestFrames = OUTPUT_RATE * 2;
    Started = GlobalInfo->Running || EngineStart(GlobalInfo);
    KeReleaseSpinLock(&GlobalInfo->EngineLock, Irql);
    if (!Started) {
        BcmSoundLog(GlobalInfo, "Self-test: the output did not start");
        GlobalInfo->SelfTestFrames = 0;
        return;
    }

    for (Step = 0; Step < 4; Step += 1) {

        //
        // 100 ms.
        //

        Delay.QuadPart = -1000000;
        KeDelayExecutionThread(KernelMode, FALSE, &Delay);
        if (GlobalInfo->Output == OUTPUT_NONE) {
            BcmSoundLog(GlobalInfo, "Self-test +%u ms: %u halves", (Step + 1) * 100, GlobalInfo->HalvesDone);
        } else {
            BcmSoundLog(GlobalInfo,
                        "Self-test +%u ms: %u halves (interrupts); DMA CS %08X CONBLK %08X LEN %08X DEBUG %08X",
                        (Step + 1) * 100, GlobalInfo->HalvesDone,
                        READ_REGISTER(GlobalInfo->DmaChannel, DMA_CS),
                        READ_REGISTER(GlobalInfo->DmaChannel, DMA_CONBLK_AD),
                        READ_REGISTER(GlobalInfo->DmaChannel, DMA_TXFR_LEN),
                        READ_REGISTER(GlobalInfo->DmaChannel, DMA_DEBUG));
            if (GlobalInfo->Output == OUTPUT_HDMI) {
                BcmSoundLog(GlobalInfo, "    MAI_CTL %08X", HdmiReadControl(GlobalInfo));
            } else {
                BcmSoundLog(GlobalInfo, "    PWM STA %08X", READ_REGISTER(GlobalInfo->Pwm, PWM_STA));
            }
        }
    }

    if (GlobalInfo->Output == OUTPUT_HDMI) {
        HdmiLogState(GlobalInfo);
    }

    //
    // The output runs on; the engine stops by itself after it (but HDMI).
    //
}

ULONG
OutputTakeCrossings(
    IN PGLOBAL_DEVICE_INFO GlobalInfo
    )

/*++

Routine Description:

    This routine takes one of the halves the reading has passed, for a call
    of soundlib's deferred routine; it returns 0 when there is none.

Arguments:

    GlobalInfo - Supplies the global device information.

Return Value:

    1 if a half was taken, else 0.

--*/

{
    ULONG Taken = 0;
    KIRQL Irql;

    KeAcquireSpinLock(&GlobalInfo->EngineLock, &Irql);
    if (GlobalInfo->WaveCrossed != 0) {
        GlobalInfo->WaveCrossed -= 1;
        Taken = 1;
    }

    KeReleaseSpinLock(&GlobalInfo->EngineLock, Irql);
    return Taken;
}
