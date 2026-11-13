/*++

Module Name:

    bcmemmc.c

Abstract:

    This is the miniport driver for the SD card of the Raspberry Pi, on the
    controller the ARC firmware found it on and describes as the ScsiAdapter
    of the system: the SD host controller (SDHOST, "BCM2835-SDHOST") of the
    BCM2837 in the Raspberry Pi 3 and Zero 2 W, from which the VideoCore
    boots and which leaves the EMMC to the wireless chip; the EMMC (an Arasan
    SD host controller, SDHCI 3.0, "BCM2835-EMMC"), to which the firmware
    connects the slot on a board without the wireless chip (Pi 2) or when no
    card answers on the SDHOST, and the card controller of QEMU's raspi4b; or the
    EMMC2 of the BCM2711 in the Raspberry Pi 4 (SDHCI 3.0 too,
    "BCM2711-EMMC2"), which holds the card slot of every BCM2711 board. The
    controllers are told apart by the offset of their registers into the
    BCM2835 peripherals: 0x202000, 0x300000 and 0x340000. The card is
    presented to the SCSI class drivers as a direct access device at target 0,
    as the ATAPI miniport presents IDE disks.

    The firmware describes the controller with its registers and the VideoCore
    mailbox as its two memory ranges. Blocks move by programmed I/O through
    the controller's data port on its interrupts. The controller's base clock
    is asked of the VideoCore through the mailbox property channel: the clock
    of the EMMC or the EMMC2, or for the SDHOST the core clock, whose maximum
    rate is taken, as the VideoCore may raise the core clock with load.

    SDHCI: a block moves on each buffer ready interrupt, and the transfer
    complete interrupt, after the automatic CMD12 of a multiple block
    transfer, ends the request.

    SDHOST: a command goes to SDCMD with the NEW flag, which clears when the
    response is in SDRSP0-3 (a long response keeps its CRC byte, which the
    SDHCI drops), and its FAIL flag and SDHSTS report errors. Data moves
    through a 16-word FIFO whose level SDEDM shows: a block of a read on each
    data interrupt; the first block of a write on the data interrupt (the FIFO
    has room), the next ones on the block interrupts that end each block, as
    Linux does. There is no automatic CMD12: every transfer is a multiple
    block command (CMD18, CMD25) and ends with a CMD12, whose busy interrupt,
    when the card is done, ends the request.

Environment:

    kernel mode only

--*/

#include "miniport.h"
#include "scsi.h"

//
// A command as the card code gives it: the index of the command, its response
// and its data. Each controller translates it to its own registers. A short
// response has 48 bits (R1, R3, R6, R7), then busy on DAT0 for R1b; a long
// one 136 bits (R2). R3 is a response without a CRC. An application command
// has CMD55 first.
//

#define SD_INDEX_MASK       0x0000003F
#define SD_RESP_NONE        0x00000000
#define SD_RESP_SHORT       0x00000100
#define SD_RESP_SHORT_BUSY  0x00000200
#define SD_RESP_LONG        0x00000300
#define SD_RESP_MASK        0x00000300
#define SD_RESP_NO_CRC      0x00000400
#define SD_DATA_READ        0x00001000
#define SD_DATA_WRITE       0x00002000
#define SD_DATA_MULTIPLE    0x00004000
#define SD_APP              0x00008000

#define CMD(n)              (n)
#define APP                 SD_APP
#define R1                  SD_RESP_SHORT
#define R1B                 SD_RESP_SHORT_BUSY
#define R2                  SD_RESP_LONG
#define R3                  (SD_RESP_SHORT | SD_RESP_NO_CRC)
#define R6                  SD_RESP_SHORT
#define R7                  SD_RESP_SHORT

//
// SDHCI (EMMC, EMMC2) registers and their fields.
//

#define BLKSIZECNT          0x04
#define ARG1                0x08
#define CMDTM               0x0C
#define RESP0               0x10
#define RESP1               0x14
#define RESP2               0x18
#define RESP3               0x1C
#define DATA                0x20
#define STATUS              0x24
#define CONTROL0            0x28
#define CONTROL1            0x2C
#define INTERRUPT           0x30
#define IRPT_MASK           0x34
#define IRPT_EN             0x38

#define STATUS_CMD_INHIBIT  0x00000001
#define STATUS_DAT_INHIBIT  0x00000002

#define C0_HCTL_DWIDTH      0x00000002
#define C0_POWER_330        0x00000F00

#define C1_CLK_INTLEN       0x00000001
#define C1_CLK_STABLE       0x00000002
#define C1_CLK_EN           0x00000004
#define C1_TOUNIT_MAX       0x000E0000
#define C1_SRST_HC          0x01000000
#define C1_SRST_CMD         0x02000000
#define C1_SRST_DATA        0x04000000

#define INT_CMD_DONE        0x00000001
#define INT_DATA_DONE       0x00000002
#define INT_WRITE_RDY       0x00000010
#define INT_READ_RDY        0x00000020
#define INT_ERR             0x00008000
#define INT_ERRORS          0x017F0000

#define TM_BLKCNT_EN        0x00000002
#define TM_AUTO_CMD12       0x00000004
#define TM_DAT_DIR_READ     0x00000010
#define TM_MULTI_BLOCK      0x00000020

#define RSP_NONE            0x00000000
#define RSP_136             0x00010000
#define RSP_48              0x00020000
#define RSP_48_BUSY         0x00030000
#define CMD_CRCCHK          0x00080000
#define CMD_IXCHK           0x00100000
#define CMD_ISDATA          0x00200000

//
// SDHOST registers and their fields.
//

#define SDCMD                       0x00
#define SDARG                       0x04
#define SDTOUT                      0x08
#define SDCDIV                      0x0C
#define SDRSP0                      0x10
#define SDRSP1                      0x14
#define SDRSP2                      0x18
#define SDRSP3                      0x1C
#define SDHSTS                      0x20
#define SDVDD                       0x30
#define SDEDM                       0x34
#define SDHCFG                      0x38
#define SDHBCT                      0x3C
#define SDDATA                      0x40
#define SDHBLC                      0x50

#define SDCMD_NEW                   0x00008000
#define SDCMD_FAIL                  0x00004000
#define SDCMD_BUSYWAIT              0x00000800
#define SDCMD_NO_RESPONSE           0x00000400
#define SDCMD_LONG_RESPONSE         0x00000200
#define SDCMD_WRITE                 0x00000080
#define SDCMD_READ                  0x00000040

#define SDCDIV_MAX                  0x7FF

#define SDHSTS_BUSY_IRPT            0x00000400
#define SDHSTS_BLOCK_IRPT           0x00000200
#define SDHSTS_SDIO_IRPT            0x00000100
#define SDHSTS_REW_TIME_OUT         0x00000080
#define SDHSTS_CMD_TIME_OUT         0x00000040
#define SDHSTS_CRC16_ERROR          0x00000020
#define SDHSTS_CRC7_ERROR           0x00000010
#define SDHSTS_FIFO_ERROR           0x00000008
#define SDHSTS_DATA_FLAG            0x00000001
#define SDHSTS_ERRORS               (SDHSTS_REW_TIME_OUT | SDHSTS_CMD_TIME_OUT | SDHSTS_CRC16_ERROR | \
                                     SDHSTS_CRC7_ERROR | SDHSTS_FIFO_ERROR)
#define SDHSTS_INTERRUPTS           (SDHSTS_BUSY_IRPT | SDHSTS_BLOCK_IRPT | SDHSTS_SDIO_IRPT)
#define SDHSTS_CLEAR                (SDHSTS_INTERRUPTS | SDHSTS_ERRORS)

#define SDHCFG_BUSY_IRPT_EN         0x00000400
#define SDHCFG_BLOCK_IRPT_EN        0x00000100
#define SDHCFG_SDIO_IRPT_EN         0x00000020
#define SDHCFG_DATA_IRPT_EN         0x00000010
#define SDHCFG_SLOW_CARD            0x00000008
#define SDHCFG_WIDE_EXT_BUS         0x00000004
#define SDHCFG_WIDE_INT_BUS         0x00000002

#define SDEDM_FSM_MASK              0x0000000F
#define SDEDM_FSM_READWAIT          0x4
#define SDEDM_FSM_WRITESTART1       0xA
#define SDEDM_FIFO_LEVEL(e)         (((e) >> 4) & 0x1F)
#define SDEDM_WRITE_THRESHOLD_SHIFT 9
#define SDEDM_READ_THRESHOLD_SHIFT  14
#define SDEDM_THRESHOLD_MASK        0x1F
#define SDEDM_FORCE_DATA_MODE       0x00080000

#define SDVDD_POWER_ON              1
#define SDHOST_FIFO_WORDS           16
#define SDHOST_FIFO_THRESHOLD       4
#define SDHOST_PIO_BURST            8
#define SDHOST_RESET_TIMEOUT        0xF00000

//
// VideoCore mailbox registers and the property channel.
//

#define MBOX_READ           0x00
#define MBOX_STATUS         0x18
#define MBOX_WRITE          0x20
#define MBOX_WR_FULL        0x80000000
#define MBOX_RD_EMPTY       0x40000000
#define MBOX_PROPERTY       8
#define MBOX_SUCCESS        0x80000000
#define TAG_GET_CLOCK_RATE  0x00030002
#define TAG_GET_MAX_CLOCK_RATE 0x00030004
#define CLOCK_EMMC          1
#define CLOCK_CORE          4
#define CLOCK_EMMC2         12
#define SDHOST_OFFSET       0x202000
#define EMMC2_OFFSET        0x340000
#define PROPERTY_LENGTH     8

#define SECTOR_SIZE         512
#define SECTOR_ULONGS       (SECTOR_SIZE / sizeof(ULONG))
#define IDENT_CLOCK         400000
#define TRANSFER_CLOCK      25000000
#define MAXIMUM_TRANSFER    0x10000
#define TRANSFER_PAGES      (MAXIMUM_TRANSFER / 0x1000)
#define TIMEOUT_US          1000000
#define BLOCK_TIMEOUT_US    10000

typedef struct _HW_DEVICE_EXTENSION {
    PULONG Registers;
    PULONG Mailbox;
    PULONG Property;
    ULONG PropertyAddress;
    BOOLEAN Sdhost;
    ULONG ClockId;
    ULONG BaseClock;
    ULONG WriteDelay;
    ULONG Config;
    ULONG ResponseType;
    ULONG Rca;
    ULONG Sectors;
    BOOLEAN HighCapacity;
    BOOLEAN CardPresent;
    BOOLEAN Read;
    BOOLEAN Stopping;
    BOOLEAN DataActive;
    PSCSI_REQUEST_BLOCK CurrentSrb;
    PUCHAR DataBuffer;
    ULONG BlocksLeft;
    ULONG Bounce[SECTOR_ULONGS];
} HW_DEVICE_EXTENSION, *PHW_DEVICE_EXTENSION;

BOOLEAN
BcmEmmcResetBus(
    IN PVOID HwDeviceExtension,
    IN ULONG PathId
    );

static ULONG
HostRead(
    IN PHW_DEVICE_EXTENSION DeviceExtension,
    IN ULONG Register
    )
{
    return ScsiPortReadRegisterUlong(DeviceExtension->Registers + Register / sizeof(ULONG));
}

static VOID
HostWrite(
    IN PHW_DEVICE_EXTENSION DeviceExtension,
    IN ULONG Register,
    IN ULONG Value
    )
{
    ScsiPortWriteRegisterUlong(DeviceExtension->Registers + Register / sizeof(ULONG), Value);
}

//
// The SDHCI loses a register write that follows another within two SD clocks,
// so every write but the data port's is followed by a delay.
//

static VOID
EmmcWrite(
    IN PHW_DEVICE_EXTENSION DeviceExtension,
    IN ULONG Register,
    IN ULONG Value
    )
{
    HostWrite(DeviceExtension, Register, Value);
    ScsiPortStallExecution(DeviceExtension->WriteDelay);
}

static BOOLEAN
HostWait(
    IN PHW_DEVICE_EXTENSION DeviceExtension,
    IN ULONG Register,
    IN ULONG Mask,
    IN ULONG Value
    )
{
    ULONG Time;

    for (Time = 0; Time < TIMEOUT_US; Time += 1) {
        if ((HostRead(DeviceExtension, Register) & Mask) == Value) {
            return TRUE;
        }

        ScsiPortStallExecution(1);
    }

    return FALSE;
}

static BOOLEAN
MailboxWait(
    IN PHW_DEVICE_EXTENSION DeviceExtension,
    IN ULONG Mask
    )
{
    ULONG Time;

    for (Time = 0; Time < TIMEOUT_US; Time += 1) {
        if ((ScsiPortReadRegisterUlong(DeviceExtension->Mailbox + MBOX_STATUS / sizeof(ULONG)) & Mask) == 0) {
            return TRUE;
        }

        ScsiPortStallExecution(1);
    }

    return FALSE;
}

//
// Ask the VideoCore for a rate of the controller's clock: its rate, or its
// maximum rate (Tag). The property buffer is uncached memory the VideoCore
// reads and writes; it is filled with register writes so it is in memory
// before the mailbox write that hands it over.
//

static ULONG
HostGetClock(
    IN PHW_DEVICE_EXTENSION DeviceExtension,
    IN ULONG Tag
    )
{
    PULONG Buffer = DeviceExtension->Property;
    ULONG Request[PROPERTY_LENGTH];
    ULONG Index;
    ULONG Message;

    Request[0] = PROPERTY_LENGTH * sizeof(ULONG);
    Request[1] = 0;
    Request[2] = Tag;
    Request[3] = 2 * sizeof(ULONG);
    Request[4] = sizeof(ULONG);
    Request[5] = DeviceExtension->ClockId;
    Request[6] = 0;
    Request[7] = 0;
    for (Index = 0; Index < PROPERTY_LENGTH; Index += 1) {
        ScsiPortWriteRegisterUlong(&Buffer[Index], Request[Index]);
    }

    if (!MailboxWait(DeviceExtension, MBOX_WR_FULL)) {
        return 0;
    }

    ScsiPortWriteRegisterUlong(DeviceExtension->Mailbox + MBOX_WRITE / sizeof(ULONG),
                               DeviceExtension->PropertyAddress | MBOX_PROPERTY);

    do {
        if (!MailboxWait(DeviceExtension, MBOX_RD_EMPTY)) {
            return 0;
        }

        Message = ScsiPortReadRegisterUlong(DeviceExtension->Mailbox + MBOX_READ / sizeof(ULONG));
    } while ((Message & 0xF) != MBOX_PROPERTY);

    ScsiPortWriteRegisterUlong(&Buffer[7], 0);
    if (ScsiPortReadRegisterUlong(&Buffer[1]) != MBOX_SUCCESS) {
        return 0;
    }

    return ScsiPortReadRegisterUlong(&Buffer[6]);
}

//
// The SDHCI.
//

//
// Program the SD clock: the 10-bit divided clock is base / (2 * divisor).
//

static BOOLEAN
EmmcSetClock(
    IN PHW_DEVICE_EXTENSION DeviceExtension,
    IN ULONG Hz
    )
{
    ULONG Divisor = (DeviceExtension->BaseClock + 2 * Hz - 1) / (2 * Hz);
    ULONG Control;

    if (Divisor > 0x3FF) {
        Divisor = 0x3FF;
    }

    Control = HostRead(DeviceExtension, CONTROL1) & ~(C1_CLK_EN | 0xFFE0);
    EmmcWrite(DeviceExtension, CONTROL1, Control);
    Control |= ((Divisor & 0xFF) << 8) | ((Divisor >> 8) << 6) | C1_CLK_INTLEN | C1_TOUNIT_MAX;
    EmmcWrite(DeviceExtension, CONTROL1, Control);
    if (!HostWait(DeviceExtension, CONTROL1, C1_CLK_STABLE, C1_CLK_STABLE)) {
        return FALSE;
    }

    EmmcWrite(DeviceExtension, CONTROL1, Control | C1_CLK_EN);
    DeviceExtension->WriteDelay = 2 * 1000000 / (DeviceExtension->BaseClock / (2 * Divisor)) + 1;
    return TRUE;
}

static VOID
EmmcResetLines(
    IN PHW_DEVICE_EXTENSION DeviceExtension,
    IN ULONG Lines
    )
{
    EmmcWrite(DeviceExtension, CONTROL1, HostRead(DeviceExtension, CONTROL1) | Lines);
    HostWait(DeviceExtension, CONTROL1, Lines, 0);
}

//
// Reset the controller, power the card and start the identification clock,
// with the interrupts off.
//

static BOOLEAN
EmmcReset(
    IN PHW_DEVICE_EXTENSION DeviceExtension
    )
{
    EmmcWrite(DeviceExtension, CONTROL0, 0);
    EmmcWrite(DeviceExtension, CONTROL1, C1_SRST_HC);
    if (!HostWait(DeviceExtension, CONTROL1, C1_SRST_HC, 0)) {
        return FALSE;
    }

    EmmcWrite(DeviceExtension, CONTROL0, C0_POWER_330);
    if (!EmmcSetClock(DeviceExtension, IDENT_CLOCK)) {
        return FALSE;
    }

    EmmcWrite(DeviceExtension, IRPT_EN, 0);
    EmmcWrite(DeviceExtension, IRPT_MASK, 0xFFFFFFFF);
    EmmcWrite(DeviceExtension, INTERRUPT, 0xFFFFFFFF);
    return TRUE;
}

//
// Issue a command and wait for its response. A data command's blocks move
// afterwards; a multiple block transfer ends with the controller's automatic
// CMD12.
//

static BOOLEAN
EmmcCommand(
    IN PHW_DEVICE_EXTENSION DeviceExtension,
    IN ULONG Command,
    IN ULONG Argument,
    IN ULONG Blocks
    )
{
    ULONG Cmdtm = (Command & SD_INDEX_MASK) << 24;
    ULONG Busy = STATUS_CMD_INHIBIT;

    switch (Command & SD_RESP_MASK) {
    case SD_RESP_SHORT:
        Cmdtm |= (Command & SD_RESP_NO_CRC) != 0 ? RSP_48 : (RSP_48 | CMD_CRCCHK | CMD_IXCHK);
        break;

    case SD_RESP_SHORT_BUSY:
        Cmdtm |= RSP_48_BUSY | CMD_CRCCHK | CMD_IXCHK;
        break;

    case SD_RESP_LONG:
        Cmdtm |= RSP_136 | CMD_CRCCHK;
        break;
    }

    if ((Command & (SD_DATA_READ | SD_DATA_WRITE)) != 0) {
        Cmdtm |= CMD_ISDATA;
        if ((Command & SD_DATA_READ) != 0) {
            Cmdtm |= TM_DAT_DIR_READ;
        }

        if ((Command & SD_DATA_MULTIPLE) != 0) {
            Cmdtm |= TM_MULTI_BLOCK | TM_BLKCNT_EN | TM_AUTO_CMD12;
        }
    }

    if ((Cmdtm & (CMD_ISDATA | RSP_48_BUSY)) != 0) {
        Busy |= STATUS_DAT_INHIBIT;
    }

    if (!HostWait(DeviceExtension, STATUS, Busy, 0)) {
        return FALSE;
    }

    EmmcWrite(DeviceExtension, INTERRUPT, 0xFFFFFFFF);
    if (Blocks != 0) {
        EmmcWrite(DeviceExtension, BLKSIZECNT, (Blocks << 16) | SECTOR_SIZE);
    }

    EmmcWrite(DeviceExtension, ARG1, Argument);
    EmmcWrite(DeviceExtension, CMDTM, Cmdtm);
    if (!HostWait(DeviceExtension, INTERRUPT, INT_CMD_DONE | INT_ERR, INT_CMD_DONE) ||
        (HostRead(DeviceExtension, INTERRUPT) & INT_ERR) != 0) {
        EmmcWrite(DeviceExtension, INTERRUPT, 0xFFFFFFFF);
        EmmcResetLines(DeviceExtension, C1_SRST_CMD | C1_SRST_DATA);
        return FALSE;
    }

    EmmcWrite(DeviceExtension, INTERRUPT, INT_CMD_DONE);
    if ((Cmdtm & RSP_48_BUSY) == RSP_48_BUSY && (Cmdtm & CMD_ISDATA) == 0) {
        HostWait(DeviceExtension, INTERRUPT, INT_DATA_DONE, INT_DATA_DONE);
        EmmcWrite(DeviceExtension, INTERRUPT, INT_DATA_DONE);
    }

    return TRUE;
}

static VOID
EmmcSetBusWidth(
    IN PHW_DEVICE_EXTENSION DeviceExtension,
    IN ULONG Bits
    )
{
    ULONG Control = HostRead(DeviceExtension, CONTROL0);

    EmmcWrite(DeviceExtension, CONTROL0,
              Bits == 4 ? Control | C0_HCTL_DWIDTH : Control & ~C0_HCTL_DWIDTH);
}

//
// The SDHOST.
//

//
// Program the SD clock: the core clock divided by SDCDIV + 2, the largest
// rate at or below Hz; and the timeout of a command or a block, half a second.
//

static BOOLEAN
SdhostSetClock(
    IN PHW_DEVICE_EXTENSION DeviceExtension,
    IN ULONG Hz
    )
{
    ULONG Divisor = DeviceExtension->BaseClock / Hz;

    if (Divisor < 2) {
        Divisor = 2;
    }

    if (DeviceExtension->BaseClock / Divisor > Hz) {
        Divisor += 1;
    }

    Divisor -= 2;
    if (Divisor > SDCDIV_MAX) {
        Divisor = SDCDIV_MAX;
    }

    HostWrite(DeviceExtension, SDCDIV, Divisor);
    HostWrite(DeviceExtension, SDTOUT, DeviceExtension->BaseClock / (Divisor + 2) / 2);
    return TRUE;
}

//
// Clear the errors and take the state machine out of a wait a failed transfer
// left it in; the data interrupts are turned off.
//

static VOID
SdhostRecover(
    IN PHW_DEVICE_EXTENSION DeviceExtension
    )
{
    ULONG Edm = HostRead(DeviceExtension, SDEDM);
    ULONG Fsm = Edm & SDEDM_FSM_MASK;

    HostWrite(DeviceExtension, SDHSTS, SDHSTS_CLEAR | SDHSTS_DATA_FLAG);
    if (Fsm == SDEDM_FSM_READWAIT || Fsm == SDEDM_FSM_WRITESTART1) {
        HostWrite(DeviceExtension, SDEDM, Edm | SDEDM_FORCE_DATA_MODE);
    }

    DeviceExtension->Config &= ~(SDHCFG_DATA_IRPT_EN | SDHCFG_BLOCK_IRPT_EN);
    HostWrite(DeviceExtension, SDHCFG, DeviceExtension->Config);
    DeviceExtension->Stopping = FALSE;
    DeviceExtension->DataActive = FALSE;
}

//
// Reset the controller as Linux does: power off, every register cleared, the
// FIFO thresholds lowered to 4 words (a silicon bug), power on, and the busy
// interrupt enabled, which marks the end of an R1b command's busy. The slow
// card bit keeps the controller from switching to its own fast clock divisor
// in data mode.
//

static BOOLEAN
SdhostReset(
    IN PHW_DEVICE_EXTENSION DeviceExtension
    )
{
    ULONG Edm;

    HostWrite(DeviceExtension, SDVDD, 0);
    HostWrite(DeviceExtension, SDCMD, 0);
    HostWrite(DeviceExtension, SDARG, 0);
    HostWrite(DeviceExtension, SDTOUT, SDHOST_RESET_TIMEOUT);
    HostWrite(DeviceExtension, SDCDIV, 0);
    HostWrite(DeviceExtension, SDHSTS, SDHSTS_CLEAR);
    HostWrite(DeviceExtension, SDHCFG, 0);
    HostWrite(DeviceExtension, SDHBCT, 0);
    HostWrite(DeviceExtension, SDHBLC, 0);

    Edm = HostRead(DeviceExtension, SDEDM);
    Edm &= ~((SDEDM_THRESHOLD_MASK << SDEDM_READ_THRESHOLD_SHIFT) |
             (SDEDM_THRESHOLD_MASK << SDEDM_WRITE_THRESHOLD_SHIFT));
    Edm |= (SDHOST_FIFO_THRESHOLD << SDEDM_READ_THRESHOLD_SHIFT) |
           (SDHOST_FIFO_THRESHOLD << SDEDM_WRITE_THRESHOLD_SHIFT);
    HostWrite(DeviceExtension, SDEDM, Edm);
    ScsiPortStallExecution(20000);
    HostWrite(DeviceExtension, SDVDD, SDVDD_POWER_ON);
    ScsiPortStallExecution(20000);

    DeviceExtension->Config = SDHCFG_BUSY_IRPT_EN | SDHCFG_SLOW_CARD | SDHCFG_WIDE_INT_BUS;
    HostWrite(DeviceExtension, SDHCFG, DeviceExtension->Config);
    DeviceExtension->Stopping = FALSE;
    return SdhostSetClock(DeviceExtension, IDENT_CLOCK);
}

//
// Issue a command: wait for the NEW flag to clear, then look at the FAIL flag.
// The CRC check fails on a response that has no CRC (R3), which is no error.
// An R1b command is complete when the busy interrupt status is set, which is
// waited for (WaitBusy) at initialization; the interrupt routine takes it
// for the CMD12 that ends a transfer.
//

static BOOLEAN
SdhostCommand(
    IN PHW_DEVICE_EXTENSION DeviceExtension,
    IN ULONG Command,
    IN ULONG Argument,
    IN ULONG Blocks,
    IN BOOLEAN WaitBusy
    )
{
    ULONG Sdcmd = Command & SD_INDEX_MASK;
    ULONG Status;

    if (!HostWait(DeviceExtension, SDCMD, SDCMD_NEW, 0)) {
        return FALSE;
    }

    Status = HostRead(DeviceExtension, SDHSTS);
    if ((Status & SDHSTS_ERRORS) != 0) {
        HostWrite(DeviceExtension, SDHSTS, Status & SDHSTS_ERRORS);
    }

    switch (Command & SD_RESP_MASK) {
    case SD_RESP_NONE:
        Sdcmd |= SDCMD_NO_RESPONSE;
        break;

    case SD_RESP_SHORT_BUSY:
        Sdcmd |= SDCMD_BUSYWAIT;
        break;

    case SD_RESP_LONG:
        Sdcmd |= SDCMD_LONG_RESPONSE;
        break;
    }

    if ((Command & SD_DATA_READ) != 0) {
        Sdcmd |= SDCMD_READ;
    }

    if ((Command & SD_DATA_WRITE) != 0) {
        Sdcmd |= SDCMD_WRITE;
    }

    if (Blocks != 0) {
        HostWrite(DeviceExtension, SDHBCT, SECTOR_SIZE);
        HostWrite(DeviceExtension, SDHBLC, Blocks);
    }

    DeviceExtension->ResponseType = Command & SD_RESP_MASK;
    HostWrite(DeviceExtension, SDARG, Argument);
    HostWrite(DeviceExtension, SDCMD, Sdcmd | SDCMD_NEW);

    if (!HostWait(DeviceExtension, SDCMD, SDCMD_NEW, 0)) {
        SdhostRecover(DeviceExtension);
        return FALSE;
    }

    if ((HostRead(DeviceExtension, SDCMD) & SDCMD_FAIL) != 0) {
        Status = HostRead(DeviceExtension, SDHSTS) & SDHSTS_ERRORS;
        if (Status != SDHSTS_CRC7_ERROR || (Command & SD_RESP_NO_CRC) == 0) {
            SdhostRecover(DeviceExtension);
            return FALSE;
        }

        HostWrite(DeviceExtension, SDHSTS, SDHSTS_CRC7_ERROR);
    }

    if (WaitBusy && (Command & SD_RESP_MASK) == SD_RESP_SHORT_BUSY) {
        if (!HostWait(DeviceExtension, SDHSTS, SDHSTS_BUSY_IRPT, SDHSTS_BUSY_IRPT)) {
            SdhostRecover(DeviceExtension);
            return FALSE;
        }

        HostWrite(DeviceExtension, SDHSTS, SDHSTS_BUSY_IRPT);
    }

    return TRUE;
}

static VOID
SdhostSetBusWidth(
    IN PHW_DEVICE_EXTENSION DeviceExtension,
    IN ULONG Bits
    )
{
    DeviceExtension->Config &= ~SDHCFG_WIDE_EXT_BUS;
    if (Bits == 4) {
        DeviceExtension->Config |= SDHCFG_WIDE_EXT_BUS;
    }

    HostWrite(DeviceExtension, SDHCFG, DeviceExtension->Config);
}

//
// Move the next block of the current request through the FIFO, in bursts of
// up to 8 words as its level allows; a misaligned buffer goes through the
// bounce block. The FIFO fills, or drains, at the SD clock: a block takes
// some 160 microseconds at 25 MHz on the 4-bit bus, and a card that stops
// within one is given up after BLOCK_TIMEOUT_US. The block moves in the
// interrupt service routine: a move at a lower level (CallEnableInterrupts)
// would need the controller's interrupts off meanwhile, and the end of a
// block that comes then is not noted (so in QEMU's model of the controller).
//

static BOOLEAN
SdhostMoveBlock(
    IN PHW_DEVICE_EXTENSION DeviceExtension
    )
{
    PULONG Start = ((ULONG)DeviceExtension->DataBuffer & 3) == 0 ?
                   (PULONG)DeviceExtension->DataBuffer : DeviceExtension->Bounce;
    PULONG Buffer = Start;
    ULONG Words = SECTOR_ULONGS;
    ULONG Time = 0;
    ULONG Level;
    ULONG Count;
    ULONG Burst;

    if (!DeviceExtension->Read && Start == DeviceExtension->Bounce) {
        ScsiPortMoveMemory(Start, DeviceExtension->DataBuffer, SECTOR_SIZE);
    }

    while (Words != 0) {
        Level = SDEDM_FIFO_LEVEL(HostRead(DeviceExtension, SDEDM));
        Count = DeviceExtension->Read ? Level : SDHOST_FIFO_WORDS - Level;
        Burst = Words < SDHOST_PIO_BURST ? Words : SDHOST_PIO_BURST;
        if (Count < Burst) {
            if ((HostRead(DeviceExtension, SDHSTS) & SDHSTS_ERRORS) != 0 || Time > BLOCK_TIMEOUT_US) {
                return FALSE;
            }

            ScsiPortStallExecution(1);
            Time += 1;
            continue;
        }

        if (Count > Words) {
            Count = Words;
        }

        Words -= Count;
        while (Count-- != 0) {
            if (DeviceExtension->Read) {
                *Buffer++ = HostRead(DeviceExtension, SDDATA);
            } else {
                HostWrite(DeviceExtension, SDDATA, *Buffer++);
            }
        }
    }

    if (DeviceExtension->Read && Start == DeviceExtension->Bounce) {
        ScsiPortMoveMemory(DeviceExtension->DataBuffer, Start, SECTOR_SIZE);
    }

    DeviceExtension->DataBuffer += SECTOR_SIZE;
    return TRUE;
}

//
// The controllers behind one interface.
//

static BOOLEAN
HostReset(
    IN PHW_DEVICE_EXTENSION DeviceExtension
    )
{
    return DeviceExtension->Sdhost ? SdhostReset(DeviceExtension) : EmmcReset(DeviceExtension);
}

static BOOLEAN
HostSetClock(
    IN PHW_DEVICE_EXTENSION DeviceExtension,
    IN ULONG Hz
    )
{
    return DeviceExtension->Sdhost ? SdhostSetClock(DeviceExtension, Hz) :
                                     EmmcSetClock(DeviceExtension, Hz);
}

//
// Issue a command and wait for its response, and for the end of an R1b
// command's busy. An application command is prefixed with CMD55.
//

static BOOLEAN
HostCommand(
    IN PHW_DEVICE_EXTENSION DeviceExtension,
    IN ULONG Command,
    IN ULONG Argument,
    IN ULONG Blocks
    )
{
    if ((Command & SD_APP) != 0 &&
        !HostCommand(DeviceExtension, CMD(55) | R1, DeviceExtension->Rca << 16, 0)) {
        return FALSE;
    }

    Command &= ~SD_APP;
    return DeviceExtension->Sdhost ? SdhostCommand(DeviceExtension, Command, Argument, Blocks, TRUE) :
                                     EmmcCommand(DeviceExtension, Command, Argument, Blocks);
}

//
// Word Word of the last response; a long response (CSD, CID) in words 3 to 0
// as the SDHCI holds it, bits 127:8 with the CRC byte dropped.
//

static ULONG
HostResponse(
    IN PHW_DEVICE_EXTENSION DeviceExtension,
    IN ULONG Word
    )
{
    ULONG Low;

    if (!DeviceExtension->Sdhost) {
        return HostRead(DeviceExtension, RESP0 + Word * 4);
    }

    Low = HostRead(DeviceExtension, SDRSP0 + Word * 4);
    if (DeviceExtension->ResponseType != SD_RESP_LONG) {
        return Low;
    }

    return (Low >> 8) | (Word < 3 ? HostRead(DeviceExtension, SDRSP0 + (Word + 1) * 4) << 24 : 0);
}

static VOID
HostSetBusWidth(
    IN PHW_DEVICE_EXTENSION DeviceExtension,
    IN ULONG Bits
    )
{
    if (DeviceExtension->Sdhost) {
        SdhostSetBusWidth(DeviceExtension, Bits);
    } else {
        EmmcSetBusWidth(DeviceExtension, Bits);
    }
}

//
// The card's capacity in sectors from its CSD, whose bits 127:8 the response
// words 3 to 0 hold.
//

static ULONG
HostCapacity(
    IN PHW_DEVICE_EXTENSION DeviceExtension
    )
{
    ULONG Resp1 = HostResponse(DeviceExtension, 1);
    ULONG Resp2 = HostResponse(DeviceExtension, 2);
    ULONG Size;

    if (((HostResponse(DeviceExtension, 3) >> 22) & 3) == 1) {
        return (((Resp1 >> 8) & 0x3FFFFF) + 1) * 1024;
    }

    Size = ((Resp2 & 0x3) << 10) | (Resp1 >> 22);
    return ((Size + 1) << (((Resp1 >> 7) & 0x7) + 2 + ((Resp2 >> 8) & 0xF))) / SECTOR_SIZE;
}

//
// Reset the controller and bring the card to the transfer state, as the SD
// physical layer specifies: CMD0, CMD8, ACMD41, CMD2, CMD3, CMD9, CMD7 and
// ACMD6 for the 4-bit bus. A standard capacity card is byte addressed, a high
// capacity card block addressed.
//

static BOOLEAN
HostInitializeCard(
    IN PHW_DEVICE_EXTENSION DeviceExtension
    )
{
    ULONG Ocr = 0;
    ULONG Index;
    BOOLEAN Version2;

    DeviceExtension->WriteDelay = 2 * 1000000 / IDENT_CLOCK + 1;
    if (!HostReset(DeviceExtension)) {
        return FALSE;
    }

    DeviceExtension->Rca = 0;
    HostCommand(DeviceExtension, CMD(0) | SD_RESP_NONE, 0, 0);
    Version2 = HostCommand(DeviceExtension, CMD(8) | R7, 0x1AA, 0) &&
               (HostResponse(DeviceExtension, 0) & 0xFFF) == 0x1AA;

    for (Index = 0; Index < 1000; Index += 1) {
        if (!HostCommand(DeviceExtension, APP | CMD(41) | R3, (Version2 ? 0x40000000 : 0) | 0x00FF8000, 0)) {
            return FALSE;
        }

        Ocr = HostResponse(DeviceExtension, 0);
        if ((Ocr & 0x80000000) != 0) {
            break;
        }

        ScsiPortStallExecution(1000);
    }

    if ((Ocr & 0x80000000) == 0) {
        return FALSE;
    }

    DeviceExtension->HighCapacity = (BOOLEAN)((Ocr & 0x40000000) != 0);
    if (!HostCommand(DeviceExtension, CMD(2) | R2, 0, 0) ||
        !HostCommand(DeviceExtension, CMD(3) | R6, 0, 0)) {
        return FALSE;
    }

    DeviceExtension->Rca = HostResponse(DeviceExtension, 0) >> 16;
    if (!HostCommand(DeviceExtension, CMD(9) | R2, DeviceExtension->Rca << 16, 0)) {
        return FALSE;
    }

    DeviceExtension->Sectors = HostCapacity(DeviceExtension);
    if (!HostCommand(DeviceExtension, CMD(7) | R1B, DeviceExtension->Rca << 16, 0) ||
        !HostSetClock(DeviceExtension, TRANSFER_CLOCK)) {
        return FALSE;
    }

    if (HostCommand(DeviceExtension, APP | CMD(6) | R1, 2, 0)) {
        HostSetBusWidth(DeviceExtension, 4);
    }

    if (!DeviceExtension->HighCapacity &&
        !HostCommand(DeviceExtension, CMD(16) | R1, SECTOR_SIZE, 0)) {
        return FALSE;
    }

    return TRUE;
}

//
// Complete the current request.
//

static VOID
HostComplete(
    IN PHW_DEVICE_EXTENSION DeviceExtension,
    IN PSCSI_REQUEST_BLOCK Srb,
    IN UCHAR Status
    )
{
    Srb->SrbStatus = Status;
    ScsiPortNotification(RequestComplete, DeviceExtension, Srb);
    ScsiPortNotification(NextRequest, DeviceExtension, NULL);
}

//
// Start a read or write: the blocks then move on the controller's interrupts.
// On the SDHOST every transfer is a multiple block command, ended with CMD12
// by the interrupt routine; its data interrupt is enabled before the command,
// as the FIFO fills (a read) or asks for data (a write) as soon as the command
// is accepted.
//

static UCHAR
HostReadWrite(
    IN PHW_DEVICE_EXTENSION DeviceExtension,
    IN PSCSI_REQUEST_BLOCK Srb
    )
{
    PCDB Cdb = (PCDB)Srb->Cdb;
    ULONG Lba;
    ULONG Count;
    ULONG Command;
    BOOLEAN Read = (BOOLEAN)(Srb->Cdb[0] == SCSIOP_READ);

    Lba = ((ULONG)Cdb->CDB10.LogicalBlockByte0 << 24) | ((ULONG)Cdb->CDB10.LogicalBlockByte1 << 16) |
          ((ULONG)Cdb->CDB10.LogicalBlockByte2 << 8) | Cdb->CDB10.LogicalBlockByte3;
    Count = ((ULONG)Cdb->CDB10.TransferBlocksMsb << 8) | Cdb->CDB10.TransferBlocksLsb;
    if (Count == 0) {
        return SRB_STATUS_SUCCESS;
    }

    if (Lba + Count > DeviceExtension->Sectors || Count * SECTOR_SIZE > Srb->DataTransferLength) {
        return SRB_STATUS_ERROR;
    }

    if (Count == 1 && !DeviceExtension->Sdhost) {
        Command = Read ? (CMD(17) | R1 | SD_DATA_READ) : (CMD(24) | R1 | SD_DATA_WRITE);
    } else {
        Command = Read ? (CMD(18) | R1 | SD_DATA_READ | SD_DATA_MULTIPLE) :
                         (CMD(25) | R1 | SD_DATA_WRITE | SD_DATA_MULTIPLE);
    }

    DeviceExtension->CurrentSrb = Srb;
    DeviceExtension->DataBuffer = Srb->DataBuffer;
    DeviceExtension->BlocksLeft = Count;
    DeviceExtension->Read = Read;
    DeviceExtension->Stopping = FALSE;
    if (DeviceExtension->Sdhost) {
        HostWrite(DeviceExtension, SDHSTS, SDHSTS_DATA_FLAG);
        DeviceExtension->Config |= SDHCFG_DATA_IRPT_EN;
        HostWrite(DeviceExtension, SDHCFG, DeviceExtension->Config);
    }

    if (!HostCommand(DeviceExtension, Command,
                     DeviceExtension->HighCapacity ? Lba : Lba * SECTOR_SIZE, Count)) {
        if (DeviceExtension->Sdhost) {
            SdhostRecover(DeviceExtension);
        }

        DeviceExtension->CurrentSrb = NULL;
        return SRB_STATUS_ERROR;
    }

    DeviceExtension->DataActive = DeviceExtension->Sdhost;
    return SRB_STATUS_PENDING;
}

//
// Emulate the SCSI commands of a direct access device.
//

static UCHAR
HostExecuteScsi(
    IN PHW_DEVICE_EXTENSION DeviceExtension,
    IN PSCSI_REQUEST_BLOCK Srb
    )
{
    INQUIRYDATA Inquiry;
    PREAD_CAPACITY_DATA Capacity;
    PMODE_PARAMETER_HEADER Header;
    ULONG Last;
    ULONG Index;

    switch (Srb->Cdb[0]) {

    case SCSIOP_INQUIRY:

        for (Index = 0; Index < sizeof(INQUIRYDATA); Index += 1) {
            ((PUCHAR)&Inquiry)[Index] = 0;
        }

        Inquiry.DeviceType = DIRECT_ACCESS_DEVICE;
        Inquiry.Versions = 2;
        Inquiry.ResponseDataFormat = 2;
        Inquiry.AdditionalLength = sizeof(INQUIRYDATA) - 5;
        ScsiPortMoveMemory(Inquiry.VendorId, "BCM2835 ", 8);
        ScsiPortMoveMemory(Inquiry.ProductId, "SD Card         ", 16);
        ScsiPortMoveMemory(Inquiry.ProductRevisionLevel, "1.0 ", 4);
        if (Srb->DataTransferLength > sizeof(INQUIRYDATA)) {
            Srb->DataTransferLength = sizeof(INQUIRYDATA);
        }

        ScsiPortMoveMemory(Srb->DataBuffer, &Inquiry, Srb->DataTransferLength);
        return SRB_STATUS_SUCCESS;

    case SCSIOP_READ_CAPACITY:

        if (Srb->DataTransferLength < sizeof(READ_CAPACITY_DATA)) {
            return SRB_STATUS_ERROR;
        }

        Capacity = Srb->DataBuffer;
        Last = DeviceExtension->Sectors - 1;
        Capacity->LogicalBlockAddress = (Last >> 24) | ((Last >> 8) & 0xFF00) |
                                        ((Last << 8) & 0xFF0000) | (Last << 24);
        Capacity->BytesPerBlock = (SECTOR_SIZE >> 8) << 16;
        return SRB_STATUS_SUCCESS;

    case SCSIOP_MODE_SENSE:

        if (Srb->DataTransferLength < sizeof(MODE_PARAMETER_HEADER)) {
            return SRB_STATUS_ERROR;
        }

        Header = Srb->DataBuffer;
        Header->ModeDataLength = sizeof(MODE_PARAMETER_HEADER) - 1;
        Header->MediumType = 0;
        Header->DeviceSpecificParameter = 0;
        Header->BlockDescriptorLength = 0;
        Srb->DataTransferLength = sizeof(MODE_PARAMETER_HEADER);
        return SRB_STATUS_SUCCESS;

    case SCSIOP_READ:
    case SCSIOP_WRITE:

        return HostReadWrite(DeviceExtension, Srb);

    case SCSIOP_TEST_UNIT_READY:
    case SCSIOP_VERIFY:
    case SCSIOP_START_STOP_UNIT:
    case SCSIOP_MEDIUM_REMOVAL:
    case SCSIOP_REZERO_UNIT:
    case SCSIOP_SEEK:
    case SCSIOP_RESERVE_UNIT:
    case SCSIOP_RELEASE_UNIT:

        return SRB_STATUS_SUCCESS;

    default:

        return SRB_STATUS_INVALID_REQUEST;
    }
}

BOOLEAN
BcmEmmcStartIo(
    IN PVOID HwDeviceExtension,
    IN PSCSI_REQUEST_BLOCK Srb
    )

/*++

Routine Description:

    This routine is called from the SCSI port driver synchronized with the
    kernel to start a request.

Arguments:

    HwDeviceExtension - Supplies the device extension.

    Srb - Supplies the request.

Return Value:

    TRUE

--*/

{
    PHW_DEVICE_EXTENSION DeviceExtension = HwDeviceExtension;
    UCHAR Status;

    switch (Srb->Function) {

    case SRB_FUNCTION_EXECUTE_SCSI:

        if (Srb->PathId != 0 || Srb->TargetId != 0 || Srb->Lun != 0 ||
            !DeviceExtension->CardPresent) {
            Status = SRB_STATUS_SELECTION_TIMEOUT;
            break;
        }

        Status = HostExecuteScsi(DeviceExtension, Srb);
        break;

    case SRB_FUNCTION_ABORT_COMMAND:
    case SRB_FUNCTION_RESET_BUS:

        BcmEmmcResetBus(DeviceExtension, Srb->PathId);
        Status = SRB_STATUS_SUCCESS;
        break;

    default:

        Status = SRB_STATUS_INVALID_REQUEST;
        break;
    }

    if (Status != SRB_STATUS_PENDING) {
        HostComplete(DeviceExtension, Srb, Status);
    }

    return TRUE;
}

//
// The interrupt service routine of the SDHCI: move a block through the data
// port on a buffer ready interrupt, and complete the request when the transfer
// is done or has failed. Every status bit taken is acknowledged first: the
// interrupt is level sensitive, and the controller can signal buffer ready
// when no block is left to move; with no request, the data line is reset.
//

static BOOLEAN
EmmcInterrupt(
    IN PHW_DEVICE_EXTENSION DeviceExtension
    )
{
    PSCSI_REQUEST_BLOCK Srb = DeviceExtension->CurrentSrb;
    PULONG Data = DeviceExtension->Registers + DATA / sizeof(ULONG);
    PULONG Buffer;
    ULONG Status;
    ULONG Index;

    Status = HostRead(DeviceExtension, INTERRUPT) &
             (INT_DATA_DONE | INT_WRITE_RDY | INT_READ_RDY | INT_ERR | INT_ERRORS);
    if (Status == 0) {
        return FALSE;
    }

    EmmcWrite(DeviceExtension, INTERRUPT, Status);
    if (Srb == NULL) {
        if ((Status & (INT_READ_RDY | INT_WRITE_RDY)) != 0) {
            EmmcResetLines(DeviceExtension, C1_SRST_DATA);
        }

        return TRUE;
    }

    if ((Status & (INT_ERR | INT_ERRORS)) != 0) {
        EmmcWrite(DeviceExtension, INTERRUPT, 0xFFFFFFFF);
        EmmcResetLines(DeviceExtension, C1_SRST_CMD | C1_SRST_DATA);
        DeviceExtension->CurrentSrb = NULL;
        HostComplete(DeviceExtension, Srb, SRB_STATUS_ERROR);
        return TRUE;
    }

    if ((Status & (INT_READ_RDY | INT_WRITE_RDY)) != 0 && DeviceExtension->BlocksLeft != 0) {
        Buffer = ((ULONG)DeviceExtension->DataBuffer & 3) == 0 ?
                 (PULONG)DeviceExtension->DataBuffer : DeviceExtension->Bounce;
        if (DeviceExtension->Read) {
            for (Index = 0; Index < SECTOR_ULONGS; Index += 1) {
                Buffer[Index] = ScsiPortReadRegisterUlong(Data);
            }

            if (Buffer == DeviceExtension->Bounce) {
                ScsiPortMoveMemory(DeviceExtension->DataBuffer, Buffer, SECTOR_SIZE);
            }

        } else {
            if (Buffer == DeviceExtension->Bounce) {
                ScsiPortMoveMemory(Buffer, DeviceExtension->DataBuffer, SECTOR_SIZE);
            }

            for (Index = 0; Index < SECTOR_ULONGS; Index += 1) {
                ScsiPortWriteRegisterUlong(Data, Buffer[Index]);
            }
        }

        DeviceExtension->DataBuffer += SECTOR_SIZE;
        DeviceExtension->BlocksLeft -= 1;
    }

    if ((Status & INT_DATA_DONE) != 0) {
        DeviceExtension->CurrentSrb = NULL;
        HostComplete(DeviceExtension, Srb,
                     DeviceExtension->BlocksLeft == 0 ? SRB_STATUS_SUCCESS : SRB_STATUS_ERROR);
    }

    return TRUE;
}

//
// End a multiple block transfer cut short: the card is left sending or
// receiving data until a CMD12, which every later data command would fail
// without.
//

static VOID
SdhostStopTransfer(
    IN PHW_DEVICE_EXTENSION DeviceExtension
    )
{
    BOOLEAN Active = DeviceExtension->DataActive;

    SdhostRecover(DeviceExtension);
    if (Active) {
        SdhostCommand(DeviceExtension, CMD(12) | R1B, 0, 0, TRUE);
    }
}

//
// Fail the current request on the SDHOST.
//

static VOID
SdhostAbort(
    IN PHW_DEVICE_EXTENSION DeviceExtension,
    IN PSCSI_REQUEST_BLOCK Srb
    )
{
    SdhostStopTransfer(DeviceExtension);
    DeviceExtension->CurrentSrb = NULL;
    HostComplete(DeviceExtension, Srb, SRB_STATUS_ERROR);
}

//
// The interrupt service routine of the SDHOST. The data flag is the state of
// the FIFO, not an interrupt of its own: it counts while the data interrupt is
// enabled. A block of a read moves on each data interrupt; the first block of
// a write on the data interrupt, the next ones on the block interrupts that
// end each block. When every block has moved, CMD12 ends the transfer, and
// its busy interrupt, when the card is done, completes the request.
//

static BOOLEAN
SdhostInterrupt(
    IN PHW_DEVICE_EXTENSION DeviceExtension
    )
{
    PSCSI_REQUEST_BLOCK Srb = DeviceExtension->CurrentSrb;
    ULONG Status = HostRead(DeviceExtension, SDHSTS);
    ULONG Pending;

    Pending = Status & (SDHSTS_INTERRUPTS | SDHSTS_ERRORS);
    if ((Status & SDHSTS_DATA_FLAG) != 0 && (DeviceExtension->Config & SDHCFG_DATA_IRPT_EN) != 0) {
        Pending |= SDHSTS_DATA_FLAG;
    }

    if (Pending == 0) {
        return FALSE;
    }

    HostWrite(DeviceExtension, SDHSTS, Pending | SDHSTS_DATA_FLAG);
    if (Srb == NULL) {
        return TRUE;
    }

    if ((Status & SDHSTS_ERRORS) != 0) {
        SdhostAbort(DeviceExtension, Srb);
        return TRUE;
    }

    if (DeviceExtension->Stopping) {
        if ((Status & SDHSTS_BUSY_IRPT) != 0) {
            DeviceExtension->Stopping = FALSE;
            DeviceExtension->CurrentSrb = NULL;
            HostComplete(DeviceExtension, Srb,
                         (HostRead(DeviceExtension, SDCMD) & SDCMD_FAIL) != 0 ?
                         SRB_STATUS_ERROR : SRB_STATUS_SUCCESS);
        }

        return TRUE;
    }

    if (DeviceExtension->Read) {
        if ((Pending & SDHSTS_DATA_FLAG) != 0 && DeviceExtension->BlocksLeft != 0) {
            if (!SdhostMoveBlock(DeviceExtension)) {
                SdhostAbort(DeviceExtension, Srb);
                return TRUE;
            }

            DeviceExtension->BlocksLeft -= 1;
        }

    } else if ((Pending & SDHSTS_DATA_FLAG) != 0) {

        //
        // The FIFO has room for the first block; from here the block
        // interrupts end each block, and the next one follows.
        //

        DeviceExtension->Config = (DeviceExtension->Config & ~SDHCFG_DATA_IRPT_EN) | SDHCFG_BLOCK_IRPT_EN;
        HostWrite(DeviceExtension, SDHCFG, DeviceExtension->Config);
        if (!SdhostMoveBlock(DeviceExtension)) {
            SdhostAbort(DeviceExtension, Srb);
            return TRUE;
        }

    } else if ((Pending & SDHSTS_BLOCK_IRPT) != 0 && DeviceExtension->BlocksLeft != 0) {
        DeviceExtension->BlocksLeft -= 1;
        if (DeviceExtension->BlocksLeft != 0 && !SdhostMoveBlock(DeviceExtension)) {
            SdhostAbort(DeviceExtension, Srb);
            return TRUE;
        }
    }

    if (DeviceExtension->BlocksLeft == 0) {

        //
        // An error on the last burst of a read: the CMD12 below would clear
        // it unseen.
        //

        if ((HostRead(DeviceExtension, SDHSTS) & SDHSTS_ERRORS) != 0) {
            SdhostAbort(DeviceExtension, Srb);
            return TRUE;
        }

        DeviceExtension->Config &= ~(SDHCFG_DATA_IRPT_EN | SDHCFG_BLOCK_IRPT_EN);
        HostWrite(DeviceExtension, SDHCFG, DeviceExtension->Config);
        DeviceExtension->Stopping = TRUE;
        DeviceExtension->DataActive = FALSE;
        if (!SdhostCommand(DeviceExtension, CMD(12) | R1B, 0, 0, FALSE)) {
            SdhostAbort(DeviceExtension, Srb);
        }
    }

    return TRUE;
}

BOOLEAN
BcmEmmcInterrupt(
    IN PVOID HwDeviceExtension
    )

/*++

Routine Description:

    This is the interrupt service routine: that of the controller.

Arguments:

    HwDeviceExtension - Supplies the device extension.

Return Value:

    TRUE if the controller was interrupting.

--*/

{
    PHW_DEVICE_EXTENSION DeviceExtension = HwDeviceExtension;

    return DeviceExtension->Sdhost ? SdhostInterrupt(DeviceExtension) : EmmcInterrupt(DeviceExtension);
}

BOOLEAN
BcmEmmcResetBus(
    IN PVOID HwDeviceExtension,
    IN ULONG PathId
    )

/*++

Routine Description:

    This routine resets the command and data lines of the controller and
    completes the outstanding request.

Arguments:

    HwDeviceExtension - Supplies the device extension.

    PathId - Supplies the bus to reset.

Return Value:

    TRUE

--*/

{
    PHW_DEVICE_EXTENSION DeviceExtension = HwDeviceExtension;

    if (DeviceExtension->Sdhost) {
        SdhostStopTransfer(DeviceExtension);
    } else {
        EmmcResetLines(DeviceExtension, C1_SRST_CMD | C1_SRST_DATA);
        EmmcWrite(DeviceExtension, INTERRUPT, 0xFFFFFFFF);
    }

    DeviceExtension->CurrentSrb = NULL;
    ScsiPortCompleteRequest(DeviceExtension, (UCHAR)PathId, (UCHAR)-1, (UCHAR)-1,
                            SRB_STATUS_BUS_RESET);
    ScsiPortNotification(NextRequest, DeviceExtension, NULL);
    return TRUE;
}

BOOLEAN
BcmEmmcInitialize(
    IN PVOID HwDeviceExtension
    )

/*++

Routine Description:

    This routine enables the interrupts of the controller: on the SDHCI the
    end of a transfer, buffer ready and the errors; on the SDHOST the busy
    interrupt, the data and block interrupts being enabled by each transfer.

Arguments:

    HwDeviceExtension - Supplies the device extension.

Return Value:

    TRUE

--*/

{
    PHW_DEVICE_EXTENSION DeviceExtension = HwDeviceExtension;

    if (DeviceExtension->Sdhost) {
        HostWrite(DeviceExtension, SDHSTS, SDHSTS_CLEAR | SDHSTS_DATA_FLAG);
        DeviceExtension->Config &= ~(SDHCFG_DATA_IRPT_EN | SDHCFG_BLOCK_IRPT_EN);
        HostWrite(DeviceExtension, SDHCFG, DeviceExtension->Config);
    } else {
        EmmcWrite(DeviceExtension, INTERRUPT, 0xFFFFFFFF);
        EmmcWrite(DeviceExtension, IRPT_EN, INT_DATA_DONE | INT_WRITE_RDY | INT_READ_RDY | INT_ERRORS);
    }

    return TRUE;
}

ULONG
BcmEmmcFindAdapter(
    IN PVOID HwDeviceExtension,
    IN PVOID Context,
    IN PVOID BusInformation,
    IN PCHAR ArgumentString,
    IN OUT PPORT_CONFIGURATION_INFORMATION ConfigInfo,
    OUT PBOOLEAN Again
    )

/*++

Routine Description:

    This routine maps the controller and the mailbox the firmware described,
    tells the controller from its registers' offset into the peripherals, asks
    the VideoCore for its base clock and brings the card to the transfer state.

Arguments:

    HwDeviceExtension - Supplies the device extension.

    Context - Not used.

    BusInformation - Not used.

    ArgumentString - Not used.

    ConfigInfo - Supplies the configuration information.

    Again - Returns FALSE: there is one controller.

Return Value:

    SP_RETURN_FOUND, SP_RETURN_NOT_FOUND or SP_RETURN_ERROR.

--*/

{
    PHW_DEVICE_EXTENSION DeviceExtension = HwDeviceExtension;
    PACCESS_RANGE Range = &(*ConfigInfo->AccessRanges)[0];
    ULONG Offset;
    ULONG Length;

    *Again = FALSE;
    if (Range[0].RangeLength == 0 || Range[1].RangeLength == 0) {
        return SP_RETURN_NOT_FOUND;
    }

    DeviceExtension->Registers = ScsiPortGetDeviceBase(DeviceExtension,
                                                       ConfigInfo->AdapterInterfaceType,
                                                       ConfigInfo->SystemIoBusNumber,
                                                       Range[0].RangeStart,
                                                       Range[0].RangeLength,
                                                       (BOOLEAN)!Range[0].RangeInMemory);
    DeviceExtension->Mailbox = ScsiPortGetDeviceBase(DeviceExtension,
                                                     ConfigInfo->AdapterInterfaceType,
                                                     ConfigInfo->SystemIoBusNumber,
                                                     Range[1].RangeStart,
                                                     Range[1].RangeLength,
                                                     (BOOLEAN)!Range[1].RangeInMemory);
    if (DeviceExtension->Registers == NULL || DeviceExtension->Mailbox == NULL) {
        return SP_RETURN_ERROR;
    }

    //
    // The VideoCore masters the property buffer.
    //

    ConfigInfo->Master = TRUE;
    ConfigInfo->ScatterGather = TRUE;
    ConfigInfo->NumberOfBuses = 1;
    ConfigInfo->MaximumTransferLength = MAXIMUM_TRANSFER;
    ConfigInfo->NumberOfPhysicalBreaks = TRANSFER_PAGES;
    ConfigInfo->InterruptMode = LevelSensitive;
    if (ConfigInfo->InitiatorBusId[0] == (CCHAR)SP_UNINITIALIZED_VALUE) {
        ConfigInfo->InitiatorBusId[0] = 7;
    }

    //
    // The mailbox takes the buffer's address with the channel in its low four
    // bits: the buffer is aligned to 16 bytes (scsiport's uncached extension
    // is, diskdump's need not be).
    //

    DeviceExtension->Property = ScsiPortGetUncachedExtension(DeviceExtension, ConfigInfo,
                                                             PROPERTY_LENGTH * sizeof(ULONG) + 16);
    if (DeviceExtension->Property == NULL) {
        return SP_RETURN_ERROR;
    }

    DeviceExtension->Property = (PULONG)(((ULONG)DeviceExtension->Property + 15) & ~15);

    DeviceExtension->PropertyAddress = ScsiPortConvertPhysicalAddressToUlong(
        ScsiPortGetPhysicalAddress(DeviceExtension, NULL, DeviceExtension->Property, &Length));

    //
    // The VideoCore reads it through its bus, which sees memory at 0xC0000000
    // (uncached); scsiport gives that address, but the crash dump driver
    // (diskdump), which runs a copy of this driver after a crash, gives the
    // physical one.
    //

    if (DeviceExtension->PropertyAddress < 0x40000000) {
        DeviceExtension->PropertyAddress |= 0xC0000000;
    }

    Offset = ScsiPortConvertPhysicalAddressToUlong(Range[0].RangeStart) & 0xFFFFFF;
    DeviceExtension->Sdhost = (BOOLEAN)(Offset == SDHOST_OFFSET);
    DeviceExtension->ClockId = DeviceExtension->Sdhost ? CLOCK_CORE :
                               Offset == EMMC2_OFFSET ? CLOCK_EMMC2 : CLOCK_EMMC;

    //
    // The SDHOST runs from the core clock, which the VideoCore may raise with
    // load: its maximum rate is taken, so that the card's clock stays in range.
    //

    if (DeviceExtension->Sdhost) {
        DeviceExtension->BaseClock = HostGetClock(DeviceExtension, TAG_GET_MAX_CLOCK_RATE);
        if (DeviceExtension->BaseClock == 0) {
            DeviceExtension->BaseClock = HostGetClock(DeviceExtension, TAG_GET_CLOCK_RATE);
        }

    } else {
        DeviceExtension->BaseClock = HostGetClock(DeviceExtension, TAG_GET_CLOCK_RATE);
        if (DeviceExtension->BaseClock == 0) {
            DeviceExtension->BaseClock = HostGetClock(DeviceExtension, TAG_GET_MAX_CLOCK_RATE);
        }
    }

    if (DeviceExtension->BaseClock == 0) {
        return SP_RETURN_ERROR;
    }

    DeviceExtension->CardPresent = HostInitializeCard(DeviceExtension);
    return SP_RETURN_FOUND;
}

VOID
BcmEmmcSetCrashDump(
    VOID
    );

ULONG
DriverEntry(
    IN PVOID DriverObject,
    IN PVOID Argument2
    )

/*++

Routine Description:

    Installable driver initialization entry point for system.

Arguments:

    DriverObject - Supplies the driver object.

    Argument2 - Supplies the registry path.

Return Value:

    Status from ScsiPortInitialize()

--*/

{
    HW_INITIALIZATION_DATA HwInitializationData;
    ULONG Index;

    //
    // The system's copy (not the crash dump driver's, which passes no driver
    // object): the crash dump setting of this boot (crashdmp.c).
    //

    if (DriverObject != NULL) {
        BcmEmmcSetCrashDump();
    }

    for (Index = 0; Index < sizeof(HW_INITIALIZATION_DATA); Index += 1) {
        ((PUCHAR)&HwInitializationData)[Index] = 0;
    }

    HwInitializationData.HwInitializationDataSize = sizeof(HW_INITIALIZATION_DATA);
    HwInitializationData.AdapterInterfaceType = Internal;
    HwInitializationData.HwInitialize = BcmEmmcInitialize;
    HwInitializationData.HwStartIo = BcmEmmcStartIo;
    HwInitializationData.HwInterrupt = BcmEmmcInterrupt;
    HwInitializationData.HwFindAdapter = BcmEmmcFindAdapter;
    HwInitializationData.HwResetBus = BcmEmmcResetBus;
    HwInitializationData.DeviceExtensionSize = sizeof(HW_DEVICE_EXTENSION);
    HwInitializationData.NumberOfAccessRanges = 2;
    HwInitializationData.MapBuffers = TRUE;

    return ScsiPortInitialize(DriverObject, Argument2, &HwInitializationData, NULL);
}
