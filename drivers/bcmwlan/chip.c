/*++

Module Name:

    chip.c

Abstract:

    The wireless chip of the wireless miniport: a Broadcom/Cypress FullMAC
    chip on SDIO (BCM43430, CYW43436, CYW43455, CYW43456). The host loads the
    chip's firmware into its RAM and talks to it through two SDIO functions:
    F1, a 32 KB window onto the chip's backplane (its cores' registers and
    RAM), and F2, a FIFO of SDPCM frames: control requests and responses
    (CDC), events and Ethernet frames (BDC). The firmware does the rest of
    802.11, including the WPA2 supplicant.

    Bring-up (ChipStart): the SDIO interface, the ALP clock, the chip id and
    its cores (from the enumeration ROM), the ARM core halted, the firmware
    and the NVRAM (key=value text) written into RAM, the ARM released, the HT
    clock, F2 ready (the firmware runs); then the regulatory data (CLM), the
    station address, the country and the event mask, and the radio up. The
    first scan's networks are logged.

    Every routine runs in the thread of the miniport or in its
    initialization, at PASSIVE_LEVEL: a control request polls the chip for
    its response, and handles the frames that come first.

    The protocol and the chip handling follow the cyw43-driver (MIT license,
    Copyright (C) 2019-2026 George Robotics Pty Ltd) and the documented
    behaviour of Linux's brcmfmac; this is new code.

Environment:

    kernel mode only

--*/

#include "bcmwlan.h"

//
// SDIO: the CCCR, and the Broadcom registers of F1.
//

#define CCCR_IOEX                   0x02
#define CCCR_IORX                   0x03
#define CCCR_IENX                   0x04
#define CCCR_ABORT                  0x06

#define SB_WATERMARK                0x10008
#define SB_DEVICE_CTL               0x10009
#define SB_ADDR_LOW                 0x1000A
#define SB_ADDR_MID                 0x1000B
#define SB_ADDR_HIGH                0x1000C
#define SB_FRAME_CTRL               0x1000D
#define SB_CHIP_CLOCK_CSR           0x1000E
#define SB_PULLUP                   0x1000F
#define SB_RFRAME_BC_LOW            0x1001B
#define SB_RFRAME_BC_HIGH           0x1001C
#define SB_MES_BUSY_CTRL            0x1001D
#define SB_SLEEP_CSR                0x1001F

#define SB_ACCESS_4B                0x8000
#define SB_WINDOW_MASK              0xFFFF8000
#define SB_OFFSET_MASK              0x7FFF

#define CLK_FORCE_ALP               0x01
#define CLK_FORCE_HT                0x02
#define CLK_ALP_REQ                 0x08
#define CLK_HT_REQ                  0x10
#define CLK_HW_REQ_OFF              0x20
#define CLK_ALP_AVAIL               0x40
#define CLK_HT_AVAIL                0x80

//
// The backplane: chip common and its registers, the core ids of the
// enumeration ROM, and the AI wrapper of a core.
//

#define CHIPCOMMON_BASE             0x18000000
#define CC_EROM_PTR                 0xFC

#define CORE_CHIPCOMMON             0x800
#define CORE_SOCRAM                 0x80E
#define CORE_80211                  0x812
#define CORE_PMU                    0x827
#define CORE_SDIO_DEV               0x829
#define CORE_ARM_CM3                0x82A
#define CORE_ARM_CR4                0x83E
#define CORE_GCI                    0x840

#define AI_IOCTRL                   0x408
#define AI_RESETCTRL                0x800
#define IOCTRL_CLK                  0x01
#define IOCTRL_FGC                  0x02
#define IOCTRL_PHYCLOCKEN           0x04
#define IOCTRL_PHYRESET             0x08
#define IOCTRL_CPUHALT              0x20

#define CHIP_43430                  0xA9A6
#define CHIP_4345                   0x4345

//
// The SDIO device core of the chip.
//

#define SDIO_INTSTATUS              0x20
#define SDIO_HOSTINTMASK            0x24
#define SDIO_TOSBMAILBOX            0x40
#define SDIO_TOSBMAILBOXDATA        0x48
#define SDIO_TOHOSTMAILBOXDATA      0x4C

#define I_HMB_FC_STATE              0x10
#define I_HMB_FC_CHANGE             0x20
#define I_HMB_FRAME_IND             0x40
#define I_HMB_HOST_INT              0x80
#define HOSTINTMASK                 0x200000F0

#define SMB_INT_ACK                 0x02
#define HMB_DATA_NAKHANDLED         0x01
#define HMB_DATA_FWHALT             0x10

//
// SDPCM, CDC and BDC.
//

#define SDPCM_HEADER                12
#define CDC_HEADER                  16
#define BDC_HEADER                  4
#define CHANNEL_CONTROL             0
#define CHANNEL_EVENT               1
#define CHANNEL_DATA                2
#define CHANNEL_GLOM                3
#define CDC_FLAG_ERROR              0x01
#define CDC_FLAG_SET                0x02
#define BDC_VERSION                 2

//
// Ioctls and their values.
//

#define WLC_UP                      2
#define WLC_SET_PROMISC             10
#define WLC_SET_INFRA               20
#define WLC_SET_AUTH                22
#define WLC_GET_BSSID               23
#define WLC_SET_SSID                26
#define WLC_DISASSOC                52
#define WLC_SET_PM                  86
#define WLC_GET_RSSI                127
#define WLC_SET_WSEC                134
#define WLC_SET_WPA_AUTH            165
#define WLC_GET_VAR                 262
#define WLC_SET_VAR                 263
#define WLC_SET_WSEC_PMK            268

#define WSEC_AES                    4
#define WPA_AUTH_DISABLED           0
#define WPA2_AUTH_PSK               0x80
#define WPA3_AUTH_SAE_PSK           0x40000
#define WSEC_PASSPHRASE             1

//
// Events, their status and flags, and their reasons: the supplicant's, a
// pruned network's, and those of 802.11.
//

#define E_SET_SSID                  0
#define E_JOIN                      1
#define E_AUTH                      3
#define E_DEAUTH                    5
#define E_DEAUTH_IND                6
#define E_ASSOC                     7
#define E_REASSOC                   9
#define E_DISASSOC                  11
#define E_DISASSOC_IND              12
#define E_LINK                      16
#define E_ROAM                      19
#define E_PRUNE                     23
#define E_PSK_SUP                   46
#define E_ESCAN_RESULT              69

#define E_STATUS_SUCCESS            0
#define E_STATUS_TIMEOUT            2
#define E_STATUS_NO_NETWORKS        3
#define E_STATUS_NO_ACK             5
#define E_STATUS_UNSOLICITED        6
#define E_STATUS_PARTIAL            8

#define E_FLAG_LINK                 0x0001

#define SUP_KEYED                   6
#define E_SUP_WPA_PSK_TMO           15
#define E_SUP_WPA_PSK_M1_TMO        16
#define E_SUP_WPA_PSK_M3_TMO        17

#define E_PRUNE_RSN_MISMATCH        8

#define DOT11_RC_AUTH_INVALID       2
#define DOT11_RC_4WAY_TIMEOUT       15
#define DOT11_RC_8021X_FAILED       23

#define IOCTL_TIMEOUT_MS            2500
#define JOIN_TIMEOUT_MS             20000
#define REJOIN_DELAY_MS             10000
#define CLM_CHUNK                   1400

#define BCME_TIMEOUT                (-100)

static const PCHAR EventNames[] = {
    "SET_SSID", "JOIN", "START", "AUTH", "AUTH_IND", "DEAUTH", "DEAUTH_IND", "ASSOC", "ASSOC_IND",
    "REASSOC", "REASSOC_IND", "DISASSOC", "DISASSOC_IND", "QUIET_START", "QUIET_END", "BEACON_RX",
    "LINK", "MIC_ERROR", "NDIS_LINK", "ROAM", "TXFAIL", "PMKID_CACHE", "RETROGRADE_TSF", "PRUNE"
};

static VOID
ChipProcessFrames(
    IN PWLAN_ADAPTER Adapter,
    IN ULONG Bound
    );

static VOID
ChipPoll(
    IN PWLAN_ADAPTER Adapter
    );

//
// Little and big endian fields of byte strings.
//

static ULONG
Le16(
    IN PUCHAR Bytes
    )
{
    return Bytes[0] | ((ULONG)Bytes[1] << 8);
}

static ULONG
Le32(
    IN PUCHAR Bytes
    )
{
    return Bytes[0] | ((ULONG)Bytes[1] << 8) | ((ULONG)Bytes[2] << 16) | ((ULONG)Bytes[3] << 24);
}

static ULONG
Be16(
    IN PUCHAR Bytes
    )
{
    return ((ULONG)Bytes[0] << 8) | Bytes[1];
}

static ULONG
Be32(
    IN PUCHAR Bytes
    )
{
    return ((ULONG)Bytes[0] << 24) | ((ULONG)Bytes[1] << 16) | ((ULONG)Bytes[2] << 8) | Bytes[3];
}

static VOID
PutLe16(
    OUT PUCHAR Bytes,
    IN ULONG Value
    )
{
    Bytes[0] = (UCHAR)Value;
    Bytes[1] = (UCHAR)(Value >> 8);
}

static VOID
PutLe32(
    OUT PUCHAR Bytes,
    IN ULONG Value
    )
{
    Bytes[0] = (UCHAR)Value;
    Bytes[1] = (UCHAR)(Value >> 8);
    Bytes[2] = (UCHAR)(Value >> 16);
    Bytes[3] = (UCHAR)(Value >> 24);
}

static VOID
ChipSleep(
    IN ULONG Milliseconds
    )
{
    LARGE_INTEGER Interval;

    Interval.QuadPart = -10000 * (LONGLONG)Milliseconds;
    KeDelayExecutionThread(KernelMode, FALSE, &Interval);
}

//
// Milliseconds of the performance counter: unlike the system time, it does
// not move when the clock is set.
//

static ULONG
NowMs(
    VOID
    )
{
    LARGE_INTEGER Frequency;
    LARGE_INTEGER Counter = KeQueryPerformanceCounter(&Frequency);

    if (Frequency.QuadPart == 0) {
        return 0;
    }

    return (ULONG)(Counter.QuadPart * 1000 / Frequency.QuadPart);
}

//
// The backplane, through F1.
//

static BOOLEAN
BpSetWindow(
    IN PWLAN_ADAPTER Adapter,
    IN ULONG Address
    )
{
    ULONG Window = Address & SB_WINDOW_MASK;

    if (Window == Adapter->BackplaneWindow) {
        return TRUE;
    }

    if (!SdioWrite8(Adapter, 1, SB_ADDR_LOW, (UCHAR)(Window >> 8)) ||
        !SdioWrite8(Adapter, 1, SB_ADDR_MID, (UCHAR)(Window >> 16)) ||
        !SdioWrite8(Adapter, 1, SB_ADDR_HIGH, (UCHAR)(Window >> 24))) {
        Adapter->BackplaneWindow = 0xFFFFFFFF;
        return FALSE;
    }

    Adapter->BackplaneWindow = Window;
    return TRUE;
}

static ULONG
BpRead32(
    IN PWLAN_ADAPTER Adapter,
    IN ULONG Address
    )
{
    ULONG Value = 0xFFFFFFFF;

    if (BpSetWindow(Adapter, Address)) {
        SdioCmd53(Adapter, 1, (Address & SB_OFFSET_MASK) | SB_ACCESS_4B, &Value, 4, FALSE, TRUE);
    }

    return Value;
}

static BOOLEAN
BpWrite32(
    IN PWLAN_ADAPTER Adapter,
    IN ULONG Address,
    IN ULONG Value
    )
{
    return BpSetWindow(Adapter, Address) &&
           SdioCmd53(Adapter, 1, (Address & SB_OFFSET_MASK) | SB_ACCESS_4B, &Value, 4, TRUE, TRUE);
}

//
// Move a block of the backplane (the chip's RAM): the buffer is longword
// aligned and its length a multiple of four; a transfer stays within a
// window.
//

static BOOLEAN
BpMove(
    IN PWLAN_ADAPTER Adapter,
    IN ULONG Address,
    IN PUCHAR Buffer,
    IN ULONG Length,
    IN BOOLEAN Write
    )
{
    ULONG Chunk;

    while (Length != 0) {
        Chunk = 0x8000 - (Address & SB_OFFSET_MASK);
        if (Chunk > Length) {
            Chunk = Length;
        }

        if (Chunk > 2048) {
            Chunk = 2048;
        }

        if (!BpSetWindow(Adapter, Address) ||
            !SdioCmd53(Adapter, 1, (Address & SB_OFFSET_MASK) | SB_ACCESS_4B, Buffer, Chunk, Write, TRUE)) {
            return FALSE;
        }

        Address += Chunk;
        Buffer += Chunk;
        Length -= Chunk;
    }

    return TRUE;
}

//
// The AI wrapper of a core: disable it (held in reset), reset it, and
// whether it is up.
//

static VOID
AiCoreDisable(
    IN PWLAN_ADAPTER Adapter,
    IN ULONG Wrapper,
    IN ULONG Pre,
    IN ULONG Reset
    )
{
    ULONG Index;

    if ((BpRead32(Adapter, Wrapper + AI_RESETCTRL) & 1) == 0) {
        BpWrite32(Adapter, Wrapper + AI_IOCTRL, Pre | IOCTRL_FGC | IOCTRL_CLK);
        BpRead32(Adapter, Wrapper + AI_IOCTRL);
        BpWrite32(Adapter, Wrapper + AI_RESETCTRL, 1);
        KeStallExecutionProcessor(20);
        for (Index = 0; Index < 30 && (BpRead32(Adapter, Wrapper + AI_RESETCTRL) & 1) == 0; Index += 1) {
            KeStallExecutionProcessor(10);
        }
    }

    BpWrite32(Adapter, Wrapper + AI_IOCTRL, Reset | IOCTRL_FGC | IOCTRL_CLK);
    BpRead32(Adapter, Wrapper + AI_IOCTRL);
}

static VOID
AiResetCore(
    IN PWLAN_ADAPTER Adapter,
    IN ULONG Wrapper,
    IN ULONG Pre,
    IN ULONG Reset,
    IN ULONG Post
    )
{
    ULONG Index;

    AiCoreDisable(Adapter, Wrapper, Pre, Reset);
    for (Index = 0; Index < 50 && (BpRead32(Adapter, Wrapper + AI_RESETCTRL) & 1) != 0; Index += 1) {
        BpWrite32(Adapter, Wrapper + AI_RESETCTRL, 0);
        KeStallExecutionProcessor(50);
    }

    BpWrite32(Adapter, Wrapper + AI_IOCTRL, Post | IOCTRL_CLK);
    BpRead32(Adapter, Wrapper + AI_IOCTRL);
}

static BOOLEAN
AiCoreIsUp(
    IN PWLAN_ADAPTER Adapter,
    IN ULONG Wrapper
    )
{
    return (BpRead32(Adapter, Wrapper + AI_IOCTRL) & (IOCTRL_FGC | IOCTRL_CLK)) == IOCTRL_CLK &&
           (BpRead32(Adapter, Wrapper + AI_RESETCTRL) & 1) == 0;
}

//
// The cores, from the enumeration ROM: the register and wrapper base of the
// ARM, SOCRAM, SDIO device and 802.11 cores.
//

typedef struct _CHIP_CORES {
    ULONG Arm;
    ULONG ArmWrapper;
    ULONG ArmId;
    ULONG Socram;
    ULONG SocramWrapper;
    ULONG SocramRevision;
    ULONG Sdio;
    ULONG SdioRevision;
    ULONG D11Wrapper;
} CHIP_CORES, *PCHIP_CORES;

static ULONG
EromType(
    IN ULONG Descriptor
    )
{
    ULONG Type = Descriptor & 0xF;

    return (Type & ~8) == 5 ? 5 : Type;
}

static BOOLEAN
ChipScanCores(
    IN PWLAN_ADAPTER Adapter,
    OUT PCHIP_CORES Cores
    )
{
    ULONG Pointer = BpRead32(Adapter, CHIPCOMMON_BASE + CC_EROM_PTR);
    ULONG End = Pointer + 0x1000;
    ULONG Descriptor;
    ULONG Second;
    ULONG Type;
    ULONG Id;
    ULONG Revision;
    ULONG WrapType;
    ULONG RegBase;
    ULONG WrapBase;
    ULONG Size;
    ULONG SlaveType;
    ULONG Masters;
    ULONG Slaves;

    NdisZeroMemory(Cores, sizeof(CHIP_CORES));
    WlanLog(Adapter, "EROM at %08lx", Pointer);
    if (Pointer == 0 || Pointer == 0xFFFFFFFF) {
        return FALSE;
    }

    while (Pointer < End) {
        Descriptor = BpRead32(Adapter, Pointer);
        Pointer += 4;
        Type = EromType(Descriptor);
        if (Type == 0xF) {
            break;
        }

        if ((Descriptor & 1) == 0 || Type != 1) {
            continue;
        }

        Id = (Descriptor >> 8) & 0xFFF;
        Second = BpRead32(Adapter, Pointer);
        Pointer += 4;
        Masters = (Second >> 14) & 0x1F;
        Slaves = (Second >> 19) & 0x1F;
        Revision = Second >> 24;
        if (Masters + Slaves == 0 && Id != CORE_PMU && Id != CORE_GCI) {
            continue;
        }

        Descriptor = BpRead32(Adapter, Pointer);
        Pointer += 4;
        Type = EromType(Descriptor);
        if (Type == 3) {
            WrapType = 3;
        } else if (Type == 5) {
            Pointer -= 4;
            WrapType = 2;
        } else {
            Pointer -= 4;
            continue;
        }

        RegBase = WrapBase = 0;
        while (RegBase == 0 || WrapBase == 0) {
            do {
                Descriptor = BpRead32(Adapter, Pointer);
                Pointer += 4;
                Type = EromType(Descriptor);
                if (Type == 0xF || Pointer >= End) {
                    Pointer -= 4;
                    goto Next;
                }
            } while (Type != 5 && Type != 1);

            if (Type == 1) {
                Pointer -= 4;
                break;
            }

            if (Descriptor & 8) {
                Pointer += 4;
            }

            Size = (Descriptor >> 4) & 3;
            if (Size == 3) {
                Second = BpRead32(Adapter, Pointer);
                Pointer += 4;
                if (Second & 8) {
                    Pointer += 4;
                }
            }

            if (Size > 1) {
                continue;
            }

            SlaveType = (Descriptor >> 6) & 3;
            if (RegBase == 0 && SlaveType == 0) {
                RegBase = Descriptor & 0xFFFFF000;
            }

            if (WrapBase == 0 && SlaveType == WrapType) {
                WrapBase = Descriptor & 0xFFFFF000;
            }
        }

        WlanLog(Adapter, "Core %03lx rev %lu at %08lx, wrapper %08lx", Id, Revision, RegBase, WrapBase);
        switch (Id) {
        case CORE_ARM_CM3:
        case CORE_ARM_CR4:
            Cores->Arm = RegBase;
            Cores->ArmWrapper = WrapBase;
            Cores->ArmId = Id;
            break;

        case CORE_SOCRAM:
            Cores->Socram = RegBase;
            Cores->SocramWrapper = WrapBase;
            Cores->SocramRevision = Revision;
            break;

        case CORE_SDIO_DEV:
            Cores->Sdio = RegBase;
            Cores->SdioRevision = Revision;
            break;

        case CORE_80211:
            if (Cores->D11Wrapper == 0) {
                Cores->D11Wrapper = WrapBase;
            }
            break;
        }

Next:
        ;
    }

    return Cores->Arm != 0 && Cores->ArmWrapper != 0 && Cores->Sdio != 0;
}

//
// The RAM of the chip: the SOCRAM's banks of a CM3 chip at 0, the CR4's
// tightly coupled memory of a 4345 at 0x198000.
//

static VOID
ChipRamSize(
    IN PWLAN_ADAPTER Adapter,
    IN PCHIP_CORES Cores
    )
{
    ULONG Info;
    ULONG Banks;
    ULONG BankSize;
    ULONG Lss;
    ULONG Index;
    ULONG Bank;
    ULONG Size = 0;

    if (Adapter->ArmCr4) {
        Adapter->RamBase = 0x198000;
        Info = BpRead32(Adapter, Cores->Arm + 0x04);
        Banks = (Info & 0xF) + ((Info >> 4) & 0xF);
        for (Index = 0; Index < Banks; Index += 1) {
            BpWrite32(Adapter, Cores->Arm + 0x40, Index);
            Bank = BpRead32(Adapter, Cores->Arm + 0x44);
            Size += ((Bank & 0x7F) + 1) * ((Bank & 0x200) != 0 ? 1024 : 8192);
        }

        if (Size == 0 || Size > 0x200000) {
            Size = 0xC8000;
        }
    } else {
        Adapter->RamBase = 0;
        Info = BpRead32(Adapter, Cores->Socram);
        if (Cores->SocramRevision <= 7 || Cores->SocramRevision == 12) {
            Banks = (Info >> 4) & 0xF;
            BankSize = Info & 0xF;
            Lss = (Info >> 20) & 0xF;
            Size = (Banks - (Lss != 0 ? 1 : 0)) << (BankSize + 14);
            if (Lss != 0) {
                Size += 1 << (Lss - 1 + 14);
            }
        } else {
            Banks = (Info >> 4) & (Cores->SocramRevision >= 23 ? 0x1F : 0xF);
            for (Index = 0; Index < Banks; Index += 1) {
                BpWrite32(Adapter, Cores->Socram + 0x10, Index);
                Bank = BpRead32(Adapter, Cores->Socram + 0x40);
                Size += ((Bank & 0x7F) + 1) * 8192;
            }
        }

        if (Size == 0 || Size > 0x100000) {
            Size = 0x80000;
        }
    }

    Adapter->RamSize = Size;
}

//
// The chip's files: by chip, revision and board.
//

static PCHAR
ChipFileBase(
    IN PWLAN_ADAPTER Adapter
    )
{
    ULONG BoardType = (Adapter->BoardRevision & (1 << 23)) != 0 ? (Adapter->BoardRevision >> 4) & 0xFF : 0;

    if (Adapter->ChipId == CHIP_43430) {
        if (Adapter->ChipRevision >= 2) {
            return "43436";
        }

        return BoardType == 0x12 ? "43436S" : "43430";
    }

    if (Adapter->ChipId == CHIP_4345) {
        return Adapter->ChipRevision == 9 ? "43456" : "43455";
    }

    return NULL;
}

static PUCHAR
ChipReadFile(
    IN PWLAN_ADAPTER Adapter,
    IN PCHAR Base,
    IN PCHAR Extension,
    OUT PULONG Length
    )
{
    WCHAR Path[80];
    PWCHAR Next = Path;
    PCHAR Prefix = "\\SystemRoot\\System32\\Drivers\\BCMWLAN\\";
    PCHAR Part;
    PUCHAR Data;

    for (Part = Prefix; *Part != 0; Part += 1) {
        *Next++ = *Part;
    }

    for (Part = Base; *Part != 0; Part += 1) {
        *Next++ = *Part;
    }

    *Next++ = L'.';
    for (Part = Extension; *Part != 0; Part += 1) {
        *Next++ = *Part;
    }

    *Next = 0;
    Data = WlanReadFile(Adapter, Path, Length);
    WlanLog(Adapter, "%s.%s: %s (%lu bytes)", Base, Extension, Data != NULL ? "read" : "not found", *Length);
    return Data;
}

//
// The NVRAM: key=value lines of text, made NUL separated, padded to a
// longword after an extra NUL, and closed by its length token (in words, and
// its complement) for the last word of RAM.
//

static ULONG
ChipFormatNvram(
    IN PUCHAR Text,
    IN ULONG Length,
    OUT PUCHAR Out
    )
{
    ULONG In = 0;
    ULONG Size = 0;
    ULONG Start;
    ULONG Words;
    BOOLEAN BoardRev = FALSE;
    UCHAR Character;

    while (In < Length) {
        Character = Text[In];
        if (Character == '#') {
            while (In < Length && Text[In] != '\n') {
                In += 1;
            }

            continue;
        }

        if (Character <= ' ' || Character > 0x7E) {
            In += 1;
            continue;
        }

        Start = In;
        while (In < Length && Text[In] != '=' && Text[In] > ' ' && Text[In] <= 0x7E) {
            In += 1;
        }

        if (In >= Length || Text[In] != '=' || (In - Start >= 4 && RtlCompareMemory(Text + Start, "RAW1", 4) == 4)) {
            while (In < Length && Text[In] != '\n') {
                In += 1;
            }

            continue;
        }

        In += 1;
        while (In < Length && Text[In] >= ' ' && Text[In] <= 0x7E && Text[In] != '#') {
            In += 1;
        }

        if (In - Start >= 9 && RtlCompareMemory(Text + Start, "boardrev=", 9) == 9) {
            BoardRev = TRUE;
        }

        NdisMoveMemory(Out + Size, Text + Start, In - Start);
        Size += In - Start;
        Out[Size++] = 0;
    }

    if (!BoardRev) {
        NdisMoveMemory(Out + Size, "boardrev=0xff", 14);
        Size += 14;
    }

    Out[Size++] = 0;
    while (Size % 4 != 0) {
        Out[Size++] = 0;
    }

    Words = Size / 4;
    PutLe32(Out + Size, ((~Words & 0xFFFF) << 16) | (Words & 0xFFFF));
    return Size + 4;
}

//
// SDPCM: send a frame to F2 (its header at the start of a longword aligned
// buffer with room for the padding), and the credit the firmware gives.
//

static BOOLEAN
ChipCanSend(
    IN PWLAN_ADAPTER Adapter
    )
{
    UCHAR Window = (UCHAR)(Adapter->TxMaxSequence - Adapter->TxSequence);

    return Window != 0 && (Window & 0x80) == 0;
}

static BOOLEAN
ChipSendFrame(
    IN PWLAN_ADAPTER Adapter,
    IN PUCHAR Frame,
    IN ULONG Length,
    IN ULONG Channel,
    IN ULONG DataOffset
    )
{
    ULONG Padded;

    PutLe16(Frame, Length);
    PutLe16(Frame + 2, ~Length & 0xFFFF);
    Frame[4] = Adapter->TxSequence;
    Frame[5] = (UCHAR)Channel;
    Frame[6] = 0;
    Frame[7] = (UCHAR)DataOffset;
    Frame[8] = Frame[9] = Frame[10] = Frame[11] = 0;
    Padded = (Length + 3) & ~3;
    if (Padded > 512) {
        Padded = (Padded + Adapter->F2BlockSize - 1) & ~(Adapter->F2BlockSize - 1);
    }

    NdisZeroMemory(Frame + Length, Padded - Length);
    if (!SdioCmd53(Adapter, 2, 0, Frame, Padded, TRUE, TRUE)) {
        WlanLog(Adapter, "F2 write of %lu bytes failed (status %08lx)", Padded, Adapter->LastSdioStatus);
        SdioWrite8(Adapter, 0, CCCR_ABORT, 2);
        SdioWrite8(Adapter, 1, SB_FRAME_CTRL, 0x02);
        return FALSE;
    }

    Adapter->TxSequence += 1;
    return TRUE;
}

//
// Wait for credit to send, reading what the chip has meanwhile.
//

static BOOLEAN
ChipWaitCredit(
    IN PWLAN_ADAPTER Adapter,
    IN ULONG TimeoutMs
    )
{
    ULONG Start = NowMs();

    while (!ChipCanSend(Adapter)) {
        if (NowMs() - Start > TimeoutMs) {
            return FALSE;
        }

        ChipPoll(Adapter);
        if (!ChipCanSend(Adapter)) {
            BpWrite32(Adapter, Adapter->SdioCoreBase + SDIO_TOSBMAILBOX, 0x08);
            ChipSleep(2);
        }
    }

    return TRUE;
}

//
// A control request (an ioctl, or an iovar through WLC_GET_VAR/WLC_SET_VAR):
// the CDC header and the data, and the response's data copied back into it
// for a get. Returns the firmware's status (0 or a negative BCME code), or
// BCME_TIMEOUT.
//

static LONG
ChipIoctl(
    IN PWLAN_ADAPTER Adapter,
    IN ULONG Command,
    IN BOOLEAN Set,
    IN OUT PVOID Data,
    IN ULONG Length,
    IN ULONG ResponseLength
    )
{
    PUCHAR Frame = Adapter->ControlBuffer;
    ULONG Start;
    ULONG Total = SDPCM_HEADER + CDC_HEADER + Length;

    if (Total + 64 > CONTROL_BUFFER_SIZE || ResponseLength > RECEIVE_BUFFER_SIZE - 64) {
        return -24;
    }

    if (!ChipWaitCredit(Adapter, 1000)) {
        WlanLog(Adapter, "No credit to send ioctl %lu", Command);
        return BCME_TIMEOUT;
    }

    Adapter->ControlId += 1;
    Adapter->ResponseId = Adapter->ControlId;
    Adapter->ResponseReady = FALSE;
    PutLe32(Frame + SDPCM_HEADER, Command);
    PutLe32(Frame + SDPCM_HEADER + 4, Set ? Length : (ResponseLength > Length ? ResponseLength : Length));
    PutLe32(Frame + SDPCM_HEADER + 8, ((ULONG)Adapter->ControlId << 16) | (Set ? CDC_FLAG_SET : 0));
    PutLe32(Frame + SDPCM_HEADER + 12, 0);
    if (Length != 0) {
        NdisMoveMemory(Frame + SDPCM_HEADER + CDC_HEADER, Data, Length);
    }

    if (!ChipSendFrame(Adapter, Frame, Total, CHANNEL_CONTROL, SDPCM_HEADER)) {
        return BCME_TIMEOUT;
    }

    Start = NowMs();
    while (!Adapter->ResponseReady) {
        if (NowMs() - Start > IOCTL_TIMEOUT_MS) {
            WlanLog(Adapter, "Ioctl %lu timed out", Command);
            return BCME_TIMEOUT;
        }

        ChipPoll(Adapter);
        if (!Adapter->ResponseReady) {
            ChipSleep(1);
        }
    }

    if (!Set && Adapter->ResponseStatus == 0 && Data != NULL) {
        NdisMoveMemory(Data, Adapter->ResponseBuffer,
                       Adapter->ResponseLength < Length ? Adapter->ResponseLength : Length);
    }

    return Adapter->ResponseStatus;
}

static LONG
ChipSetIoctl32(
    IN PWLAN_ADAPTER Adapter,
    IN ULONG Command,
    IN ULONG Value
    )
{
    UCHAR Data[4];

    PutLe32(Data, Value);
    return ChipIoctl(Adapter, Command, TRUE, Data, 4, 0);
}

static LONG
ChipVar(
    IN PWLAN_ADAPTER Adapter,
    IN PCHAR Name,
    IN BOOLEAN Set,
    IN OUT PVOID Data,
    IN ULONG Length
    )
{
    PUCHAR Buffer = Adapter->VarBuffer;
    ULONG NameLength = strlen(Name) + 1;
    LONG Status;

    if (NameLength + Length + 4 > CONTROL_BUFFER_SIZE - 128) {
        return -24;
    }

    NdisMoveMemory(Buffer, Name, NameLength);
    if (Set) {
        NdisMoveMemory(Buffer + NameLength, Data, Length);
        return ChipIoctl(Adapter, WLC_SET_VAR, TRUE, Buffer, NameLength + Length, 0);
    }

    NdisZeroMemory(Buffer + NameLength, Length);
    Status = ChipIoctl(Adapter, WLC_GET_VAR, FALSE, Buffer, NameLength + Length > Length + 4 ?
                       NameLength + Length : Length + 4, 0);
    if (Status == 0) {
        NdisMoveMemory(Data, Buffer, Length);
    }

    return Status;
}

static LONG
ChipSetVar32(
    IN PWLAN_ADAPTER Adapter,
    IN PCHAR Name,
    IN ULONG Value
    )
{
    UCHAR Data[4];

    PutLe32(Data, Value);
    return ChipVar(Adapter, Name, TRUE, Data, 4);
}

static LONG
ChipGetVar32(
    IN PWLAN_ADAPTER Adapter,
    IN PCHAR Name,
    OUT PULONG Value
    )
{
    UCHAR Data[4];
    LONG Status = ChipVar(Adapter, Name, FALSE, Data, 4);

    *Value = Le32(Data);
    return Status;
}

static LONG
ChipSetBss32(
    IN PWLAN_ADAPTER Adapter,
    IN PCHAR Name,
    IN ULONG Value
    )
{
    CHAR Full[48];
    UCHAR Data[8];

    strcpy(Full, "bsscfg:");
    strcat(Full, Name);
    PutLe32(Data, 0);
    PutLe32(Data + 4, Value);
    return ChipVar(Adapter, Full, TRUE, Data, 8);
}

//
// An event of the firmware: its message (big endian) after the Ethernet and
// Broadcom headers.
//

static VOID
ChipLogScanResult(
    IN PWLAN_ADAPTER Adapter,
    IN PUCHAR Data,
    IN ULONG Length
    )
{
    PUCHAR Bss;
    PUCHAR Ie;
    PUCHAR End;
    ULONG BssLength;
    ULONG Index;
    ULONG IeOffset;
    ULONG IeLength;
    ULONG SsidLength;
    ULONG Akm;
    ULONG Count;
    ULONG Caps;
    CHAR Ssid[33];
    CHAR Security[32];
    BOOLEAN Rsn = FALSE;
    BOOLEAN Wpa = FALSE;
    BOOLEAN Sae = FALSE;
    BOOLEAN Psk = FALSE;
    LONG Rssi;

    if (Length < 12 + 128) {
        return;
    }

    Bss = Data + 12;
    BssLength = Le32(Bss + 4);
    if (BssLength < 128 || BssLength > Length - 12) {
        return;
    }

    for (Index = 0; Index < Adapter->ScanCount; Index += 1) {
        if (RtlCompareMemory(Adapter->ScanBssid[Index], Bss + 8, 6) == 6) {
            return;
        }
    }

    if (Adapter->ScanCount < 32) {
        NdisMoveMemory(Adapter->ScanBssid[Adapter->ScanCount], Bss + 8, 6);
        Adapter->ScanCount += 1;
    }

    SsidLength = Bss[18] > 32 ? 32 : Bss[18];
    NdisMoveMemory(Ssid, Bss + 19, SsidLength);
    Ssid[SsidLength] = 0;
    for (Index = 0; Index < SsidLength; Index += 1) {
        if (Ssid[Index] < ' ' || Ssid[Index] > 0x7E) {
            Ssid[Index] = '?';
        }
    }

    Caps = Le16(Bss + 16);
    Rssi = (SHORT)Le16(Bss + 78);
    IeOffset = Le16(Bss + 116);
    IeLength = Le32(Bss + 120);
    if (IeOffset + IeLength <= BssLength) {
        Ie = Bss + IeOffset;
        End = Ie + IeLength;
        while (Ie + 2 <= End && Ie + 2 + Ie[1] <= End) {
            if (Ie[0] == 48 && Ie[1] >= 8) {
                Rsn = TRUE;
                Count = Le16(Ie + 8);
                if (10 + 4 * Count + 2 <= (ULONG)Ie[1] + 2) {
                    Akm = 10 + 4 * Count;
                    Count = Le16(Ie + Akm);
                    for (Index = 0; Index < Count && Akm + 2 + 4 * Index + 4 <= (ULONG)Ie[1] + 2; Index += 1) {
                        switch (Ie[Akm + 2 + 4 * Index + 3]) {
                        case 2:
                        case 6:
                            Psk = TRUE;
                            break;

                        case 8:
                        case 9:
                            Sae = TRUE;
                            break;
                        }
                    }
                }
            } else if (Ie[0] == 221 && Ie[1] >= 4 && Ie[2] == 0x00 && Ie[3] == 0x50 && Ie[4] == 0xF2 &&
                       Ie[5] == 1) {
                Wpa = TRUE;
            }

            Ie += 2 + Ie[1];
        }
    }

    if (Rsn) {
        strcpy(Security, Psk && Sae ? "WPA2/WPA3" : Sae ? "WPA3" : Psk ? "WPA2" : "WPA2-Enterprise");
    } else if (Wpa) {
        strcpy(Security, "WPA");
    } else if (Caps & 0x10) {
        strcpy(Security, "WEP");
    } else {
        strcpy(Security, "open");
    }

    WlanLog(Adapter, "  %-32s %02x:%02x:%02x:%02x:%02x:%02x ch %3lu %4ld dBm %s", Ssid, Bss[8], Bss[9], Bss[10],
            Bss[11], Bss[12], Bss[13], Bss[88] != 0 ? Bss[88] : Le16(Bss + 72) & 0xFF, Rssi, Security);
}

static VOID
ChipJoinFailed(
    IN PWLAN_ADAPTER Adapter,
    IN PCHAR Why
    )
{
    CHAR Text[160];

    sprintf(Text, "Join failed: %s; again in %u s", Why, REJOIN_DELAY_MS / 1000);
    WlanNotice(Adapter, Text);
    Adapter->JoinFailures += 1;
    Adapter->LinkState = LINK_IDLE;
    Adapter->NextJoin.LowPart = NowMs() + REJOIN_DELAY_MS;
}

static VOID
ChipEvent(
    IN PWLAN_ADAPTER Adapter,
    IN PUCHAR Ethernet,
    IN ULONG Length
    )
{
    UCHAR Message[48];
    ULONG Type;
    ULONG Status;
    ULONG Reason;
    ULONG Flags;
    ULONG DataLength;

    if (Length < 72 || Be16(Ethernet + 12) != 0x886C || Ethernet[19] != 0x00 || Ethernet[20] != 0x10 ||
        Ethernet[21] != 0x18 || Be16(Ethernet + 22) != 1) {
        return;
    }

    NdisMoveMemory(Message, Ethernet + 24, sizeof(Message));
    Flags = Be16(Message + 2);
    Type = Be32(Message + 4);
    Status = Be32(Message + 8);
    Reason = Be32(Message + 12);
    DataLength = Be32(Message + 20);
    if (DataLength > Length - 72) {
        DataLength = Length - 72;
    }

    if (Type == E_ESCAN_RESULT) {
        if (Status == E_STATUS_PARTIAL) {
            ChipLogScanResult(Adapter, Ethernet + 72, DataLength);
        } else {
            WlanLog(Adapter, "Scan done (status %lu): %lu networks", Status, Adapter->ScanCount);
            Adapter->Scanning = FALSE;
        }

        return;
    }

    Adapter->LastEvent = Type;
    Adapter->LastEventStatus = Status;
    Adapter->LastEventReason = Reason;
    WlanLog(Adapter, "Event %s (%lu) status %lu reason %lu flags %lx",
            Type < sizeof(EventNames) / sizeof(EventNames[0]) ? EventNames[Type] :
            Type == E_PSK_SUP ? "PSK_SUP" : "?", Type, Status, Reason, Flags);

    switch (Type) {
    case E_SET_SSID:
        if (Adapter->LinkState == LINK_JOINING && Status != E_STATUS_SUCCESS) {
            ChipJoinFailed(Adapter, Status == E_STATUS_NO_NETWORKS ? "network not found" :
                           "the join did not complete");
        }
        break;

    case E_AUTH:
        if (Adapter->LinkState == LINK_JOINING && Status != E_STATUS_SUCCESS && Status != E_STATUS_TIMEOUT &&
            Status != E_STATUS_NO_ACK && Status != E_STATUS_UNSOLICITED) {
            ChipJoinFailed(Adapter, "authentication refused");
        }
        break;

    case E_LINK:
        if (Status == E_STATUS_SUCCESS && (Flags & E_FLAG_LINK) != 0) {
            Adapter->Associated = TRUE;
            if (Adapter->Security == SECURITY_OPEN || Adapter->Keyed) {
                WlanSetLink(Adapter, TRUE);
            }
        } else {
            Adapter->Associated = FALSE;
            Adapter->Keyed = FALSE;
            if (Adapter->LinkState == LINK_UP) {
                WlanSetLink(Adapter, FALSE);
                Adapter->NextJoin.LowPart = NowMs() + 2000;
            } else if (Adapter->LinkState == LINK_JOINING) {
                ChipJoinFailed(Adapter, "the link went down");
            }
        }
        break;

    case E_PSK_SUP:
        if (Status == SUP_KEYED) {
            Adapter->Keyed = TRUE;
            if (Adapter->Associated) {
                WlanSetLink(Adapter, TRUE);
            }
        } else if (Adapter->LinkState == LINK_JOINING) {
            ChipJoinFailed(Adapter, Reason == E_SUP_WPA_PSK_TMO || Reason == E_SUP_WPA_PSK_M1_TMO ||
                           Reason == E_SUP_WPA_PSK_M3_TMO ? "the WPA handshake timed out (wrong password?)" :
                           "the WPA handshake failed (wrong password?)");
        }
        break;

    case E_DEAUTH:
    case E_DISASSOC:

        //
        // The station's own teardown: of an old association when a join
        // starts, or of one that timed out.
        //

        if (Adapter->LinkState == LINK_UP) {
            Adapter->Associated = FALSE;
            Adapter->Keyed = FALSE;
            WlanSetLink(Adapter, FALSE);
            Adapter->NextJoin.LowPart = NowMs() + 2000;
        }
        break;

    case E_DEAUTH_IND:
    case E_DISASSOC_IND:
        Adapter->Associated = FALSE;
        Adapter->Keyed = FALSE;
        if (Adapter->LinkState == LINK_UP) {
            WlanSetLink(Adapter, FALSE);
            Adapter->NextJoin.LowPart = NowMs() + 2000;
        } else if (Adapter->LinkState == LINK_JOINING) {
            ChipJoinFailed(Adapter, Reason == DOT11_RC_AUTH_INVALID || Reason == DOT11_RC_4WAY_TIMEOUT ||
                           Reason == DOT11_RC_8021X_FAILED ? "the network refused the station (wrong password?)" :
                           "the network refused the station");
        }
        break;

    case E_PRUNE:
        if (Adapter->LinkState == LINK_JOINING && Reason == E_PRUNE_RSN_MISMATCH) {
            WlanLog(Adapter, "The network's security does not match SECURITY=");
        }
        break;
    }
}

//
// A frame from F2: a control response, an event or an Ethernet frame.
//

static VOID
ChipFrame(
    IN PWLAN_ADAPTER Adapter,
    IN PUCHAR Frame,
    IN ULONG Length
    )
{
    ULONG Channel = Frame[5] & 0x0F;
    ULONG Offset = Frame[7];
    UCHAR Credit = Frame[9];
    PUCHAR Payload;
    ULONG PayloadLength;
    ULONG Flags;

    if (Channel < CHANNEL_GLOM) {
        if ((UCHAR)(Credit - Adapter->TxSequence) > 0x40) {
            Adapter->TxMaxSequence = (UCHAR)(Adapter->TxSequence + 2);
        } else {
            Adapter->TxMaxSequence = Credit;
        }

        Adapter->FlowStopped = Frame[8] != 0;
    }

    if (Length <= SDPCM_HEADER || Offset < SDPCM_HEADER || Offset > Length) {
        return;
    }

    Payload = Frame + Offset;
    PayloadLength = Length - Offset;
    switch (Channel) {
    case CHANNEL_CONTROL:
        if (PayloadLength < CDC_HEADER) {
            return;
        }

        Flags = Le32(Payload + 8);
        if ((Flags >> 16) != Adapter->ResponseId) {
            return;
        }

        Adapter->ResponseStatus = (Flags & CDC_FLAG_ERROR) != 0 ? (LONG)Le32(Payload + 12) : 0;
        Adapter->ResponseLength = PayloadLength - CDC_HEADER;
        Adapter->ResponseBuffer = Payload + CDC_HEADER;
        Adapter->ResponseReady = TRUE;
        break;

    case CHANNEL_EVENT:
    case CHANNEL_DATA:
        if (PayloadLength < BDC_HEADER || (Payload[0] >> 4) != BDC_VERSION ||
            BDC_HEADER + 4 * (ULONG)Payload[3] > PayloadLength) {
            return;
        }

        PayloadLength -= BDC_HEADER + 4 * Payload[3];
        Payload += BDC_HEADER + 4 * Payload[3];
        if (PayloadLength < ETH_HEADER_SIZE) {
            return;
        }

        //
        // Events come on their channel only: one in a data frame came from
        // the network, and is dropped.
        //

        if (Channel == CHANNEL_EVENT) {
            ChipEvent(Adapter, Payload, PayloadLength);
        } else if (Be16(Payload + 12) != 0x886C) {
            WlanQueueReceive(Adapter, Payload, PayloadLength);
        }
        break;

    default:
        WlanLog(Adapter, "Frame on channel %lu (%lu bytes) dropped", Channel, Length);
        break;
    }
}

//
// Read the frames the chip has, up to Bound: a frame's length first (four
// bytes; zero when there is none), then the rest of it, padded.
//

static VOID
ChipRecoverRead(
    IN PWLAN_ADAPTER Adapter
    )
{
    UCHAR High;
    UCHAR Low;
    ULONG Index;

    SdioWrite8(Adapter, 0, CCCR_ABORT, 2);
    SdioWrite8(Adapter, 1, SB_FRAME_CTRL, 0x01);
    for (Index = 0; Index < 10; Index += 1) {
        if (SdioRead8(Adapter, 1, SB_RFRAME_BC_HIGH, &High) && SdioRead8(Adapter, 1, SB_RFRAME_BC_LOW, &Low) &&
            High == 0 && Low == 0) {
            break;
        }
    }
}

static VOID
ChipProcessFrames(
    IN PWLAN_ADAPTER Adapter,
    IN ULONG Bound
    )
{
    PUCHAR Frame = Adapter->FrameBuffer;
    ULONG Length;
    ULONG Rest;
    ULONG Count;
    BOOLEAN Ready;

    for (Count = 0; Count < Bound; Count += 1) {
        if (!SdioCmd53(Adapter, 2, 0, Frame, 4, FALSE, TRUE)) {
            ChipRecoverRead(Adapter);
            return;
        }

        Length = Le16(Frame);
        if (Length == 0 && Le16(Frame + 2) == 0) {
            return;
        }

        if ((Length ^ Le16(Frame + 2)) != 0xFFFF || Length < SDPCM_HEADER || Length > RECEIVE_BUFFER_SIZE - 512) {
            WlanLog(Adapter, "Bad frame header %04lx/%04lx", Length, Le16(Frame + 2));
            ChipRecoverRead(Adapter);
            return;
        }

        Rest = (Length - 4 + 3) & ~3;
        if (Rest > 512) {
            Rest = (Rest + Adapter->F2BlockSize - 1) & ~(Adapter->F2BlockSize - 1);
        }

        if (Rest != 0 && !SdioCmd53(Adapter, 2, 0, Frame + 4, Rest, FALSE, TRUE)) {
            ChipRecoverRead(Adapter);
            return;
        }

        //
        // A control response is copied out of the buffer before the next frame
        // is read into it.
        //

        Ready = Adapter->ResponseReady;
        ChipFrame(Adapter, Frame, Length);
        if (!Ready && Adapter->ResponseReady) {
            Adapter->FramesPending = TRUE;
            return;
        }
    }

    Adapter->FramesPending = TRUE;
}

//
// The interrupt status of the chip's SDIO core: frames for the host, flow
// control, and the mailbox.
//

static ULONG
ChipServiceInterrupt(
    IN PWLAN_ADAPTER Adapter
    )
{
    ULONG Status = BpRead32(Adapter, Adapter->SdioCoreBase + SDIO_INTSTATUS) & HOSTINTMASK;
    ULONG Data;

    if (Status == 0 || Status == (0xFFFFFFFF & HOSTINTMASK)) {
        return 0;
    }

    BpWrite32(Adapter, Adapter->SdioCoreBase + SDIO_INTSTATUS, Status);
    if (Status & I_HMB_FC_CHANGE) {
        Adapter->FlowState = (BpRead32(Adapter, Adapter->SdioCoreBase + SDIO_INTSTATUS) & 0x30) != 0;
    }

    if (Status & I_HMB_HOST_INT) {
        Data = BpRead32(Adapter, Adapter->SdioCoreBase + SDIO_TOHOSTMAILBOXDATA);
        BpWrite32(Adapter, Adapter->SdioCoreBase + SDIO_TOSBMAILBOX, SMB_INT_ACK);
        if (Data & HMB_DATA_FWHALT) {
            WlanLog(Adapter, "The chip's firmware halted (mailbox %08lx)", Data);
        }

        if (Data & HMB_DATA_NAKHANDLED) {
            Status |= I_HMB_FRAME_IND;
        }
    }

    return Status;
}

//
// Read the frames the chip signals (its frame indication, or frames left
// from the last read).
//

static VOID
ChipPoll(
    IN PWLAN_ADAPTER Adapter
    )
{
    ULONG Status = ChipServiceInterrupt(Adapter);

    if ((Status & I_HMB_FRAME_IND) != 0 || Adapter->FramesPending) {
        Adapter->FramesPending = FALSE;
        ChipProcessFrames(Adapter, 50);
    }
}

//
// Load the CLM (regulatory) data in chunks.
//

static VOID
ChipLoadClm(
    IN PWLAN_ADAPTER Adapter,
    IN PUCHAR Clm,
    IN ULONG Length
    )
{
    PUCHAR Data = ExAllocatePool(NonPagedPool, 12 + CLM_CHUNK + 4);
    ULONG Offset = 0;
    ULONG Chunk;
    ULONG Flag;
    ULONG Status32 = 0;
    LONG Status = 0;

    if (Data == NULL) {
        return;
    }

    while (Offset < Length) {
        Chunk = Length - Offset > CLM_CHUNK ? CLM_CHUNK : Length - Offset;
        Flag = 0x1000 | (Offset == 0 ? 0x0002 : 0) | (Offset + Chunk == Length ? 0x0004 : 0);
        PutLe16(Data, Flag);
        PutLe16(Data + 2, 2);
        PutLe32(Data + 4, Chunk);
        PutLe32(Data + 8, 0);
        NdisMoveMemory(Data + 12, Clm + Offset, Chunk);
        Status = ChipVar(Adapter, "clmload", TRUE, Data, 12 + ((Chunk + 3) & ~3));
        if (Status != 0) {
            break;
        }

        Offset += Chunk;
    }

    ExFreePool(Data);
    ChipGetVar32(Adapter, "clmload_status", &Status32);
    WlanLog(Adapter, "CLM: %lu bytes loaded (status %ld, clmload_status %lu)", Offset, Status, Status32);
}

NDIS_STATUS
ChipStart(
    IN PWLAN_ADAPTER Adapter
    )

/*++

Routine Description:

    This routine brings the chip up: from the SDIO interface to the radio, and
    the station address.

Arguments:

    Adapter - Supplies the adapter.

Return Value:

    NDIS_STATUS_SUCCESS; NDIS_STATUS_ADAPTER_NOT_FOUND if there is no chip,
    the chip is not a supported one or its files are missing;
    NDIS_STATUS_RESOURCES; or NDIS_STATUS_HARD_ERRORS.

--*/

{
    CHIP_CORES Cores;
    PUCHAR Firmware = NULL;
    PUCHAR Nvram = NULL;
    PUCHAR Clm = NULL;
    PUCHAR Blob = NULL;
    PUCHAR Check = NULL;
    ULONG FirmwareLength;
    ULONG NvramLength;
    ULONG ClmLength = 0;
    ULONG BlobLength;
    ULONG ChipId;
    ULONG Index;
    ULONG Value;
    ULONG Start;
    PCHAR Base;
    UCHAR Byte;
    UCHAR Csr;
    UCHAR Data[64];
    UCHAR Mask[27];
    LONG Status;
    NDIS_STATUS Result = NDIS_STATUS_HARD_ERRORS;

    Adapter->BackplaneWindow = 0xFFFFFFFF;
    if (!SdioInitialize(Adapter, Adapter->TransferClock)) {
        return NDIS_STATUS_ADAPTER_NOT_FOUND;
    }

    //
    // F1, the block sizes and the interrupts.
    //

    for (Index = 0; Index < 100; Index += 1) {
        if (SdioWrite8(Adapter, 0, CCCR_IOEX, 0x02) && SdioRead8(Adapter, 0, CCCR_IOEX, &Byte) && Byte == 0x02) {
            break;
        }

        ChipSleep(1);
    }

    if (!SdioSetBlockSize(Adapter, 0, 64) || !SdioSetBlockSize(Adapter, 1, 64) ||
        !SdioWrite8(Adapter, 0, CCCR_IENX, 0x07)) {
        WlanLog(Adapter, "SDIO: CCCR setup failed (status %08lx)", Adapter->LastSdioStatus);
        return NDIS_STATUS_ADAPTER_NOT_FOUND;
    }

    Byte = 0;
    for (Index = 0; Index < 100; Index += 1) {
        if (SdioRead8(Adapter, 0, CCCR_IORX, &Byte) && (Byte & 0x02) != 0) {
            break;
        }

        ChipSleep(1);
    }

    if ((Byte & 0x02) == 0) {
        WlanLog(Adapter, "SDIO: F1 not ready");
        return NDIS_STATUS_ADAPTER_NOT_FOUND;
    }

    //
    // The ALP clock.
    //

    SdioWrite8(Adapter, 1, SB_CHIP_CLOCK_CSR, CLK_HW_REQ_OFF | CLK_ALP_REQ);
    if (!SdioRead8(Adapter, 1, SB_CHIP_CLOCK_CSR, &Csr) || (Csr & ~0xC0) != (CLK_HW_REQ_OFF | CLK_ALP_REQ)) {
        WlanLog(Adapter, "Chip clock CSR reads %02x", Csr);
        return NDIS_STATUS_ADAPTER_NOT_FOUND;
    }

    for (Start = NowMs(); NowMs() - Start < 1000; ) {
        if (SdioRead8(Adapter, 1, SB_CHIP_CLOCK_CSR, &Csr) && (Csr & CLK_ALP_AVAIL) != 0) {
            break;
        }

        ChipSleep(1);
    }

    if ((Csr & CLK_ALP_AVAIL) == 0) {
        WlanLog(Adapter, "No ALP clock (CSR %02x)", Csr);
        return NDIS_STATUS_HARD_ERRORS;
    }

    SdioWrite8(Adapter, 1, SB_CHIP_CLOCK_CSR, CLK_HW_REQ_OFF | CLK_FORCE_ALP);
    KeStallExecutionProcessor(65);
    SdioWrite8(Adapter, 1, SB_PULLUP, 0);

    //
    // The chip and its cores.
    //

    ChipId = BpRead32(Adapter, CHIPCOMMON_BASE);
    Adapter->ChipId = ChipId & 0xFFFF;
    Adapter->ChipRevision = (ChipId >> 16) & 0xF;
    WlanLog(Adapter, "Chip %04lx (%lu) rev %lu, %lu cores", Adapter->ChipId, Adapter->ChipId,
            Adapter->ChipRevision, (ChipId >> 24) & 0xF);
    if (Adapter->ChipId != CHIP_43430 && Adapter->ChipId != CHIP_4345) {
        WlanLog(Adapter, "Unsupported chip");
        return NDIS_STATUS_ADAPTER_NOT_FOUND;
    }

    if (!ChipScanCores(Adapter, &Cores)) {
        if (Adapter->ChipId != CHIP_43430) {
            WlanLog(Adapter, "The enumeration ROM has not the cores");
            return NDIS_STATUS_HARD_ERRORS;
        }

        Cores.Arm = 0x18003000;
        Cores.ArmWrapper = 0x18103000;
        Cores.ArmId = CORE_ARM_CM3;
        Cores.Socram = 0x18004000;
        Cores.SocramWrapper = 0x18104000;
        Cores.Sdio = 0x18002000;
        Cores.SdioRevision = 14;
        Cores.D11Wrapper = 0x18101000;
    }

    Adapter->ArmCr4 = Cores.ArmId == CORE_ARM_CR4;
    Adapter->ArmCoreBase = Cores.Arm;
    Adapter->ArmWrapperBase = Cores.ArmWrapper;
    Adapter->SocramBase = Cores.Socram;
    Adapter->SocramWrapperBase = Cores.SocramWrapper;
    Adapter->SdioCoreBase = Cores.Sdio;
    Adapter->SdioCoreRevision = Cores.SdioRevision;
    Adapter->F2BlockSize = Adapter->ChipId == CHIP_4345 ? 512 : 64;

    if (Cores.SdioRevision >= 12 && SdioRead8(Adapter, 1, SB_SLEEP_CSR, &Byte) && (Byte & 1) == 0) {
        SdioWrite8(Adapter, 1, SB_SLEEP_CSR, (UCHAR)(Byte | 1));
    }

    //
    // The ARM halted, and the RAM.
    //

    if (Adapter->ArmCr4) {
        Value = BpRead32(Adapter, Cores.ArmWrapper + AI_IOCTRL) & IOCTRL_CPUHALT;
        AiResetCore(Adapter, Cores.ArmWrapper, Value, IOCTRL_CPUHALT, IOCTRL_CPUHALT);
        if (Cores.D11Wrapper != 0) {
            AiCoreDisable(Adapter, Cores.D11Wrapper, IOCTRL_PHYRESET | IOCTRL_PHYCLOCKEN, IOCTRL_PHYCLOCKEN);
        }
    } else {
        AiCoreDisable(Adapter, Cores.ArmWrapper, 0, 0);
        if (Cores.D11Wrapper != 0) {
            AiResetCore(Adapter, Cores.D11Wrapper, IOCTRL_PHYRESET | IOCTRL_PHYCLOCKEN, IOCTRL_PHYCLOCKEN,
                        IOCTRL_PHYCLOCKEN);
        }

        AiResetCore(Adapter, Cores.SocramWrapper, 0, 0, 0);
        if (Adapter->ChipId == CHIP_43430) {
            BpWrite32(Adapter, Cores.Socram + 0x10, 3);
            BpWrite32(Adapter, Cores.Socram + 0x44, 0);
        }
    }

    ChipRamSize(Adapter, &Cores);
    WlanLog(Adapter, "ARM %s, RAM %lu KB at %08lx", Adapter->ArmCr4 ? "Cortex-R4" : "Cortex-M3",
            Adapter->RamSize / 1024, Adapter->RamBase);

    //
    // The files.
    //

    Base = ChipFileBase(Adapter);
    Firmware = ChipReadFile(Adapter, Base, "BIN", &FirmwareLength);
    Nvram = ChipReadFile(Adapter, Base, "TXT", &NvramLength);
    Clm = ChipReadFile(Adapter, Base, "CLM", &ClmLength);
    if (Firmware == NULL || Nvram == NULL) {
        WlanLog(Adapter, "The chip's firmware or NVRAM is missing from \\WINNT\\System32\\Drivers\\BCMWLAN");
        Result = NDIS_STATUS_ADAPTER_NOT_FOUND;
        goto Done;
    }

    for (Index = FirmwareLength > 800 ? FirmwareLength - 800 : 0; Index + 9 < FirmwareLength; Index += 1) {
        if (RtlCompareMemory(Firmware + Index, "Version: ", 9) == 9) {
            for (Value = 0; Value < sizeof(Adapter->FirmwareVersion) - 1 && Index + 9 + Value < FirmwareLength &&
                 Firmware[Index + 9 + Value] >= ' '; Value += 1) {
                Adapter->FirmwareVersion[Value] = Firmware[Index + 9 + Value];
            }

            Adapter->FirmwareVersion[Value] = 0;
            WlanLog(Adapter, "Firmware: %s", Adapter->FirmwareVersion);
            break;
        }
    }

    //
    // The firmware at the base of RAM (checked), the NVRAM at its end.
    //

    while (FirmwareLength % 4 != 0) {
        Firmware[FirmwareLength++] = 0;
    }

    if (FirmwareLength > Adapter->RamSize - 0x1000 ||
        !BpMove(Adapter, Adapter->RamBase, Firmware, FirmwareLength, TRUE)) {
        WlanLog(Adapter, "Firmware download failed (status %08lx, address %08lx)", Adapter->LastSdioStatus,
                Adapter->LastSdioAddress);
        goto Done;
    }

    Check = ExAllocatePool(NonPagedPool, 2048);
    if (Check != NULL && BpMove(Adapter, Adapter->RamBase, Check, 2048, FALSE) &&
        RtlCompareMemory(Check, Firmware, 2048) != 2048) {
        WlanLog(Adapter, "The firmware reads back wrong");
        WlanLogBytes(Adapter, "wrote", Firmware, 32);
        WlanLogBytes(Adapter, "read", Check, 32);
        goto Done;
    }

    Blob = ExAllocatePool(NonPagedPool, NvramLength + 64);
    if (Blob == NULL) {
        Result = NDIS_STATUS_RESOURCES;
        goto Done;
    }

    BlobLength = ChipFormatNvram(Nvram, NvramLength, Blob);
    if (!BpMove(Adapter, Adapter->RamBase + Adapter->RamSize - BlobLength, Blob, BlobLength, TRUE)) {
        WlanLog(Adapter, "NVRAM download failed");
        goto Done;
    }

    WlanLog(Adapter, "Firmware (%lu bytes) and NVRAM (%lu bytes) loaded", FirmwareLength, BlobLength);

    //
    // The ARM released: the firmware runs.
    //

    BpWrite32(Adapter, Cores.Sdio + SDIO_INTSTATUS, 0xFFFFFFFF);
    if (Adapter->ArmCr4) {
        BpWrite32(Adapter, 0, Le32(Firmware));
        AiResetCore(Adapter, Cores.ArmWrapper, IOCTRL_CPUHALT, 0, 0);
    } else {
        AiResetCore(Adapter, Cores.ArmWrapper, 0, 0, 0);
    }

    SdioWrite8(Adapter, 1, SB_CHIP_CLOCK_CSR, 0);
    SdioWrite8(Adapter, 1, SB_CHIP_CLOCK_CSR, CLK_HT_REQ);
    for (Start = NowMs(); NowMs() - Start < 1000; ) {
        if (SdioRead8(Adapter, 1, SB_CHIP_CLOCK_CSR, &Csr) && (Csr & 0xC0) == 0xC0) {
            break;
        }

        ChipSleep(5);
    }

    if ((Csr & 0xC0) != 0xC0) {
        WlanLog(Adapter, "No HT clock (CSR %02x): the firmware did not start", Csr);
        goto Done;
    }

    SdioWrite8(Adapter, 1, SB_CHIP_CLOCK_CSR, (UCHAR)(Csr | CLK_FORCE_HT));

    //
    // F2: ready when the firmware has booted.
    //

    BpWrite32(Adapter, Cores.Sdio + SDIO_TOSBMAILBOXDATA, 0x00040000);
    SdioWrite8(Adapter, 0, CCCR_IOEX, 0x06);
    for (Start = NowMs(); NowMs() - Start < 3000; ) {
        if (SdioRead8(Adapter, 0, CCCR_IORX, &Byte) && (Byte & 0x04) != 0) {
            break;
        }

        ChipSleep(5);
    }

    if ((Byte & 0x04) == 0) {
        WlanLog(Adapter, "F2 not ready: the firmware did not boot");
        goto Done;
    }

    if (!SdioSetBlockSize(Adapter, 2, Adapter->F2BlockSize)) {
        WlanLog(Adapter, "F2 block size not set");
        goto Done;
    }

    BpWrite32(Adapter, Cores.Sdio + SDIO_HOSTINTMASK, HOSTINTMASK);
    if (Adapter->ChipId == CHIP_4345) {
        SdioWrite8(Adapter, 1, SB_WATERMARK, 0x60);
        if (SdioRead8(Adapter, 1, SB_DEVICE_CTL, &Byte)) {
            SdioWrite8(Adapter, 1, SB_DEVICE_CTL, (UCHAR)(Byte | 0x10));
        }

        SdioWrite8(Adapter, 1, SB_MES_BUSY_CTRL, 0xD0);
    } else {
        SdioWrite8(Adapter, 1, SB_WATERMARK, 0x08);
    }

    WlanLog(Adapter, "The firmware runs (F2 ready)");
    Adapter->TxSequence = 0;
    Adapter->TxMaxSequence = 1;
    ChipSleep(150);
    Adapter->FramesPending = TRUE;
    ChipPoll(Adapter);

    //
    // The firmware's setup: CLM, no receive glomming, the station address,
    // the country, the events, no roaming, the radio up, no power save.
    //

    if (Clm != NULL) {
        ChipLoadClm(Adapter, Clm, ClmLength);
    }

    ChipSetVar32(Adapter, "bus:txglom", 0);
    NdisZeroMemory(Data, sizeof(Data));
    if (ChipVar(Adapter, "ver", FALSE, Data, sizeof(Data) - 1) == 0) {
        for (Index = 0; Index < sizeof(Data) - 1 && Data[Index] != 0; Index += 1) {
            if (Data[Index] < ' ') {
                Data[Index] = ' ';
            }
        }

        WlanLog(Adapter, "Firmware reports: %s", Data);
    }

    Status = ChipVar(Adapter, "cur_etheraddr", FALSE, Adapter->PermanentAddress, 6);
    WlanLog(Adapter, "Station address %02x:%02x:%02x:%02x:%02x:%02x (status %ld)", Adapter->PermanentAddress[0],
            Adapter->PermanentAddress[1], Adapter->PermanentAddress[2], Adapter->PermanentAddress[3],
            Adapter->PermanentAddress[4], Adapter->PermanentAddress[5], Status);
    if (Status != 0) {
        goto Done;
    }

    NdisZeroMemory(Data, 12);
    Data[0] = Data[8] = Adapter->Country[0];
    Data[1] = Data[9] = Adapter->Country[1];
    PutLe32(Data + 4, 0xFFFFFFFF);
    Status = ChipVar(Adapter, "country", TRUE, Data, 12);
    WlanLog(Adapter, "Country %s (status %ld)", Adapter->Country, Status);
    ChipSleep(50);

    NdisZeroMemory(Mask, sizeof(Mask));
    Status = ChipVar(Adapter, "event_msgs", FALSE, Mask, sizeof(Mask));
    Value = sizeof(Mask);
    if (Status != 0) {
        Value = 16;
        NdisZeroMemory(Mask, sizeof(Mask));
        Status = ChipVar(Adapter, "event_msgs", FALSE, Mask, Value);
    }

    for (Index = 0; Index < 24; Index += 1) {
        if (Index != 2 && Index != 4 && Index != 8 && Index != 10 && Index != 13 && Index != 14 && Index != 15 &&
            Index != 18 && Index != 20 && Index != 21 && Index != 22) {
            Mask[Index / 8] |= (UCHAR)(1 << (Index % 8));
        }
    }

    Mask[E_PSK_SUP / 8] |= (UCHAR)(1 << (E_PSK_SUP % 8));
    Mask[E_ESCAN_RESULT / 8] |= (UCHAR)(1 << (E_ESCAN_RESULT % 8));
    Status = ChipVar(Adapter, "event_msgs", TRUE, Mask, Value);
    WlanLog(Adapter, "Event mask (%lu bytes) status %ld", Value, Status);

    ChipSetVar32(Adapter, "roam_off", 1);
    ChipSetVar32(Adapter, "bcn_timeout", 4);
    ChipSetVar32(Adapter, "mpc", 0);
    Status = ChipIoctl(Adapter, WLC_UP, TRUE, NULL, 0, 0);
    WlanLog(Adapter, "Radio up (status %ld)", Status);
    if (Status != 0) {
        goto Done;
    }

    ChipSleep(50);
    ChipSetIoctl32(Adapter, WLC_SET_PM, 0);
    Adapter->Supplicant = ChipGetVar32(Adapter, "sup_wpa", &Value) == 0;
    Adapter->Mfp = ChipGetVar32(Adapter, "mfp", &Value) == 0;
    Adapter->InChipSae = FALSE;
    WlanLog(Adapter, "In-chip WPA supplicant: %s; protected management frames: %s",
            Adapter->Supplicant ? "yes" : "NO", Adapter->Mfp ? "yes" : "no");

    //
    // A scan, for the log.
    //

    NdisZeroMemory(Data, 64);
    PutLe32(Data, 1);
    PutLe16(Data + 4, 1);
    PutLe16(Data + 6, 0x1234);
    NdisFillMemory(Data + 44, 6, 0xFF);
    Data[50] = 2;
    PutLe32(Data + 52, 0xFFFFFFFF);
    PutLe32(Data + 56, 0xFFFFFFFF);
    PutLe32(Data + 60, 0xFFFFFFFF);
    {
        UCHAR Scan[76];

        NdisMoveMemory(Scan, Data, 64);
        PutLe32(Scan + 64, 0xFFFFFFFF);
        PutLe32(Scan + 68, 0);
        PutLe32(Scan + 72, 0);
        Adapter->ScanCount = 0;
        Adapter->Scanning = TRUE;
        Status = ChipVar(Adapter, "escan", TRUE, Scan, 72);
        WlanLog(Adapter, "Scanning (status %ld):", Status);
        for (Start = NowMs(); Status == 0 && Adapter->Scanning && NowMs() - Start < 5000; ) {
            ChipPoll(Adapter);
            ChipSleep(10);
        }

        Adapter->Scanning = FALSE;
    }

    Adapter->NextJoin.LowPart = NowMs();
    Result = NDIS_STATUS_SUCCESS;

Done:
    if (Firmware != NULL) {
        ExFreePool(Firmware);
    }

    if (Nvram != NULL) {
        ExFreePool(Nvram);
    }

    if (Clm != NULL) {
        ExFreePool(Clm);
    }

    if (Blob != NULL) {
        ExFreePool(Blob);
    }

    if (Check != NULL) {
        ExFreePool(Check);
    }

    return Result;
}

VOID
ChipStop(
    IN PWLAN_ADAPTER Adapter
    )

/*++

Routine Description:

    This routine leaves the network, if one was joined or a join is under way,
    and shuts the SDIO host down.

Arguments:

    Adapter - Supplies the adapter.

Return Value:

    None.

--*/

{
    if (Adapter->LinkState != LINK_IDLE && Adapter->F2BlockSize != 0) {
        ChipIoctl(Adapter, WLC_DISASSOC, TRUE, NULL, 0, 0);
    }

    SdioShutdown(Adapter);
}

BOOLEAN
ChipJoin(
    IN PWLAN_ADAPTER Adapter
    )

/*++

Routine Description:

    This routine joins the configured network with the firmware's supplicant:
    open, or WPA2-PSK with the passphrase (which the firmware turns into the
    key). The firmware here does no WPA3 (SAE) by itself: a WPA3 network that
    also takes WPA2 is joined with WPA2.

Arguments:

    Adapter - Supplies the adapter.

Return Value:

    TRUE if the join started (the firmware's events tell how it ends), FALSE
    if no network is configured, the passphrase is not one, or the firmware
    refused the join.

--*/

{
    UCHAR Data[132];
    LONG Status;
    ULONG Security = Adapter->Security;
    ULONG Index;
    ULONG Digit;

    if (!Adapter->Configured) {
        return FALSE;
    }

    if (Security != SECURITY_OPEN && (Adapter->PassphraseLength < 8 || Adapter->PassphraseLength > 64)) {
        WlanLog(Adapter, "The passphrase must have 8 to 63 characters (or be the key, 64 hex digits)");
        Adapter->Configured = FALSE;
        return FALSE;
    }

    if (Security == SECURITY_WPA3 || Security == SECURITY_WPA2_WPA3) {
        WlanLog(Adapter, "WPA3: the chip's firmware has no in-chip SAE; joining with WPA2-PSK (works on WPA2/WPA3 "
                "networks)");
        Security = SECURITY_WPA2;
    }

    Adapter->JoinAttempts += 1;
    WlanLog(Adapter, "Joining \"%s\" (%s), attempt %lu", Adapter->Ssid,
            Security == SECURITY_OPEN ? "open" : "WPA2-PSK", Adapter->JoinAttempts);
    Adapter->Keyed = FALSE;
    Adapter->Associated = FALSE;
    Adapter->LinkState = LINK_JOINING;
    Adapter->JoinStarted.LowPart = NowMs();

    if (Security == SECURITY_OPEN) {
        ChipSetIoctl32(Adapter, WLC_SET_WSEC, 0);
        ChipSetBss32(Adapter, "sup_wpa", 0);
    } else {
        ChipSetIoctl32(Adapter, WLC_SET_WSEC, WSEC_AES);
        Status = ChipSetBss32(Adapter, "sup_wpa", 1);
        if (Status != 0) {
            WlanLog(Adapter, "sup_wpa: %ld (the firmware has no in-chip supplicant)", Status);
        }

        ChipSetBss32(Adapter, "sup_wpa2_eapver", 0xFFFFFFFF);
        ChipSetBss32(Adapter, "sup_wpa_tmo", 5000);
        ChipSleep(2);
        NdisZeroMemory(Data, sizeof(Data));
        if (Adapter->PassphraseLength == 64) {

            //
            // 64 hex digits: the key itself (flags 0).
            //

            for (Index = 0; Index < 64; Index += 1) {
                Digit = Adapter->Passphrase[Index];
                Digit = Digit >= '0' && Digit <= '9' ? Digit - '0' : Digit >= 'a' && Digit <= 'f' ? Digit - 'a' + 10 :
                        Digit >= 'A' && Digit <= 'F' ? Digit - 'A' + 10 : 0;
                Data[4 + Index / 2] = (UCHAR)((Data[4 + Index / 2] << 4) | Digit);
            }

            PutLe16(Data, 32);
            PutLe16(Data + 2, 0);
        } else {
            PutLe16(Data, Adapter->PassphraseLength);
            PutLe16(Data + 2, WSEC_PASSPHRASE);
            NdisMoveMemory(Data + 4, Adapter->Passphrase, Adapter->PassphraseLength);
        }

        Status = ChipIoctl(Adapter, WLC_SET_WSEC_PMK, TRUE, Data, 132, 0);
        if (Status != 0) {
            Status = ChipIoctl(Adapter, WLC_SET_WSEC_PMK, TRUE, Data, 68, 0);
        }

        if (Status != 0) {
            WlanLog(Adapter, "The passphrase was not taken (status %ld)", Status);
        }
    }

    ChipSetIoctl32(Adapter, WLC_SET_INFRA, 1);
    ChipSetIoctl32(Adapter, WLC_SET_AUTH, 0);
    if (Adapter->Mfp) {
        ChipSetVar32(Adapter, "mfp", Security == SECURITY_OPEN ? 0 : 1);
    }

    ChipSetIoctl32(Adapter, WLC_SET_WPA_AUTH, Security == SECURITY_OPEN ? WPA_AUTH_DISABLED : WPA2_AUTH_PSK);
    NdisZeroMemory(Data, 36);
    PutLe32(Data, Adapter->SsidLength);
    NdisMoveMemory(Data + 4, Adapter->Ssid, Adapter->SsidLength);
    Status = ChipIoctl(Adapter, WLC_SET_SSID, TRUE, Data, 36, 0);
    if (Status != 0) {
        ChipJoinFailed(Adapter, "the firmware refused the join");
        return FALSE;
    }

    return TRUE;
}

BOOLEAN
ChipSend(
    IN PWLAN_ADAPTER Adapter,
    IN PWLAN_SLOT Slot
    )

/*++

Routine Description:

    This routine sends a frame of the transmit ring: SDPCM (12 bytes) and BDC
    (4) headers in front of the Ethernet frame, in the slot's headroom.

Arguments:

    Adapter - Supplies the adapter.

    Slot - Supplies the slot of the frame.

Return Value:

    TRUE if the chip took the frame.

--*/

{
    PUCHAR Frame = (PUCHAR)Slot->Data + FRAME_HEADROOM - SDPCM_HEADER - BDC_HEADER;

    Frame[SDPCM_HEADER] = BDC_VERSION << 4;
    Frame[SDPCM_HEADER + 1] = 0;
    Frame[SDPCM_HEADER + 2] = 0;
    Frame[SDPCM_HEADER + 3] = 0;
    return ChipSendFrame(Adapter, Frame, SDPCM_HEADER + BDC_HEADER + Slot->Length, CHANNEL_DATA, SDPCM_HEADER);
}

VOID
ChipSetFilter(
    IN PWLAN_ADAPTER Adapter
    )

/*++

Routine Description:

    This routine sets the receive filter: all multicast, or the list;
    promiscuous.

Arguments:

    Adapter - Supplies the adapter.

Return Value:

    None.

--*/

{
    UCHAR Data[4 + WLAN_MULTICAST_LIST * ETH_LENGTH_OF_ADDRESS];

    PutLe32(Data, Adapter->MulticastCount);
    NdisMoveMemory(Data + 4, Adapter->Multicast, Adapter->MulticastCount * ETH_LENGTH_OF_ADDRESS);
    ChipVar(Adapter, "mcast_list", TRUE, Data, 4 + Adapter->MulticastCount * ETH_LENGTH_OF_ADDRESS);
    ChipSetVar32(Adapter, "allmulti", (Adapter->PacketFilter & NDIS_PACKET_TYPE_ALL_MULTICAST) != 0);
    ChipSetIoctl32(Adapter, WLC_SET_PROMISC, (Adapter->PacketFilter & NDIS_PACKET_TYPE_PROMISCUOUS) != 0);
}

VOID
ChipCheckLink(
    IN PWLAN_ADAPTER Adapter
    )

/*++

Routine Description:

    This routine has nothing to do: the firmware's events tell the state of
    the link.

Arguments:

    Adapter - Supplies the adapter.

Return Value:

    None.

--*/

{
}

BOOLEAN
ChipService(
    IN PWLAN_ADAPTER Adapter
    )

/*++

Routine Description:

    This routine does the work of the thread: what the chip has for the host,
    the frames to send, a join to (re)start or time out, and a new receive
    filter.

Arguments:

    Adapter - Supplies the adapter.

Return Value:

    TRUE if received frames wait for the timer to indicate them.

--*/

{
    PWLAN_SLOT Slot;
    ULONG Now = NowMs();
    ULONG Sent = 0;

    ChipPoll(Adapter);

    if (Adapter->FilterChanged) {
        Adapter->FilterChanged = FALSE;
        ChipSetFilter(Adapter);
    }

    if (Adapter->Configured) {
        if (Adapter->LinkState == LINK_IDLE && (LONG)(Now - Adapter->NextJoin.LowPart) >= 0) {
            ChipJoin(Adapter);
        } else if (Adapter->LinkState == LINK_JOINING && Now - Adapter->JoinStarted.LowPart > JOIN_TIMEOUT_MS) {
            ChipIoctl(Adapter, WLC_DISASSOC, TRUE, NULL, 0, 0);
            ChipJoinFailed(Adapter, "no answer within 20 s");
        }
    }

    while (Sent < 20 && WlanNextTransmit(Adapter, &Slot)) {
        if (Adapter->LinkState != LINK_UP) {
            WlanTransmitDone(Adapter, FALSE);
            continue;
        }

        if (!ChipCanSend(Adapter) || Adapter->FlowStopped || Adapter->FlowState) {
            ChipPoll(Adapter);
            if (!ChipCanSend(Adapter) || Adapter->FlowStopped || Adapter->FlowState) {
                break;
            }
        }

        WlanTransmitDone(Adapter, ChipSend(Adapter, Slot));
        Sent += 1;
    }

    return Adapter->ReceiveCount != 0;
}
