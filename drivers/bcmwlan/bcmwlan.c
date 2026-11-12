/*++

Module Name:

    bcmwlan.c

Abstract:

    This is the NDIS 3.0 miniport driver of the on-board wireless chip of the
    Raspberry Pi 3, 3+, 4, 400 and Zero 2 W: a Broadcom BCM43430 or Cypress
    CYW43436 or CYW43455 FullMAC chip on SDIO, wired to the EMMC (the Arasan
    SD host controller) on GPIO 34-39, whose firmware runs 802.11, scanning,
    association and the WPA2/WPA3 supplicant and exchanges Ethernet frames
    with the host. The adapter is an 802.3 adapter to the protocols: NT 4.0
    and 3.5 know no 802.11 media.

    The firmware describes the chip as the network controller BCMWLAN_ID of
    the internal bus (bcmwlanhw.h), when an SDIO card answers on the EMMC.
    The chip's firmware, NVRAM and regulatory (CLM) files are read from
    \SystemRoot\System32\Drivers\BCMWLAN (chip.c). The network to join is in
    C:\CONFIG.TXT, the config.txt of the Raspberry Pi's boot firmware (which
    passes over keys it does not know), that can be edited on another
    computer; the adapter's Parameters (Ssid, Passphrase, Security, Country)
    take precedence:

        wifi_ssid=MyNetwork
        wifi_password=my passphrase
        wifi_security=WPA2      (OPEN, WPA2, WPA3 or WPA2/WPA3)
        wifi_country=US         (the regulatory domain, default XX)
        debug_wifi=1            (log to C:\WLAN.LOG)

    A thread owns the SDIO host: it brings the chip up, joins the network
    (and again when the link drops), sends the frames the send handler
    queued and reads what the chip has for the host: frames, events and the
    responses to its control requests. It wakes on the card interrupt (the
    interrupt service routine turns its signal off; the thread turns it on
    again when the chip is served) and every 10 ms. The frames it received
    and the changes of the link go to the timer, which indicates them in the
    miniport's context. With debug_wifi=1, each step and error goes to
    C:\WLAN.LOG.

Environment:

    kernel mode only

--*/

#include "bcmwlan.h"

NDIS_PHYSICAL_ADDRESS HighestAcceptableMax = NDIS_PHYSICAL_ADDRESS_CONST(-1, -1);

#define POLL_INTERVAL_MS            10
#define JOIN_WAIT_MS                20000
#define LOG_LINES                   4000

static const NDIS_OID WlanSupportedOids[] = {
    OID_GEN_SUPPORTED_LIST,
    OID_GEN_HARDWARE_STATUS,
    OID_GEN_MEDIA_SUPPORTED,
    OID_GEN_MEDIA_IN_USE,
    OID_GEN_MAXIMUM_LOOKAHEAD,
    OID_GEN_MAXIMUM_FRAME_SIZE,
    OID_GEN_MAXIMUM_TOTAL_SIZE,
    OID_GEN_MAC_OPTIONS,
    OID_GEN_PROTOCOL_OPTIONS,
    OID_GEN_LINK_SPEED,
    OID_GEN_TRANSMIT_BUFFER_SPACE,
    OID_GEN_RECEIVE_BUFFER_SPACE,
    OID_GEN_TRANSMIT_BLOCK_SIZE,
    OID_GEN_RECEIVE_BLOCK_SIZE,
    OID_GEN_VENDOR_ID,
    OID_GEN_VENDOR_DESCRIPTION,
    OID_GEN_DRIVER_VERSION,
    OID_GEN_CURRENT_PACKET_FILTER,
    OID_GEN_CURRENT_LOOKAHEAD,
#ifdef OID_GEN_MEDIA_CONNECT_STATUS
    OID_GEN_MEDIA_CONNECT_STATUS,
#endif
    OID_GEN_XMIT_OK,
    OID_GEN_RCV_OK,
    OID_GEN_XMIT_ERROR,
    OID_GEN_RCV_ERROR,
    OID_GEN_RCV_NO_BUFFER,
    OID_GEN_RCV_CRC_ERROR,
    OID_802_3_PERMANENT_ADDRESS,
    OID_802_3_CURRENT_ADDRESS,
    OID_802_3_MULTICAST_LIST,
    OID_802_3_MAXIMUM_LIST_SIZE,
    OID_802_3_RCV_ERROR_ALIGNMENT,
    OID_802_3_XMIT_ONE_COLLISION,
    OID_802_3_XMIT_MORE_COLLISIONS
};

static const UCHAR WlanDescription[] = "Broadcom/Cypress FullMAC Wireless Adapter";

NTSTATUS
DriverEntry(
    IN PDRIVER_OBJECT DriverObject,
    IN PUNICODE_STRING RegistryPath
    );

//
// The log.
//

static VOID
WlanOpenLog(
    IN PWLAN_ADAPTER Adapter
    )
{
    UNICODE_STRING Name;
    OBJECT_ATTRIBUTES Attributes;
    IO_STATUS_BLOCK Iosb;

    RtlInitUnicodeString(&Name, L"\\DosDevices\\C:\\WLAN.LOG");
    InitializeObjectAttributes(&Attributes, &Name, OBJ_CASE_INSENSITIVE, NULL, NULL);
    if (!NT_SUCCESS(ZwCreateFile(&Adapter->LogFile, FILE_APPEND_DATA | SYNCHRONIZE, &Attributes, &Iosb,
                                 NULL, FILE_ATTRIBUTE_NORMAL, FILE_SHARE_READ, FILE_OVERWRITE_IF,
                                 FILE_SYNCHRONOUS_IO_NONALERT | FILE_NON_DIRECTORY_FILE, NULL, 0))) {
        Adapter->LogFile = NULL;
    }
}

VOID
WlanLog(
    IN PWLAN_ADAPTER Adapter,
    IN PCHAR Format,
    ...
    )

/*++

Routine Description:

    This routine writes a line to the log file, the time of day in front of
    it. Nothing is written without the file, above PASSIVE_LEVEL, or once the
    log has LOG_LINES lines.

Arguments:

    Adapter - Supplies the adapter, or NULL (nothing is written).

    Format - Supplies the format of the line, as of printf, and its values
        after it.

Return Value:

    None.

--*/

{
    CHAR Line[512];
    LARGE_INTEGER Time;
    TIME_FIELDS Fields;
    IO_STATUS_BLOCK Iosb;
    va_list Arguments;
    LONG Length;
    LONG Written;

    KeQuerySystemTime(&Time);
    RtlTimeToTimeFields(&Time, &Fields);
    Length = sprintf(Line, "%02u:%02u:%02u.%03u ", Fields.Hour, Fields.Minute, Fields.Second,
                     Fields.Milliseconds);
    va_start(Arguments, Format);
    Written = _vsnprintf(Line + Length, sizeof(Line) - Length - 3, Format, Arguments);
    va_end(Arguments);
    Length = Written < 0 ? sizeof(Line) - 3 : Length + Written;
    if (Length > (LONG)sizeof(Line) - 3) {
        Length = sizeof(Line) - 3;
    }

    Line[Length++] = '\r';
    Line[Length++] = '\n';
    Line[Length] = 0;
    if (Adapter == NULL || Adapter->LogFile == NULL || KeGetCurrentIrql() != PASSIVE_LEVEL ||
        Adapter->LogLines >= LOG_LINES) {
        return;
    }

    Adapter->LogLines += 1;
    ZwWriteFile(Adapter->LogFile, NULL, NULL, NULL, &Iosb, Line, Length, NULL, NULL);
}

VOID
WlanLogBytes(
    IN PWLAN_ADAPTER Adapter,
    IN PCHAR Title,
    IN PUCHAR Data,
    IN ULONG Length
    )

/*++

Routine Description:

    This routine writes bytes to the log in hexadecimal, 32 to a line.

Arguments:

    Adapter - Supplies the adapter.

    Title - Supplies the name of the bytes, which starts each line.

    Data - Supplies the bytes.

    Length - Supplies their number.

Return Value:

    None.

--*/

{
    CHAR Hex[3 * 32 + 1];
    ULONG Offset;
    ULONG Index;

    for (Offset = 0; Offset < Length; Offset += 32) {
        for (Index = 0; Index < 32 && Offset + Index < Length; Index += 1) {
            sprintf(Hex + 3 * Index, "%02x ", Data[Offset + Index]);
        }

        WlanLog(Adapter, "%s +%03lx: %s", Title, Offset, Hex);
    }
}

PUCHAR
WlanReadFile(
    IN PWLAN_ADAPTER Adapter,
    IN PWCHAR Path,
    OUT PULONG Length
    )

/*++

Routine Description:

    This routine reads a whole file into nonpaged pool, which the caller
    frees.

Arguments:

    Adapter - Supplies the adapter.

    Path - Supplies the path of the file.

    Length - Receives the length of the file, in bytes.

Return Value:

    The file's data, a NUL after it, or NULL if the file could not be read.

--*/

{
    UNICODE_STRING Name;
    OBJECT_ATTRIBUTES Attributes;
    IO_STATUS_BLOCK Iosb;
    FILE_STANDARD_INFORMATION Information;
    HANDLE File;
    PUCHAR Data = NULL;
    NTSTATUS Status;

    *Length = 0;
    RtlInitUnicodeString(&Name, Path);
    InitializeObjectAttributes(&Attributes, &Name, OBJ_CASE_INSENSITIVE, NULL, NULL);
    Status = ZwCreateFile(&File, GENERIC_READ | SYNCHRONIZE, &Attributes, &Iosb, NULL, FILE_ATTRIBUTE_NORMAL,
                          FILE_SHARE_READ, FILE_OPEN, FILE_SYNCHRONOUS_IO_NONALERT | FILE_NON_DIRECTORY_FILE,
                          NULL, 0);
    if (!NT_SUCCESS(Status)) {
        return NULL;
    }

    Status = ZwQueryInformationFile(File, &Iosb, &Information, sizeof(Information), FileStandardInformation);
    if (NT_SUCCESS(Status) && Information.EndOfFile.HighPart == 0 && Information.EndOfFile.LowPart != 0 &&
        Information.EndOfFile.LowPart < 0x200000) {
        Data = ExAllocatePool(NonPagedPool, Information.EndOfFile.LowPart + 4);
        if (Data != NULL) {
            Status = ZwReadFile(File, NULL, NULL, NULL, &Iosb, Data, Information.EndOfFile.LowPart, NULL, NULL);
            if (NT_SUCCESS(Status) && Iosb.Information == Information.EndOfFile.LowPart) {
                *Length = Information.EndOfFile.LowPart;
                Data[*Length] = 0;
            } else {
                ExFreePool(Data);
                Data = NULL;
            }
        }
    }

    ZwClose(File);
    return Data;
}

//
// The hardware description the firmware gives: among the network
// controllers of the internal bus, the one named BCMWLAN_ID.
//

typedef struct _WLAN_HARDWARE {
    BOOLEAN Wlan;
    BOOLEAN Found;
    ULONG InterruptVector;
    ULONG InterruptLevel;
    ULONG MemoryBase;
    ULONG MemoryLength;
    ULONG BoardRevision;
    ULONG BaseClock;
} WLAN_HARDWARE, *PWLAN_HARDWARE;

static NTSTATUS
WlanSaveIdentifier(
    IN PWSTR ValueName,
    IN ULONG ValueType,
    IN PVOID ValueData,
    IN ULONG ValueLength,
    IN PVOID Context,
    IN PVOID EntryContext
    )
{
    PWLAN_HARDWARE Hardware = Context;
    UNICODE_STRING Identifier;
    UNICODE_STRING Wlan;

    if (ValueType == REG_SZ) {
        RtlInitUnicodeString(&Identifier, ValueData);
        RtlInitUnicodeString(&Wlan, L"BCM43XX-WLAN");
        Hardware->Wlan = RtlEqualUnicodeString(&Identifier, &Wlan, TRUE);
    }

    return STATUS_SUCCESS;
}

static NTSTATUS
WlanSaveHardware(
    IN PWSTR ValueName,
    IN ULONG ValueType,
    IN PVOID ValueData,
    IN ULONG ValueLength,
    IN PVOID Context,
    IN PVOID EntryContext
    )
{
    PWLAN_HARDWARE Hardware = Context;
    PCM_PARTIAL_RESOURCE_LIST List;
    PCM_PARTIAL_RESOURCE_DESCRIPTOR Partial;
    PCM_BCMWLAN_DEVICE_DATA Data;
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
            Hardware->Found = TRUE;
        } else if (Partial->Type == CmResourceTypeMemory) {
            Hardware->MemoryBase = Partial->u.Memory.Start.LowPart;
            Hardware->MemoryLength = Partial->u.Memory.Length;
        } else if (Partial->Type == CmResourceTypeDeviceSpecific && Index == List->Count - 1 &&
                   Partial->u.DeviceSpecificData.DataSize >= sizeof(CM_BCMWLAN_DEVICE_DATA)) {
            Data = (PCM_BCMWLAN_DEVICE_DATA)(Partial + 1);
            Hardware->BoardRevision = Data->BoardRevision;
            Hardware->BaseClock = Data->BaseClock;
        }
    }

    return STATUS_SUCCESS;
}

static BOOLEAN
WlanReadHardware(
    OUT PWLAN_HARDWARE Hardware
    )
{
    WCHAR Path[] = L"\\Registry\\Machine\\Hardware\\Description\\System\\MultifunctionAdapter\\0\\NetworkController\\#";
    RTL_QUERY_REGISTRY_TABLE QueryTable[3];
    PWCHAR Mark = wcschr(Path, L'#');
    ULONG Controller;

    for (Controller = 0; Controller < 8; Controller += 1) {
        *Mark = (WCHAR)(L'0' + Controller);
        NdisZeroMemory(QueryTable, sizeof(QueryTable));
        NdisZeroMemory(Hardware, sizeof(WLAN_HARDWARE));
        QueryTable[0].QueryRoutine = WlanSaveIdentifier;
        QueryTable[0].Name = L"Identifier";
        QueryTable[0].DefaultType = REG_NONE;
        QueryTable[1].QueryRoutine = WlanSaveHardware;
        QueryTable[1].Flags = RTL_QUERY_REGISTRY_REQUIRED;
        QueryTable[1].Name = L"Configuration Data";
        QueryTable[1].DefaultType = REG_NONE;
        if (!NT_SUCCESS(RtlQueryRegistryValues(RTL_REGISTRY_ABSOLUTE, Path, QueryTable, Hardware, NULL))) {
            break;
        }

        if (Hardware->Wlan && Hardware->Found && Hardware->MemoryLength != 0) {
            return TRUE;
        }
    }

    return FALSE;
}

//
// The network to join.
//

static ULONG
WlanParseSecurity(
    IN PCHAR Value
    )
{
    if (_stricmp(Value, "OPEN") == 0 || _stricmp(Value, "NONE") == 0) {
        return SECURITY_OPEN;
    }

    if (_stricmp(Value, "WPA3") == 0 || _stricmp(Value, "SAE") == 0 || _stricmp(Value, "WPA3-SAE") == 0) {
        return SECURITY_WPA3;
    }

    if (_stricmp(Value, "WPA2/WPA3") == 0 || _stricmp(Value, "WPA3/WPA2") == 0 ||
        _stricmp(Value, "WPA2-WPA3") == 0 || _stricmp(Value, "TRANSITION") == 0) {
        return SECURITY_WPA2_WPA3;
    }

    return SECURITY_WPA2;
}

static VOID
WlanSetSetting(
    IN PWLAN_ADAPTER Adapter,
    IN PCHAR Key,
    IN PCHAR Value,
    IN ULONG ValueLength
    )
{
    CHAR Text[32];

    if (_stricmp(Key, "SSID") == 0 && ValueLength <= 32) {
        NdisMoveMemory(Adapter->Ssid, Value, ValueLength);
        Adapter->Ssid[ValueLength] = 0;
        Adapter->SsidLength = ValueLength;
    } else if ((_stricmp(Key, "PASSWORD") == 0 || _stricmp(Key, "PASSPHRASE") == 0 || _stricmp(Key, "PSK") == 0 ||
                _stricmp(Key, "KEY") == 0) && ValueLength < sizeof(Adapter->Passphrase)) {
        NdisMoveMemory(Adapter->Passphrase, Value, ValueLength);
        Adapter->Passphrase[ValueLength] = 0;
        Adapter->PassphraseLength = ValueLength;
    } else if (_stricmp(Key, "SECURITY") == 0 && ValueLength < sizeof(Text)) {
        NdisMoveMemory(Text, Value, ValueLength);
        Text[ValueLength] = 0;
        Adapter->Security = WlanParseSecurity(Text);
    } else if (_stricmp(Key, "COUNTRY") == 0 && ValueLength == 2) {
        Adapter->Country[0] = (CHAR)(Value[0] & ~0x20);
        Adapter->Country[1] = (CHAR)(Value[1] & ~0x20);
        Adapter->Country[2] = 0;
    } else if (_stricmp(Key, "CLOCK") == 0 && ValueLength < sizeof(Text)) {
        NdisMoveMemory(Text, Value, ValueLength);
        Text[ValueLength] = 0;
        Adapter->TransferClock = atol(Text) * 1000000;
    }
}

//
// C:\CONFIG.TXT: KEY=VALUE lines, of which those of the driver are wifi_*
// and debug_wifi; # starts a comment line, [ a section of the boot
// firmware's (the driver reads them all). A value keeps its inner spaces (an
// SSID may have them) but not those at its ends.
//

static BOOLEAN
WlanParseBoolean(
    IN PCHAR Value,
    IN ULONG Length
    )
{
    return Length != 0 && (Value[0] == '1' || Value[0] == 'y' || Value[0] == 'Y' || Value[0] == 't' ||
                           Value[0] == 'T' || (Length >= 2 && (Value[1] == 'n' || Value[1] == 'N')));
}

static VOID
WlanReadSettingsFile(
    IN PWLAN_ADAPTER Adapter
    )
{
    PUCHAR Text;
    ULONG Length;
    PCHAR Line;
    PCHAR End;
    PCHAR Equal;
    PCHAR Key;
    PCHAR Value;
    PCHAR Last;
    CHAR Name[16];

    Text = WlanReadFile(Adapter, L"\\DosDevices\\C:\\CONFIG.TXT", &Length);
    if (Text == NULL) {
        return;
    }

    Line = (PCHAR)Text;
    if (Length >= 3 && Text[0] == 0xEF && Text[1] == 0xBB && Text[2] == 0xBF) {
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

        if (Line == Last || *Line == '#' || *Line == ';') {
            continue;
        }

        for (Equal = Line; Equal < Last && *Equal != '='; Equal += 1) {
        }

        if (Equal == Last) {
            continue;
        }

        Key = Line;
        Value = Equal + 1;
        while (Equal > Key && (Equal[-1] == ' ' || Equal[-1] == '\t')) {
            Equal -= 1;
        }

        while (Value < Last && (*Value == ' ' || *Value == '\t')) {
            Value += 1;
        }

        if ((ULONG)(Equal - Key) >= sizeof(Name)) {
            continue;
        }

        NdisMoveMemory(Name, Key, Equal - Key);
        Name[Equal - Key] = 0;
        if (_stricmp(Name, "DEBUG_WIFI") == 0) {
            Adapter->Debug = WlanParseBoolean(Value, Last - Value);
        } else if (_strnicmp(Name, "WIFI_", 5) == 0) {
            WlanSetSetting(Adapter, Name + 5, Value, Last - Value);
        }
    }

    ExFreePool(Text);
}

static VOID
WlanReadString(
    IN PWLAN_ADAPTER Adapter,
    IN NDIS_HANDLE ConfigurationHandle,
    IN PWCHAR Name,
    IN PCHAR Key
    )
{
    PNDIS_CONFIGURATION_PARAMETER Value;
    NDIS_STRING Keyword;
    NDIS_STATUS Status;
    ANSI_STRING Ansi;
    CHAR Text[160];

    RtlInitUnicodeString(&Keyword, Name);
    NdisReadConfiguration(&Status, &Value, ConfigurationHandle, &Keyword, NdisParameterString);
    if (Status != NDIS_STATUS_SUCCESS || Value->ParameterData.StringData.Length == 0) {
        return;
    }

    Ansi.Buffer = Text;
    Ansi.Length = 0;
    Ansi.MaximumLength = sizeof(Text) - 1;
    if (NT_SUCCESS(RtlUnicodeStringToAnsiString(&Ansi, &Value->ParameterData.StringData, FALSE))) {
        WlanSetSetting(Adapter, Key, Text, Ansi.Length);
    }
}

static VOID
WlanReadSettings(
    IN PWLAN_ADAPTER Adapter,
    IN NDIS_HANDLE WrapperConfigurationContext
    )
{
    NDIS_HANDLE ConfigurationHandle;
    NDIS_STATUS Status;

    Adapter->Security = SECURITY_WPA2;
    Adapter->Country[0] = 'X';
    Adapter->Country[1] = 'X';
    Adapter->TransferClock = 25000000;
    WlanReadSettingsFile(Adapter);
    NdisOpenConfiguration(&Status, &ConfigurationHandle, WrapperConfigurationContext);
    if (Status == NDIS_STATUS_SUCCESS) {
        WlanReadString(Adapter, ConfigurationHandle, L"Ssid", "SSID");
        WlanReadString(Adapter, ConfigurationHandle, L"Passphrase", "PASSWORD");
        WlanReadString(Adapter, ConfigurationHandle, L"Security", "SECURITY");
        WlanReadString(Adapter, ConfigurationHandle, L"Country", "COUNTRY");
        WlanReadString(Adapter, ConfigurationHandle, L"SdioClock", "CLOCK");
        NdisCloseConfiguration(ConfigurationHandle);
    }

    Adapter->Configured = Adapter->SsidLength != 0;
    if (Adapter->TransferClock < 400000 || Adapter->TransferClock > 50000000) {
        Adapter->TransferClock = 25000000;
    }

    if (Adapter->Debug) {
        WlanOpenLog(Adapter);
    }

    WlanLog(Adapter, "Raspberry Pi wireless miniport, built " __DATE__ " " __TIME__);
    WlanLog(Adapter, "Network \"%s\", security %s, passphrase %lu characters, country %s, SDIO clock %lu",
            Adapter->Ssid,
            Adapter->Security == SECURITY_OPEN ? "open" : Adapter->Security == SECURITY_WPA3 ? "WPA3" :
            Adapter->Security == SECURITY_WPA2_WPA3 ? "WPA2/WPA3" : "WPA2",
            Adapter->PassphraseLength, Adapter->Country, Adapter->TransferClock);
}

//
// From the thread: a frame received, for the timer to indicate, and a
// change of the link.
//

static VOID
WlanKickTimer(
    IN PWLAN_ADAPTER Adapter
    )
{
    BOOLEAN Set;

    NdisAcquireSpinLock(&Adapter->Lock);
    Set = !Adapter->TimerSet;
    Adapter->TimerSet = TRUE;
    NdisReleaseSpinLock(&Adapter->Lock);
    if (Set) {
        NdisMSetTimer(&Adapter->Timer, 0);
    }
}

VOID
WlanQueueReceive(
    IN PWLAN_ADAPTER Adapter,
    IN PUCHAR Frame,
    IN ULONG Length
    )

/*++

Routine Description:

    This routine puts a frame the chip received into the receive ring, for the
    timer to indicate. It runs in the thread.

Arguments:

    Adapter - Supplies the adapter.

    Frame - Supplies the Ethernet frame.

    Length - Supplies its length, in bytes.

Return Value:

    None.

--*/

{
    PWLAN_SLOT Slot;

    if (!Adapter->Initialized) {
        return;
    }

    if (Length < ETH_HEADER_SIZE || Length > ETH_MAXIMUM_FRAME) {
        Adapter->ReceiveError += 1;
        return;
    }

    NdisAcquireSpinLock(&Adapter->Lock);
    if (Adapter->ReceiveCount == RECEIVE_SLOTS) {
        NdisReleaseSpinLock(&Adapter->Lock);
        Adapter->ReceiveNoBuffer += 1;
        return;
    }

    Slot = &Adapter->ReceiveSlots[Adapter->ReceiveTail];
    NdisReleaseSpinLock(&Adapter->Lock);

    //
    // The IP header of an indicated frame falls on a longword.
    //

    NdisMoveMemory((PUCHAR)Slot->Data + 2, Frame, Length);
    Slot->Length = Length;
    NdisAcquireSpinLock(&Adapter->Lock);
    Adapter->ReceiveTail = (Adapter->ReceiveTail + 1) % RECEIVE_SLOTS;
    Adapter->ReceiveCount += 1;
    NdisReleaseSpinLock(&Adapter->Lock);
}

VOID
WlanSetLink(
    IN PWLAN_ADAPTER Adapter,
    IN BOOLEAN Up
    )

/*++

Routine Description:

    This routine sets the state of the link; a change is logged, and left for
    the timer to indicate. It runs in the thread.

Arguments:

    Adapter - Supplies the adapter.

    Up - Supplies whether the network is joined.

Return Value:

    None.

--*/

{
    if ((Adapter->LinkState == LINK_UP) != Up) {
        Adapter->LinkState = Up ? LINK_UP : LINK_IDLE;
        Adapter->LinkChanged = TRUE;
        WlanLog(Adapter, "Link %s", Up ? "up" : "down");
    }
}

//
// The thread.
//

static VOID
WlanThread(
    IN PVOID Context
    )
{
    PWLAN_ADAPTER Adapter = Context;
    LARGE_INTEGER Interval;
    BOOLEAN Kick;

    KeSetPriorityThread(KeGetCurrentThread(), LOW_REALTIME_PRIORITY);
    Interval.QuadPart = -10000 * (LONGLONG)POLL_INTERVAL_MS;
    while (!Adapter->Halting) {
        KeWaitForSingleObject(&Adapter->WorkEvent, Executive, KernelMode, FALSE, &Interval);
        if (Adapter->Halting) {
            break;
        }

        Kick = ChipService(Adapter);
        if (Adapter->LinkChanged || Adapter->ReceiveCount != 0 ||
            (Adapter->ResourcesNeeded && Adapter->TransmitCount < TRANSMIT_SLOTS)) {
            Kick = TRUE;
        }

        if (Kick && Adapter->Initialized) {
            WlanKickTimer(Adapter);
        }

        SdioEnableInterrupt(Adapter, TRUE);
    }

    //
    // The log's handle is of the system process, which this thread runs in;
    // halt may run in another (winlogon at shutdown), so it is closed here.
    //

    if (Adapter->LogFile != NULL) {
        WlanLog(Adapter, "Thread stopped");
        ZwClose(Adapter->LogFile);
        Adapter->LogFile = NULL;
    }

    PsTerminateSystemThread(STATUS_SUCCESS);
}

//
// The timer: the frames received, the link, and room in the transmit ring.
//

static VOID
WlanTimer(
    IN PVOID SystemSpecific1,
    IN PVOID FunctionContext,
    IN PVOID SystemSpecific2,
    IN PVOID SystemSpecific3
    )
{
    PWLAN_ADAPTER Adapter = FunctionContext;
    PWLAN_SLOT Slot;
    BOOLEAN Indicated = FALSE;
    BOOLEAN Resources = FALSE;

    NdisAcquireSpinLock(&Adapter->Lock);
    Adapter->TimerSet = FALSE;
    NdisReleaseSpinLock(&Adapter->Lock);

    if (Adapter->LinkChanged) {
        Adapter->LinkChanged = FALSE;
        if (Adapter->LinkIndicated != (Adapter->LinkState == LINK_UP)) {
            Adapter->LinkIndicated = Adapter->LinkState == LINK_UP;

            //
            // (the NDIS of NT 3.5 has no media status to indicate)
            //

#ifdef NDIS_STATUS_MEDIA_CONNECT
            NdisMIndicateStatus(Adapter->MiniportAdapterHandle,
                                Adapter->LinkIndicated ? NDIS_STATUS_MEDIA_CONNECT : NDIS_STATUS_MEDIA_DISCONNECT,
                                NULL, 0);
            NdisMIndicateStatusComplete(Adapter->MiniportAdapterHandle);
#endif
        }
    }

    for (;;) {
        NdisAcquireSpinLock(&Adapter->Lock);
        if (Adapter->ReceiveCount == 0) {
            NdisReleaseSpinLock(&Adapter->Lock);
            break;
        }

        Slot = &Adapter->ReceiveSlots[Adapter->ReceiveHead];
        NdisReleaseSpinLock(&Adapter->Lock);

        Adapter->IndicatedFrame = (PUCHAR)Slot->Data + 2;
        Adapter->IndicatedLength = Slot->Length;
        Adapter->ReceiveOk += 1;
        NdisMEthIndicateReceive(Adapter->MiniportAdapterHandle, (NDIS_HANDLE)Adapter, Adapter->IndicatedFrame,
                                ETH_HEADER_SIZE, Adapter->IndicatedFrame + ETH_HEADER_SIZE,
                                Slot->Length - ETH_HEADER_SIZE, Slot->Length - ETH_HEADER_SIZE);
        Indicated = TRUE;

        NdisAcquireSpinLock(&Adapter->Lock);
        Adapter->ReceiveHead = (Adapter->ReceiveHead + 1) % RECEIVE_SLOTS;
        Adapter->ReceiveCount -= 1;
        NdisReleaseSpinLock(&Adapter->Lock);
    }

    if (Indicated) {
        NdisMEthIndicateReceiveComplete(Adapter->MiniportAdapterHandle);
    }

    NdisAcquireSpinLock(&Adapter->Lock);
    if (Adapter->ResourcesNeeded && Adapter->TransmitCount < TRANSMIT_SLOTS) {
        Adapter->ResourcesNeeded = FALSE;
        Resources = TRUE;
    }

    NdisReleaseSpinLock(&Adapter->Lock);
    if (Resources) {
        NdisMSendResourcesAvailable(Adapter->MiniportAdapterHandle);
    }
}

//
// The card interrupt: its signal off, and the thread woken.
//

static VOID
WlanIsr(
    OUT PBOOLEAN InterruptRecognized,
    OUT PBOOLEAN QueueMiniportHandleInterrupt,
    IN NDIS_HANDLE MiniportAdapterContext
    )
{
    PWLAN_ADAPTER Adapter = MiniportAdapterContext;
    BOOLEAN Recognized = SdioIsr(Adapter);

    *InterruptRecognized = Recognized;
    *QueueMiniportHandleInterrupt = Recognized;
}

static VOID
WlanHandleInterrupt(
    IN NDIS_HANDLE MiniportAdapterContext
    )
{
    PWLAN_ADAPTER Adapter = MiniportAdapterContext;

    KeSetEvent(&Adapter->WorkEvent, 0, FALSE);
}

static VOID
WlanDisableInterrupt(
    IN NDIS_HANDLE MiniportAdapterContext
    )
{
}

static VOID
WlanEnableInterrupt(
    IN NDIS_HANDLE MiniportAdapterContext
    )
{
}

//
// The resources of the adapter.
//

static VOID
WlanFreeAdapter(
    IN PWLAN_ADAPTER Adapter
    )
{
    if (Adapter->Registers != NULL) {
        NdisMUnmapIoSpace(Adapter->MiniportAdapterHandle, Adapter->Registers, Adapter->RegistersLength);
    }

    if (Adapter->TransmitSlots != NULL) {
        NdisFreeMemory(Adapter->TransmitSlots, TRANSMIT_SLOTS * sizeof(WLAN_SLOT), 0);
    }

    if (Adapter->ReceiveSlots != NULL) {
        NdisFreeMemory(Adapter->ReceiveSlots, RECEIVE_SLOTS * sizeof(WLAN_SLOT), 0);
    }

    if (Adapter->ControlBuffer != NULL) {
        NdisFreeMemory(Adapter->ControlBuffer, CONTROL_BUFFER_SIZE + 64, 0);
    }

    if (Adapter->FrameBuffer != NULL) {
        NdisFreeMemory(Adapter->FrameBuffer, RECEIVE_BUFFER_SIZE + 64, 0);
    }

    if (Adapter->VarBuffer != NULL) {
        NdisFreeMemory(Adapter->VarBuffer, CONTROL_BUFFER_SIZE, 0);
    }

    if (Adapter->LogFile != NULL) {
        ZwClose(Adapter->LogFile);
    }

    NdisFreeSpinLock(&Adapter->Lock);
    NdisFreeMemory(Adapter, sizeof(WLAN_ADAPTER), 0);
}

static VOID
WlanLogFailure(
    IN PWLAN_ADAPTER Adapter,
    IN ULONG Step,
    IN NDIS_STATUS Status
    )
{
    NdisWriteErrorLogEntry(Adapter->MiniportAdapterHandle,
                           Step <= 1 ? NDIS_ERROR_CODE_ADAPTER_NOT_FOUND : NDIS_ERROR_CODE_HARDWARE_FAILURE, 6,
                           Step, Status, Adapter->ChipId, Adapter->SdioErrors, Adapter->LastSdioStatus,
                           Adapter->LastSdioAddress);
}

static NDIS_STATUS
WlanInitialize(
    OUT PNDIS_STATUS OpenErrorStatus,
    OUT PUINT SelectedMediumIndex,
    IN PNDIS_MEDIUM MediumArray,
    IN UINT MediumArraySize,
    IN NDIS_HANDLE MiniportAdapterHandle,
    IN NDIS_HANDLE WrapperConfigurationContext
    )

/*++

Routine Description:

    This routine maps the EMMC the firmware describes, brings the chip up
    (its firmware loaded and running, its station address read), connects
    the card interrupt and starts the thread, which joins the network. The
    first join is waited for (up to JOIN_WAIT_MS), so that the protocols
    bound next find the link up: TCP/IP's DHCP asks for an address at once.

Arguments:

    OpenErrorStatus - Not used.

    SelectedMediumIndex - Receives the index of 802.3.

    MediumArray, MediumArraySize - Supply the media the protocols take.

    MiniportAdapterHandle - Supplies the handle of the adapter.

    WrapperConfigurationContext - Supplies the configuration of the adapter.

Return Value:

    NDIS_STATUS_SUCCESS, or why the adapter could not be initialized.

--*/

{
    PWLAN_ADAPTER Adapter;
    WLAN_HARDWARE Hardware;
    NDIS_PHYSICAL_ADDRESS Physical;
    NDIS_STATUS Status;
    HANDLE Thread;
    LARGE_INTEGER Interval;
    ULONG Waited;
    UINT Index;

    for (Index = 0; Index < MediumArraySize && MediumArray[Index] != NdisMedium802_3; Index += 1) {
    }

    if (Index == MediumArraySize) {
        return NDIS_STATUS_UNSUPPORTED_MEDIA;
    }

    *SelectedMediumIndex = Index;
    if (NdisAllocateMemory((PVOID *)&Adapter, sizeof(WLAN_ADAPTER), 0, HighestAcceptableMax) !=
        NDIS_STATUS_SUCCESS) {
        return NDIS_STATUS_RESOURCES;
    }

    NdisZeroMemory(Adapter, sizeof(WLAN_ADAPTER));
    Adapter->MiniportAdapterHandle = MiniportAdapterHandle;
    NdisAllocateSpinLock(&Adapter->Lock);
    KeInitializeEvent(&Adapter->WorkEvent, SynchronizationEvent, FALSE);
    WlanReadSettings(Adapter, WrapperConfigurationContext);

    if (!WlanReadHardware(&Hardware)) {
        WlanLog(Adapter, "The firmware describes no wireless controller");
        WlanLogFailure(Adapter, 1, NDIS_STATUS_ADAPTER_NOT_FOUND);
        WlanFreeAdapter(Adapter);
        return NDIS_STATUS_ADAPTER_NOT_FOUND;
    }

    Adapter->RegistersPhysical = Hardware.MemoryBase;
    Adapter->RegistersLength = Hardware.MemoryLength;
    Adapter->InterruptVector = Hardware.InterruptVector;
    Adapter->InterruptLevel = Hardware.InterruptLevel;
    Adapter->BoardRevision = Hardware.BoardRevision;
    Adapter->BaseClock = Hardware.BaseClock != 0 ? Hardware.BaseClock : 50000000;
    WlanLog(Adapter, "EMMC at %08lx, vector %lu, base clock %lu Hz, board revision %08lx",
            Adapter->RegistersPhysical, Adapter->InterruptVector, Adapter->BaseClock, Adapter->BoardRevision);

    NdisAllocateMemory((PVOID *)&Adapter->TransmitSlots, TRANSMIT_SLOTS * sizeof(WLAN_SLOT), 0,
                       HighestAcceptableMax);
    NdisAllocateMemory((PVOID *)&Adapter->ReceiveSlots, RECEIVE_SLOTS * sizeof(WLAN_SLOT), 0,
                       HighestAcceptableMax);
    NdisAllocateMemory((PVOID *)&Adapter->ControlBuffer, CONTROL_BUFFER_SIZE + 64, 0, HighestAcceptableMax);
    NdisAllocateMemory((PVOID *)&Adapter->FrameBuffer, RECEIVE_BUFFER_SIZE + 64, 0, HighestAcceptableMax);
    NdisAllocateMemory((PVOID *)&Adapter->VarBuffer, CONTROL_BUFFER_SIZE, 0, HighestAcceptableMax);
    if (Adapter->TransmitSlots == NULL || Adapter->ReceiveSlots == NULL || Adapter->ControlBuffer == NULL ||
        Adapter->FrameBuffer == NULL || Adapter->VarBuffer == NULL) {
        WlanLogFailure(Adapter, 2, NDIS_STATUS_RESOURCES);
        WlanFreeAdapter(Adapter);
        return NDIS_STATUS_RESOURCES;
    }

    NdisMSetAttributes(MiniportAdapterHandle, (NDIS_HANDLE)Adapter, FALSE, NdisInterfaceInternal);
    NdisSetPhysicalAddressHigh(Physical, 0);
    NdisSetPhysicalAddressLow(Physical, Adapter->RegistersPhysical);
    Status = NdisMMapIoSpace((PVOID *)&Adapter->Registers, MiniportAdapterHandle, Physical,
                             Adapter->RegistersLength);
    if (Status != NDIS_STATUS_SUCCESS) {
        Adapter->Registers = NULL;
        WlanLogFailure(Adapter, 2, Status);
        WlanFreeAdapter(Adapter);
        return Status;
    }

    NdisMInitializeTimer(&Adapter->Timer, MiniportAdapterHandle, WlanTimer, Adapter);
    Status = ChipStart(Adapter);
    if (Status != NDIS_STATUS_SUCCESS) {
        WlanLog(Adapter, "The chip did not start (status %08lx); the adapter is not installed", Status);
        WlanLogFailure(Adapter, 3, Status);
        ChipStop(Adapter);
        WlanFreeAdapter(Adapter);
        return Status;
    }

    NdisMoveMemory(Adapter->CurrentAddress, Adapter->PermanentAddress, ETH_LENGTH_OF_ADDRESS);
    Status = NdisMRegisterInterrupt(&Adapter->Interrupt, MiniportAdapterHandle, Adapter->InterruptVector,
                                    Adapter->InterruptLevel, TRUE, TRUE, NdisInterruptLevelSensitive);
    if (Status == NDIS_STATUS_SUCCESS) {
        Adapter->InterruptConnected = TRUE;
    } else {
        WlanLog(Adapter, "No interrupt (status %08lx): the chip is polled every %u ms", Status,
                POLL_INTERVAL_MS);
    }

    if (NT_SUCCESS(PsCreateSystemThread(&Thread, THREAD_ALL_ACCESS, NULL, NULL, NULL, WlanThread, Adapter))) {
        ObReferenceObjectByHandle(Thread, THREAD_ALL_ACCESS, NULL, KernelMode, &Adapter->Thread, NULL);
        ZwClose(Thread);
    }

    if (Adapter->Thread == NULL) {
        WlanLogFailure(Adapter, 4, NDIS_STATUS_RESOURCES);
        if (Adapter->InterruptConnected) {
            NdisMDeregisterInterrupt(&Adapter->Interrupt);
        }

        ChipStop(Adapter);
        WlanFreeAdapter(Adapter);
        return NDIS_STATUS_RESOURCES;
    }

    //
    // The first join.
    //

    if (Adapter->Configured) {
        Interval.QuadPart = -10000 * 100;
        for (Waited = 0; Waited < JOIN_WAIT_MS && Adapter->LinkState != LINK_UP; Waited += 100) {
            KeDelayExecutionThread(KernelMode, FALSE, &Interval);
        }

        WlanLog(Adapter, "Initialized; the link is %s", Adapter->LinkState == LINK_UP ? "up" : "not up yet");
    } else {
        WlanLog(Adapter, "Initialized; no network configured (wifi_ssid of C:\\CONFIG.TXT or Parameters\\Ssid)");
    }

    //
    // From here the timer indicates: the link as the protocols will find it
    // (OID_GEN_MEDIA_CONNECT_STATUS), and the frames received from now on.
    //

    Adapter->LinkIndicated = Adapter->LinkState == LINK_UP;
    Adapter->LinkChanged = FALSE;
    Adapter->Initialized = TRUE;
    return NDIS_STATUS_SUCCESS;
}

static VOID
WlanHalt(
    IN NDIS_HANDLE MiniportAdapterContext
    )
{
    PWLAN_ADAPTER Adapter = MiniportAdapterContext;
    BOOLEAN Cancelled;

    Adapter->Halting = TRUE;
    KeSetEvent(&Adapter->WorkEvent, 0, FALSE);
    if (Adapter->Thread != NULL) {
        KeWaitForSingleObject(Adapter->Thread, Executive, KernelMode, FALSE, NULL);
        ObDereferenceObject(Adapter->Thread);
    }

    NdisMCancelTimer(&Adapter->Timer, &Cancelled);
    if (Adapter->InterruptConnected) {
        SdioEnableInterrupt(Adapter, FALSE);
        NdisMDeregisterInterrupt(&Adapter->Interrupt);
        Adapter->InterruptConnected = FALSE;
    }

    ChipStop(Adapter);
    WlanFreeAdapter(Adapter);
}

static NDIS_STATUS
WlanReset(
    OUT PBOOLEAN AddressingReset,
    IN NDIS_HANDLE MiniportAdapterContext
    )
{
    *AddressingReset = FALSE;
    return NDIS_STATUS_SUCCESS;
}

static BOOLEAN
WlanCheckForHang(
    IN NDIS_HANDLE MiniportAdapterContext
    )
{
    return FALSE;
}

//
// A transmit: the frame is copied into the next slot of the ring, behind
// room for the chip's headers, and completes; the thread sends it. Without a
// link it is dropped (and completes).
//

static NDIS_STATUS
WlanSend(
    IN NDIS_HANDLE MiniportAdapterContext,
    IN PNDIS_PACKET Packet,
    IN UINT Flags
    )
{
    PWLAN_ADAPTER Adapter = MiniportAdapterContext;
    PNDIS_BUFFER Buffer;
    PWLAN_SLOT Slot;
    PUCHAR Frame;
    PVOID Data;
    UINT Length;
    UINT Total;
    UINT Copied = 0;

    NdisQueryPacket(Packet, NULL, NULL, &Buffer, &Total);
    if (Total < ETH_HEADER_SIZE || Total > ETH_MAXIMUM_FRAME) {
        Adapter->TransmitError += 1;
        return NDIS_STATUS_FAILURE;
    }

    if (Adapter->LinkState != LINK_UP) {
        Adapter->TransmitError += 1;
        return NDIS_STATUS_SUCCESS;
    }

    NdisAcquireSpinLock(&Adapter->Lock);
    if (Adapter->TransmitCount == TRANSMIT_SLOTS) {
        Adapter->ResourcesNeeded = TRUE;
        NdisReleaseSpinLock(&Adapter->Lock);
        KeSetEvent(&Adapter->WorkEvent, 0, FALSE);
        return NDIS_STATUS_RESOURCES;
    }

    Slot = &Adapter->TransmitSlots[Adapter->TransmitTail];
    NdisReleaseSpinLock(&Adapter->Lock);

    Frame = (PUCHAR)Slot->Data + FRAME_HEADROOM;
    while (Buffer != NULL) {
        NdisQueryBuffer(Buffer, &Data, &Length);
        NdisMoveMemory(Frame + Copied, Data, Length);
        Copied += Length;
        NdisGetNextBuffer(Buffer, &Buffer);
    }

    if (Copied < ETH_MINIMUM_FRAME) {
        NdisZeroMemory(Frame + Copied, ETH_MINIMUM_FRAME - Copied);
        Copied = ETH_MINIMUM_FRAME;
    }

    Slot->Length = Copied;
    NdisAcquireSpinLock(&Adapter->Lock);
    Adapter->TransmitTail = (Adapter->TransmitTail + 1) % TRANSMIT_SLOTS;
    Adapter->TransmitCount += 1;
    NdisReleaseSpinLock(&Adapter->Lock);
    KeSetEvent(&Adapter->WorkEvent, 0, FALSE);
    return NDIS_STATUS_SUCCESS;
}

BOOLEAN
WlanNextTransmit(
    IN PWLAN_ADAPTER Adapter,
    OUT PWLAN_SLOT *Slot
    )

/*++

Routine Description:

    This routine finds the next frame of the transmit ring, for the thread to
    send (chip.c).

Arguments:

    Adapter - Supplies the adapter.

    Slot - Receives the slot of the frame.

Return Value:

    TRUE if the ring has a frame.

--*/

{
    BOOLEAN Found;

    NdisAcquireSpinLock(&Adapter->Lock);
    Found = Adapter->TransmitCount != 0;
    if (Found) {
        *Slot = &Adapter->TransmitSlots[Adapter->TransmitHead];
    }

    NdisReleaseSpinLock(&Adapter->Lock);
    return Found;
}

VOID
WlanTransmitDone(
    IN PWLAN_ADAPTER Adapter,
    IN BOOLEAN Ok
    )

/*++

Routine Description:

    This routine takes the frame the thread sent, or failed to, off the
    transmit ring, and counts it.

Arguments:

    Adapter - Supplies the adapter.

    Ok - Supplies whether the frame was sent.

Return Value:

    None.

--*/

{
    if (Ok) {
        Adapter->TransmitOk += 1;
    } else {
        Adapter->TransmitError += 1;
    }

    NdisAcquireSpinLock(&Adapter->Lock);
    Adapter->TransmitHead = (Adapter->TransmitHead + 1) % TRANSMIT_SLOTS;
    Adapter->TransmitCount -= 1;
    NdisReleaseSpinLock(&Adapter->Lock);
}

//
// The rest of a frame indicated, copied into a packet.
//

static NDIS_STATUS
WlanTransferData(
    OUT PNDIS_PACKET Packet,
    OUT PUINT BytesTransferred,
    IN NDIS_HANDLE MiniportAdapterContext,
    IN NDIS_HANDLE MiniportReceiveContext,
    IN UINT ByteOffset,
    IN UINT BytesToTransfer
    )
{
    PWLAN_ADAPTER Adapter = MiniportAdapterContext;
    PUCHAR Source = Adapter->IndicatedFrame + ETH_HEADER_SIZE + ByteOffset;
    PNDIS_BUFFER Buffer;
    PVOID Data;
    UINT Length;
    UINT Left;
    UINT Moved = 0;

    if (ETH_HEADER_SIZE + ByteOffset >= Adapter->IndicatedLength) {
        *BytesTransferred = 0;
        return NDIS_STATUS_SUCCESS;
    }

    Left = Adapter->IndicatedLength - ETH_HEADER_SIZE - ByteOffset;
    if (BytesToTransfer < Left) {
        Left = BytesToTransfer;
    }

    NdisQueryPacket(Packet, NULL, NULL, &Buffer, NULL);
    while (Buffer != NULL && Left != 0) {
        NdisQueryBuffer(Buffer, &Data, &Length);
        if (Length > Left) {
            Length = Left;
        }

        NdisMoveMemory(Data, Source + Moved, Length);
        Moved += Length;
        Left -= Length;
        NdisGetNextBuffer(Buffer, &Buffer);
    }

    *BytesTransferred = Moved;
    return NDIS_STATUS_SUCCESS;
}

static NDIS_STATUS
WlanQueryInformation(
    IN NDIS_HANDLE MiniportAdapterContext,
    IN NDIS_OID Oid,
    IN PVOID InformationBuffer,
    IN ULONG InformationBufferLength,
    OUT PULONG BytesWritten,
    OUT PULONG BytesNeeded
    )
{
    PWLAN_ADAPTER Adapter = MiniportAdapterContext;
    PVOID Source;
    ULONG SourceLength = sizeof(ULONG);
    ULONG Value = 0;
    USHORT Version;
    UCHAR VendorId[4];

    Source = &Value;
    switch (Oid) {
    case OID_GEN_SUPPORTED_LIST:
        Source = (PVOID)WlanSupportedOids;
        SourceLength = sizeof(WlanSupportedOids);
        break;

    case OID_GEN_HARDWARE_STATUS:
        Value = NdisHardwareStatusReady;
        break;

    case OID_GEN_MEDIA_SUPPORTED:
    case OID_GEN_MEDIA_IN_USE:
        Value = NdisMedium802_3;
        break;

    case OID_GEN_MAXIMUM_LOOKAHEAD:
    case OID_GEN_CURRENT_LOOKAHEAD:
    case OID_GEN_MAXIMUM_FRAME_SIZE:
        Value = ETH_MAXIMUM_FRAME - ETH_HEADER_SIZE;
        break;

    case OID_GEN_MAXIMUM_TOTAL_SIZE:
    case OID_GEN_TRANSMIT_BLOCK_SIZE:
    case OID_GEN_RECEIVE_BLOCK_SIZE:
        Value = ETH_MAXIMUM_FRAME;
        break;

    case OID_GEN_MAC_OPTIONS:
        Value = NDIS_MAC_OPTION_TRANSFERS_NOT_PEND | NDIS_MAC_OPTION_COPY_LOOKAHEAD_DATA |
                NDIS_MAC_OPTION_RECEIVE_SERIALIZED | NDIS_MAC_OPTION_NO_LOOPBACK;
        break;

    case OID_GEN_LINK_SPEED:
        Value = 540000;
        break;

#ifdef OID_GEN_MEDIA_CONNECT_STATUS
    case OID_GEN_MEDIA_CONNECT_STATUS:
        Value = Adapter->LinkState == LINK_UP ? NdisMediaStateConnected : NdisMediaStateDisconnected;
        break;
#endif

    case OID_GEN_TRANSMIT_BUFFER_SPACE:
        Value = TRANSMIT_SLOTS * ETH_MAXIMUM_FRAME;
        break;

    case OID_GEN_RECEIVE_BUFFER_SPACE:
        Value = RECEIVE_SLOTS * ETH_MAXIMUM_FRAME;
        break;

    case OID_GEN_VENDOR_ID:
        NdisMoveMemory(VendorId, Adapter->PermanentAddress, 3);
        VendorId[3] = 0;
        Source = VendorId;
        SourceLength = sizeof(VendorId);
        break;

    case OID_GEN_VENDOR_DESCRIPTION:
        Source = (PVOID)WlanDescription;
        SourceLength = sizeof(WlanDescription);
        break;

    case OID_GEN_DRIVER_VERSION:
        Version = (BCMWLAN_NDIS_MAJOR_VERSION << 8) | BCMWLAN_NDIS_MINOR_VERSION;
        Source = &Version;
        SourceLength = sizeof(USHORT);
        break;

    case OID_GEN_CURRENT_PACKET_FILTER:
        Value = Adapter->PacketFilter;
        break;

    case OID_GEN_XMIT_OK:
        Value = Adapter->TransmitOk;
        break;

    case OID_GEN_RCV_OK:
        Value = Adapter->ReceiveOk;
        break;

    case OID_GEN_XMIT_ERROR:
        Value = Adapter->TransmitError;
        break;

    case OID_GEN_RCV_ERROR:
        Value = Adapter->ReceiveError;
        break;

    case OID_GEN_RCV_NO_BUFFER:
        Value = Adapter->ReceiveNoBuffer;
        break;

    case OID_GEN_RCV_CRC_ERROR:
        Value = 0;
        break;

    case OID_802_3_PERMANENT_ADDRESS:
        Source = Adapter->PermanentAddress;
        SourceLength = ETH_LENGTH_OF_ADDRESS;
        break;

    case OID_802_3_CURRENT_ADDRESS:
        Source = Adapter->CurrentAddress;
        SourceLength = ETH_LENGTH_OF_ADDRESS;
        break;

    case OID_802_3_MULTICAST_LIST:
        Source = Adapter->Multicast;
        SourceLength = Adapter->MulticastCount * ETH_LENGTH_OF_ADDRESS;
        break;

    case OID_802_3_MAXIMUM_LIST_SIZE:
        Value = WLAN_MULTICAST_LIST;
        break;

    case OID_802_3_RCV_ERROR_ALIGNMENT:
    case OID_802_3_XMIT_ONE_COLLISION:
    case OID_802_3_XMIT_MORE_COLLISIONS:
        Value = 0;
        break;

    default:
        *BytesWritten = 0;
        return NDIS_STATUS_INVALID_OID;
    }

    if (SourceLength > InformationBufferLength) {
        *BytesNeeded = SourceLength;
        return NDIS_STATUS_INVALID_LENGTH;
    }

    NdisMoveMemory(InformationBuffer, Source, SourceLength);
    *BytesWritten = SourceLength;
    return NDIS_STATUS_SUCCESS;
}

//
// The receive filter and multicast list take effect at once; the thread
// gives them to the chip.
//

static NDIS_STATUS
WlanSetInformation(
    IN NDIS_HANDLE MiniportAdapterContext,
    IN NDIS_OID Oid,
    IN PVOID InformationBuffer,
    IN ULONG InformationBufferLength,
    OUT PULONG BytesRead,
    OUT PULONG BytesNeeded
    )
{
    PWLAN_ADAPTER Adapter = MiniportAdapterContext;
    ULONG Filter;

    switch (Oid) {
    case OID_802_3_MULTICAST_LIST:
        if (InformationBufferLength % ETH_LENGTH_OF_ADDRESS != 0 ||
            InformationBufferLength > WLAN_MULTICAST_LIST * ETH_LENGTH_OF_ADDRESS) {
            *BytesNeeded = ETH_LENGTH_OF_ADDRESS;
            return NDIS_STATUS_INVALID_LENGTH;
        }

        NdisMoveMemory(Adapter->Multicast, InformationBuffer, InformationBufferLength);
        Adapter->MulticastCount = InformationBufferLength / ETH_LENGTH_OF_ADDRESS;
        *BytesRead = InformationBufferLength;
        break;

    case OID_GEN_CURRENT_PACKET_FILTER:
        if (InformationBufferLength != sizeof(ULONG)) {
            *BytesNeeded = sizeof(ULONG);
            return NDIS_STATUS_INVALID_LENGTH;
        }

        NdisMoveMemory(&Filter, InformationBuffer, sizeof(ULONG));
        *BytesRead = sizeof(ULONG);
        if (Filter & (NDIS_PACKET_TYPE_SOURCE_ROUTING | NDIS_PACKET_TYPE_SMT | NDIS_PACKET_TYPE_MAC_FRAME |
                      NDIS_PACKET_TYPE_FUNCTIONAL | NDIS_PACKET_TYPE_ALL_FUNCTIONAL | NDIS_PACKET_TYPE_GROUP)) {
            return NDIS_STATUS_NOT_SUPPORTED;
        }

        Adapter->PacketFilter = Filter;
        break;

    case OID_GEN_CURRENT_LOOKAHEAD:
    case OID_GEN_PROTOCOL_OPTIONS:
        *BytesRead = InformationBufferLength;
        return NDIS_STATUS_SUCCESS;

    default:
        return NDIS_STATUS_INVALID_OID;
    }

    Adapter->FilterChanged = TRUE;
    KeSetEvent(&Adapter->WorkEvent, 0, FALSE);
    return NDIS_STATUS_SUCCESS;
}

NTSTATUS
DriverEntry(
    IN PDRIVER_OBJECT DriverObject,
    IN PUNICODE_STRING RegistryPath
    )

/*++

Routine Description:

    This routine registers the miniport with the wrapper, which initializes
    each adapter of its service.

Arguments:

    DriverObject - Supplies the driver object.

    RegistryPath - Supplies the service key of the driver.

Return Value:

    The status of the registration.

--*/

{
    NDIS_MINIPORT_CHARACTERISTICS Characteristics;
    NDIS_HANDLE WrapperHandle;
    NDIS_STATUS Status;

    NdisMInitializeWrapper(&WrapperHandle, DriverObject, RegistryPath, NULL);
    NdisZeroMemory(&Characteristics, sizeof(Characteristics));
    Characteristics.MajorNdisVersion = BCMWLAN_NDIS_MAJOR_VERSION;
    Characteristics.MinorNdisVersion = BCMWLAN_NDIS_MINOR_VERSION;
    Characteristics.CheckForHangHandler = WlanCheckForHang;
    Characteristics.DisableInterruptHandler = WlanDisableInterrupt;
    Characteristics.EnableInterruptHandler = WlanEnableInterrupt;
    Characteristics.HaltHandler = WlanHalt;
    Characteristics.HandleInterruptHandler = WlanHandleInterrupt;
    Characteristics.InitializeHandler = WlanInitialize;
    Characteristics.ISRHandler = WlanIsr;
    Characteristics.QueryInformationHandler = WlanQueryInformation;
    Characteristics.ReconfigureHandler = NULL;
    Characteristics.ResetHandler = WlanReset;
    Characteristics.SendHandler = WlanSend;
    Characteristics.SetInformationHandler = WlanSetInformation;
    Characteristics.TransferDataHandler = WlanTransferData;

    Status = NdisMRegisterMiniport(WrapperHandle, &Characteristics, sizeof(Characteristics));
    if (Status != NDIS_STATUS_SUCCESS) {
        NdisTerminateWrapper(WrapperHandle, NULL);
        return NDIS_STATUS_FAILURE;
    }

    return NDIS_STATUS_SUCCESS;
}
