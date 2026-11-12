/*++

Module Name:

    synth.c

Abstract:

    The FM synthesizer device of the sound driver (\Device\BcmSynth0), the
    MIDI output of the system. It takes what the Windows Sound System's
    synthesizer device (soundlib's synth.c) takes: the writes of the ports of
    an OPL3 at 388h-38Bh (SYNTH_DATA pairs, from synthlib, the MIDI driver of
    bcmsnd32.dll, which plays MIDI as FM notes with the patches of the WSS),
    a read of its status port, and IOCTL_MIDI_SET_OPL3_MODE. The chip is
    emulated (opl3.c), and its sound mixed into the output (output.c), which
    plays while the device is open.

--*/

#include "bcmsound.h"

//
// The silence of the chip, as soundlib's synth.c sets it on an open
// (AdLib mode) and on IOCTL_MIDI_SET_OPL3_MODE (OPL3 mode, 4-operator voices
// available): the registers synthlib takes for set.
//

static VOID
SynthQuiet(
    IN OPL3 *Chip,
    IN BOOLEAN Opl3
    )
{
    static const UCHAR Slots[18] = {0, 1, 2, 3, 4, 5, 8, 9, 10, 11, 12, 13, 16, 17, 18, 19, 20, 21};
    ULONG Index;

    Opl3Reset(Chip);
    if (Opl3) {
        Opl3Write(Chip, AD_NEW, 0x01);
        Opl3Write(Chip, AD_MASK, 0x60);
        Opl3Write(Chip, AD_CONNECTION, 0x3F);
        Opl3Write(Chip, AD_NTS, 0x00);
        Opl3Write(Chip, AD_DRUM, 0xC0);
        for (Index = 0; Index < 0x15; Index += 1) {
            Opl3Write(Chip, AD_LEVEL + Index, 0x3F);
            Opl3Write(Chip, AD_LEVEL2 + Index, 0x3F);
        }

        for (Index = 0; Index < 0x08; Index += 1) {
            Opl3Write(Chip, AD_BLOCK + Index, 0x00);
            Opl3Write(Chip, AD_BLOCK2 + Index, 0x00);
        }
    } else {
        Opl3Write(Chip, AD_NEW, 0x00);
        for (Index = 0; Index < 18; Index += 1) {
            Opl3Write(Chip, AD_LEVEL + Slots[Index], 0x3F);
        }

        for (Index = 0; Index <= 8; Index += 1) {
            Opl3Write(Chip, AD_FNUMBER + Index, 0);
            Opl3Write(Chip, AD_BLOCK + Index, 0);
        }

        Opl3Write(Chip, AD_DRUM, 0x20);
    }
}

static VOID
SynthReset(
    IN PGLOBAL_DEVICE_INFO GlobalInfo,
    IN BOOLEAN Opl3
    )
{
    KIRQL Irql;

    KeAcquireSpinLock(&GlobalInfo->EngineLock, &Irql);
    SynthQuiet(&GlobalInfo->Synth, Opl3);
    GlobalInfo->SynthAddress = 0;
    KeReleaseSpinLock(&GlobalInfo->EngineLock, Irql);
}

static NTSTATUS
SynthWrite(
    IN PGLOBAL_DEVICE_INFO GlobalInfo,
    IN PIRP Irp,
    IN PIO_STACK_LOCATION IrpStack
    )

/*++

Routine Description:

    This routine plays the port writes of a buffer: 388h and 38Ah select a
    register (of the first or second bank), 389h and 38Bh write it. They are
    copied from the caller's buffer a batch at a time, and applied under the
    lock of the output.

Arguments:

    GlobalInfo - Supplies the global device information.

    Irp - Supplies the write request, whose buffer holds the port writes.

    IrpStack - Supplies the stack location of the request.

Return Value:

    STATUS_SUCCESS, STATUS_BUFFER_TOO_SMALL for a length that is not of whole
    port writes, STATUS_ACCESS_VIOLATION for a bad buffer, or
    STATUS_INVALID_PARAMETER for a port that is not the chip's.

--*/

{
    SYNTH_DATA Batch[64];
    PSYNTH_DATA Data = (PSYNTH_DATA)Irp->UserBuffer;
    ULONG Length = IrpStack->Parameters.Write.Length;
    ULONG Count;
    ULONG Index;
    NTSTATUS Status = STATUS_SUCCESS;
    KIRQL Irql;

    if (Length % sizeof(SYNTH_DATA) != 0) {
        return STATUS_BUFFER_TOO_SMALL;
    }
    while (Length != 0) {
        Count = min(Length / sizeof(SYNTH_DATA), sizeof(Batch) / sizeof(Batch[0]));
        try {
            if (Irp->RequestorMode != KernelMode &&
                (ULONG)(Data + Count) > (ULONG)MM_HIGHEST_USER_ADDRESS) {
                ExRaiseStatus(STATUS_ACCESS_VIOLATION);
            }

            RtlCopyMemory(Batch, Data, Count * sizeof(SYNTH_DATA));
        } except (EXCEPTION_EXECUTE_HANDLER) {
            Status = STATUS_ACCESS_VIOLATION;
            break;
        }

        KeAcquireSpinLock(&GlobalInfo->EngineLock, &Irql);
        for (Index = 0; Index < Count; Index += 1) {
            switch (Batch[Index].IoPort) {
            case SYNTH_PORT:
                GlobalInfo->SynthAddress = Batch[Index].PortData & 0xFF;
                break;

            case SYNTH_PORT + 2:
                GlobalInfo->SynthAddress = 0x100 | (Batch[Index].PortData & 0xFF);
                break;

            case SYNTH_PORT + 1:
            case SYNTH_PORT + 3:
                Opl3Write(&GlobalInfo->Synth, GlobalInfo->SynthAddress, (UCHAR)Batch[Index].PortData);
                break;

            default:
                Status = STATUS_INVALID_PARAMETER;
                break;
            }

            if (!NT_SUCCESS(Status)) {
                break;
            }
        }

        KeReleaseSpinLock(&GlobalInfo->EngineLock, Irql);
        if (!NT_SUCCESS(Status)) {
            Count = Index;
            Length -= Count * sizeof(SYNTH_DATA);
            break;
        }

        Data += Count;
        Length -= Count * sizeof(SYNTH_DATA);
    }

    Irp->IoStatus.Information = IrpStack->Parameters.Write.Length - Length;
    return Status;
}

static NTSTATUS
SynthReadStatus(
    IN PGLOBAL_DEVICE_INFO GlobalInfo,
    IN PIRP Irp,
    IN PIO_STACK_LOCATION IrpStack
    )
{
    UCHAR Value;
    KIRQL Irql;

    if (IrpStack->Parameters.Read.Length != sizeof(UCHAR)) {
        return STATUS_INVALID_PARAMETER;
    }

    KeAcquireSpinLock(&GlobalInfo->EngineLock, &Irql);
    Value = Opl3ReadStatus(&GlobalInfo->Synth);
    KeReleaseSpinLock(&GlobalInfo->EngineLock, Irql);
    try {
        if (Irp->RequestorMode != KernelMode) {
            ProbeForWrite(Irp->UserBuffer, sizeof(UCHAR), sizeof(UCHAR));
        }

        *(PUCHAR)Irp->UserBuffer = Value;
    } except (EXCEPTION_EXECUTE_HANDLER) {
        return STATUS_ACCESS_VIOLATION;
    }

    Irp->IoStatus.Information = sizeof(UCHAR);
    return STATUS_SUCCESS;
}

NTSTATUS
SynthGetCaps(
    IN PLOCAL_DEVICE_INFO LocalInfo,
    IN OUT PIRP Irp,
    IN PIO_STACK_LOCATION IrpStack
    )
{
    MIDIOUTCAPSW Caps;

    RtlZeroMemory(&Caps, sizeof(Caps));
    Caps.wMid = MM_MICROSOFT;
    Caps.wPid = BCMSOUND_PID_SYNTH;
    Caps.vDriverVersion = BCMSOUND_VERSION;
    Caps.wTechnology = MOD_FMSYNTH;
    Caps.wVoices = 128;
    Caps.wNotes = 18;
    Caps.wChannelMask = 0xFFFF;
    Caps.dwSupport = MIDICAPS_VOLUME | MIDICAPS_LRVOLUME;
    *(PULONG)Caps.szPname = IDS_SYNTH;
    Irp->IoStatus.Information = min(sizeof(Caps), IrpStack->Parameters.DeviceIoControl.OutputBufferLength);
    RtlCopyMemory(Irp->AssociatedIrp.SystemBuffer, &Caps, Irp->IoStatus.Information);
    return STATUS_SUCCESS;
}

NTSTATUS
SynthDispatch(
    IN OUT PLOCAL_DEVICE_INFO LocalInfo,
    IN PIRP Irp,
    IN PIO_STACK_LOCATION IrpStack
    )

/*++

Routine Description:

    This routine handles the requests of the synthesizer device, as
    soundlib's SynthDispatch does those of a WSS's (at PASSIVE_LEVEL, holding
    the driver's mutex). One handle at a time may write: the opening one
    starts the output and the closing one silences the chip.

Arguments:

    LocalInfo - Supplies the synthesizer device.

    Irp - Supplies the request.

    IrpStack - Supplies the stack location of the request.

Return Value:

    The status of the request.

--*/

{
    PGLOBAL_DEVICE_INFO GlobalInfo = LocalInfo->pGlobalInfo;
    NTSTATUS Status = STATUS_SUCCESS;

    switch (IrpStack->MajorFunction) {
    case IRP_MJ_CREATE:
        Status = SoundSetShareAccess(LocalInfo, IrpStack);
        if (NT_SUCCESS(Status) && IrpStack->FileObject->WriteAccess) {
            SynthReset(GlobalInfo, FALSE);
            GlobalInfo->SynthOpen = TRUE;
            OutputEngineUpdate(GlobalInfo);
            OutputLogState(GlobalInfo, "Synthesizer opened");
            SoundLineNotify(LocalInfo, 0);
        }

        break;

    case IRP_MJ_WRITE:
        Status = IrpStack->FileObject->WriteAccess ? SynthWrite(GlobalInfo, Irp, IrpStack) : STATUS_ACCESS_DENIED;
        break;

    case IRP_MJ_READ:
        Status = IrpStack->FileObject->WriteAccess ? SynthReadStatus(GlobalInfo, Irp, IrpStack) : STATUS_ACCESS_DENIED;
        break;

    case IRP_MJ_DEVICE_CONTROL:
        switch (IrpStack->Parameters.DeviceIoControl.IoControlCode) {
        case IOCTL_MIDI_SET_VOLUME:
            Status = LocalInfo->PreventVolumeSetting ? STATUS_ACCESS_DENIED :
                     SoundIoctlSetVolume(LocalInfo, Irp, IrpStack);
            break;

        case IOCTL_MIDI_GET_VOLUME:
            Status = SoundIoctlGetVolume(LocalInfo, Irp, IrpStack);
            break;

        case IOCTL_SOUND_GET_CHANGED_VOLUME:
            Status = SoundIoctlGetChangedVolume(LocalInfo, Irp, IrpStack);
            break;

        case IOCTL_MIDI_GET_CAPABILITIES:
            Status = SynthGetCaps(LocalInfo, Irp, IrpStack);
            break;

        case IOCTL_MIDI_SET_OPL3_MODE:
            if (IrpStack->FileObject->WriteAccess) {
                SynthReset(GlobalInfo, TRUE);
            } else {
                Status = STATUS_ACCESS_DENIED;
            }

            break;

        default:
            Status = STATUS_INVALID_DEVICE_REQUEST;
            break;
        }

        break;

    case IRP_MJ_CLEANUP:
        if (IrpStack->FileObject->WriteAccess) {
            SynthReset(GlobalInfo, FALSE);
            (*LocalInfo->DeviceInit->ExclusionRoutine)(LocalInfo, SoundExcludeClose);
            LocalInfo->PreventVolumeSetting = FALSE;
            GlobalInfo->SynthOpen = FALSE;
            OutputEngineUpdate(GlobalInfo);
            OutputLogState(GlobalInfo, "Synthesizer closed");
            SoundLineNotify(LocalInfo, 0);
        }

        break;

    case IRP_MJ_CLOSE:
        break;

    default:
        Status = STATUS_INVALID_DEVICE_REQUEST;
        break;
    }

    return Status;
}
