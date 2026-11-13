/*++

Module Name:

    sdio.c

Abstract:

    The SDIO host of the wireless miniport: the EMMC of the BCM2835 (an Arasan
    SD host controller, SDHCI 3.0), wired to the wireless chip on GPIO 34-39.
    It brings the chip's SDIO interface up (CMD0, CMD5, CMD3, CMD7, the 4-bit
    bus, the transfer clock) and moves data with CMD52 (one byte of a
    function's register space) and CMD53 (bytes or blocks, by programmed I/O
    through the data port).

    Only the thread of the miniport (and its initialization) uses the
    controller. The one register the interrupt service routine writes is the
    signal enable of the card interrupt (IRPT_EN). The controller loses a
    register write that follows another within two SD clocks: so each write
    is followed by a delay, and once the interrupt is connected the writes of
    the thread are synchronized with the interrupt service routine, which may
    run on another processor.

Environment:

    kernel mode only

--*/

#include "bcmwlan.h"

//
// SDHCI registers (the BCM2835 names) and their fields.
//

#define BLKSIZECNT          0x04
#define ARG1                0x08
#define CMDTM               0x0C
#define RESP0               0x10
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
#define C0_HCTL_HS_EN       0x00000004
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
#define INT_CARD            0x00000100
#define INT_ERR             0x00008000

#define TM_BLKCNT_EN        0x00000002
#define TM_DAT_DIR_READ     0x00000010
#define TM_MULTI_BLOCK      0x00000020

#define RSP_NONE            0x00000000
#define RSP_136             0x00010000
#define RSP_48              0x00020000
#define RSP_48_BUSY         0x00030000
#define CMD_CRCCHK          0x00080000
#define CMD_IXCHK           0x00100000
#define CMD_ISDATA          0x00200000

#define CMD(n)              ((ULONG)(n) << 24)
#define R1                  (RSP_48 | CMD_CRCCHK | CMD_IXCHK)
#define R1B                 (RSP_48_BUSY | CMD_CRCCHK | CMD_IXCHK)
#define R4                  RSP_48
#define R5                  (RSP_48 | CMD_CRCCHK | CMD_IXCHK)
#define R6                  R1

//
// The R5 response flags of CMD52 and CMD53 that are errors: CRC, illegal
// command, general error, invalid function, out of range.
//

#define R5_ERRORS           0xCB00

#define IDENT_CLOCK         400000
#define TIMEOUT_US          1000000

static ULONG
HostRead(
    IN PWLAN_ADAPTER Adapter,
    IN ULONG Register
    )
{
    return READ_REGISTER_ULONG((PULONG)(Adapter->Registers + Register));
}

//
// A register write and the delay after it, which the interrupt service
// routine must not come between (NdisMSynchronizeWithInterrupt).
//

typedef struct _HOST_WRITE {
    PWLAN_ADAPTER Adapter;
    ULONG Register;
    ULONG Value;
} HOST_WRITE, *PHOST_WRITE;

static BOOLEAN
HostWriteSynchronized(
    IN PVOID Context
    )
{
    PHOST_WRITE Write = Context;

    WRITE_REGISTER_ULONG((PULONG)(Write->Adapter->Registers + Write->Register), Write->Value);
    KeStallExecutionProcessor(Write->Adapter->WriteDelay);
    return TRUE;
}

static VOID
HostWrite(
    IN PWLAN_ADAPTER Adapter,
    IN ULONG Register,
    IN ULONG Value
    )
{
    HOST_WRITE Write;

    Write.Adapter = Adapter;
    Write.Register = Register;
    Write.Value = Value;
    if (Adapter->InterruptConnected) {
        NdisMSynchronizeWithInterrupt(&Adapter->Interrupt, HostWriteSynchronized, &Write);
    } else {
        HostWriteSynchronized(&Write);
    }
}

static BOOLEAN
HostWait(
    IN PWLAN_ADAPTER Adapter,
    IN ULONG Register,
    IN ULONG Mask,
    IN ULONG Value
    )
{
    ULONG Time;

    for (Time = 0; Time < TIMEOUT_US; Time += 1) {
        if ((HostRead(Adapter, Register) & Mask) == Value) {
            return TRUE;
        }

        KeStallExecutionProcessor(1);
    }

    return FALSE;
}

//
// Wait for any of the bits of Mask; their value, or 0 after the timeout.
//

static ULONG
HostWaitAny(
    IN PWLAN_ADAPTER Adapter,
    IN ULONG Register,
    IN ULONG Mask
    )
{
    ULONG Time;
    ULONG Value;

    for (Time = 0; Time < TIMEOUT_US; Time += 1) {
        Value = HostRead(Adapter, Register) & Mask;
        if (Value != 0) {
            return Value;
        }

        KeStallExecutionProcessor(1);
    }

    return 0;
}

//
// Program the SD clock: the 10-bit divided clock is base / (2 * divisor).
//

static BOOLEAN
HostSetClock(
    IN PWLAN_ADAPTER Adapter,
    IN ULONG Hz
    )
{
    ULONG Divisor = (Adapter->BaseClock + 2 * Hz - 1) / (2 * Hz);
    ULONG Control;

    if (Divisor > 0x3FF) {
        Divisor = 0x3FF;
    }

    if (Divisor == 0) {
        Divisor = 1;
    }

    Control = HostRead(Adapter, CONTROL1) & ~(C1_CLK_EN | 0xFFE0);
    HostWrite(Adapter, CONTROL1, Control);
    Control |= ((Divisor & 0xFF) << 8) | ((Divisor >> 8) << 6) | C1_CLK_INTLEN | C1_TOUNIT_MAX;
    HostWrite(Adapter, CONTROL1, Control);
    if (!HostWait(Adapter, CONTROL1, C1_CLK_STABLE, C1_CLK_STABLE)) {
        return FALSE;
    }

    HostWrite(Adapter, CONTROL1, Control | C1_CLK_EN);
    Adapter->SdClock = Adapter->BaseClock / (2 * Divisor);
    Adapter->WriteDelay = 2 * 1000000 / Adapter->SdClock + 1;
    return TRUE;
}

static VOID
HostResetLines(
    IN PWLAN_ADAPTER Adapter
    )
{
    HostWrite(Adapter, CONTROL1, HostRead(Adapter, CONTROL1) | C1_SRST_CMD | C1_SRST_DATA);
    HostWait(Adapter, CONTROL1, C1_SRST_CMD | C1_SRST_DATA, 0);
}

//
// Issue a command and wait for its response; a data command's blocks move
// afterwards (HostData).
//

static BOOLEAN
HostCommand(
    IN PWLAN_ADAPTER Adapter,
    IN ULONG Command,
    IN ULONG Argument,
    IN ULONG BlockSize,
    IN ULONG Blocks,
    OUT PULONG Response
    )
{
    ULONG Busy = STATUS_CMD_INHIBIT;
    ULONG Status;

    if ((Command & (CMD_ISDATA | RSP_48_BUSY)) != 0) {
        Busy |= STATUS_DAT_INHIBIT;
    }

    if (!HostWait(Adapter, STATUS, Busy, 0)) {
        Adapter->SdioErrors += 1;
        HostResetLines(Adapter);
        return FALSE;
    }

    HostWrite(Adapter, INTERRUPT, 0xFFFFFFFF & ~INT_CARD);
    if ((Command & CMD_ISDATA) != 0) {
        HostWrite(Adapter, BLKSIZECNT, (Blocks << 16) | BlockSize);
    }

    HostWrite(Adapter, ARG1, Argument);
    HostWrite(Adapter, CMDTM, Command);
    Status = HostWaitAny(Adapter, INTERRUPT, INT_CMD_DONE | INT_ERR);
    if (Status == 0 || (Status & INT_ERR) != 0) {
        Adapter->SdioErrors += 1;
        Adapter->LastSdioStatus = HostRead(Adapter, INTERRUPT);
        HostWrite(Adapter, INTERRUPT, 0xFFFFFFFF & ~INT_CARD);
        HostResetLines(Adapter);
        return FALSE;
    }

    HostWrite(Adapter, INTERRUPT, INT_CMD_DONE);
    if (Response != NULL) {
        *Response = HostRead(Adapter, RESP0);
    }

    if ((Command & RSP_48_BUSY) == RSP_48_BUSY && (Command & CMD_ISDATA) == 0) {
        HostWait(Adapter, INTERRUPT, INT_DATA_DONE, INT_DATA_DONE);
        HostWrite(Adapter, INTERRUPT, INT_DATA_DONE);
    }

    return TRUE;
}

//
// Move the blocks of a data command through the data port, a block on each
// buffer ready, then wait for the end of the transfer. The buffer is longword
// aligned; the last word of a block that is not a multiple of four bytes
// moves whole.
//

static BOOLEAN
HostData(
    IN PWLAN_ADAPTER Adapter,
    IN PULONG Buffer,
    IN ULONG BlockSize,
    IN ULONG Blocks,
    IN BOOLEAN Write
    )
{
    ULONG Ready = Write ? INT_WRITE_RDY : INT_READ_RDY;
    PULONG Data = (PULONG)(Adapter->Registers + DATA);
    ULONG Words = (BlockSize + 3) / 4;
    ULONG Index;
    ULONG Status;

    while (Blocks-- != 0) {
        Status = HostWaitAny(Adapter, INTERRUPT, Ready | INT_ERR);
        if (Status == 0 || (Status & INT_ERR) != 0) {
            goto Failed;
        }

        HostWrite(Adapter, INTERRUPT, Ready);
        for (Index = 0; Index < Words; Index += 1) {
            if (Write) {
                WRITE_REGISTER_ULONG(Data, *Buffer++);
            } else {
                *Buffer++ = READ_REGISTER_ULONG(Data);
            }
        }
    }

    Status = HostWaitAny(Adapter, INTERRUPT, INT_DATA_DONE | INT_ERR);
    if (Status == 0 || (Status & INT_ERR) != 0) {
        goto Failed;
    }

    HostWrite(Adapter, INTERRUPT, INT_DATA_DONE);
    return TRUE;

Failed:
    Adapter->SdioErrors += 1;
    Adapter->LastSdioStatus = HostRead(Adapter, INTERRUPT);
    HostWrite(Adapter, INTERRUPT, 0xFFFFFFFF & ~INT_CARD);
    HostResetLines(Adapter);
    return FALSE;
}

BOOLEAN
SdioCmd52(
    IN PWLAN_ADAPTER Adapter,
    IN ULONG Function,
    IN ULONG Address,
    IN BOOLEAN Write,
    IN UCHAR Data,
    OUT PUCHAR Value OPTIONAL
    )

/*++

Routine Description:

    This routine reads or writes one byte of a function's register space
    (CMD52). A write returns the byte read back after it when Value is given.

Arguments:

    Adapter - Supplies the adapter.

    Function - Supplies the number of the function.

    Address - Supplies the address of the register.

    Write - Supplies whether the byte is written.

    Data - Supplies the byte to write.

    Value - Receives the byte read, if given.

Return Value:

    TRUE if the card took the command.

--*/

{
    ULONG Argument = (Write ? 0x80000000 : 0) | (Function << 28) | ((Address & 0x1FFFF) << 9) | Data;
    ULONG Response;

    if (!HostCommand(Adapter, CMD(52) | R5, Argument, 0, 0, &Response)) {
        Adapter->LastSdioAddress = (Function << 28) | Address;
        return FALSE;
    }

    if ((Response & R5_ERRORS) != 0) {
        Adapter->SdioErrors += 1;
        Adapter->LastSdioAddress = (Function << 28) | Address;
        Adapter->LastSdioStatus = Response;
        return FALSE;
    }

    if (Value != NULL) {
        *Value = (UCHAR)Response;
    }

    return TRUE;
}

BOOLEAN
SdioRead8(
    IN PWLAN_ADAPTER Adapter,
    IN ULONG Function,
    IN ULONG Address,
    OUT PUCHAR Value
    )

/*++

Routine Description:

    This routine reads one byte of a function's register space.

Arguments:

    Adapter - Supplies the adapter.

    Function - Supplies the number of the function.

    Address - Supplies the address of the register.

    Value - Receives the byte.

Return Value:

    TRUE if the byte was read.

--*/

{
    return SdioCmd52(Adapter, Function, Address, FALSE, 0, Value);
}

BOOLEAN
SdioWrite8(
    IN PWLAN_ADAPTER Adapter,
    IN ULONG Function,
    IN ULONG Address,
    IN UCHAR Value
    )

/*++

Routine Description:

    This routine writes one byte of a function's register space.

Arguments:

    Adapter - Supplies the adapter.

    Function - Supplies the number of the function.

    Address - Supplies the address of the register.

    Value - Supplies the byte.

Return Value:

    TRUE if the byte was written.

--*/

{
    return SdioCmd52(Adapter, Function, Address, TRUE, Value, NULL);
}

BOOLEAN
SdioCmd53(
    IN PWLAN_ADAPTER Adapter,
    IN ULONG Function,
    IN ULONG Address,
    IN PVOID Buffer,
    IN ULONG Length,
    IN BOOLEAN Write,
    IN BOOLEAN Increment
    )

/*++

Routine Description:

    This routine moves Length bytes (a multiple of four) between a function
    and a longword aligned buffer (CMD53): up to 512 bytes in one byte mode
    command; more in block mode, whole blocks of the function's block size
    (the callers pad to it), the rest in byte mode. A frame of F2 thus goes in
    one command, as the chip takes it. Increment moves through the function's
    address space; without it every byte goes to the same address (a FIFO).

Arguments:

    Adapter - Supplies the adapter.

    Function - Supplies the number of the function.

    Address - Supplies the address in the function's address space.

    Buffer - Supplies the buffer, which receives the bytes of a read.

    Length - Supplies the number of bytes.

    Write - Supplies whether the bytes are written.

    Increment - Supplies whether the address moves with the bytes.

Return Value:

    TRUE if all the bytes moved.

--*/

{
    PUCHAR Next = Buffer;
    ULONG BlockSize = Adapter->BlockSize[Function];
    ULONG Command;
    ULONG Argument;
    ULONG Blocks;
    ULONG Count;
    ULONG Response;

    while (Length != 0) {
        Argument = (Write ? 0x80000000 : 0) | (Function << 28) | (Increment ? 0x04000000 : 0) |
                   ((Address & 0x1FFFF) << 9);
        Command = CMD(53) | R5 | CMD_ISDATA | (Write ? 0 : TM_DAT_DIR_READ);
        Blocks = BlockSize != 0 && Length > 512 ? Length / BlockSize : 0;
        if (Blocks > 511) {
            Blocks = 511;
        }

        if (Blocks != 0) {
            Argument |= 0x08000000 | Blocks;
            Command |= TM_BLKCNT_EN | (Blocks > 1 ? TM_MULTI_BLOCK : 0);
            Count = Blocks * BlockSize;
            if (!HostCommand(Adapter, Command, Argument, BlockSize, Blocks, &Response) ||
                !HostData(Adapter, (PULONG)Next, BlockSize, Blocks, Write)) {
                Adapter->LastSdioAddress = (Function << 28) | Address;
                return FALSE;
            }
        } else {
            Count = Length;
            if (Count > 512) {
                Count = 512;
            }

            Argument |= Count & 0x1FF;
            if (!HostCommand(Adapter, Command, Argument, Count, 1, &Response) ||
                !HostData(Adapter, (PULONG)Next, Count, 1, Write)) {
                Adapter->LastSdioAddress = (Function << 28) | Address;
                return FALSE;
            }
        }

        if ((Response & R5_ERRORS) != 0) {
            Adapter->SdioErrors += 1;
            Adapter->LastSdioAddress = (Function << 28) | Address;
            Adapter->LastSdioStatus = Response;
            return FALSE;
        }

        Next += Count;
        Length -= Count;
        if (Increment) {
            Address += Count;
        }
    }

    return TRUE;
}

BOOLEAN
SdioSetBlockSize(
    IN PWLAN_ADAPTER Adapter,
    IN ULONG Function,
    IN ULONG Size
    )

/*++

Routine Description:

    This routine sets the block size of a function: FBR 0x10-0x11 of the
    function (F0's in the CCCR), and the size CMD53 uses in block mode.

Arguments:

    Adapter - Supplies the adapter.

    Function - Supplies the number of the function.

    Size - Supplies the block size, in bytes.

Return Value:

    TRUE if the card took the block size.

--*/

{
    ULONG Base = Function * 0x100 + 0x10;

    if (!SdioWrite8(Adapter, 0, Base, (UCHAR)Size) || !SdioWrite8(Adapter, 0, Base + 1, (UCHAR)(Size >> 8))) {
        return FALSE;
    }

    Adapter->BlockSize[Function] = Size;
    return TRUE;
}

BOOLEAN
SdioInitialize(
    IN PWLAN_ADAPTER Adapter,
    IN ULONG TransferClock
    )

/*++

Routine Description:

    This routine resets the controller and brings the chip's SDIO interface
    up: CMD0, CMD5 until the card is ready, CMD3 (its address), CMD7 (select),
    the 4-bit bus (CCCR 0x07) and high speed (CCCR 0x13) when the card has it,
    then the transfer clock. The card interrupt shows in the interrupt status
    register (its status enable is on, its signal off until
    SdioEnableInterrupt).

Arguments:

    Adapter - Supplies the adapter.

    TransferClock - Supplies the clock of the transfers, in Hz.

Return Value:

    TRUE if the card is ready for transfers.

--*/

{
    ULONG Ocr = 0;
    ULONG Rca;
    ULONG Index;
    UCHAR Value;

    Adapter->WriteDelay = 2 * 1000000 / IDENT_CLOCK + 1;
    HostWrite(Adapter, CONTROL0, 0);
    HostWrite(Adapter, CONTROL1, C1_SRST_HC);
    if (!HostWait(Adapter, CONTROL1, C1_SRST_HC, 0)) {
        WlanLog(Adapter, "SDIO: the controller does not come out of reset");
        return FALSE;
    }

    HostWrite(Adapter, CONTROL0, C0_POWER_330);
    if (!HostSetClock(Adapter, IDENT_CLOCK)) {
        WlanLog(Adapter, "SDIO: the clock does not settle");
        return FALSE;
    }

    HostWrite(Adapter, IRPT_EN, 0);
    HostWrite(Adapter, IRPT_MASK, 0xFFFFFFFF);
    HostWrite(Adapter, INTERRUPT, 0xFFFFFFFF);
    Adapter->InterruptEnable = 0;
    KeStallExecutionProcessor(10000);

    //
    // An I/O reset (CCCR 0x06 RES) first: the card may still be selected, its
    // functions on, from an earlier start. It fails on a card that is not.
    //

    SdioCmd52(Adapter, 0, 0x06, TRUE, 0x08, NULL);
    KeStallExecutionProcessor(10000);
    HostCommand(Adapter, CMD(0) | RSP_NONE, 0, 0, 0, NULL);
    if (!HostCommand(Adapter, CMD(5) | R4, 0, 0, 0, &Ocr)) {
        WlanLog(Adapter, "SDIO: no card answers CMD5 (status %08lx)", Adapter->LastSdioStatus);
        return FALSE;
    }

    for (Index = 0; Index < 100; Index += 1) {
        if (HostCommand(Adapter, CMD(5) | R4, Ocr & 0x00FF8000, 0, 0, &Ocr) && (Ocr & 0x80000000) != 0) {
            break;
        }

        KeStallExecutionProcessor(10000);
    }

    if ((Ocr & 0x80000000) == 0) {
        WlanLog(Adapter, "SDIO: no card answers CMD5 (OCR %08lx, status %08lx)", Ocr, Adapter->LastSdioStatus);
        return FALSE;
    }

    WlanLog(Adapter, "SDIO: card ready, OCR %08lx, %lu functions", Ocr, (Ocr >> 28) & 7);
    if (!HostCommand(Adapter, CMD(3) | R6, 0, 0, 0, &Rca) ||
        !HostCommand(Adapter, CMD(7) | R1B, Rca & 0xFFFF0000, 0, 0, NULL)) {
        WlanLog(Adapter, "SDIO: CMD3/CMD7 failed (status %08lx)", Adapter->LastSdioStatus);
        return FALSE;
    }

    //
    // The 4-bit bus, then high speed if the card supports it.
    //

    if (!SdioRead8(Adapter, 0, 0x07, &Value) || !SdioWrite8(Adapter, 0, 0x07, (UCHAR)((Value & ~3) | 2))) {
        WlanLog(Adapter, "SDIO: CCCR bus width write failed");
        return FALSE;
    }

    HostWrite(Adapter, CONTROL0, HostRead(Adapter, CONTROL0) | C0_HCTL_DWIDTH);
    if (TransferClock > 25000000) {
        if (SdioRead8(Adapter, 0, 0x13, &Value) && (Value & 1) != 0 &&
            SdioWrite8(Adapter, 0, 0x13, (UCHAR)(Value | 2))) {
            HostWrite(Adapter, CONTROL0, HostRead(Adapter, CONTROL0) | C0_HCTL_HS_EN);
        } else {
            TransferClock = 25000000;
        }
    }

    if (!HostSetClock(Adapter, TransferClock)) {
        WlanLog(Adapter, "SDIO: the transfer clock does not settle");
        return FALSE;
    }

    WlanLog(Adapter, "SDIO: 4-bit bus at %lu Hz (base %lu Hz)", Adapter->SdClock, Adapter->BaseClock);
    return TRUE;
}

BOOLEAN
SdioInterruptPending(
    IN PWLAN_ADAPTER Adapter
    )

/*++

Routine Description:

    This routine tells whether the card interrupt is pending (its status,
    whether or not it is signalled).

Arguments:

    Adapter - Supplies the adapter.

Return Value:

    TRUE if the card interrupt is pending.

--*/

{
    return (HostRead(Adapter, INTERRUPT) & INT_CARD) != 0;
}

//
// The signal of the card interrupt: on once the thread has served the chip,
// off in the interrupt service routine until then. As the writes of the
// thread (HostWrite), with the delay after it.
//

static BOOLEAN
SdioSetInterrupt(
    IN PVOID Context
    )
{
    PWLAN_ADAPTER Adapter = Context;

    WRITE_REGISTER_ULONG((PULONG)(Adapter->Registers + IRPT_EN), Adapter->InterruptEnable);
    KeStallExecutionProcessor(Adapter->WriteDelay);
    return TRUE;
}

VOID
SdioEnableInterrupt(
    IN PWLAN_ADAPTER Adapter,
    IN BOOLEAN Enable
    )

/*++

Routine Description:

    This routine sets the signal of the card interrupt on or off, synchronized
    with the interrupt service routine.

Arguments:

    Adapter - Supplies the adapter.

    Enable - Supplies whether the card interrupt is signalled.

Return Value:

    None.

--*/

{
    //
    // Without its interrupt connected (on a BCM2711 the EMMC shares the
    // EMMC2's, which the SD card's miniport holds) the card is polled and its
    // interrupt never signalled.
    //

    if (!Adapter->InterruptConnected) {
        Adapter->InterruptEnable = 0;
        return;
    }

    Adapter->InterruptEnable = Enable ? INT_CARD : 0;
    NdisMSynchronizeWithInterrupt(&Adapter->Interrupt, SdioSetInterrupt, Adapter);
}

BOOLEAN
SdioIsr(
    IN PWLAN_ADAPTER Adapter
    )

/*++

Routine Description:

    This routine is the interrupt service routine's part: whether the card
    interrupts, and then its signal off.

Arguments:

    Adapter - Supplies the adapter.

Return Value:

    TRUE if the card interrupted.

--*/

{
    if (Adapter->InterruptEnable == 0 ||
        (READ_REGISTER_ULONG((PULONG)(Adapter->Registers + INTERRUPT)) & INT_CARD) == 0) {
        return FALSE;
    }

    Adapter->InterruptEnable = 0;
    SdioSetInterrupt(Adapter);
    return TRUE;
}

VOID
SdioShutdown(
    IN PWLAN_ADAPTER Adapter
    )

/*++

Routine Description:

    This routine leaves the controller reset, its card powered down.

Arguments:

    Adapter - Supplies the adapter.

Return Value:

    None.

--*/

{
    HostWrite(Adapter, IRPT_EN, 0);
    SdioCmd52(Adapter, 0, 0x06, TRUE, 0x08, NULL);
    HostWrite(Adapter, CONTROL0, 0);
    HostWrite(Adapter, CONTROL1, C1_SRST_HC);
}
