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
// The lines and controls.
//

static CONST MIXER_DD_LINE_CONFIGURATION_DATA MixerLineInit[MixerLines] = {
    {   // the destination: the speakers
        0, 0, 2, 2, 2, 0,
        IDS_DEST_SPEAKERS_SHORT, IDS_DEST_SPEAKERS_LONG,
        MIXERLINE_COMPONENTTYPE_DST_SPEAKERS,
        MIXERLINE_TARGETTYPE_UNDEFINED, 0, 0
    },
    {   // its source 0: the wave output (the name of the device: set as it plays)
        0, 0, 2, 0, 2, 0,
        IDS_SRC_WAVEOUT_SHORT, IDS_SRC_WAVEOUT_LONG,
        MIXERLINE_COMPONENTTYPE_SRC_WAVEOUT,
        MIXERLINE_TARGETTYPE_WAVEOUT, BCMSOUND_PID_WAVEOUT, IDS_WAVEOUT_SILENT
    },
    {   // its source 1: the FM synthesizer
        0, 1, 2, 0, 2, 0,
        IDS_SRC_SYNTH_SHORT, IDS_SRC_SYNTH_LONG,
        MIXERLINE_COMPONENTTYPE_SRC_SYNTHESIZER,
        MIXERLINE_TARGETTYPE_MIDIOUT, BCMSOUND_PID_SYNTH, IDS_SYNTH
    }
};

#define VOLUME_CONTROL(Line, Long)                                              \
    { MIXERCONTROL_CONTROLTYPE_VOLUME, 0, (Line), 0,                            \
      IDS_CONTROL_VOLUME_SHORT, (Long), { { 0, 0xFFFF } }, { 32 }, 0 }
#define MUTE_CONTROL(Line, Long)                                                \
    { MIXERCONTROL_CONTROLTYPE_MUTE, MIXERCONTROL_CONTROLF_UNIFORM, (Line), 0,  \
      IDS_CONTROL_MUTE_SHORT, (Long), { { 0, 1 } }, { 0 }, 0 }

static CONST MIXER_DD_CONTROL_CONFIGURATION_DATA MixerControlInit[MixerControls] = {
    VOLUME_CONTROL(LineSpeakers, IDS_CONTROL_VOLUME_LONG),
    MUTE_CONTROL(LineSpeakers, IDS_CONTROL_MUTE_LONG),
    VOLUME_CONTROL(LineWaveOut, IDS_CONTROL_WAVEVOLUME_LONG),
    MUTE_CONTROL(LineWaveOut, IDS_CONTROL_WAVEMUTE_LONG),
    VOLUME_CONTROL(LineSynth, IDS_CONTROL_SYNTHVOLUME_LONG),
    MUTE_CONTROL(LineSynth, IDS_CONTROL_SYNTHMUTE_LONG)
};

#define IsMute(Control)         (((Control) & 1) != 0)

static NTSTATUS HwGetLineFlags(PMIXER_INFO, ULONG, ULONG, PVOID);
static NTSTATUS HwGetControl(PMIXER_INFO, ULONG, ULONG, PVOID);
static NTSTATUS HwGetCombinedControl(PMIXER_INFO, ULONG, ULONG, PVOID);
static NTSTATUS HwSetControl(PMIXER_INFO, ULONG, ULONG, PVOID);

//
// The gains of the sources (0-0x10000), from the controls.
//

static VOID
MixerUpdateGains(
    IN PGLOBAL_DEVICE_INFO pGDI
    )
{
    ULONG Channel, Master, Wave, Synth;

    for (Channel = 0; Channel < 2; Channel += 1) {
        Master = pGDI->MixerValue[ControlMasterMute][0] ? 0 : pGDI->MixerValue[ControlMasterVolume][Channel];
        Wave = pGDI->MixerValue[ControlWaveMute][0] ? 0 : pGDI->MixerValue[ControlWaveVolume][Channel];
        Synth = pGDI->MixerValue[ControlSynthMute][0] ? 0 : pGDI->MixerValue[ControlSynthVolume][Channel];

        //
        // 0xFFFF is full: 0x10000.
        //

        Master += Master >> 15;
        Wave += Wave >> 15;
        Synth += Synth >> 15;
        pGDI->WaveGain[Channel] = (ULONG)(((ULONGLONG)Master * Wave) >> 16);
        pGDI->SynthGain[Channel] = (ULONG)(((ULONGLONG)Master * Synth) >> 16);
    }
}

static VOID
MixerSaveSettings(
    IN PGLOBAL_DEVICE_INFO pGDI
    )
{
    RtlWriteRegistryValue(RTL_REGISTRY_ABSOLUTE, pGDI->DevicePathName, SOUND_MIXER_SETTINGS_NAME, REG_BINARY,
                          pGDI->MixerValue, sizeof(pGDI->MixerValue));
}

//
// The notifications of the lines whose activity changes: the wave output
// and the synthesizer, active when open.
//

static VOID
MixerWaveLineChanged(
    IN PLOCAL_DEVICE_INFO pLDI,
    IN UCHAR Code
    )
{
    PGLOBAL_DEVICE_INFO pGDI = pLDI->pGlobalInfo;

    SoundMixerChangedItem(&pGDI->MixerInfo, &pGDI->LineNotification[LineWaveOut]);
}

static VOID
MixerSynthLineChanged(
    IN PLOCAL_DEVICE_INFO pLDI,
    IN UCHAR Code
    )
{
    PGLOBAL_DEVICE_INFO pGDI = pLDI->pGlobalInfo;

    SoundMixerChangedItem(&pGDI->MixerInfo, &pGDI->LineNotification[LineSynth]);
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
    if (ValueType == REG_BINARY && ValueLength == MixerControls * 2 * sizeof(USHORT)) {
        RtlCopyMemory(EntryContext, ValueData, ValueLength);
        *(PULONG)Context = ValueLength;
    }
    return STATUS_SUCCESS;
}

VOID
MixerInitialize(
    IN PGLOBAL_DEVICE_INFO pGDI
    )
/*++

Routine Description:

    This routine sets the mixer device up, after the devices are made: the
    controls (those saved, else full volume and no mute), their notifications
    and those of the lines, and the volumes of the wave output and the
    synthesizer, which are their controls.

--*/
{
    RTL_QUERY_REGISTRY_TABLE QueryTable[2];
    USHORT Saved[MixerControls][2];
    PLOCAL_DEVICE_INFO pLDI;
    ULONG Length, Index;

    SoundInitMixerInfo(&pGDI->MixerInfo, HwGetLineFlags, HwGetControl, HwGetCombinedControl, HwSetControl);

    pLDI = pGDI->DeviceObject[MixerDeviceIndex]->DeviceExtension;
    pLDI->DeviceType = MIXER_DEVICE;
    pLDI->DeviceSpecificData = &pGDI->MixerInfo;
    for (Index = 0; Index < NumberOfDevices; Index += 1) {
        ((PLOCAL_DEVICE_INFO)pGDI->DeviceObject[Index]->DeviceExtension)->MixerDevice = pLDI;
    }

    for (Index = 0; Index < MixerControls; Index += 1) {
        pGDI->MixerValue[Index][0] = IsMute(Index) ? 0 : 0xFFFF;
        pGDI->MixerValue[Index][1] = pGDI->MixerValue[Index][0];
        SoundInitDataItem(&pGDI->MixerInfo, &pGDI->ControlNotification[Index], (USHORT)MM_MIXM_CONTROL_CHANGE,
                          (USHORT)Index);
    }
    for (Index = 0; Index < MixerLines; Index += 1) {
        SoundInitDataItem(&pGDI->MixerInfo, &pGDI->LineNotification[Index], (USHORT)MM_MIXM_LINE_CHANGE,
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
    if (NT_SUCCESS(RtlQueryRegistryValues(RTL_REGISTRY_ABSOLUTE, pGDI->DevicePathName, QueryTable, &Length, NULL)) &&
        Length == sizeof(Saved)) {
        for (Index = 0; Index < MixerControls; Index += 1) {
            if (IsMute(Index)) {
                Saved[Index][0] = Saved[Index][1] = Saved[Index][0] != 0;
            }
            pGDI->MixerValue[Index][0] = Saved[Index][0];
            pGDI->MixerValue[Index][1] = Saved[Index][1];
        }
        BcmSoundLog(pGDI, "Mixer settings: volume %04X/%04X%s, wave %04X/%04X%s, synthesizer %04X/%04X%s",
                    Saved[0][0], Saved[0][1], Saved[1][0] ? " muted" : "",
                    Saved[2][0], Saved[2][1], Saved[3][0] ? " muted" : "",
                    Saved[4][0], Saved[4][1], Saved[5][0] ? " muted" : "");
    }
    MixerUpdateGains(pGDI);

    SoundSetLineNotify(pGDI->DeviceObject[WaveOutDevice]->DeviceExtension, MixerWaveLineChanged);
    SoundSetLineNotify(pGDI->DeviceObject[SynthDeviceIndex]->DeviceExtension, MixerSynthLineChanged);
    SoundSetVolumeControlId(pGDI->DeviceObject[WaveOutDevice]->DeviceExtension, ControlWaveVolume);
    SoundSetVolumeControlId(pGDI->DeviceObject[SynthDeviceIndex]->DeviceExtension, ControlSynthVolume);
}

VOID
MixerShutdown(
    IN PGLOBAL_DEVICE_INFO pGDI
    )
{
    MixerSaveSettings(pGDI);
}

NTSTATUS
MixerDumpConfiguration(
    IN PLOCAL_DEVICE_INFO pLDI,
    IN OUT PIRP Irp,
    IN PIO_STACK_LOCATION IrpStack
    )
/*++

Routine Description:

    This routine returns the mixer's configuration (IOCTL_MIX_GET_CONFIGURATION):
    its capabilities, lines and controls, which drvlib names from the strings
    of the multimedia driver.

--*/
{
    PGLOBAL_DEVICE_INFO pGDI = pLDI->pGlobalInfo;
    struct {
        MIXER_DD_CONFIGURATION_DATA Header;
        MIXER_DD_LINE_CONFIGURATION_DATA LineData[MixerLines];
        MIXER_DD_CONTROL_CONFIGURATION_DATA ControlData[MixerControls];
    } *Data;
    ULONG Length;

    Data = ExAllocatePool(PagedPool, sizeof(*Data));
    if (Data == NULL) {
        return STATUS_INSUFFICIENT_RESOURCES;
    }
    RtlZeroMemory(Data, sizeof(*Data));
    Data->Header.cbSize = sizeof(*Data);
    Data->Header.NumberOfLines = MixerLines;
    Data->Header.NumberOfControls = MixerControls;
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

    Data->LineData[LineWaveOut].PnameStringId = (SHORT)WaveOutNameId(pGDI);

    Length = min(sizeof(*Data), IrpStack->Parameters.DeviceIoControl.OutputBufferLength);
    RtlCopyMemory(Irp->AssociatedIrp.SystemBuffer, Data, Length);
    Irp->IoStatus.Information = Length;
    ExFreePool(Data);

    //
    // (as the sound drivers of NT answer it: drvlib asks for the size first,
    // with a buffer of four bytes, and the DeviceIoControl of NT 3.5 returns
    // no count of bytes with a warning status)
    //

    return STATUS_SUCCESS;
}

//
// soundlib's callouts (at PASSIVE_LEVEL, holding the driver's mutex).
//

static NTSTATUS
HwGetLineFlags(
    IN PMIXER_INFO MixerInfo,
    IN ULONG LineId,
    IN ULONG Length,
    OUT PVOID Data
    )
{
    PGLOBAL_DEVICE_INFO pGDI = CONTAINING_RECORD(MixerInfo, GLOBAL_DEVICE_INFO, MixerInfo);
    PULONG Flags = Data;

    if (Length != sizeof(ULONG)) {
        return STATUS_BUFFER_TOO_SMALL;
    }
    switch (LineId) {
    case LineSpeakers:
        *Flags = MIXERLINE_LINEF_ACTIVE;
        break;
    case LineWaveOut:
        *Flags = MIXERLINE_LINEF_SOURCE | (pGDI->DeviceInUse ? MIXERLINE_LINEF_ACTIVE : 0);
        break;
    case LineSynth:
        *Flags = MIXERLINE_LINEF_SOURCE | (pGDI->SynthOpen ? MIXERLINE_LINEF_ACTIVE : 0);
        break;
    default:
        return STATUS_INVALID_PARAMETER;
    }
    return STATUS_SUCCESS;
}

static NTSTATUS
HwGetControl(
    IN PMIXER_INFO MixerInfo,
    IN ULONG ControlId,
    IN ULONG Length,
    OUT PVOID Data
    )
{
    PGLOBAL_DEVICE_INFO pGDI = CONTAINING_RECORD(MixerInfo, GLOBAL_DEVICE_INFO, MixerInfo);
    PULONG Values = Data;

    if (ControlId >= MixerControls) {
        return STATUS_INVALID_PARAMETER;
    }
    if (Length == sizeof(ULONG)) {
        Values[0] = max(pGDI->MixerValue[ControlId][0], pGDI->MixerValue[ControlId][1]);
    } else if (Length == 2 * sizeof(ULONG)) {
        Values[0] = pGDI->MixerValue[ControlId][0];
        Values[1] = pGDI->MixerValue[ControlId][1];
    } else {
        return STATUS_BUFFER_TOO_SMALL;
    }
    return STATUS_SUCCESS;
}

static NTSTATUS
HwGetCombinedControl(
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
HwSetControl(
    IN PMIXER_INFO MixerInfo,
    IN ULONG ControlId,
    IN ULONG Length,
    IN PVOID Data
    )
{
    PGLOBAL_DEVICE_INFO pGDI = CONTAINING_RECORD(MixerInfo, GLOBAL_DEVICE_INFO, MixerInfo);
    PULONG Values = Data;
    ULONG Value[2], Channel;
    BOOLEAN Changed = FALSE;

    if (ControlId >= MixerControls) {
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
    if (IsMute(ControlId)) {
        Value[0] = Value[1] = (Value[0] != 0 || Value[1] != 0);
    }
    for (Channel = 0; Channel < 2; Channel += 1) {
        if (Value[Channel] > 0xFFFF) {
            return STATUS_INVALID_PARAMETER;
        }
    }
    for (Channel = 0; Channel < 2; Channel += 1) {
        if (pGDI->MixerValue[ControlId][Channel] != (USHORT)Value[Channel]) {
            pGDI->MixerValue[ControlId][Channel] = (USHORT)Value[Channel];
            Changed = TRUE;
        }
    }
    if (Changed) {
        MixerUpdateGains(pGDI);
        SoundMixerChangedItem(MixerInfo, &pGDI->ControlNotification[ControlId]);
        MixerSaveSettings(pGDI);
    }
    return STATUS_SUCCESS;
}
