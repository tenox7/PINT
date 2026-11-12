/*++

Module Name:

    hdmi.c

Abstract:

    HDMI audio on the display the VideoCore firmware brought up, driven
    directly (not through the firmware's audio service): the MAI (the audio
    FIFO of the HD block, which a DMA channel feeds), its sample rate divider,
    the audio clock recovery (N and CTS) and the audio InfoFrame in slot 4 of
    the packet RAM. Video is left alone: of the registers it shares, only bit
    4 of RAM_PACKET_CONFIG (the slot of the InfoFrame) and the RNG power-down
    bit of the PHY are changed. The audio clock recovery packets carry the N
    and CTS the driver computes (CTS from the pixel clock), as Circle and
    Linux send them.

    Raspberry Pi 2, 3 and Zero 2 W: the VC4 HDMI (core 0x902000, HD 0x808000
    of the peripherals), whose audio clock is the HSM clock (PLLD divided by
    CM_HSMDIV) and whose pixel clock is PLLH over 10, DREQ 17. Raspberry Pi 4
    and 400: HDMI0 or HDMI1 of the VC5 (cores 0xF00700 and 0xF05700, one HD
    block 0xF20000), audio clock 108 MHz, pixel clock the firmware's clock 14,
    DREQ 10 and 17, through a legacy DMA channel (as Circle plays it; Circle
    drives HDMI0 only).

    The samples are IEC 60958 subframes, a 32-bit word a channel, as Circle
    makes them: the B frame identifier 0x0F in bits 3:0 of both subframes of
    the first frame of a block of 192 (0 elsewhere), the sample (bits 27:4,
    the 16 bits in 27:12), the validity, user and channel status bits (28-30)
    and the even parity of bits 0-30 (31). The channel status says consumer
    PCM, the rate, 24-bit samples and the original rate.

--*/

#include "bcmsound.h"

//
// The VideoCore mailbox (property channel).
//

#define MAILBOX_OFFSET              0xB880
#define MAILBOX_READ                0x00
#define MAILBOX_STATUS              0x18
#define MAILBOX_WRITE               0x20
#define MAILBOX_FULL                0x80000000
#define MAILBOX_EMPTY               0x40000000
#define MAILBOX_PROPERTY            8
#define MAILBOX_SUCCESS             0x80000000

#define TAG_GET_CLOCK_RATE          0x00030002
#define TAG_GET_EDID_BLOCK          0x00030020
#define TAG_GET_EDID_BLOCK_DISPLAY  0x00030023
#define TAG_GET_NUM_DISPLAYS        0x00040013
#define TAG_GET_DISPLAY_ID          0x00040016

//
// The clocks of the firmware: the pixel clock (the firmware's view), and
// the HDMI clock and the pixel clock of a BCM2711.
//

#define CLOCK_PIXEL                 9
#define CLOCK_HSM                   13
#define CLOCK_PIXEL_BVB             14

//
// The clocks of a BCM2835-7 (the clock manager): the HSM divider of PLLD
// (500 MHz) and PLLH (from the 19.2 MHz crystal), whose tenth is the pixel
// clock. A2W_PLLH_PIX is the divider of PLLH's pixel channel (0 is 256).
//

#define CM_OFFSET                   0x101000
#define CM_SIZE                     0x2000
#define CM_HSMDIV                   0x08C
#define A2W_PLLH_ANA1               0x1074
#define A2W_PLLH_CTRLR              0x1960
#define A2W_PLLH_FRACR              0x1A60
#define A2W_PLLH_PIX                0x1560
#define PLLD_RATE                   500000000
#define CRYSTAL_RATE                19200000

#define DISPLAY_HDMI0               2
#define DISPLAY_HDMI1               7

//
// The registers (offsets of their blocks).
//

#define VC4_CORE_OFFSET             0x902000
#define VC4_HD_OFFSET               0x808000
#define VC5_CORE_OFFSET(Port)       (0xF00700 + (Port) * 0x5000)
#define VC5_PHY_OFFSET(Port)        (0xF00F00 + (Port) * 0x5000)
#define VC5_PACKET_OFFSET(Port)     (0xF01B00 + (Port) * 0x5000)
#define VC5_HD_OFFSET               0xF20000

#define MAI_CTL_RESET               0x00000001
#define MAI_CTL_ERRORF              0x00000002
#define MAI_CTL_ERRORE              0x00000004
#define MAI_CTL_ENABLE              0x00000008
#define MAI_CTL_CHNUM(Channels)     ((Channels) << 4)
#define MAI_CTL_FLUSH               0x00000200
#define MAI_CTL_CHALIGN             0x00001000
#define MAI_CTL_WHOLSMP             0x00002000
#define MAI_CTL_DLATE               0x00008000

//
// The scheduler in HDMI mode (MODE_HDMI | HDMI_ACTIVE), the packet RAM and
// its slot of the audio InfoFrame (slot 4), and the clock recovery.
//

#define SCHEDULER_HDMI              0x00000003
#define RAM_PACKET_ENABLE           0x00010000
#define RAM_PACKET_AUDIO            0x00000010
#define AUDIO_SLOT_OFFSET           (4 * 0x24)
#define CRP_EXTERNAL_CTS_EN         0x01000000

//
// The audio packets: zero data on flat samples and inactive channels, B
// frame 0x0F, channels 0-1.
//

#define MAI_CONFIG_VALUE            0x0C000003
#define AUDIO_PACKET_VALUE          0x21003C03

typedef struct _HDMI_STATE {
    BOOLEAN Vc5;
    ULONG Port;
    PUCHAR Core;
    PUCHAR Hd;
    PUCHAR Packet;
    PUCHAR Phy;
    PUCHAR Mailbox;
    PULONG MailboxBuffer;
    ULONG MailboxBufferBus;

    //
    // The offsets of the registers (the MAI's in the HD block, the others
    // in the core, the control of the PHY) and the RNG power-down bit of
    // the PHY; the values of the threshold and of the channel map, the
    // clocks, and the DREQ and the bus address of the FIFO of the MAI.
    //

    ULONG MaiCtl;
    ULONG MaiThr;
    ULONG MaiFmt;
    ULONG MaiData;
    ULONG MaiSmp;
    ULONG ChannelMap;
    ULONG MaiConfig;
    ULONG AudioPacket;
    ULONG RamConfig;
    ULONG RamStatus;
    ULONG Crp;
    ULONG Cts0;
    ULONG Cts1;
    ULONG Scheduler;
    ULONG PhyControl;
    ULONG PhyRngBit;
    ULONG ThresholdValue;
    ULONG ChannelMapValue;
    ULONG AudioClock;
    ULONG PixelClock;
    ULONG Dreq;
    ULONG FifoBus;

    //
    // The audio clock recovery last set (HdmiStart), and the frame of the
    // block of 192 and the channel status of the samples.
    //

    ULONG N;
    ULONG Cts;
    ULONG Frame;
    UCHAR ChannelStatus[5];
} HDMI_STATE, *PHDMI_STATE;

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

//
// The mailbox: a property message of one tag, at PASSIVE_LEVEL (the start).
// The display driver uses the mailbox too, but not while drivers start.
//

static BOOLEAN
MailboxTag(
    IN PHDMI_STATE Hdmi,
    IN ULONG Tag,
    IN ULONG ValueSize,
    IN OUT PULONG Value,
    IN ULONG RequestWords
    )
{
    PULONG Buffer = Hdmi->MailboxBuffer;
    ULONG Words = ValueSize / sizeof(ULONG);
    ULONG Index;
    ULONG Count;

    Buffer[0] = (6 + Words) * sizeof(ULONG);
    Buffer[1] = 0;
    Buffer[2] = Tag;
    Buffer[3] = ValueSize;
    Buffer[4] = RequestWords * sizeof(ULONG);
    for (Index = 0; Index < Words; Index += 1) {
        Buffer[5 + Index] = Index < RequestWords ? Value[Index] : 0;
    }

    Buffer[5 + Words] = 0;
    MEMORY_BARRIER();
    for (Count = 0; (READ_REGISTER(Hdmi->Mailbox, MAILBOX_STATUS) & MAILBOX_FULL) != 0; Count += 1) {
        if (Count > 100000) {
            return FALSE;
        }

        KeStallExecutionProcessor(1);
    }

    WRITE_REGISTER(Hdmi->Mailbox, MAILBOX_WRITE, Hdmi->MailboxBufferBus | MAILBOX_PROPERTY);
    for (Count = 0; ; ) {
        while ((READ_REGISTER(Hdmi->Mailbox, MAILBOX_STATUS) & MAILBOX_EMPTY) != 0) {
            if (++Count > 1000000) {
                return FALSE;
            }

            KeStallExecutionProcessor(1);
        }

        if ((READ_REGISTER(Hdmi->Mailbox, MAILBOX_READ) & 0xF) == MAILBOX_PROPERTY) {
            break;
        }
    }

    MEMORY_BARRIER();
    if (Buffer[1] != MAILBOX_SUCCESS || (Buffer[4] & 0x80000000) == 0) {
        return FALSE;
    }

    for (Index = 0; Index < Words; Index += 1) {
        Value[Index] = Buffer[5 + Index];
    }

    return TRUE;
}

static ULONG
MailboxClockRate(
    IN PHDMI_STATE Hdmi,
    IN ULONG Clock
    )
{
    ULONG Value[2];

    Value[0] = Clock;
    return MailboxTag(Hdmi, TAG_GET_CLOCK_RATE, sizeof(Value), Value, 1) ? Value[1] : 0;
}

static ULONG
Vc4HsmRate(
    IN PUCHAR Clock
    )
{
    ULONG Divider = (READ_REGISTER(Clock, CM_HSMDIV) >> 4) & 0xFFF;

    return Divider == 0 ? 0 : (ULONG)(((ULONGLONG)PLLD_RATE << 8) / Divider);
}

static ULONG
Vc4PixelRate(
    IN PUCHAR Clock,
    OUT PULONG Pllh,
    OUT PULONG PixelDivider
    )

/*++

Routine Description:

    This routine returns the pixel clock of a BCM2835-7: PLLH (from the
    crystal, its multiplier and fraction, doubled with the prescaler, over
    its predivider), through the divider of its pixel channel (A2W_PLLH_PIX,
    as Linux's clock driver reads it), over a fixed 10.

Arguments:

    Clock - Supplies the registers of the clock manager.

    Pllh - Receives the rate of PLLH.

    PixelDivider - Receives the divider of the pixel channel of PLLH.

Return Value:

    The pixel clock in Hz, or 0 if PLLH is not set up.

--*/

{
    ULONG Control = READ_REGISTER(Clock, A2W_PLLH_CTRLR);
    ULONG Fraction = READ_REGISTER(Clock, A2W_PLLH_FRACR) & 0xFFFFF;
    ULONG Multiplier = Control & 0x3FF;
    ULONG Predivider = (Control >> 12) & 7;
    ULONG Divider = READ_REGISTER(Clock, A2W_PLLH_PIX) & 0xFF;

    if (Divider == 0) {
        Divider = 256;
    }

    *PixelDivider = Divider;
    if ((READ_REGISTER(Clock, A2W_PLLH_ANA1) & (1 << 11)) != 0) {
        Multiplier *= 2;
        Fraction *= 2;
    }

    if (Predivider == 0) {
        *Pllh = 0;
        return 0;
    }

    *Pllh = (ULONG)((((ULONGLONG)CRYSTAL_RATE * (((ULONGLONG)Multiplier << 20) + Fraction)) / Predivider) >> 20);
    return *Pllh / Divider / 10;
}

//
// The display's EDID: does it take sound (a CEA extension with the basic
// audio bit or an LPCM descriptor, from an HDMI sink)?
//

static BOOLEAN
EdidBlock(
    IN PHDMI_STATE Hdmi,
    IN ULONG Block,
    OUT PUCHAR Data
    )
{
    ULONG Value[2 + 128 / sizeof(ULONG)];

    Value[0] = Block;
    if (Hdmi->Vc5) {
        Value[1] = Hdmi->Port == 0 ? DISPLAY_HDMI0 : DISPLAY_HDMI1;
        if (!MailboxTag(Hdmi, TAG_GET_EDID_BLOCK_DISPLAY, sizeof(Value), Value, 2)) {
            Value[0] = Block;
            if (!MailboxTag(Hdmi, TAG_GET_EDID_BLOCK, sizeof(Value), Value, 1)) {
                return FALSE;
            }
        }
    } else if (!MailboxTag(Hdmi, TAG_GET_EDID_BLOCK, sizeof(Value), Value, 1)) {
        return FALSE;
    }

    if (Value[1] != 0) {
        return FALSE;
    }

    RtlCopyMemory(Data, &Value[2], 128);
    return TRUE;
}

BOOLEAN
HdmiSinkHasAudio(
    IN PGLOBAL_DEVICE_INFO GlobalInfo
    )
{
    PHDMI_STATE Hdmi = GlobalInfo->Hdmi;
    UCHAR Data[128];
    ULONG Extensions;
    ULONG Block;
    ULONG Offset;
    ULONG End;
    ULONG Length;
    ULONG Index;
    BOOLEAN Audio = FALSE;
    BOOLEAN HdmiSink = FALSE;

    if (Hdmi == NULL || !EdidBlock(Hdmi, 0, Data)) {
        BcmSoundLog(GlobalInfo, "HDMI: no EDID");
        return FALSE;
    }

    Extensions = Data[126];
    for (Block = 1; Block <= Extensions && Block < 4; Block += 1) {
        if (!EdidBlock(Hdmi, Block, Data) || Data[0] != 0x02) {
            continue;
        }

        if ((Data[3] & 0x40) != 0) {
            Audio = TRUE;
        }

        End = Data[2] >= 4 && Data[2] <= 127 ? Data[2] : 4;
        for (Offset = 4; Offset < End; Offset += 1 + Length) {
            Length = Data[Offset] & 0x1F;
            if (Offset + 1 + Length > End) {
                break;
            }

            switch (Data[Offset] >> 5) {
            case 1:
                for (Index = 0; Index + 3 <= Length; Index += 3) {
                    if (((Data[Offset + 1 + Index] >> 3) & 0xF) == 1) {
                        Audio = TRUE;
                    }
                }

                break;

            case 3:
                if (Length >= 3 && Data[Offset + 1] == 0x03 && Data[Offset + 2] == 0x0C && Data[Offset + 3] == 0x00) {
                    HdmiSink = TRUE;
                }

                break;
            }
        }
    }

    BcmSoundLog(GlobalInfo, "HDMI: EDID with %u extension(s): %s sink, audio %s", Extensions,
                HdmiSink ? "HDMI" : "DVI", Audio ? "yes" : "no");
    return Audio && HdmiSink;
}

BOOLEAN
HdmiInitialize(
    IN PGLOBAL_DEVICE_INFO GlobalInfo
    )

/*++

Routine Description:

    This routine finds the HDMI port the firmware drives, and whether audio
    can go on it: its clock runs (else its registers stall the processor),
    it sends HDMI (not DVI) and its packet RAM is on.

Arguments:

    GlobalInfo - Supplies the global device information.

Return Value:

    TRUE if audio can go on the HDMI port.

--*/

{
    ULONG Base = GlobalInfo->Hardware.PeripheralBase;
    PHDMI_STATE Hdmi;
    PADAPTER_OBJECT Adapter;
    DEVICE_DESCRIPTION Description;
    PHYSICAL_ADDRESS Logical;
    ULONG MapRegisterCount;
    ULONG Value[1];
    ULONG Count;
    ULONG Index;
    ULONG Hsm;
    ULONG Scheduler;
    ULONG Packet;
    ULONG Pllh = 0;
    ULONG PixelDivider = 0;
    ULONG FirmwarePixel = 0;
    ULONG Candidate[3];
    ULONG Horizontal;
    ULONG Vertical;
    ULONG Total;
    ULONG Refresh;
    PUCHAR Clock;
    BOOLEAN Hdmi0 = FALSE;
    BOOLEAN Hdmi1 = FALSE;
    UCHAR Edid[128];

    Hdmi = ExAllocatePool(NonPagedPool, sizeof(HDMI_STATE));
    if (Hdmi == NULL) {
        return FALSE;
    }

    RtlZeroMemory(Hdmi, sizeof(HDMI_STATE));
    Hdmi->Vc5 = (GlobalInfo->Hardware.Flags & BCMSOUND_BCM2711) != 0;
    Hdmi->Mailbox = MapRegisters(Base + MAILBOX_OFFSET, 0x40);
    RtlZeroMemory(&Description, sizeof(Description));
    Description.Version = DEVICE_DESCRIPTION_VERSION;
    Description.Master = TRUE;
    Description.Dma32BitAddresses = TRUE;
    Description.InterfaceType = Internal;
    Description.MaximumLength = PAGE_SIZE;
    Adapter = HalGetAdapter(&Description, &MapRegisterCount);
    if (Adapter != NULL) {
        Hdmi->MailboxBuffer = HalAllocateCommonBuffer(Adapter, PAGE_SIZE, &Logical, FALSE);
        Hdmi->MailboxBufferBus = Logical.LowPart;
    }

    if (Hdmi->Mailbox == NULL || Hdmi->MailboxBuffer == NULL) {
        BcmSoundLog(GlobalInfo, "HDMI: no mailbox");
        ExFreePool(Hdmi);
        return FALSE;
    }

    if (Hdmi->Vc5) {
        Hsm = MailboxClockRate(Hdmi, CLOCK_HSM);
    } else {
        Clock = MapRegisters(Base + CM_OFFSET, CM_SIZE);
        Hsm = Clock != NULL ? Vc4HsmRate(Clock) : 0;
        if (Clock != NULL) {
            Hdmi->PixelClock = Vc4PixelRate(Clock, &Pllh, &PixelDivider);
            FirmwarePixel = MailboxClockRate(Hdmi, CLOCK_PIXEL);
            BcmSoundLog(GlobalInfo,
                        "HDMI: PLLH %u Hz, pixel divider %u (A2W_PLLH_PIX %08X); firmware pixel clock %u Hz",
                        Pllh, PixelDivider, READ_REGISTER(Clock, A2W_PLLH_PIX), FirmwarePixel);
        }
    }

    BcmSoundLog(GlobalInfo, "HDMI: HSM clock %u Hz", Hsm);
    if (Hsm == 0) {
        ExFreePool(Hdmi);
        return FALSE;
    }

    if (Hdmi->Vc5) {
        Value[0] = 0;
        Count = MailboxTag(Hdmi, TAG_GET_NUM_DISPLAYS, sizeof(Value), Value, 0) ? Value[0] : 0;
        for (Index = 0; Index < Count && Index < 8; Index += 1) {
            Value[0] = Index;
            if (MailboxTag(Hdmi, TAG_GET_DISPLAY_ID, sizeof(Value), Value, 1)) {
                Hdmi0 |= Value[0] == DISPLAY_HDMI0;
                Hdmi1 |= Value[0] == DISPLAY_HDMI1;
            }
        }

        Hdmi->Port = !Hdmi0 && Hdmi1 ? 1 : 0;
        BcmSoundLog(GlobalInfo, "HDMI: %u display(s)%s%s; port HDMI%u", Count, Hdmi0 ? ", HDMI0" : "",
                    Hdmi1 ? ", HDMI1" : "", Hdmi->Port);
    }

    //
    // A display that gave the firmware its EDID: else no audio, and no HDMI
    // register is touched (QEMU has none).
    //

    if (!EdidBlock(Hdmi, 0, Edid)) {
        BcmSoundLog(GlobalInfo, "HDMI: no EDID from the firmware: no HDMI audio");
        ExFreePool(Hdmi);
        return FALSE;
    }

    if (Hdmi->Vc5) {
        Hdmi->Core = MapRegisters(Base + VC5_CORE_OFFSET(Hdmi->Port), 0x300);
        Hdmi->Hd = MapRegisters(Base + VC5_HD_OFFSET, 0x100);
        Hdmi->Packet = MapRegisters(Base + VC5_PACKET_OFFSET(Hdmi->Port), 0x200);
        Hdmi->Phy = MapRegisters(Base + VC5_PHY_OFFSET(Hdmi->Port), 0x80);
        Hdmi->MaiCtl = Hdmi->Port == 0 ? 0x10 : 0x30;
        Hdmi->MaiThr = Hdmi->MaiCtl + 0x04;
        Hdmi->MaiFmt = Hdmi->MaiCtl + 0x08;
        Hdmi->MaiData = Hdmi->MaiCtl + 0x0C;
        Hdmi->MaiSmp = Hdmi->MaiCtl + 0x10;
        Hdmi->ChannelMap = 0x9C;
        Hdmi->MaiConfig = 0xA0;
        Hdmi->AudioPacket = 0xB8;
        Hdmi->RamConfig = 0xBC;
        Hdmi->RamStatus = 0xC4;
        Hdmi->Crp = 0xC8;
        Hdmi->Cts0 = 0xCC;
        Hdmi->Cts1 = 0xD0;
        Hdmi->Scheduler = 0xE0;
        Hdmi->PhyControl = 0x004;
        Hdmi->PhyRngBit = 1 << 4;
        Hdmi->ThresholdValue = 0x10101010;
        Hdmi->ChannelMapValue = 0x10;
        Hdmi->AudioClock = 108000000;
        Hdmi->PixelClock = MailboxClockRate(Hdmi, CLOCK_PIXEL_BVB);
        Hdmi->Dreq = Hdmi->Port == 0 ? 10 : 17;
        Hdmi->FifoBus = BUS_PERIPHERAL_BASE + VC5_HD_OFFSET + Hdmi->MaiData;
    } else {
        Hdmi->Core = MapRegisters(Base + VC4_CORE_OFFSET, 0x600);
        Hdmi->Hd = MapRegisters(Base + VC4_HD_OFFSET, 0x100);
        Hdmi->Packet = Hdmi->Core != NULL ? Hdmi->Core + 0x400 : NULL;
        Hdmi->Phy = Hdmi->Core;
        Hdmi->MaiCtl = 0x14;
        Hdmi->MaiThr = 0x18;
        Hdmi->MaiFmt = 0x1C;
        Hdmi->MaiData = 0x20;
        Hdmi->MaiSmp = 0x2C;
        Hdmi->ChannelMap = 0x90;
        Hdmi->MaiConfig = 0x94;
        Hdmi->AudioPacket = 0x9C;
        Hdmi->RamConfig = 0xA0;
        Hdmi->RamStatus = 0xA4;
        Hdmi->Crp = 0xA8;
        Hdmi->Cts0 = 0xAC;
        Hdmi->Cts1 = 0xB0;
        Hdmi->Scheduler = 0xC0;
        Hdmi->PhyControl = 0x2C4;
        Hdmi->PhyRngBit = 1 << 25;
        Hdmi->ThresholdValue = 0x10101010;
        Hdmi->ChannelMapValue = 0x08;
        Hdmi->AudioClock = Hsm;
        Hdmi->Dreq = 17;
        Hdmi->FifoBus = BUS_PERIPHERAL_BASE + VC4_HD_OFFSET + Hdmi->MaiData;
    }

    if (Hdmi->Core == NULL || Hdmi->Hd == NULL || Hdmi->Packet == NULL || Hdmi->Phy == NULL) {
        BcmSoundLog(GlobalInfo, "HDMI: cannot map the registers");
        ExFreePool(Hdmi);
        return FALSE;
    }

    Scheduler = READ_REGISTER(Hdmi->Core, Hdmi->Scheduler);
    Packet = READ_REGISTER(Hdmi->Core, Hdmi->RamConfig);
    BcmSoundLog(GlobalInfo, "HDMI: scheduler %08X, packet RAM %08X, MAI %08X, pixel clock %u Hz", Scheduler, Packet,
                READ_REGISTER(Hdmi->Hd, Hdmi->MaiCtl), Hdmi->PixelClock);
    if (!Hdmi->Vc5) {

        //
        // The video timing (HORZA, HORZB, VERTA0, VERTB0), whose totals say
        // which of the pixel clocks is the mode's: PLLH through its pixel
        // divider (as Linux reads it), the firmware's, or PLLH's tenth (as
        // Circle reads it); the first that gives a refresh rate of 23 to
        // 122 Hz, else the totals at 60 Hz. The audio clock recovery (CTS)
        // must be of the real pixel clock, or the display cannot make the
        // sample rate and stays silent.
        //

        ULONG HorzA = READ_REGISTER(Hdmi->Core, 0xC4);
        ULONG HorzB = READ_REGISTER(Hdmi->Core, 0xC8);
        ULONG VertA = READ_REGISTER(Hdmi->Core, 0xCC);
        ULONG VertB = READ_REGISTER(Hdmi->Core, 0xD0);

        BcmSoundLog(GlobalInfo, "HDMI: timing %08X %08X %08X %08X", HorzA, HorzB, VertA, VertB);
        Horizontal = (HorzA & 0x1FFF) + (HorzB & 0x3FF) + ((HorzB >> 10) & 0x3FF) + ((HorzB >> 20) & 0x3FF);
        Vertical = (VertA & 0x1FFF) + ((VertA >> 13) & 0x7F) + ((VertA >> 20) & 0x1F) + (VertB & 0x1FF);
        Total = Horizontal * Vertical;
        Candidate[0] = Hdmi->PixelClock;
        Candidate[1] = FirmwarePixel;
        Candidate[2] = Pllh / 10;
        if (Total != 0) {
            for (Index = 0; Index < 3; Index += 1) {
                Refresh = (ULONG)(((ULONGLONG)Candidate[Index] * 100) / Total);
                BcmSoundLog(GlobalInfo, "HDMI: pixel clock %u Hz would be %u.%02u Hz of refresh", Candidate[Index],
                            Refresh / 100, Refresh % 100);
                if (Refresh >= 2300 && Refresh <= 12200) {
                    break;
                }
            }

            Hdmi->PixelClock = Index < 3 ? Candidate[Index] : Total * 60;
        }

        BcmSoundLog(GlobalInfo, "HDMI: mode %ux%u (total %ux%u), pixel clock %u Hz", HorzA & 0x1FFF, VertA & 0x1FFF,
                    Horizontal, Vertical, Hdmi->PixelClock);
    }

    //
    // What the firmware left of the audio.
    //

    BcmSoundLog(GlobalInfo, "HDMI: firmware's audio: CRP %08X CTS %08X %08X, MAI SMP %08X FMT %08X THR %08X",
                READ_REGISTER(Hdmi->Core, Hdmi->Crp), READ_REGISTER(Hdmi->Core, Hdmi->Cts0),
                READ_REGISTER(Hdmi->Core, Hdmi->Cts1), READ_REGISTER(Hdmi->Hd, Hdmi->MaiSmp),
                READ_REGISTER(Hdmi->Hd, Hdmi->MaiFmt), READ_REGISTER(Hdmi->Hd, Hdmi->MaiThr));
    BcmSoundLog(GlobalInfo, "    MAI config %08X, channel map %08X, audio packet %08X, PHY %08X, InfoFrame %08X %08X",
                READ_REGISTER(Hdmi->Core, Hdmi->MaiConfig), READ_REGISTER(Hdmi->Core, Hdmi->ChannelMap),
                READ_REGISTER(Hdmi->Core, Hdmi->AudioPacket), READ_REGISTER(Hdmi->Phy, Hdmi->PhyControl),
                READ_REGISTER(Hdmi->Packet, AUDIO_SLOT_OFFSET), READ_REGISTER(Hdmi->Packet, AUDIO_SLOT_OFFSET + 4));

    if ((Scheduler & SCHEDULER_HDMI) != SCHEDULER_HDMI || (Packet & RAM_PACKET_ENABLE) == 0 ||
        Hdmi->PixelClock == 0) {
        BcmSoundLog(GlobalInfo, "HDMI: the display is not in HDMI mode with packets: no HDMI audio");
        ExFreePool(Hdmi);
        return FALSE;
    }

    GlobalInfo->Hdmi = Hdmi;
    return TRUE;
}

ULONG
HdmiFifoBus(
    IN PGLOBAL_DEVICE_INFO GlobalInfo
    )
{
    return ((PHDMI_STATE)GlobalInfo->Hdmi)->FifoBus;
}

ULONG
HdmiDreq(
    IN PGLOBAL_DEVICE_INFO GlobalInfo
    )
{
    return ((PHDMI_STATE)GlobalInfo->Hdmi)->Dreq;
}

//
// The sample rate divider: n/m nearest the audio clock over the rate, n at
// most 24 bits, m at most 256.
//

static ULONG
SampleDivider(
    IN ULONG Clock,
    IN ULONG Rate
    )
{
    ULONGLONG Best = (ULONGLONG)-1;
    ULONGLONG Error;
    ULONGLONG Product;
    ULONG M;
    ULONG N;
    ULONG BestN = 1;
    ULONG BestM = 1;

    for (M = 1; M <= 256; M += 1) {
        Product = (ULONGLONG)Clock * M;
        N = (ULONG)((Product + Rate / 2) / Rate);
        if (N == 0 || N > 0xFFFFFF) {
            continue;
        }

        Error = Product > (ULONGLONG)N * Rate ? Product - (ULONGLONG)N * Rate : (ULONGLONG)N * Rate - Product;
        if (Error * BestM < Best * M || Best == (ULONGLONG)-1) {
            Best = Error;
            BestN = N;
            BestM = M;
            if (Error == 0) {
                break;
            }
        }
    }

    return (BestN << 8) | (BestM - 1);
}

static BOOLEAN
WaitPacket(
    IN PHDMI_STATE Hdmi,
    IN ULONG Value
    )
{
    ULONG Count;

    for (Count = 0; Count < 10000; Count += 1) {
        if ((READ_REGISTER(Hdmi->Core, Hdmi->RamStatus) & RAM_PACKET_AUDIO) == Value) {
            return TRUE;
        }

        KeStallExecutionProcessor(10);
    }

    return FALSE;
}

BOOLEAN
HdmiStart(
    IN PGLOBAL_DEVICE_INFO GlobalInfo,
    IN ULONG Rate
    )

/*++

Routine Description:

    This routine sets HDMI audio up for 32, 44.1 or 48 kHz, two channels,
    before the DMA channel starts (HdmiEnable turns it on after). It is
    called at DISPATCH_LEVEL.

Arguments:

    GlobalInfo - Supplies the global device information.

    Rate - Supplies the sample rate.

Return Value:

    TRUE, or FALSE for another rate.

--*/

{
    PHDMI_STATE Hdmi = GlobalInfo->Hdmi;
    ULONG N;
    ULONG Cts;
    ULONG Code;
    ULONG Index;
    PUCHAR Slot;

    switch (Rate) {
    case 32000:
        Code = 7;
        Hdmi->ChannelStatus[3] = 3;
        Hdmi->ChannelStatus[4] = 0x0B | (12 << 4);
        break;

    case 44100:
        Code = 8;
        Hdmi->ChannelStatus[3] = 0;
        Hdmi->ChannelStatus[4] = 0x0B | (15 << 4);
        break;

    case 48000:
        Code = 9;
        Hdmi->ChannelStatus[3] = 2;
        Hdmi->ChannelStatus[4] = 0x0B | (13 << 4);
        break;

    default:
        return FALSE;
    }

    Hdmi->ChannelStatus[0] = 0x04;
    Hdmi->ChannelStatus[1] = 0x00;
    Hdmi->ChannelStatus[2] = 0x00;

    WRITE_REGISTER(Hdmi->Hd, Hdmi->MaiCtl, MAI_CTL_RESET | MAI_CTL_FLUSH | MAI_CTL_DLATE | MAI_CTL_ERRORE |
                                           MAI_CTL_ERRORF);
    WRITE_REGISTER(Hdmi->Hd, Hdmi->MaiSmp, SampleDivider(Hdmi->AudioClock, Rate));
    WRITE_REGISTER(Hdmi->Hd, Hdmi->MaiFmt, (2 << 16) | (Code << 8));
    WRITE_REGISTER(Hdmi->Hd, Hdmi->MaiThr, Hdmi->ThresholdValue);
    WRITE_REGISTER(Hdmi->Core, Hdmi->MaiConfig, MAI_CONFIG_VALUE);
    WRITE_REGISTER(Hdmi->Core, Hdmi->ChannelMap, Hdmi->ChannelMapValue);
    WRITE_REGISTER(Hdmi->Core, Hdmi->AudioPacket, AUDIO_PACKET_VALUE);

    N = 128 * Rate / 1000;
    Cts = (ULONG)(((ULONGLONG)Hdmi->PixelClock * N) / (128 * Rate));
    Hdmi->N = N;
    Hdmi->Cts = Cts;
    WRITE_REGISTER(Hdmi->Core, Hdmi->Crp, CRP_EXTERNAL_CTS_EN | N);
    WRITE_REGISTER(Hdmi->Core, Hdmi->Cts0, Cts);
    WRITE_REGISTER(Hdmi->Core, Hdmi->Cts1, Cts);

    //
    // The audio InfoFrame (type 0x84, version 1, length 10; two channels,
    // the rest from the stream), into slot 4 while it is off.
    //

    WRITE_REGISTER(Hdmi->Core, Hdmi->RamConfig, READ_REGISTER(Hdmi->Core, Hdmi->RamConfig) & ~RAM_PACKET_AUDIO);
    WaitPacket(Hdmi, 0);
    Slot = Hdmi->Packet + AUDIO_SLOT_OFFSET;
    WRITE_REGISTER(Slot, 0, 0x000A0184);
    WRITE_REGISTER(Slot, 4, 0x00000170);
    for (Index = 2; Index < 9; Index += 1) {
        WRITE_REGISTER(Slot, Index * 4, 0);
    }

    WRITE_REGISTER(Hdmi->Core, Hdmi->RamConfig, READ_REGISTER(Hdmi->Core, Hdmi->RamConfig) | RAM_PACKET_AUDIO);
    WaitPacket(Hdmi, RAM_PACKET_AUDIO);
    return TRUE;
}

VOID
HdmiEnable(
    IN PGLOBAL_DEVICE_INFO GlobalInfo
    )

/*++

Routine Description:

    This routine turns the audio on once the DMA channel feeds the MAI: the
    RNG of the PHY, then the MAI (as Circle and Linux do, last).

Arguments:

    GlobalInfo - Supplies the global device information.

Return Value:

    None.

--*/

{
    PHDMI_STATE Hdmi = GlobalInfo->Hdmi;

    WRITE_REGISTER(Hdmi->Phy, Hdmi->PhyControl, READ_REGISTER(Hdmi->Phy, Hdmi->PhyControl) & ~Hdmi->PhyRngBit);
    WRITE_REGISTER(Hdmi->Hd, Hdmi->MaiCtl, MAI_CTL_CHNUM(2) | MAI_CTL_WHOLSMP | MAI_CTL_CHALIGN | MAI_CTL_ENABLE);
}

VOID
HdmiStop(
    IN PGLOBAL_DEVICE_INFO GlobalInfo
    )
{
    PHDMI_STATE Hdmi = GlobalInfo->Hdmi;

    WRITE_REGISTER(Hdmi->Hd, Hdmi->MaiCtl, MAI_CTL_DLATE | MAI_CTL_ERRORE | MAI_CTL_ERRORF);
    WRITE_REGISTER(Hdmi->Phy, Hdmi->PhyControl, READ_REGISTER(Hdmi->Phy, Hdmi->PhyControl) | Hdmi->PhyRngBit);
    WRITE_REGISTER(Hdmi->Core, Hdmi->RamConfig, READ_REGISTER(Hdmi->Core, Hdmi->RamConfig) & ~RAM_PACKET_AUDIO);
    WRITE_REGISTER(Hdmi->Hd, Hdmi->MaiCtl, MAI_CTL_RESET);
    WRITE_REGISTER(Hdmi->Hd, Hdmi->MaiCtl, MAI_CTL_ERRORF);
    WRITE_REGISTER(Hdmi->Hd, Hdmi->MaiCtl, MAI_CTL_FLUSH);
}

VOID
HdmiResetFrames(
    IN PGLOBAL_DEVICE_INFO GlobalInfo
    )
{
    ((PHDMI_STATE)GlobalInfo->Hdmi)->Frame = 0;
}

static ULONG
Subframe(
    IN ULONG Preamble,
    IN LONG Sample,
    IN ULONG Status
    )
{
    ULONG Word = ((ULONG)(USHORT)(SHORT)Sample << 12) | (Status << 30);
    ULONG Parity = Word;

    Parity ^= Parity >> 16;
    Parity ^= Parity >> 8;
    Parity ^= Parity >> 4;
    return Word | (((0x6996 >> (Parity & 0xF)) & 1) << 31) | Preamble;
}

VOID
HdmiEncode(
    IN PGLOBAL_DEVICE_INFO GlobalInfo,
    OUT PULONG Words,
    IN LONG Left,
    IN LONG Right
    )
{
    PHDMI_STATE Hdmi = GlobalInfo->Hdmi;
    ULONG Frame = Hdmi->Frame;
    ULONG Status = Frame < 40 ? (Hdmi->ChannelStatus[Frame >> 3] >> (Frame & 7)) & 1 : 0;
    ULONG Preamble = Frame == 0 ? 0x0F : 0;

    Words[0] = Subframe(Preamble, Left, Status);
    Words[1] = Subframe(Preamble, Right, Status);
    Hdmi->Frame = Frame == 191 ? 0 : Frame + 1;
}

ULONG
HdmiReadControl(
    IN PGLOBAL_DEVICE_INFO GlobalInfo
    )
{
    PHDMI_STATE Hdmi = GlobalInfo->Hdmi;

    return READ_REGISTER(Hdmi->Hd, Hdmi->MaiCtl);
}

VOID
HdmiLogState(
    IN PGLOBAL_DEVICE_INFO GlobalInfo
    )

/*++

Routine Description:

    This routine logs (debug_sound=1) the audio of the HDMI as it runs: the
    clock recovery set and read back, the packets and the MAI. It is called
    at PASSIVE_LEVEL.

Arguments:

    GlobalInfo - Supplies the global device information.

Return Value:

    None.

--*/

{
    PHDMI_STATE Hdmi = GlobalInfo->Hdmi;

    BcmSoundLog(GlobalInfo, "    HDMI: N %u CTS %u (pixel clock %u Hz); CRP %08X CTS %08X %08X, packets %08X/%08X",
                Hdmi->N, Hdmi->Cts, Hdmi->PixelClock, READ_REGISTER(Hdmi->Core, Hdmi->Crp),
                READ_REGISTER(Hdmi->Core, Hdmi->Cts0), READ_REGISTER(Hdmi->Core, Hdmi->Cts1),
                READ_REGISTER(Hdmi->Core, Hdmi->RamConfig), READ_REGISTER(Hdmi->Core, Hdmi->RamStatus));
    BcmSoundLog(GlobalInfo, "    MAI CTL %08X SMP %08X FMT %08X, audio packet %08X, InfoFrame %08X %08X",
                READ_REGISTER(Hdmi->Hd, Hdmi->MaiCtl), READ_REGISTER(Hdmi->Hd, Hdmi->MaiSmp),
                READ_REGISTER(Hdmi->Hd, Hdmi->MaiFmt), READ_REGISTER(Hdmi->Core, Hdmi->AudioPacket),
                READ_REGISTER(Hdmi->Packet, AUDIO_SLOT_OFFSET), READ_REGISTER(Hdmi->Packet, AUDIO_SLOT_OFFSET + 4));
}
