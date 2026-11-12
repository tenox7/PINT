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

#define MEMORY_BARRIER()        __dsb(_ARM_BARRIER_SY)

#define READ_REGISTER(Base, Offset)         READ_REGISTER_ULONG((PULONG)((Base) + (Offset)))
#define WRITE_REGISTER(Base, Offset, Value) WRITE_REGISTER_ULONG((PULONG)((Base) + (Offset)), (Value))

//
// The PWM, its clock and its pins.
//

#define PWM_OFFSET              0x20C000
#define PWM1_OFFSET             0x20C800        // the jack's PWM of a BCM2711
#define PWM_SIZE                0x28
#define PWM_CTL                 0x00
#define PWM_STA                 0x04
#define PWM_DMAC                0x08
#define PWM_RNG1                0x10
#define PWM_DAT1                0x14
#define PWM_FIF1                0x18
#define PWM_RNG2                0x20
#define PWM_DAT2                0x24

#define PWM_CTL_PWEN1           0x00000001
#define PWM_CTL_RPTL1           0x00000004
#define PWM_CTL_USEF1           0x00000020
#define PWM_CTL_MSEN1           0x00000080
#define PWM_CTL_CLRF1           0x00000040
#define PWM_CTL_PWEN2           0x00000100
#define PWM_CTL_RPTL2           0x00000400
#define PWM_CTL_USEF2           0x00002000
#define PWM_CTL_MSEN2           0x00008000
#define PWM_STA_ERRORS          0x0000010C
#define PWM_DMAC_ENAB           0x80000000
#define PWM_DMAC_PANIC(x)       ((x) << 8)
#define PWM_DMAC_DREQ(x)        (x)

#define PWM_RUN                 (PWM_CTL_PWEN1 | PWM_CTL_USEF1 | PWM_CTL_MSEN1 | PWM_CTL_RPTL1 | \
                                 PWM_CTL_PWEN2 | PWM_CTL_USEF2 | PWM_CTL_MSEN2 | PWM_CTL_RPTL2)

#define PWM_DREQ                5               // PWM (of a BCM2835-7)
#define PWM1_DREQ               1               // PWM1 (of a BCM2711)

#define CLOCK_OFFSET            0x101000
#define CLOCK_SIZE              0x100
#define CM_PWMCTL               0xA0
#define CM_PWMDIV               0xA4
#define CM_PASSWORD             0x5A000000
#define CM_SRC_PLLD             6
#define CM_ENAB                 0x00000010
#define CM_BUSY                 0x00000080

#define PWM_CLOCK               250000000
#define PWM_CLOCK_BCM2711       125000000
#define PLLD_RATE               500000000       // of a BCM2835-7
#define PLLD_RATE_BCM2711       750000000

#define GPIO_OFFSET             0x200000
#define GPIO_SIZE               0x100
#define GPIO_GPFSEL4            0x10
#define GPIO_ALT0               4
#define BOARD_PI2               0x04

//
// The boards: the type field of a revision code (new style), and those
// with a 3.5 mm jack (2B, 3B, 3B+, 3A+, 4B).
//

#define BOARD_TYPE(Revision)    (((Revision) & 0x00800000) != 0 ? ((Revision) >> 4) & 0xFF : 0)

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
    IN PGLOBAL_DEVICE_INFO pGDI
    )

/*++

Routine Description:

    This routine tells whether the PWM answers: a range written reads back.
    In QEMU, which has no PWM, it reads 0.

--*/

{
    ULONG Saved = READ_REGISTER(pGDI->Pwm, PWM_RNG2);
    BOOLEAN Present;

    WRITE_REGISTER(pGDI->Pwm, PWM_RNG2, 0x1234);
    Present = READ_REGISTER(pGDI->Pwm, PWM_RNG2) == 0x1234;
    WRITE_REGISTER(pGDI->Pwm, PWM_RNG2, Saved);
    return Present;
}

static VOID
JackInitialize(
    IN PGLOBAL_DEVICE_INFO pGDI
    )

/*++

Routine Description:

    This routine starts the PWM of the jack: its clock (PLLD divided to
    125 MHz), its pins (GPIO 40 and 41, alternate function 0), and the two
    channels from the FIFO at the middle level of a 48 kHz range, which they
    repeat until the DMA channel feeds them.

--*/

{
    BOOLEAN Bcm2711 = (pGDI->Hardware.Flags & BCMSOUND_BCM2711) != 0;
    ULONG Plld = Bcm2711 ? PLLD_RATE_BCM2711 : PLLD_RATE;
    ULONG Rate = Bcm2711 ? PWM_CLOCK_BCM2711 : PWM_CLOCK;
    ULONG Select;

    WRITE_REGISTER(pGDI->Pwm, PWM_CTL, 0);
    WRITE_REGISTER(pGDI->Pwm, PWM_DMAC, 0);
    WRITE_REGISTER(pGDI->Clock, CM_PWMCTL, CM_PASSWORD | CM_SRC_PLLD);
    StallUntil(pGDI->Clock, CM_PWMCTL, CM_BUSY, 0);
    WRITE_REGISTER(pGDI->Clock, CM_PWMDIV, CM_PASSWORD | ((Plld / Rate) << 12));
    WRITE_REGISTER(pGDI->Clock, CM_PWMCTL, CM_PASSWORD | CM_SRC_PLLD | CM_ENAB);
    StallUntil(pGDI->Clock, CM_PWMCTL, CM_BUSY, CM_BUSY);
    pGDI->PwmClock = Rate;

    Select = READ_REGISTER(pGDI->Gpio, GPIO_GPFSEL4);
    if (pGDI->BoardType == BOARD_PI2) {
        Select = (Select & ~(7 | (7 << 15))) | GPIO_ALT0 | (GPIO_ALT0 << 15);
    } else {
        Select = (Select & ~0x3F) | GPIO_ALT0 | (GPIO_ALT0 << 3);
    }

    WRITE_REGISTER(pGDI->Gpio, GPIO_GPFSEL4, Select);

    pGDI->PwmRange = (Rate + 24000) / 48000;
    WRITE_REGISTER(pGDI->Pwm, PWM_RNG1, pGDI->PwmRange);
    WRITE_REGISTER(pGDI->Pwm, PWM_RNG2, pGDI->PwmRange);
    WRITE_REGISTER(pGDI->Pwm, PWM_STA, PWM_STA_ERRORS);
    WRITE_REGISTER(pGDI->Pwm, PWM_CTL, PWM_CTL_CLRF1);
    KeStallExecutionProcessor(10);
    WRITE_REGISTER(pGDI->Pwm, PWM_FIF1, pGDI->PwmRange / 2);
    WRITE_REGISTER(pGDI->Pwm, PWM_FIF1, pGDI->PwmRange / 2);
    WRITE_REGISTER(pGDI->Pwm, PWM_DMAC, PWM_DMAC_ENAB | PWM_DMAC_PANIC(7) | PWM_DMAC_DREQ(7));
    WRITE_REGISTER(pGDI->Pwm, PWM_CTL, PWM_RUN);
}

BOOLEAN
OutputInitialize(
    IN PGLOBAL_DEVICE_INFO pGDI
    )

/*++

Routine Description:

    This routine maps the registers, chooses the output and makes the ring.

--*/

{
    ULONG Base = pGDI->Hardware.PeripheralBase;
    BOOLEAN Bcm2711 = (pGDI->Hardware.Flags & BCMSOUND_BCM2711) != 0;
    BOOLEAN Jack = FALSE, Hdmi = FALSE, HdmiAudio = FALSE;
    DEVICE_DESCRIPTION Description;
    PHYSICAL_ADDRESS Logical;
    ULONG MapRegisters_;

    pGDI->BoardType = BOARD_TYPE(pGDI->Hardware.BoardRevision);
    pGDI->HasJack = BoardHasJack(pGDI->BoardType);
    pGDI->DmaChannel = MapRegisters(Base + DMA_OFFSET + pGDI->Hardware.DmaChannel * DMA_CHANNEL_SIZE, DMA_CHANNEL_SIZE);
    pGDI->DmaEnable = MapRegisters(Base + DMA_OFFSET + DMA_ENABLE, sizeof(ULONG));
    pGDI->Pwm = MapRegisters(Base + (Bcm2711 ? PWM1_OFFSET : PWM_OFFSET), PWM_SIZE);
    pGDI->Clock = MapRegisters(Base + CLOCK_OFFSET, CLOCK_SIZE);
    pGDI->Gpio = MapRegisters(Base + GPIO_OFFSET, GPIO_SIZE);
    if (pGDI->DmaChannel == NULL || pGDI->DmaEnable == NULL || pGDI->Pwm == NULL || pGDI->Clock == NULL ||
        pGDI->Gpio == NULL) {
        BcmSoundLog(pGDI, "Cannot map the registers");
        return FALSE;
    }

    pGDI->PwmFifoBus = BUS_PERIPHERAL_BASE + (Bcm2711 ? PWM1_OFFSET : PWM_OFFSET) + PWM_FIF1;
    pGDI->PwmDreq = Bcm2711 ? PWM1_DREQ : PWM_DREQ;

    if ((pGDI->Hardware.Flags & BCMSOUND_EMULATED) != 0) {
        BcmSoundLog(pGDI, "Emulated board (QEMU): no output, silent");
        pGDI->Setting = OUTPUT_NONE;
    } else if (pGDI->HasJack) {
        Jack = JackPresent(pGDI);
        BcmSoundLog(pGDI, "Board type %02X has a jack; the PWM %s", pGDI->BoardType,
                    Jack ? "answers" : "does not answer (QEMU?)");
    } else {
        BcmSoundLog(pGDI, "Board type %02X has no jack", pGDI->BoardType);
    }

    Hdmi = pGDI->Setting != OUTPUT_NONE && HdmiInitialize(pGDI);
    if (Hdmi) {
        HdmiAudio = HdmiSinkHasAudio(pGDI);
        BcmSoundLog(pGDI, "HDMI: the display %s sound", HdmiAudio ? "takes" : "does not say it takes");
    }

    switch (pGDI->Setting) {
    case OUTPUT_HDMI:
        pGDI->Output = Hdmi ? OUTPUT_HDMI : OUTPUT_NONE;
        break;

    case OUTPUT_JACK:
        pGDI->Output = Jack ? OUTPUT_JACK : OUTPUT_NONE;
        break;

    case OUTPUT_NONE:
        pGDI->Output = OUTPUT_NONE;
        break;

    default:
        pGDI->Output = Hdmi && (HdmiAudio || !Jack) ? OUTPUT_HDMI : Jack ? OUTPUT_JACK : OUTPUT_NONE;
        break;
    }

    BcmSoundLog(pGDI, "Setting %s: output %s",
                pGDI->Setting == OUTPUT_HDMI ? "HDMI" : pGDI->Setting == OUTPUT_JACK ? "JACK" :
                pGDI->Setting == OUTPUT_NONE ? "NONE" : "AUTO",
                pGDI->Output == OUTPUT_HDMI ? "HDMI" : pGDI->Output == OUTPUT_JACK ? "jack" : "none (silent)");

    //
    // The engine: its lock, its deferred routine (the end of a half), the
    // timer of the silent output and the synthesizer, at the output's rate.
    //

    KeInitializeSpinLock(&pGDI->EngineLock);
    KeInitializeDpc(&pGDI->EngineDpc, OutputEngineDeferred, pGDI);
    KeInitializeTimer(&pGDI->Timer);
    KeInitializeDpc(&pGDI->TimerDpc, OutputEngineDeferred, pGDI);
    Opl3Init(&pGDI->Synth, OUTPUT_RATE);

    if (pGDI->Output == OUTPUT_NONE) {
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
    Description.MaximumLength = 2 * RING_HALF_SIZE + PAGE_SIZE;
    pGDI->RingAdapter = HalGetAdapter(&Description, &MapRegisters_);
    if (pGDI->RingAdapter != NULL) {
        pGDI->Ring = HalAllocateCommonBuffer(pGDI->RingAdapter, Description.MaximumLength, &Logical, FALSE);
    }

    if (pGDI->Ring == NULL) {
        BcmSoundLog(pGDI, "No ring for the DMA channel: silent");
        pGDI->Output = OUTPUT_NONE;
        return TRUE;
    }

    pGDI->RingBus = Logical.LowPart;
    pGDI->ControlBlocks = (PDMA_CONTROL_BLOCK)(pGDI->Ring + 2 * RING_HALF_SIZE);
    pGDI->ControlBlocksBus = pGDI->RingBus + 2 * RING_HALF_SIZE;
    BcmSoundLog(pGDI, "Ring at bus address %08X", pGDI->RingBus);

    if (pGDI->Output == OUTPUT_JACK) {
        JackInitialize(pGDI);
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
    IN PGLOBAL_DEVICE_INFO pGDI,
    OUT PULONG Words,
    IN LONG Left,
    IN LONG Right
    )
{
    if (pGDI->Output == OUTPUT_JACK) {
        Words[0] = ((ULONG)(Right + 32768) * pGDI->PwmRange) >> 16;
        Words[1] = ((ULONG)(Left + 32768) * pGDI->PwmRange) >> 16;
    } else {
        HdmiEncode(pGDI, Words, Left, Right);
    }
}

static VOID
ReadWaveFrame(
    IN PGLOBAL_DEVICE_INFO pGDI,
    OUT PLONG Frame
    )

/*++

Routine Description:

    This routine reads the next frame of soundlib's buffer, as 16-bit left
    and right, and notes when the reading passes into the other half.

--*/

{
    PUCHAR Source = (PUCHAR)pGDI->WaveInfo.DMABuf.VirtualAddress + pGDI->WaveOffset;

    if (pGDI->Bits == 8) {
        Frame[0] = ((LONG)Source[0] - 128) << 8;
        Frame[1] = pGDI->Channels == 2 ? ((LONG)Source[1] - 128) << 8 : Frame[0];
    } else {
        Frame[0] = (SHORT)(Source[0] | (Source[1] << 8));
        Frame[1] = pGDI->Channels == 2 ? (SHORT)(Source[2] | (Source[3] << 8)) : Frame[0];
    }

    pGDI->WaveOffset += pGDI->FrameSize;
    if (pGDI->WaveOffset >= pGDI->BufferSize) {
        pGDI->WaveOffset = 0;
        pGDI->WaveCrossed += 1;
    } else if (pGDI->WaveOffset == pGDI->BufferSize / 2) {
        pGDI->WaveCrossed += 1;
    }
}

static VOID
MixHalf(
    IN PGLOBAL_DEVICE_INFO pGDI,
    IN ULONG Half
    )

/*++

Routine Description:

    This routine mixes the next half of the output into a half of the ring
    (nowhere for the silent output, which reads the wave output and runs
    the synthesizer all the same, in time). It is called holding the lock of
    the engine.

--*/

{
    PULONG Words = pGDI->Ring == NULL ? NULL : (PULONG)(pGDI->Ring + Half * RING_HALF_SIZE);
    BOOLEAN Wave;
    BOOLEAN Synth = pGDI->SynthOpen;
    LONG Mix[2], Interpolated;
    ULONG Frame, Channel, Phase;

    //
    // Soundlib may end the playing of its buffer without a stop (the last
    // of the sound played): the reading ends with it.
    //

    if (pGDI->WaveActive && !pGDI->WaveInfo.DMABusy) {
        pGDI->WaveActive = FALSE;
    }
    Wave = pGDI->WaveActive;

    if (Synth) {
        Opl3Render(&pGDI->Synth, pGDI->SynthFrames, RING_FRAMES);
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
            pGDI->WavePhase += pGDI->WaveStep;
            while (pGDI->WavePhase >= 0x10000) {
                pGDI->WavePhase -= 0x10000;
                pGDI->WavePrevious[0] = pGDI->WaveCurrent[0];
                pGDI->WavePrevious[1] = pGDI->WaveCurrent[1];
                ReadWaveFrame(pGDI, pGDI->WaveCurrent);
            }
            Phase = pGDI->WavePhase >> 2;
            for (Channel = 0; Channel < 2; Channel += 1) {
                Interpolated = pGDI->WavePrevious[Channel] +
                               (((pGDI->WaveCurrent[Channel] - pGDI->WavePrevious[Channel]) * (LONG)Phase) >> 14);
                Mix[Channel] += (Interpolated * (LONG)pGDI->WaveGain[Channel]) >> 16;
            }
        }

        if (Synth) {
            for (Channel = 0; Channel < 2; Channel += 1) {
                Mix[Channel] += ((LONG)pGDI->SynthFrames[2 * Frame + Channel] * (LONG)pGDI->SynthGain[Channel]) >> 16;
            }
        }

        //
        // The self-test (debug_sound) keeps the output running for its time,
        // playing silence.
        //

        if (pGDI->ToneFrames != 0) {
            pGDI->ToneFrames -= 1;
        }

        if (Words != NULL) {
            for (Channel = 0; Channel < 2; Channel += 1) {
                if (Mix[Channel] > 32767) {
                    Mix[Channel] = 32767;
                } else if (Mix[Channel] < -32768) {
                    Mix[Channel] = -32768;
                }
            }
            EncodeFrame(pGDI, Words, Mix[0], Mix[1]);
            Words += 2;
        }
    }
}

//
// The DMA channel.
//

static ULONG
PlayingHalf(
    IN PGLOBAL_DEVICE_INFO pGDI
    )
{
    return READ_REGISTER(pGDI->DmaChannel, DMA_CONBLK_AD) == pGDI->ControlBlocksBus + sizeof(DMA_CONTROL_BLOCK) ?
           1 : 0;
}

static VOID
DmaStop(
    IN PGLOBAL_DEVICE_INFO pGDI
    )
{
    WRITE_REGISTER(pGDI->DmaChannel, DMA_CS, 0);
    StallUntil(pGDI->DmaChannel, DMA_CS, DMA_CS_ACTIVE, 0);
    WRITE_REGISTER(pGDI->DmaChannel, DMA_CS, DMA_CS_RESET);
    KeStallExecutionProcessor(10);
    WRITE_REGISTER(pGDI->DmaChannel, DMA_CS, DMA_CS_INT | DMA_CS_END);
    WRITE_REGISTER(pGDI->DmaChannel, DMA_DEBUG, DMA_DEBUG_ERRORS);
}

static VOID
DmaStart(
    IN PGLOBAL_DEVICE_INFO pGDI,
    IN ULONG TransferInformation,
    IN ULONG Destination
    )

/*++

Routine Description:

    This routine starts the DMA channel on the ring: two control blocks, one
    a half, each ending in an interrupt and leading to the other; it starts
    with half 0.

--*/

{
    ULONG Half;

    for (Half = 0; Half < 2; Half += 1) {
        pGDI->ControlBlocks[Half].TransferInformation = TransferInformation;
        pGDI->ControlBlocks[Half].SourceAddress = pGDI->RingBus + Half * RING_HALF_SIZE;
        pGDI->ControlBlocks[Half].DestinationAddress = Destination;
        pGDI->ControlBlocks[Half].TransferLength = RING_HALF_SIZE;
        pGDI->ControlBlocks[Half].Stride = 0;
        pGDI->ControlBlocks[Half].NextControlBlock = pGDI->ControlBlocksBus + (1 - Half) * sizeof(DMA_CONTROL_BLOCK);
        pGDI->ControlBlocks[Half].Reserved[0] = 0;
        pGDI->ControlBlocks[Half].Reserved[1] = 0;
    }

    MEMORY_BARRIER();
    WRITE_REGISTER(pGDI->DmaEnable, 0,
                   READ_REGISTER(pGDI->DmaEnable, 0) | (1 << pGDI->Hardware.DmaChannel));
    DmaStop(pGDI);
    WRITE_REGISTER(pGDI->DmaChannel, DMA_CONBLK_AD, pGDI->ControlBlocksBus);
    WRITE_REGISTER(pGDI->DmaChannel, DMA_CS, DMA_CS_ACTIVE | DMA_CS_PRIORITY(8) | DMA_CS_PANIC_PRIORITY(15) |
                                             DMA_CS_WAIT_WRITES);
}

//
// Starting and stopping the engine (at DISPATCH_LEVEL, holding its lock).
//

static BOOLEAN
EngineStart(
    IN PGLOBAL_DEVICE_INFO pGDI
    )
{
    LARGE_INTEGER Due;
    ULONG TransferInformation, Destination;

    pGDI->IdleHalves = 0;
    pGDI->HalvesDone = 0;

    if (pGDI->Output == OUTPUT_NONE) {
        Due.QuadPart = -(LONGLONG)RING_HALF_TIME;
        KeSetTimer(&pGDI->Timer, Due, &pGDI->TimerDpc);
        pGDI->Running = TRUE;
        return TRUE;
    }

    if (pGDI->Output == OUTPUT_HDMI) {
        HdmiResetFrames(pGDI);
        if (!HdmiStart(pGDI, OUTPUT_RATE)) {
            return FALSE;
        }
        TransferInformation = DMA_TI_INTEN | DMA_TI_WAIT_RESP | DMA_TI_DEST_DREQ | DMA_TI_SRC_INC | DMA_TI_SRC_WIDTH |
                              DMA_TI_PERMAP(HdmiDreq(pGDI));
        Destination = HdmiFifoBus(pGDI);
    } else {
        WRITE_REGISTER(pGDI->Pwm, PWM_STA, PWM_STA_ERRORS);
        TransferInformation = DMA_TI_INTEN | DMA_TI_WAIT_RESP | DMA_TI_DEST_DREQ | DMA_TI_SRC_INC |
                              DMA_TI_PERMAP(pGDI->PwmDreq);
        Destination = pGDI->PwmFifoBus;
    }

    MixHalf(pGDI, 0);
    MixHalf(pGDI, 1);
    pGDI->Running = TRUE;
    DmaStart(pGDI, TransferInformation, Destination);
    if (pGDI->Output == OUTPUT_HDMI) {
        HdmiEnable(pGDI);
    }
    return TRUE;
}

static VOID
EngineStop(
    IN PGLOBAL_DEVICE_INFO pGDI
    )
{
    if (pGDI->Output == OUTPUT_NONE) {
        KeCancelTimer(&pGDI->Timer);
    } else {
        DmaStop(pGDI);
        if (pGDI->Output == OUTPUT_HDMI) {
            HdmiStop(pGDI);
        } else {

            //
            // The jack keeps the last level: the middle one.
            //

            WRITE_REGISTER(pGDI->Pwm, PWM_FIF1, pGDI->PwmRange / 2);
            WRITE_REGISTER(pGDI->Pwm, PWM_FIF1, pGDI->PwmRange / 2);
        }
    }
    pGDI->Running = FALSE;
}

VOID
OutputEngineUpdate(
    IN PGLOBAL_DEVICE_INFO pGDI
    )

/*++

Routine Description:

    This routine starts the engine when it has something to play (the
    synthesizer was opened), and at the start for HDMI, which plays on; the
    others stop by themselves, a ring after their last sound
    (OutputEngineDeferred).

--*/

{
    KIRQL Irql;

    KeAcquireSpinLock(&pGDI->EngineLock, &Irql);
    if (!pGDI->Running && (pGDI->WaveActive || pGDI->SynthOpen || pGDI->Output == OUTPUT_HDMI)) {
        EngineStart(pGDI);
    }
    KeReleaseSpinLock(&pGDI->EngineLock, Irql);
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

--*/

{
    PGLOBAL_DEVICE_INFO pGDI = Context;

    if (pGDI->WaveInfo.DMABusy && !pGDI->WaveInfo.DpcQueued) {
        pGDI->WaveInfo.DpcQueued = TRUE;
        IoRequestDpc(pGDI->WaveInfo.DeviceObject, NULL, NULL);
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

--*/

{
    PGLOBAL_DEVICE_INFO pGDI = Context;
    ULONG Crossed;
    ULONG Half;

    KeAcquireSpinLockAtDpcLevel(&pGDI->EngineLock);
    if (!pGDI->Running) {
        KeReleaseSpinLockFromDpcLevel(&pGDI->EngineLock);
        return;
    }

    //
    // Without an output the timer stands for the end of a half: NT 3.5 has no
    // periodic timer (KeSetTimerEx), so it is set again for the next half
    // (EngineStop cancels it).
    //

    if (pGDI->Output == OUTPUT_NONE) {
        LARGE_INTEGER Due;

        Due.QuadPart = -(LONGLONG)RING_HALF_TIME;
        KeSetTimer(&pGDI->Timer, Due, &pGDI->TimerDpc);
    }

    if (pGDI->WaveActive || pGDI->SynthOpen || pGDI->ToneFrames != 0) {
        pGDI->IdleHalves = 0;
    } else {
        pGDI->IdleHalves += 1;
    }

    //
    // HDMI plays on (silence) once started: a display or capture card takes
    // a while to lock on to a new audio stream, and would miss the start of
    // each sound, or a short sound whole.
    //

    if (pGDI->IdleHalves > 2 && pGDI->Output != OUTPUT_HDMI) {
        EngineStop(pGDI);
        KeReleaseSpinLockFromDpcLevel(&pGDI->EngineLock);
        return;
    }

    Half = pGDI->Output == OUTPUT_NONE ? 0 : PlayingHalf(pGDI) ^ 1;
    MixHalf(pGDI, Half);
    Crossed = pGDI->WaveCrossed;
    KeReleaseSpinLockFromDpcLevel(&pGDI->EngineLock);

    if (Crossed != 0 && pGDI->WaveInfo.Interrupt != NULL) {
        KeSynchronizeExecution(pGDI->WaveInfo.Interrupt, RequestWaveDeferred, pGDI);
    }
}

BOOLEAN
OutputInterrupt(
    IN PGLOBAL_DEVICE_INFO pGDI
    )
{
    ULONG Status;

    if (pGDI->Output == OUTPUT_NONE) {
        return FALSE;
    }
    Status = READ_REGISTER(pGDI->DmaChannel, DMA_CS);
    if ((Status & DMA_CS_INT) == 0) {
        return FALSE;
    }
    WRITE_REGISTER(pGDI->DmaChannel, DMA_CS, Status & ~(DMA_CS_ABORT | DMA_CS_RESET));
    pGDI->HalvesDone += 1;
    KeInsertQueueDpc(&pGDI->EngineDpc, NULL, NULL);
    return TRUE;
}

VOID
OutputShutdown(
    IN PGLOBAL_DEVICE_INFO pGDI
    )
{
    KIRQL Irql;

    KeAcquireSpinLock(&pGDI->EngineLock, &Irql);
    pGDI->WaveActive = FALSE;
    pGDI->SynthOpen = FALSE;
    if (pGDI->Running) {
        EngineStop(pGDI);
    }
    KeReleaseSpinLock(&pGDI->EngineLock, Irql);
    if (pGDI->Output == OUTPUT_JACK) {
        WRITE_REGISTER(pGDI->Pwm, PWM_DMAC, 0);
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
    PGLOBAL_DEVICE_INFO pGDI = WaveInfo->HwContext;

    pGDI->Rate = WaveInfo->SamplesPerSec;
    pGDI->Bits = WaveInfo->BitsPerSample;
    pGDI->Channels = WaveInfo->Channels;
    pGDI->FrameSize = pGDI->Bits / 8 * pGDI->Channels;
    pGDI->WaveStep = (ULONG)(((ULONGLONG)pGDI->Rate << 16) / OUTPUT_RATE);
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

--*/

{
    PGLOBAL_DEVICE_INFO pGDI = WaveInfo->HwContext;
    BOOLEAN Started = TRUE;

    if (pGDI->InWaveDeferred && pGDI->WaveActive) {
        return TRUE;
    }

    KeAcquireSpinLockAtDpcLevel(&pGDI->EngineLock);
    pGDI->BufferSize = WaveInfo->DoubleBuffer.BufferSize;
    pGDI->WaveOffset = 0;
    pGDI->WavePhase = 0;
    pGDI->WaveCrossed = 0;
    pGDI->WavePrevious[0] = pGDI->WavePrevious[1] = 0;
    pGDI->WaveCurrent[0] = pGDI->WaveCurrent[1] = 0;
    pGDI->WaveActive = TRUE;
    if (!pGDI->Running) {
        Started = EngineStart(pGDI);
    }
    KeReleaseSpinLockFromDpcLevel(&pGDI->EngineLock);
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

--*/

{
    PGLOBAL_DEVICE_INFO pGDI = WaveInfo->HwContext;
    KIRQL Irql;

    KeAcquireSpinLock(&pGDI->EngineLock, &Irql);
    pGDI->WaveActive = FALSE;
    pGDI->WaveCrossed = 0;
    KeReleaseSpinLock(&pGDI->EngineLock, Irql);
    return TRUE;
}

VOID
OutputLogState(
    IN PGLOBAL_DEVICE_INFO pGDI,
    IN PCHAR Event
    )

/*++

Routine Description:

    This routine logs (debug_sound=1) an event of a device and the state of
    the output: running or not, the halves played since it started (the
    interrupts), the format of the wave output and where its buffer is read,
    the gains, and the registers of the DMA channel and of the output.

--*/

{
    if (!pGDI->Debug) {
        return;
    }

    BcmSoundLog(pGDI, "%s: output %s, %u halves; wave %s %u Hz %u-bit %u ch, read at %u of %u; synth %s; gains wave %u/%u synth %u/%u",
                Event, pGDI->Running ? "running" : "stopped", pGDI->HalvesDone,
                pGDI->WaveActive ? "active" : "idle", pGDI->Rate, pGDI->Bits, pGDI->Channels,
                pGDI->WaveOffset, pGDI->BufferSize, pGDI->SynthOpen ? "open" : "closed",
                pGDI->WaveGain[0], pGDI->WaveGain[1], pGDI->SynthGain[0], pGDI->SynthGain[1]);
    if (pGDI->Output != OUTPUT_NONE) {
        BcmSoundLog(pGDI, "    DMA CS %08X CONBLK %08X LEN %08X DEBUG %08X; %s %08X",
                    READ_REGISTER(pGDI->DmaChannel, DMA_CS), READ_REGISTER(pGDI->DmaChannel, DMA_CONBLK_AD),
                    READ_REGISTER(pGDI->DmaChannel, DMA_TXFR_LEN), READ_REGISTER(pGDI->DmaChannel, DMA_DEBUG),
                    pGDI->Output == OUTPUT_HDMI ? "MAI_CTL" : "PWM STA",
                    pGDI->Output == OUTPUT_HDMI ? HdmiReadControl(pGDI) : READ_REGISTER(pGDI->Pwm, PWM_STA));
    }
}

VOID
OutputSelfTest(
    IN PGLOBAL_DEVICE_INFO pGDI
    )

/*++

Routine Description:

    This routine (debug_sound=1, at the start) runs the output for two
    seconds, silent, and logs what the hardware did in its first 400 ms: the halves played (the interrupts
    of the DMA channel), the state of the DMA channel and, for HDMI, of the
    MAI. It is called at PASSIVE_LEVEL.

--*/

{
    LARGE_INTEGER Delay;
    KIRQL Irql;
    ULONG Step;
    BOOLEAN Started;

    KeAcquireSpinLock(&pGDI->EngineLock, &Irql);
    pGDI->ToneFrames = OUTPUT_RATE * 2;
    Started = pGDI->Running || EngineStart(pGDI);
    KeReleaseSpinLock(&pGDI->EngineLock, Irql);
    if (!Started) {
        BcmSoundLog(pGDI, "Self-test: the output did not start");
        pGDI->ToneFrames = 0;
        return;
    }

    for (Step = 0; Step < 4; Step += 1) {
        Delay.QuadPart = -1000000;      // 100 ms
        KeDelayExecutionThread(KernelMode, FALSE, &Delay);
        if (pGDI->Output == OUTPUT_NONE) {
            BcmSoundLog(pGDI, "Self-test +%u ms: %u halves", (Step + 1) * 100, pGDI->HalvesDone);
        } else {
            BcmSoundLog(pGDI, "Self-test +%u ms: %u halves (interrupts); DMA CS %08X CONBLK %08X LEN %08X DEBUG %08X",
                        (Step + 1) * 100, pGDI->HalvesDone,
                        READ_REGISTER(pGDI->DmaChannel, DMA_CS), READ_REGISTER(pGDI->DmaChannel, DMA_CONBLK_AD),
                        READ_REGISTER(pGDI->DmaChannel, DMA_TXFR_LEN), READ_REGISTER(pGDI->DmaChannel, DMA_DEBUG));
            if (pGDI->Output == OUTPUT_HDMI) {
                BcmSoundLog(pGDI, "    MAI_CTL %08X", HdmiReadControl(pGDI));
            } else {
                BcmSoundLog(pGDI, "    PWM STA %08X", READ_REGISTER(pGDI->Pwm, PWM_STA));
            }
        }
    }

    if (pGDI->Output == OUTPUT_HDMI) {
        HdmiLogState(pGDI);
    }

    //
    // The output runs on; the engine stops by itself after it (but HDMI).
    //
}

ULONG
OutputTakeCrossings(
    IN PGLOBAL_DEVICE_INFO pGDI
    )

/*++

Routine Description:

    This routine takes one of the halves the reading has passed, for a call
    of soundlib's deferred routine; it returns 0 when there is none.

--*/

{
    ULONG Taken = 0;
    KIRQL Irql;

    KeAcquireSpinLock(&pGDI->EngineLock, &Irql);
    if (pGDI->WaveCrossed != 0) {
        pGDI->WaveCrossed -= 1;
        Taken = 1;
    }
    KeReleaseSpinLock(&pGDI->EngineLock, Irql);
    return Taken;
}
