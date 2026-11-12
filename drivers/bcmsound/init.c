/*++

Module Name:

    init.c

Abstract:

    The sound driver of the Raspberry Pi (bcmsound.sys): its start, the wave
    output device it makes on the sound library, the interrupt of its DMA
    channel and the configuration (C:\CONFIG.TXT, the config.txt of the boot
    firmware, which passes over keys it does not know; the registry), with a
    log of what it found (C:\SOUND.LOG) when asked:

        sound_output=AUTO       (AUTO, HDMI, JACK or NONE)
        debug_sound=1           (log to C:\SOUND.LOG)

    AUTO is HDMI when the display takes sound, else the jack of a board that
    has one, else nothing (the sounds keep their time, silent).

Environment:

    kernel mode only

--*/

#include "bcmsound.h"
#include <stdio.h>

static SOUND_EXCLUDE_ROUTINE BcmSoundExclude;
static SOUND_DISPATCH_ROUTINE BcmSoundWaveOutGetCaps;
static SOUND_QUERY_FORMAT_ROUTINE BcmSoundQueryFormat;

static VOID
BcmSoundDeferred(
    IN PKDPC Dpc,
    IN PDEVICE_OBJECT DeviceObject,
    IN PIRP Irp,
    IN PVOID Context
    );

//
// The devices, as the WSS makes them (the names get their number, 0; the
// multimedia driver finds them in Parameters\Device0\Devices).
//

static const SOUND_DEVICE_INIT DeviceInit[NUMBER_OF_DEVICES] = {
    {
        NULL, NULL,
        0,
        FILE_DEVICE_WAVE_OUT,
        WAVE_OUT,
        "LDWo",
        L"\\Device\\BcmWaveOut",
        BcmSoundDeferred,
        BcmSoundExclude,
        SoundWaveDispatch,
        BcmSoundWaveOutGetCaps,
        SoundNoVolume,
        DO_DIRECT_IO
    },
    {
        NULL, NULL,
        0,
        FILE_DEVICE_SOUND,
        MIXER_DEVICE,
        "LDMx",
        L"\\Device\\BcmMixer",
        NULL,
        BcmSoundExclude,
        SoundMixerDispatch,
        MixerDumpConfiguration,
        SoundNoVolume,
        DO_BUFFERED_IO
    },
    {
        NULL, NULL,
        0,
        FILE_DEVICE_MIDI_OUT,
        SYNTH_DEVICE,
        "LDMo",
        L"\\Device\\BcmSynth",
        NULL,
        BcmSoundExclude,
        SynthDispatch,
        SynthGetCaps,
        SoundNoVolume,
        0
    }
};

static PGLOBAL_DEVICE_INFO BcmSoundGlobal;

NTSTATUS
DriverEntry(
    IN PDRIVER_OBJECT DriverObject,
    IN PUNICODE_STRING RegistryPath
    );

//
// The files of the boot partition (CONFIG.TXT, SOUND.LOG).
//

static BOOLEAN
BcmSoundRootFile(
    OUT PUNICODE_STRING Name,
    OUT PWCHAR Buffer,
    IN ULONG BufferLength,
    IN PWSTR File
    )

/*++

Routine Description:

    This routine names a file in the root directory of the system partition,
    which is the card's FAT partition: the target of \SystemRoot (an ARC name
    while the system starts, the partition's device after it) without its
    last component, and the file. The driver starts with the system, before
    the drive letters are assigned, so C: does not name the partition yet.

Arguments:

    Name - Receives the name of the file.

    Buffer - Supplies the buffer of the name.

    BufferLength - Supplies the size of the buffer, in bytes.

    File - Supplies the file in the root directory.

Return Value:

    TRUE when the file is named.

--*/

{
    UNICODE_STRING LinkName;
    OBJECT_ATTRIBUTES Attributes;
    HANDLE Link;
    NTSTATUS Status;
    USHORT Length;

    RtlInitUnicodeString(&LinkName, L"\\SystemRoot");
    InitializeObjectAttributes(&Attributes, &LinkName, OBJ_CASE_INSENSITIVE, NULL, NULL);
    if (!NT_SUCCESS(ZwOpenSymbolicLinkObject(&Link, SYMBOLIC_LINK_QUERY, &Attributes))) {
        return FALSE;
    }

    Name->Buffer = Buffer;
    Name->Length = 0;
    Name->MaximumLength = (USHORT)BufferLength;
    Status = ZwQuerySymbolicLinkObject(Link, Name, NULL);
    ZwClose(Link);
    if (!NT_SUCCESS(Status)) {
        return FALSE;
    }

    Length = Name->Length / sizeof(WCHAR);
    while (Length != 0 && Buffer[Length - 1] != L'\\') {
        Length -= 1;
    }

    Name->Length = Length * sizeof(WCHAR);
    return (BOOLEAN)(Length != 0 && NT_SUCCESS(RtlAppendUnicodeToString(Name, File)));
}

//
// The log.
//

static VOID
BcmSoundOpenLog(
    IN PGLOBAL_DEVICE_INFO GlobalInfo
    )
{
    UNICODE_STRING Name;
    WCHAR NameBuffer[160];
    OBJECT_ATTRIBUTES Attributes;
    IO_STATUS_BLOCK Iosb;

    if (!BcmSoundRootFile(&Name, NameBuffer, sizeof(NameBuffer), L"SOUND.LOG")) {
        GlobalInfo->LogFile = NULL;
        return;
    }

    InitializeObjectAttributes(&Attributes, &Name, OBJ_CASE_INSENSITIVE, NULL, NULL);
    if (!NT_SUCCESS(ZwCreateFile(&GlobalInfo->LogFile, FILE_APPEND_DATA | SYNCHRONIZE, &Attributes, &Iosb, NULL,
                                 FILE_ATTRIBUTE_NORMAL, FILE_SHARE_READ, FILE_OVERWRITE_IF,
                                 FILE_SYNCHRONOUS_IO_NONALERT | FILE_NON_DIRECTORY_FILE, NULL, 0))) {
        GlobalInfo->LogFile = NULL;
    }
}

VOID
BcmSoundLog(
    IN PGLOBAL_DEVICE_INFO GlobalInfo,
    IN PCHAR Format,
    ...
    )

/*++

Routine Description:

    This routine adds a line to C:\SOUND.LOG (debug_sound=1), at PASSIVE_LEVEL
    only; lines at a higher level are dropped. During the start the file is
    held open; after it (the opens and closes of the devices, in the callers'
    processes, which a handle of the start's would not reach), each line opens
    it to append and closes it again.

Arguments:

    GlobalInfo - Supplies the global device information.

    Format - Supplies the format of the line, as printf takes it, and its
        arguments after it.

Return Value:

    None.

--*/

{
    CHAR Line[256];
    IO_STATUS_BLOCK Iosb;
    va_list Arguments;
    ULONG Length;
    BOOLEAN Reopened = FALSE;
    UNICODE_STRING Name;
    WCHAR NameBuffer[160];
    OBJECT_ATTRIBUTES Attributes;

    if (!GlobalInfo->Debug || KeGetCurrentIrql() != PASSIVE_LEVEL || GlobalInfo->LogLines >= 2000) {
        return;
    }

    if (GlobalInfo->LogFile == NULL) {
        if (!BcmSoundRootFile(&Name, NameBuffer, sizeof(NameBuffer), L"SOUND.LOG")) {
            return;
        }

        InitializeObjectAttributes(&Attributes, &Name, OBJ_CASE_INSENSITIVE, NULL, NULL);
        if (!NT_SUCCESS(ZwCreateFile(&GlobalInfo->LogFile, FILE_APPEND_DATA | SYNCHRONIZE, &Attributes, &Iosb, NULL,
                                     FILE_ATTRIBUTE_NORMAL, FILE_SHARE_READ | FILE_SHARE_WRITE, FILE_OPEN_IF,
                                     FILE_SYNCHRONOUS_IO_NONALERT | FILE_NON_DIRECTORY_FILE, NULL, 0))) {
            GlobalInfo->LogFile = NULL;
            return;
        }

        Reopened = TRUE;
    }

    va_start(Arguments, Format);
    Length = _vsnprintf(Line, sizeof(Line) - 2, Format, Arguments);
    va_end(Arguments);
    if (Length > sizeof(Line) - 2) {
        Length = sizeof(Line) - 2;
    }

    Line[Length++] = '\r';
    Line[Length++] = '\n';
    GlobalInfo->LogLines += 1;
    ZwWriteFile(GlobalInfo->LogFile, NULL, NULL, NULL, &Iosb, Line, Length, NULL, NULL);
    if (Reopened) {
        ZwClose(GlobalInfo->LogFile);
        GlobalInfo->LogFile = NULL;
    }
}

//
// The hardware the firmware described: the audio controller BCMSOUND_ID of
// the internal bus.
//

typedef struct _SOUND_HARDWARE {
    BOOLEAN Ours;
    BOOLEAN Found;
    ULONG InterruptVector;
    ULONG InterruptLevel;
    CM_BCMSOUND_DEVICE_DATA Data;
} SOUND_HARDWARE, *PSOUND_HARDWARE;

static NTSTATUS
BcmSoundSaveIdentifier(
    IN PWSTR ValueName,
    IN ULONG ValueType,
    IN PVOID ValueData,
    IN ULONG ValueLength,
    IN PVOID Context,
    IN PVOID EntryContext
    )
{
    PSOUND_HARDWARE Hardware = Context;
    UNICODE_STRING Identifier;
    UNICODE_STRING Ours;

    if (ValueType == REG_SZ) {
        RtlInitUnicodeString(&Identifier, ValueData);
        RtlInitUnicodeString(&Ours, L"BCM2835-AUDIO");
        Hardware->Ours = RtlEqualUnicodeString(&Identifier, &Ours, TRUE);
    }

    return STATUS_SUCCESS;
}

static NTSTATUS
BcmSoundSaveHardware(
    IN PWSTR ValueName,
    IN ULONG ValueType,
    IN PVOID ValueData,
    IN ULONG ValueLength,
    IN PVOID Context,
    IN PVOID EntryContext
    )
{
    PSOUND_HARDWARE Hardware = Context;
    PCM_PARTIAL_RESOURCE_LIST List;
    PCM_PARTIAL_RESOURCE_DESCRIPTOR Partial;
    ULONG Index;

    if ((ValueType != REG_BINARY && ValueType != REG_FULL_RESOURCE_DESCRIPTOR) ||
        ValueLength < sizeof(CM_FULL_RESOURCE_DESCRIPTOR)) {
        return STATUS_SUCCESS;
    }

    List = &((PCM_FULL_RESOURCE_DESCRIPTOR)ValueData)->PartialResourceList;
    for (Index = 0; Index < List->Count; Index += 1) {
        Partial = &List->PartialDescriptors[Index];
        if (Partial->Type == CmResourceTypeInterrupt) {
            Hardware->InterruptVector = Partial->u.Interrupt.Vector;
            Hardware->InterruptLevel = Partial->u.Interrupt.Level;
        } else if (Partial->Type == CmResourceTypeDeviceSpecific && Index == List->Count - 1 &&
                   Partial->u.DeviceSpecificData.DataSize >= sizeof(CM_BCMSOUND_DEVICE_DATA)) {
            RtlCopyMemory(&Hardware->Data, Partial + 1, sizeof(CM_BCMSOUND_DEVICE_DATA));
            Hardware->Found = TRUE;
        }
    }

    return STATUS_SUCCESS;
}

static BOOLEAN
BcmSoundReadHardware(
    OUT PSOUND_HARDWARE Hardware
    )
{
    WCHAR Path[] = L"\\Registry\\Machine\\Hardware\\Description\\System\\MultifunctionAdapter\\0\\AudioController\\#";
    RTL_QUERY_REGISTRY_TABLE QueryTable[3];
    PWCHAR Mark = wcschr(Path, L'#');
    ULONG Controller;

    for (Controller = 0; Controller < 4; Controller += 1) {
        *Mark = (WCHAR)(L'0' + Controller);
        RtlZeroMemory(QueryTable, sizeof(QueryTable));
        RtlZeroMemory(Hardware, sizeof(SOUND_HARDWARE));
        QueryTable[0].QueryRoutine = BcmSoundSaveIdentifier;
        QueryTable[0].Name = L"Identifier";
        QueryTable[0].DefaultType = REG_NONE;
        QueryTable[1].QueryRoutine = BcmSoundSaveHardware;
        QueryTable[1].Flags = RTL_QUERY_REGISTRY_REQUIRED;
        QueryTable[1].Name = L"Configuration Data";
        QueryTable[1].DefaultType = REG_NONE;
        if (!NT_SUCCESS(RtlQueryRegistryValues(RTL_REGISTRY_ABSOLUTE, Path, QueryTable, Hardware, NULL))) {
            break;
        }

        if (Hardware->Ours && Hardware->Found && Hardware->InterruptVector != 0) {
            return TRUE;
        }
    }

    return FALSE;
}

//
// The configuration: sound_output and debug_sound of C:\CONFIG.TXT (KEY=VALUE
// lines; # starts a comment, [ a section of the boot firmware's, which the
// driver reads all of); the value Output (REG_SZ) of the service's
// Parameters takes precedence.
//

static ULONG
BcmSoundParseOutput(
    IN PCHAR Value,
    IN ULONG Length
    )
{
    CHAR Text[16];

    if (Length >= sizeof(Text)) {
        return SETTING_AUTO;
    }

    RtlCopyMemory(Text, Value, Length);
    Text[Length] = 0;
    if (_stricmp(Text, "HDMI") == 0) {
        return OUTPUT_HDMI;
    }

    if (_stricmp(Text, "JACK") == 0 || _stricmp(Text, "HEADPHONES") == 0 || _stricmp(Text, "ANALOG") == 0) {
        return OUTPUT_JACK;
    }

    if (_stricmp(Text, "NONE") == 0 || _stricmp(Text, "OFF") == 0) {
        return OUTPUT_NONE;
    }

    return SETTING_AUTO;
}

static BOOLEAN
BcmSoundParseBoolean(
    IN PCHAR Value,
    IN ULONG Length
    )
{
    return Length != 0 && (Value[0] == '1' || Value[0] == 'y' || Value[0] == 'Y' || Value[0] == 't' ||
                           Value[0] == 'T' || (Length >= 2 && (Value[1] == 'n' || Value[1] == 'N')));
}

static VOID
BcmSoundReadSettingsFile(
    IN PGLOBAL_DEVICE_INFO GlobalInfo
    )
{
    UNICODE_STRING Name;
    WCHAR NameBuffer[160];
    OBJECT_ATTRIBUTES Attributes;
    IO_STATUS_BLOCK Iosb;
    FILE_STANDARD_INFORMATION Information;
    HANDLE File;
    PCHAR Text = NULL;
    PCHAR Line;
    PCHAR End;
    PCHAR Last;
    PCHAR Equal;
    PCHAR Value;
    ULONG Length = 0;

    if (!BcmSoundRootFile(&Name, NameBuffer, sizeof(NameBuffer), L"CONFIG.TXT")) {
        return;
    }

    InitializeObjectAttributes(&Attributes, &Name, OBJ_CASE_INSENSITIVE, NULL, NULL);
    if (!NT_SUCCESS(ZwCreateFile(&File, GENERIC_READ | SYNCHRONIZE, &Attributes, &Iosb, NULL, FILE_ATTRIBUTE_NORMAL,
                                 FILE_SHARE_READ, FILE_OPEN, FILE_SYNCHRONOUS_IO_NONALERT | FILE_NON_DIRECTORY_FILE,
                                 NULL, 0))) {
        return;
    }

    if (NT_SUCCESS(ZwQueryInformationFile(File, &Iosb, &Information, sizeof(Information), FileStandardInformation)) &&
        Information.EndOfFile.HighPart == 0 && Information.EndOfFile.LowPart < 0x100000) {
        Text = ExAllocatePool(PagedPool, Information.EndOfFile.LowPart + 1);
        if (Text != NULL &&
            NT_SUCCESS(ZwReadFile(File, NULL, NULL, NULL, &Iosb, Text, Information.EndOfFile.LowPart, NULL, NULL))) {
            Length = (ULONG)Iosb.Information;
        }
    }

    ZwClose(File);
    if (Text == NULL) {
        return;
    }

    Text[Length] = 0;
    Line = Text;
    if (Length >= 3 && (UCHAR)Text[0] == 0xEF && (UCHAR)Text[1] == 0xBB && (UCHAR)Text[2] == 0xBF) {
        Line += 3;
    }

    for (; *Line != 0; Line = End) {
        for (End = Line; *End != 0 && *End != '\n'; End += 1) {
        }

        Last = End;
        if (*End == '\n') {
            End += 1;
        }

        while (Last > Line && (Last[-1] == '\r' || Last[-1] == ' ' || Last[-1] == '\t')) {
            Last -= 1;
        }

        while (Line < Last && (*Line == ' ' || *Line == '\t')) {
            Line += 1;
        }

        if (Line == Last || *Line == '#' || *Line == ';' || *Line == '[') {
            continue;
        }

        for (Equal = Line; Equal < Last && *Equal != '='; Equal += 1) {
        }

        if (Equal == Last) {
            continue;
        }

        Value = Equal + 1;
        while (Equal > Line && (Equal[-1] == ' ' || Equal[-1] == '\t')) {
            Equal -= 1;
        }

        while (Value < Last && (*Value == ' ' || *Value == '\t')) {
            Value += 1;
        }

        if (Equal - Line == 12 && _strnicmp(Line, "SOUND_OUTPUT", 12) == 0) {
            GlobalInfo->Setting = BcmSoundParseOutput(Value, Last - Value);
        } else if (Equal - Line == 11 && _strnicmp(Line, "DEBUG_SOUND", 11) == 0) {
            GlobalInfo->Debug = BcmSoundParseBoolean(Value, Last - Value);
        }
    }

    ExFreePool(Text);
}

static VOID
BcmSoundReadSettings(
    IN PGLOBAL_DEVICE_INFO GlobalInfo
    )
{
    RTL_QUERY_REGISTRY_TABLE QueryTable[2];
    UNICODE_STRING Value;
    ANSI_STRING Text;
    WCHAR Path[256];

    GlobalInfo->Setting = SETTING_AUTO;
    BcmSoundReadSettingsFile(GlobalInfo);

    //
    // The Parameters of the service (soundlib's registry path).
    //

    if (wcslen(GlobalInfo->RegistryPathName) >= sizeof(Path) / sizeof(WCHAR)) {
        return;
    }

    wcscpy(Path, GlobalInfo->RegistryPathName);
    RtlZeroMemory(QueryTable, sizeof(QueryTable));
    RtlZeroMemory(&Value, sizeof(Value));
    QueryTable[0].Flags = RTL_QUERY_REGISTRY_DIRECT;
    QueryTable[0].Name = L"Output";
    QueryTable[0].EntryContext = &Value;
    QueryTable[0].DefaultType = REG_NONE;
    if (NT_SUCCESS(RtlQueryRegistryValues(RTL_REGISTRY_ABSOLUTE, Path, QueryTable, NULL, NULL)) &&
        Value.Buffer != NULL) {
        if (NT_SUCCESS(RtlUnicodeStringToAnsiString(&Text, &Value, TRUE))) {
            GlobalInfo->Setting = BcmSoundParseOutput(Text.Buffer, Text.Length);
            RtlFreeAnsiString(&Text);
        }

        ExFreePool(Value.Buffer);
    }
}

//
// Soundlib's callouts: the exclusion of the device, its capabilities, the
// formats it takes and its volume.
//

static BOOLEAN
BcmSoundExclude(
    IN PLOCAL_DEVICE_INFO LocalInfo,
    IN SOUND_EXCLUDE_CODE Code
    )
{
    PGLOBAL_DEVICE_INFO GlobalInfo = LocalInfo->pGlobalInfo;

    //
    // One mutex for all the devices (the mixer is entered from the others);
    // one writer at a time of the wave output and of the synthesizer.
    //

    switch (Code) {
    case SoundExcludeOpen:
        if (LocalInfo->DeviceIndex == WAVE_OUT_DEVICE) {
            if (GlobalInfo->DeviceInUse) {
                return FALSE;
            }

            GlobalInfo->DeviceInUse = TRUE;
            OutputLogState(GlobalInfo, "Wave output opened");
        } else if (LocalInfo->DeviceIndex == SYNTH_DEVICE_INDEX) {
            if (GlobalInfo->SynthOpen) {
                return FALSE;
            }
        }

        return TRUE;

    case SoundExcludeClose:
        if (LocalInfo->DeviceIndex == WAVE_OUT_DEVICE) {
            GlobalInfo->DeviceInUse = FALSE;
            OutputLogState(GlobalInfo, "Wave output closed");
        }

        return TRUE;

    case SoundExcludeEnter:
        KeWaitForSingleObject(&GlobalInfo->WaveMutex, Executive, KernelMode, FALSE, NULL);
        return TRUE;

    case SoundExcludeLeave:
        KeReleaseMutex(&GlobalInfo->WaveMutex, FALSE);
        return TRUE;

    case SoundExcludeQueryOpen:
        return LocalInfo->DeviceIndex == WAVE_OUT_DEVICE ? GlobalInfo->DeviceInUse :
               LocalInfo->DeviceIndex == SYNTH_DEVICE_INDEX ? GlobalInfo->SynthOpen : FALSE;
    }

    return FALSE;
}

ULONG
WaveOutNameId(
    IN PGLOBAL_DEVICE_INFO GlobalInfo
    )
{
    return GlobalInfo->Output == OUTPUT_HDMI ? IDS_WAVEOUT_HDMI :
           GlobalInfo->Output == OUTPUT_JACK ? IDS_WAVEOUT_JACK : IDS_WAVEOUT_SILENT;
}

static NTSTATUS
BcmSoundWaveOutGetCaps(
    IN PLOCAL_DEVICE_INFO LocalInfo,
    IN OUT PIRP Irp,
    IN PIO_STACK_LOCATION IrpStack
    )
{
    PGLOBAL_DEVICE_INFO GlobalInfo = LocalInfo->pGlobalInfo;
    WAVEOUTCAPSW Caps;

    //
    // The name is the number of a string of the multimedia driver, as the
    // WSS's (drvlib loads it), the same as the mixer's line of the wave
    // output names.
    //

    RtlZeroMemory(&Caps, sizeof(Caps));
    Caps.wMid = MM_MICROSOFT;
    Caps.wPid = BCMSOUND_PID_WAVEOUT;
    Caps.vDriverVersion = BCMSOUND_VERSION;
    Caps.dwFormats = WAVE_FORMAT_1M08 | WAVE_FORMAT_1S08 | WAVE_FORMAT_1M16 | WAVE_FORMAT_1S16 |
                     WAVE_FORMAT_2M08 | WAVE_FORMAT_2S08 | WAVE_FORMAT_2M16 | WAVE_FORMAT_2S16 |
                     WAVE_FORMAT_4M08 | WAVE_FORMAT_4S08 | WAVE_FORMAT_4M16 | WAVE_FORMAT_4S16;
    Caps.wChannels = 2;
    Caps.dwSupport = WAVECAPS_VOLUME | WAVECAPS_LRVOLUME;
    *(PULONG)Caps.szPname = WaveOutNameId(GlobalInfo);
    Irp->IoStatus.Information = min(sizeof(Caps), IrpStack->Parameters.DeviceIoControl.OutputBufferLength);
    RtlCopyMemory(Irp->AssociatedIrp.SystemBuffer, &Caps, Irp->IoStatus.Information);
    return STATUS_SUCCESS;
}

static NTSTATUS
BcmSoundQueryFormat(
    IN PLOCAL_DEVICE_INFO LocalInfo,
    IN PPCMWAVEFORMAT Format
    )
{
    ULONG Rate = Format->wf.nSamplesPerSec;

    if (Format->wf.wFormatTag != WAVE_FORMAT_PCM ||
        (Format->wf.nChannels != 1 && Format->wf.nChannels != 2) ||
        (Format->wBitsPerSample != 8 && Format->wBitsPerSample != 16)) {
        return STATUS_NOT_SUPPORTED;
    }

    switch (Rate) {
    case 8000:
    case 11025:
    case 12000:
    case 16000:
    case 22050:
    case 24000:
    case 32000:
    case 44100:
    case 48000:
        break;

    default:
        return STATUS_NOT_SUPPORTED;
    }

    if (Format->wf.nBlockAlign != Format->wf.nChannels * Format->wBitsPerSample / 8 ||
        Format->wf.nAvgBytesPerSec != Rate * Format->wf.nBlockAlign) {
        return STATUS_NOT_SUPPORTED;
    }

    return STATUS_SUCCESS;
}

//
// The interrupt of the DMA channel (the end of a half of the ring), or the
// timer of the silent output, and the deferred routine that follows soundlib's.
//

static BOOLEAN
BcmSoundIsr(
    IN PKINTERRUPT Interrupt,
    IN PVOID Context
    )
{
    return OutputInterrupt((PGLOBAL_DEVICE_INFO)Context);
}

static VOID
BcmSoundDeferred(
    IN PKDPC Dpc,
    IN PDEVICE_OBJECT DeviceObject,
    IN PIRP Irp,
    IN PVOID Context
    )

/*++

Routine Description:

    This routine runs soundlib's deferred routine of the wave output once for
    each half of its buffer the reading has passed (output.c); each run
    programs the next half (OutputStart, which goes on reading).

Arguments:

    Dpc - Supplies the deferred procedure call of the device.

    DeviceObject - Supplies the wave output device.

    Irp - Not used.

    Context - Not used.

Return Value:

    None.

--*/

{
    PLOCAL_DEVICE_INFO LocalInfo = DeviceObject->DeviceExtension;
    PGLOBAL_DEVICE_INFO GlobalInfo = LocalInfo->pGlobalInfo;

    while (OutputTakeCrossings(GlobalInfo) != 0) {
        GlobalInfo->InWaveDeferred = TRUE;
        SoundWaveDeferred(Dpc, DeviceObject, Irp, Context);
        GlobalInfo->InWaveDeferred = FALSE;
    }
}

//
// Parameters\Device0 of the service, made if missing.
//

static NTSTATUS
BcmSoundCreateDeviceKey(
    IN PGLOBAL_DEVICE_INFO GlobalInfo
    )
{
    UNICODE_STRING Name;
    OBJECT_ATTRIBUTES Attributes;
    HANDLE Key;
    NTSTATUS Status;
    ULONG Length = (wcslen(GlobalInfo->RegistryPathName) + 9) * sizeof(WCHAR);

    GlobalInfo->DevicePathName = ExAllocatePool(PagedPool, Length);
    if (GlobalInfo->DevicePathName == NULL) {
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    wcscpy(GlobalInfo->DevicePathName, GlobalInfo->RegistryPathName);
    wcscat(GlobalInfo->DevicePathName, L"\\Device0");
    RtlInitUnicodeString(&Name, GlobalInfo->DevicePathName);
    InitializeObjectAttributes(&Attributes, &Name, OBJ_CASE_INSENSITIVE, NULL, NULL);
    Status = ZwCreateKey(&Key, KEY_ALL_ACCESS, &Attributes, 0, NULL, REG_OPTION_NON_VOLATILE, NULL);
    if (NT_SUCCESS(Status)) {
        ZwClose(Key);
    }

    return Status;
}

//
// The shutdown: stop the output.
//

static NTSTATUS
BcmSoundShutdown(
    IN PDEVICE_OBJECT DeviceObject,
    IN PIRP Irp
    )
{
    PLOCAL_DEVICE_INFO LocalInfo = DeviceObject->DeviceExtension;

    MixerShutdown(LocalInfo->pGlobalInfo);
    OutputShutdown(LocalInfo->pGlobalInfo);
    Irp->IoStatus.Status = STATUS_SUCCESS;
    Irp->IoStatus.Information = 0;
    IoCompleteRequest(Irp, IO_NO_INCREMENT);
    return STATUS_SUCCESS;
}

NTSTATUS
DriverEntry(
    IN PDRIVER_OBJECT DriverObject,
    IN PUNICODE_STRING RegistryPath
    )

/*++

Routine Description:

    This routine finds the sound hardware the firmware described, chooses
    the output, and makes the wave output, mixer and synthesizer devices
    (\Device\BcmWaveOut, BcmMixer and BcmSynth), which the multimedia driver
    bcmsnd32.dll opens.

Arguments:

    DriverObject - Supplies the driver object.

    RegistryPath - Supplies the key of the service in the registry.

Return Value:

    STATUS_SUCCESS, or why the driver could not start.

--*/

{
    PGLOBAL_DEVICE_INFO GlobalInfo;
    SOUND_HARDWARE Hardware;
    DEVICE_DESCRIPTION Description;
    PLOCAL_DEVICE_INFO LocalInfo;
    NTSTATUS Status;
    ULONG Vector;
    ULONG Index;
    KIRQL Irql;
    KAFFINITY Affinity;

    GlobalInfo = ExAllocatePool(NonPagedPool, sizeof(GLOBAL_DEVICE_INFO));
    if (GlobalInfo == NULL) {
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    RtlZeroMemory(GlobalInfo, sizeof(GLOBAL_DEVICE_INFO));
    GlobalInfo->Key = BCMSOUND_KEY;
    GlobalInfo->DriverObject = DriverObject;
    Status = SoundSaveRegistryPath(RegistryPath, &GlobalInfo->RegistryPathName);
    if (!NT_SUCCESS(Status)) {
        goto Fail;
    }

    BcmSoundReadSettings(GlobalInfo);
    if (GlobalInfo->Debug) {
        BcmSoundOpenLog(GlobalInfo);
    }

    BcmSoundLog(GlobalInfo, "Raspberry Pi sound driver, built " __DATE__ " " __TIME__);
    BcmSoundLog(GlobalInfo, "sound_output=%s",
                GlobalInfo->Setting == OUTPUT_HDMI ? "HDMI" : GlobalInfo->Setting == OUTPUT_JACK ? "JACK" :
                GlobalInfo->Setting == OUTPUT_NONE ? "NONE" : "AUTO");

    if (!BcmSoundReadHardware(&Hardware)) {
        BcmSoundLog(GlobalInfo, "The firmware describes no sound hardware");
        Status = STATUS_NO_SUCH_DEVICE;
        goto Fail;
    }

    GlobalInfo->Hardware = Hardware.Data;
    GlobalInfo->InterruptVector = Hardware.InterruptVector;
    GlobalInfo->InterruptLevel = Hardware.InterruptLevel;
    BcmSoundLog(GlobalInfo, "Board %08X, peripherals at %08X, DMA channel %u (mask %04X), vector %u",
                GlobalInfo->Hardware.BoardRevision, GlobalInfo->Hardware.PeripheralBase,
                GlobalInfo->Hardware.DmaChannel, GlobalInfo->Hardware.DmaChannelMask, GlobalInfo->InterruptVector);

    KeInitializeMutex(&GlobalInfo->WaveMutex, 2);

    //
    // Parameters\Device0, the card's key (as the WSS's), where the devices
    // are listed for the multimedia driver and the mixer keeps its settings.
    //

    Status = BcmSoundCreateDeviceKey(GlobalInfo);
    if (!NT_SUCCESS(Status)) {
        BcmSoundLog(GlobalInfo, "No key Parameters\\Device0 (%08X)", Status);
        goto Fail;
    }

    //
    // The wave output, one of soundlib's reprogram-on-interrupt DMA devices
    // (output.c).
    //

    SoundInitializeWaveInfo(&GlobalInfo->WaveInfo, SoundReprogramOnInterruptDMA, BcmSoundQueryFormat, GlobalInfo);
    GlobalInfo->WaveInfo.HwSetupDMA = OutputStart;
    GlobalInfo->WaveInfo.HwStopDMA = OutputStop;
    GlobalInfo->WaveInfo.HwSetWaveFormat = OutputSetFormat;

    //
    // Soundlib's buffer, of a bus master of the internal bus (the HAL maps
    // it directly; it is the CPU that reads it).
    //

    RtlZeroMemory(&Description, sizeof(Description));
    Description.Version = DEVICE_DESCRIPTION_VERSION;
    Description.Master = TRUE;
    Description.Dma32BitAddresses = TRUE;
    Description.InterfaceType = Internal;
    Description.BusNumber = 0;
    Description.MaximumLength = ROUND_TO_PAGES(SOUND_BUFFER_SIZE);
    Status = SoundGetCommonBuffer(&Description, &GlobalInfo->WaveInfo.DMABuf);
    if (!NT_SUCCESS(Status)) {
        BcmSoundLog(GlobalInfo, "No sound buffer (%08X)", Status);
        goto Fail;
    }

    if (!OutputInitialize(GlobalInfo)) {
        Status = STATUS_INSUFFICIENT_RESOURCES;
        goto Fail;
    }

    //
    // The devices.
    //

    for (Index = 0; Index < NUMBER_OF_DEVICES; Index += 1) {
        Status = SoundCreateDevice(&DeviceInit[Index], 0, DriverObject, GlobalInfo,
                                   Index == WAVE_OUT_DEVICE ? &GlobalInfo->WaveInfo : NULL, GlobalInfo, Index,
                                   &GlobalInfo->DeviceObject[Index]);
        if (!NT_SUCCESS(Status)) {
            BcmSoundLog(GlobalInfo, "No device %u (%08X)", Index, Status);
            goto Fail;
        }

        LocalInfo = GlobalInfo->DeviceObject[Index]->DeviceExtension;
        Status = SoundSaveDeviceName(GlobalInfo->DevicePathName, LocalInfo);
        if (!NT_SUCCESS(Status)) {
            BcmSoundLog(GlobalInfo, "Cannot list device %u (%08X)", Index, Status);
            goto Fail;
        }
    }

    MixerInitialize(GlobalInfo);

    Vector = HalGetInterruptVector(Internal, 0, GlobalInfo->InterruptLevel, GlobalInfo->InterruptVector, &Irql,
                                   &Affinity);
    Status = IoConnectInterrupt(&GlobalInfo->WaveInfo.Interrupt, BcmSoundIsr, GlobalInfo, NULL, Vector, Irql, Irql,
                                LevelSensitive, FALSE, Affinity, FALSE);
    if (!NT_SUCCESS(Status)) {
        BcmSoundLog(GlobalInfo, "No interrupt %u (%08X)", GlobalInfo->InterruptVector, Status);
        goto Fail;
    }

    if (NT_SUCCESS(IoRegisterShutdownNotification(GlobalInfo->DeviceObject[WAVE_OUT_DEVICE]))) {
        GlobalInfo->ShutdownRegistered = TRUE;
    }

    DriverObject->MajorFunction[IRP_MJ_CREATE] = SoundDispatch;
    DriverObject->MajorFunction[IRP_MJ_CLOSE] = SoundDispatch;
    DriverObject->MajorFunction[IRP_MJ_DEVICE_CONTROL] = SoundDispatch;
    DriverObject->MajorFunction[IRP_MJ_READ] = SoundDispatch;
    DriverObject->MajorFunction[IRP_MJ_WRITE] = SoundDispatch;
    DriverObject->MajorFunction[IRP_MJ_CLEANUP] = SoundDispatch;
    DriverObject->MajorFunction[IRP_MJ_SHUTDOWN] = BcmSoundShutdown;

    BcmSoundGlobal = GlobalInfo;

    //
    // With debug_sound=1, a silent self-test and the state of the hardware.
    //

    if (GlobalInfo->Debug) {
        OutputSelfTest(GlobalInfo);
    }

    //
    // HDMI audio runs from the start (silence until a sound plays).
    //

    OutputEngineUpdate(GlobalInfo);

    BcmSoundLog(GlobalInfo, "Ready: wave output, mixer and FM synthesizer on %s",
                GlobalInfo->Output == OUTPUT_HDMI ? "HDMI" : GlobalInfo->Output == OUTPUT_JACK ? "the jack" :
                "no output (silent)");
    if (GlobalInfo->LogFile != NULL) {
        ZwClose(GlobalInfo->LogFile);
        GlobalInfo->LogFile = NULL;
    }

    return STATUS_SUCCESS;

Fail:

    //
    // A board without the hardware, or no resources: undo what was made, as
    // the system unloads a driver that fails its start.
    //

    if (GlobalInfo->LogFile != NULL) {
        ZwClose(GlobalInfo->LogFile);
        GlobalInfo->LogFile = NULL;
    }

    if (GlobalInfo->WaveInfo.Interrupt != NULL) {
        IoDisconnectInterrupt(GlobalInfo->WaveInfo.Interrupt);
    }

    for (Index = 0; Index < NUMBER_OF_DEVICES; Index += 1) {
        if (GlobalInfo->DeviceObject[Index] != NULL) {
            SoundFreeDevice(GlobalInfo->DeviceObject[Index]);
        }
    }

    if (GlobalInfo->Output == OUTPUT_JACK) {
        OutputShutdown(GlobalInfo);
    }

    if (GlobalInfo->WaveInfo.DMABuf.VirtualAddress != NULL) {
        SoundFreeCommonBuffer(&GlobalInfo->WaveInfo.DMABuf);
    }

    if (GlobalInfo->DevicePathName != NULL) {
        ExFreePool(GlobalInfo->DevicePathName);
    }

    if (GlobalInfo->RegistryPathName != NULL) {
        ExFreePool(GlobalInfo->RegistryPathName);
    }

    ExFreePool(GlobalInfo);
    return Status;
}
