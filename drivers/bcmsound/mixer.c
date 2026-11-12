/*++

Module Name:

    mixer.c

Abstract:

    The mixer device of the sound driver (\Device\BcmMixer0), on soundlib's
    mixer, as the Windows Sound System's: one destination, the speakers
    (whatever the output: HDMI, the jack or none), with a volume and a mute,
    and its two sources, the wave output and the FM synthesizer (MIDI), each
    with a volume and a mute. The output mixes them in software (output.c),
    at the gains these controls make (linear, as Windows takes volumes). The
    multimedia driver (bcmsnd32.dll, drvlib) makes of it the mixer that the
    volume control (sndvol32) and the volume of the taskbar (systray) find
    through the wave output device: its source line of the wave output names
    the same product (PID and name) as the device.

    The settings are kept in the value "Mixer Settings" of the service's
    Parameters\Device0, written when they change.

--*/

#include "bcmsound.h"

//
// The lines and controls: the destination, the speakers, then its sources,
// the wave output (named as the device, whose name is set as it plays) and
// the FM synthesizer.
//

static const MIXER_DD_LINE_CONFIGURATION_DATA MixerLineInit[MIXER_LINES] = {
    {
        0, 0, 2, 2, 2, 0,
        IDS_DEST_SPEAKERS_SHORT, IDS_DEST_SPEAKERS_LONG,
        MIXERLINE_COMPONENTTYPE_DST_SPEAKERS,
        MIXERLINE_TARGETTYPE_UNDEFINED, 0, 0
    },
    {
        0, 0, 2, 0, 2, 0,
        IDS_SRC_WAVEOUT_SHORT, IDS_SRC_WAVEOUT_LONG,
        MIXERLINE_COMPONENTTYPE_SRC_WAVEOUT,
        MIXERLINE_TARGETTYPE_WAVEOUT, BCMSOUND_PID_WAVEOUT, IDS_WAVEOUT_SILENT
    },
    {
        0, 1, 2, 0, 2, 0,
        IDS_SRC_SYNTH_SHORT, IDS_SRC_SYNTH_LONG,
        MIXERLINE_COMPONENTTYPE_SRC_SYNTHESIZER,
        MIXERLINE_TARGETTYPE_MIDIOUT, BCMSOUND_PID_SYNTH, IDS_SYNTH
    }
};

#define VOLUME_CONTROL(Line, Long) \
    {MIXERCONTROL_CONTROLTYPE_VOLUME, 0, (Line), 0, \
     IDS_CONTROL_VOLUME_SHORT, (Long), {{0, 0xFFFF}}, {32}, 0}

#define MUTE_CONTROL(Line, Long) \
    {MIXERCONTROL_CONTROLTYPE_MUTE, MIXERCONTROL_CONTROLF_UNIFORM, (Line), 0, \
     IDS_CONTROL_MUTE_SHORT, (Long), {{0, 1}}, {0}, 0}

static const MIXER_DD_CONTROL_CONFIGURATION_DATA MixerControlInit[MIXER_CONTROLS] = {
    VOLUME_CONTROL(LINE_SPEAKERS, IDS_CONTROL_VOLUME_LONG),
    MUTE_CONTROL(LINE_SPEAKERS, IDS_CONTROL_MUTE_LONG),
    VOLUME_CONTROL(LINE_WAVE_OUT, IDS_CONTROL_WAVEVOLUME_LONG),
    MUTE_CONTROL(LINE_WAVE_OUT, IDS_CONTROL_WAVEMUTE_LONG),
    VOLUME_CONTROL(LINE_SYNTH, IDS_CONTROL_SYNTHVOLUME_LONG),
    MUTE_CONTROL(LINE_SYNTH, IDS_CONTROL_SYNTHMUTE_LONG)
};

#define IS_MUTE(Control)        (((Control) & 1) != 0)

static NTSTATUS
MixerGetLineFlags(
    IN PMIXER_INFO MixerInfo,
    IN ULONG LineId,
    IN ULONG Length,
    OUT PVOID Data
    );

static NTSTATUS
MixerGetControl(
    IN PMIXER_INFO MixerInfo,
    IN ULONG ControlId,
    IN ULONG Length,
    OUT PVOID Data
    );

static NTSTATUS
MixerGetCombinedControl(
    IN PMIXER_INFO MixerInfo,
    IN ULONG ControlId,
    IN ULONG Length,
    OUT PVOID Data
    );

static NTSTATUS
MixerSetControl(
    IN PMIXER_INFO MixerInfo,
    IN ULONG ControlId,
    IN ULONG Length,
    IN PVOID Data
    );

//
// The gains of the sources (0-0x10000), from the controls.
//

static VOID
MixerUpdateGains(
    IN PGLOBAL_DEVICE_INFO GlobalInfo
    )
{
    ULONG Channel;
    ULONG Master;
    ULONG Wave;
    ULONG Synth;

    for (Channel = 0; Channel < 2; Channel += 1) {
        Master = GlobalInfo->MixerValue[CONTROL_MASTER_MUTE][0] ? 0 :
                 GlobalInfo->MixerValue[CONTROL_MASTER_VOLUME][Channel];
        Wave = GlobalInfo->MixerValue[CONTROL_WAVE_MUTE][0] ? 0 :
               GlobalInfo->MixerValue[CONTROL_WAVE_VOLUME][Channel];
        Synth = GlobalInfo->MixerValue[CONTROL_SYNTH_MUTE][0] ? 0 :
                GlobalInfo->MixerValue[CONTROL_SYNTH_VOLUME][Channel];

        //
        // 0xFFFF is full: 0x10000.
        //

        Master += Master >> 15;
        Wave += Wave >> 15;
        Synth += Synth >> 15;
        GlobalInfo->WaveGain[Channel] = (ULONG)(((ULONGLONG)Master * Wave) >> 16);
        GlobalInfo->SynthGain[Channel] = (ULONG)(((ULONGLONG)Master * Synth) >> 16);
    }
}

static VOID
MixerSaveSettings(
    IN PGLOBAL_DEVICE_INFO GlobalInfo
    )
{
    RtlWriteRegistryValue(RTL_REGISTRY_ABSOLUTE, GlobalInfo->DevicePathName, SOUND_MIXER_SETTINGS_NAME, REG_BINARY,
                          GlobalInfo->MixerValue, sizeof(GlobalInfo->MixerValue));
}

//
// The notifications of the lines whose activity changes: the wave output
// and the synthesizer, active when open.
//

static VOID
MixerWaveLineChanged(
    IN PLOCAL_DEVICE_INFO LocalInfo,
    IN UCHAR Code
    )
{
    PGLOBAL_DEVICE_INFO GlobalInfo = LocalInfo->pGlobalInfo;

    SoundMixerChangedItem(&GlobalInfo->MixerInfo, &GlobalInfo->LineNotification[LINE_WAVE_OUT]);
}

static VOID
MixerSynthLineChanged(
    IN PLOCAL_DEVICE_INFO LocalInfo,
    IN UCHAR Code
    )
{
    PGLOBAL_DEVICE_INFO GlobalInfo = LocalInfo->pGlobalInfo;

    SoundMixerChangedItem(&GlobalInfo->MixerInfo, &GlobalInfo->LineNotification[LINE_SYNTH]);
}

static NTSTATUS
MixerReadSettings(
    IN PWSTR ValueName,
    IN ULONG ValueType,
    IN PVOID ValueData,
    IN ULONG ValueLength,
    IN PVOID Context,
    IN PVOID EntryContext
    )
{
    if (ValueType == REG_BINARY && ValueLength == MIXER_CONTROLS * 2 * sizeof(USHORT)) {
        RtlCopyMemory(EntryContext, ValueData, ValueLength);
        *(PULONG)Context = ValueLength;
    }

    return STATUS_SUCCESS;
}

VOID
MixerInitialize(
    IN PGLOBAL_DEVICE_INFO GlobalInfo
    )

/*++

Routine Description:

    This routine sets the mixer device up, after the devices are made: the
    controls (those saved, else full volume and no mute), their notifications
    and those of the lines, and the volumes of the wave output and the
    synthesizer, which are their controls.

Arguments:

    GlobalInfo - Supplies the global device information.

Return Value:

    None.

--*/

{
    RTL_QUERY_REGISTRY_TABLE QueryTable[2];
    USHORT Saved[MIXER_CONTROLS][2];
    PLOCAL_DEVICE_INFO LocalInfo;
    ULONG Length;
    ULONG Index;

    SoundInitMixerInfo(&GlobalInfo->MixerInfo, MixerGetLineFlags, MixerGetControl, MixerGetCombinedControl,
                       MixerSetControl);

    LocalInfo = GlobalInfo->DeviceObject[MIXER_DEVICE_INDEX]->DeviceExtension;
    LocalInfo->DeviceType = MIXER_DEVICE;
    LocalInfo->DeviceSpecificData = &GlobalInfo->MixerInfo;
    for (Index = 0; Index < NUMBER_OF_DEVICES; Index += 1) {
        ((PLOCAL_DEVICE_INFO)GlobalInfo->DeviceObject[Index]->DeviceExtension)->MixerDevice = LocalInfo;
    }

    for (Index = 0; Index < MIXER_CONTROLS; Index += 1) {
        GlobalInfo->MixerValue[Index][0] = IS_MUTE(Index) ? 0 : 0xFFFF;
        GlobalInfo->MixerValue[Index][1] = GlobalInfo->MixerValue[Index][0];
        SoundInitDataItem(&GlobalInfo->MixerInfo, &GlobalInfo->ControlNotification[Index],
                          (USHORT)MM_MIXM_CONTROL_CHANGE, (USHORT)Index);
    }

    for (Index = 0; Index < MIXER_LINES; Index += 1) {
        SoundInitDataItem(&GlobalInfo->MixerInfo, &GlobalInfo->LineNotification[Index], (USHORT)MM_MIXM_LINE_CHANGE,
                          (USHORT)Index);
    }

    //
    // The saved settings (a REG_BINARY of the size of the controls').
    //

    RtlZeroMemory(QueryTable, sizeof(QueryTable));
    QueryTable[0].QueryRoutine = MixerReadSettings;
    QueryTable[0].Name = SOUND_MIXER_SETTINGS_NAME;
    QueryTable[0].EntryContext = Saved;
    QueryTable[0].DefaultType = REG_NONE;
    Length = 0;
    if (NT_SUCCESS(RtlQueryRegistryValues(RTL_REGISTRY_ABSOLUTE, GlobalInfo->DevicePathName, QueryTable, &Length,
                                          NULL)) &&
        Length == sizeof(Saved)) {
        for (Index = 0; Index < MIXER_CONTROLS; Index += 1) {
            if (IS_MUTE(Index)) {
                Saved[Index][0] = Saved[Index][1] = Saved[Index][0] != 0;
            }

            GlobalInfo->MixerValue[Index][0] = Saved[Index][0];
            GlobalInfo->MixerValue[Index][1] = Saved[Index][1];
        }

        BcmSoundLog(GlobalInfo, "Mixer settings: volume %04X/%04X%s, wave %04X/%04X%s, synthesizer %04X/%04X%s",
                    Saved[0][0], Saved[0][1], Saved[1][0] ? " muted" : "",
                    Saved[2][0], Saved[2][1], Saved[3][0] ? " muted" : "",
                    Saved[4][0], Saved[4][1], Saved[5][0] ? " muted" : "");
    }

    MixerUpdateGains(GlobalInfo);

    SoundSetLineNotify(GlobalInfo->DeviceObject[WAVE_OUT_DEVICE]->DeviceExtension, MixerWaveLineChanged);
    SoundSetLineNotify(GlobalInfo->DeviceObject[SYNTH_DEVICE_INDEX]->DeviceExtension, MixerSynthLineChanged);
    SoundSetVolumeControlId(GlobalInfo->DeviceObject[WAVE_OUT_DEVICE]->DeviceExtension, CONTROL_WAVE_VOLUME);
    SoundSetVolumeControlId(GlobalInfo->DeviceObject[SYNTH_DEVICE_INDEX]->DeviceExtension, CONTROL_SYNTH_VOLUME);
}

VOID
MixerShutdown(
    IN PGLOBAL_DEVICE_INFO GlobalInfo
    )
{
    MixerSaveSettings(GlobalInfo);
}

NTSTATUS
MixerDumpConfiguration(
    IN PLOCAL_DEVICE_INFO LocalInfo,
    IN OUT PIRP Irp,
    IN PIO_STACK_LOCATION IrpStack
    )

/*++

Routine Description:

    This routine returns the mixer's configuration
    (IOCTL_MIX_GET_CONFIGURATION): its capabilities, lines and controls, which
    drvlib names from the strings of the multimedia driver.

Arguments:

    LocalInfo - Supplies the mixer device.

    Irp - Supplies the request, whose buffer receives the configuration.

    IrpStack - Supplies the stack location of the request.

Return Value:

    STATUS_SUCCESS, also when the buffer holds a part of the configuration
    (as the sound drivers of NT answer it: drvlib asks for the size first,
    with a buffer of four bytes, and the DeviceIoControl of NT 3.5 returns no
    count of bytes with a warning status), or STATUS_INSUFFICIENT_RESOURCES.

--*/

{
    PGLOBAL_DEVICE_INFO GlobalInfo = LocalInfo->pGlobalInfo;
    struct {
        MIXER_DD_CONFIGURATION_DATA Header;
        MIXER_DD_LINE_CONFIGURATION_DATA LineData[MIXER_LINES];
        MIXER_DD_CONTROL_CONFIGURATION_DATA ControlData[MIXER_CONTROLS];
    } *Data;
    ULONG Length;

    Data = ExAllocatePool(PagedPool, sizeof(*Data));
    if (Data == NULL) {
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    RtlZeroMemory(Data, sizeof(*Data));
    Data->Header.cbSize = sizeof(*Data);
    Data->Header.NumberOfLines = MIXER_LINES;
    Data->Header.NumberOfControls = MIXER_CONTROLS;
    Data->Header.DeviceCaps.wMid = MM_MICROSOFT;
    Data->Header.DeviceCaps.wPid = BCMSOUND_PID_MIXER;
    Data->Header.DeviceCaps.vDriverVersion = BCMSOUND_VERSION;
    Data->Header.DeviceCaps.PnameStringId = IDS_MIXER;
    Data->Header.DeviceCaps.fdwSupport = 0;
    Data->Header.DeviceCaps.cDestinations = 1;
    RtlCopyMemory(Data->LineData, MixerLineInit, sizeof(MixerLineInit));
    RtlCopyMemory(Data->ControlData, MixerControlInit, sizeof(MixerControlInit));

    //
    // The wave output is named as it plays (its capabilities).
    //

    Data->LineData[LINE_WAVE_OUT].PnameStringId = (SHORT)WaveOutNameId(GlobalInfo);

    Length = min(sizeof(*Data), IrpStack->Parameters.DeviceIoControl.OutputBufferLength);
    RtlCopyMemory(Irp->AssociatedIrp.SystemBuffer, Data, Length);
    Irp->IoStatus.Information = Length;
    ExFreePool(Data);
    return STATUS_SUCCESS;
}

//
// soundlib's callouts (at PASSIVE_LEVEL, holding the driver's mutex).
//

static NTSTATUS
MixerGetLineFlags(
    IN PMIXER_INFO MixerInfo,
    IN ULONG LineId,
    IN ULONG Length,
    OUT PVOID Data
    )
{
    PGLOBAL_DEVICE_INFO GlobalInfo = CONTAINING_RECORD(MixerInfo, GLOBAL_DEVICE_INFO, MixerInfo);
    PULONG Flags = Data;

    if (Length != sizeof(ULONG)) {
        return STATUS_BUFFER_TOO_SMALL;
    }

    switch (LineId) {
    case LINE_SPEAKERS:
        *Flags = MIXERLINE_LINEF_ACTIVE;
        break;

    case LINE_WAVE_OUT:
        *Flags = MIXERLINE_LINEF_SOURCE | (GlobalInfo->DeviceInUse ? MIXERLINE_LINEF_ACTIVE : 0);
        break;

    case LINE_SYNTH:
        *Flags = MIXERLINE_LINEF_SOURCE | (GlobalInfo->SynthOpen ? MIXERLINE_LINEF_ACTIVE : 0);
        break;

    default:
        return STATUS_INVALID_PARAMETER;
    }

    return STATUS_SUCCESS;
}

static NTSTATUS
MixerGetControl(
    IN PMIXER_INFO MixerInfo,
    IN ULONG ControlId,
    IN ULONG Length,
    OUT PVOID Data
    )
{
    PGLOBAL_DEVICE_INFO GlobalInfo = CONTAINING_RECORD(MixerInfo, GLOBAL_DEVICE_INFO, MixerInfo);
    PULONG Values = Data;

    if (ControlId >= MIXER_CONTROLS) {
        return STATUS_INVALID_PARAMETER;
    }

    if (Length == sizeof(ULONG)) {
        Values[0] = max(GlobalInfo->MixerValue[ControlId][0], GlobalInfo->MixerValue[ControlId][1]);
    } else if (Length == 2 * sizeof(ULONG)) {
        Values[0] = GlobalInfo->MixerValue[ControlId][0];
        Values[1] = GlobalInfo->MixerValue[ControlId][1];
    } else {
        return STATUS_BUFFER_TOO_SMALL;
    }

    return STATUS_SUCCESS;
}

static NTSTATUS
MixerGetCombinedControl(
    IN PMIXER_INFO MixerInfo,
    IN ULONG ControlId,
    IN ULONG Length,
    OUT PVOID Data
    )

/*++

Routine Description:

    This routine returns the volume a device sets itself to (soundlib's
    IOCTL_SOUND_GET_CHANGED_VOLUME, which synthlib follows to scale the
    levels of its notes): always full, since the output applies the volumes
    of the mixer to what it mixes.

Arguments:

    MixerInfo - Not used.

    ControlId - Not used.

    Length - Supplies the length of the buffer.

    Data - Receives the left and right volumes.

Return Value:

    STATUS_SUCCESS, or STATUS_BUFFER_TOO_SMALL.

--*/

{
    PULONG Values = Data;

    if (Length < 2 * sizeof(ULONG)) {
        return STATUS_BUFFER_TOO_SMALL;
    }

    Values[0] = 0xFFFF;
    Values[1] = 0xFFFF;
    return STATUS_SUCCESS;
}

static NTSTATUS
MixerSetControl(
    IN PMIXER_INFO MixerInfo,
    IN ULONG ControlId,
    IN ULONG Length,
    IN PVOID Data
    )
{
    PGLOBAL_DEVICE_INFO GlobalInfo = CONTAINING_RECORD(MixerInfo, GLOBAL_DEVICE_INFO, MixerInfo);
    PULONG Values = Data;
    ULONG Value[2];
    ULONG Channel;
    BOOLEAN Changed = FALSE;

    if (ControlId >= MIXER_CONTROLS) {
        return STATUS_INVALID_PARAMETER;
    }

    if (Length == sizeof(ULONG)) {
        Value[0] = Value[1] = Values[0];
    } else if (Length == 2 * sizeof(ULONG)) {
        Value[0] = Values[0];
        Value[1] = Values[1];
    } else {
        return STATUS_BUFFER_TOO_SMALL;
    }

    if (IS_MUTE(ControlId)) {
        Value[0] = Value[1] = (Value[0] != 0 || Value[1] != 0);
    }

    for (Channel = 0; Channel < 2; Channel += 1) {
        if (Value[Channel] > 0xFFFF) {
            return STATUS_INVALID_PARAMETER;
        }
    }

    for (Channel = 0; Channel < 2; Channel += 1) {
        if (GlobalInfo->MixerValue[ControlId][Channel] != (USHORT)Value[Channel]) {
            GlobalInfo->MixerValue[ControlId][Channel] = (USHORT)Value[Channel];
            Changed = TRUE;
        }
    }

    if (Changed) {
        MixerUpdateGains(GlobalInfo);
        SoundMixerChangedItem(MixerInfo, &GlobalInfo->ControlNotification[ControlId]);
        MixerSaveSettings(GlobalInfo);
    }

    return STATUS_SUCCESS;
}
