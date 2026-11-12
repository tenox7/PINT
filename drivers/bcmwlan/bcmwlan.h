/*++

Module Name:

    bcmwlan.h

Abstract:

    The definitions of the wireless miniport driver of the Raspberry Pi: the
    adapter, the SDIO host (sdio.c) and the chip (chip.c), a Broadcom or
    Cypress FullMAC chip whose firmware runs 802.11 and the WPA2/WPA3
    supplicant and exchanges Ethernet frames with the host.

--*/

#ifndef _BCMWLAN_
#define _BCMWLAN_

#include <ndis.h>
#include <stdarg.h>
#include "bcmwlanhw.h"

#define BCMWLAN_NDIS_MAJOR_VERSION  3
#define BCMWLAN_NDIS_MINOR_VERSION  0

#define ETH_HEADER_SIZE             14
#define ETH_MAXIMUM_FRAME           1514
#define ETH_MINIMUM_FRAME           60
#define WLAN_MULTICAST_LIST         32

//
// The frames between the miniport and the thread: a ring of transmit slots
// the send handler fills and the thread sends, and a ring of received frames
// the thread fills and the timer indicates. A slot has room in front of the
// frame for the SDPCM and BDC headers.
//

#define TRANSMIT_SLOTS              32
#define RECEIVE_SLOTS               32
#define FRAME_HEADROOM              64
#define SLOT_SIZE                   2048

//
// NdisFillMemory is not in the ndis.h of NT 3.5.
//

#ifndef NdisFillMemory
#define NdisFillMemory(Destination, Length, Fill) RtlFillMemory(Destination, Length, Fill)
#endif

//
// The security of the network to join (wifi_security= of config.txt, or the
// registry value Security).
//

#define SECURITY_OPEN               0
#define SECURITY_WPA2               1
#define SECURITY_WPA3               2
#define SECURITY_WPA2_WPA3          3

//
// The state of the connection, as the thread follows the firmware's events.
//

#define LINK_IDLE                   0
#define LINK_JOINING                1
#define LINK_UP                     2

//
// The largest control message (an iovar's name and data, a scan result).
//

#define CONTROL_BUFFER_SIZE         2048
#define RECEIVE_BUFFER_SIZE         10240

typedef struct _WLAN_SLOT {
    ULONG Length;
    ULONG Data[SLOT_SIZE / sizeof(ULONG)];
} WLAN_SLOT, *PWLAN_SLOT;

typedef struct _WLAN_ADAPTER {
    NDIS_HANDLE MiniportAdapterHandle;
    NDIS_MINIPORT_INTERRUPT Interrupt;
    NDIS_MINIPORT_TIMER Timer;
    NDIS_SPIN_LOCK Lock;

    //
    // The SDIO host: the registers of the EMMC, its base clock and the SD
    // clock, the delay after a register write (two SD clocks), the block
    // size of each function, the signal enable of the card interrupt, and
    // the last error (logged).
    //

    PUCHAR Registers;
    ULONG RegistersPhysical;
    ULONG RegistersLength;
    ULONG InterruptVector;
    ULONG InterruptLevel;
    ULONG BaseClock;
    ULONG SdClock;
    ULONG WriteDelay;
    ULONG BlockSize[3];
    ULONG InterruptEnable;
    BOOLEAN InterruptConnected;
    ULONG SdioErrors;
    ULONG LastSdioStatus;
    ULONG LastSdioAddress;

    //
    // The board and the chip: the board's revision code, the chip's id and
    // revision, the cores the driver touches, its RAM, and the backplane
    // window the F1 address space shows.
    //

    ULONG BoardRevision;
    ULONG ChipId;
    ULONG ChipRevision;
    ULONG ChipCommonBase;
    ULONG ArmCoreBase;
    ULONG ArmWrapperBase;
    ULONG SocramBase;
    ULONG SocramWrapperBase;
    ULONG SdioCoreBase;
    ULONG SdioCoreRevision;
    ULONG RamBase;
    ULONG RamSize;
    BOOLEAN ArmCr4;
    ULONG BackplaneWindow;
    CHAR FirmwareVersion[128];

    //
    // SDPCM and BDC: the next transmit sequence number, the highest the
    // firmware takes (flow control credit), the flow control mask, and the
    // id of the next control request.
    //

    UCHAR TxSequence;
    UCHAR TxMaxSequence;
    UCHAR FlowControl;
    USHORT ControlId;

    //
    // The control channel: the buffer a request is built in and its
    // response read into, and the status and length of the response.
    //

    PUCHAR ControlBuffer;
    PUCHAR VarBuffer;
    PUCHAR FrameBuffer;
    PUCHAR ResponseBuffer;
    ULONG ResponseLength;
    LONG ResponseStatus;
    BOOLEAN ResponseReady;
    USHORT ResponseId;
    BOOLEAN FlowStopped;
    BOOLEAN FlowState;
    BOOLEAN FramesPending;
    BOOLEAN Supplicant;
    BOOLEAN Mfp;
    BOOLEAN InChipSae;
    BOOLEAN Scanning;
    ULONG ScanCount;
    UCHAR ScanBssid[32][ETH_LENGTH_OF_ADDRESS];
    ULONG F2BlockSize;

    //
    // The station.
    //

    UCHAR PermanentAddress[ETH_LENGTH_OF_ADDRESS];
    UCHAR CurrentAddress[ETH_LENGTH_OF_ADDRESS];
    UCHAR Bssid[ETH_LENGTH_OF_ADDRESS];

    //
    // The network to join, and the connection.
    //

    UCHAR Ssid[33];
    ULONG SsidLength;
    UCHAR Passphrase[129];
    ULONG PassphraseLength;
    ULONG Security;
    CHAR Country[4];
    ULONG TransferClock;
    BOOLEAN Configured;
    BOOLEAN Debug;
    ULONG LinkState;
    BOOLEAN Keyed;
    BOOLEAN Associated;
    BOOLEAN LinkIndicated;
    BOOLEAN LinkChanged;
    LONG Rssi;
    ULONG JoinAttempts;
    LARGE_INTEGER JoinStarted;
    LARGE_INTEGER NextJoin;
    ULONG LastEvent;
    ULONG LastEventStatus;
    ULONG LastEventReason;

    //
    // The rings: transmit slots (the next the thread sends and the send
    // handler fills, and the count), and received frames (the next the timer
    // indicates and the thread fills, and the count), each guarded by Lock;
    // the frame being indicated, which TransferData copies from.
    //

    PWLAN_SLOT TransmitSlots;
    ULONG TransmitHead;
    ULONG TransmitTail;
    ULONG TransmitCount;
    BOOLEAN ResourcesNeeded;
    PWLAN_SLOT ReceiveSlots;
    ULONG ReceiveHead;
    ULONG ReceiveTail;
    ULONG ReceiveCount;
    PUCHAR IndicatedFrame;
    ULONG IndicatedLength;

    //
    // The receive filter and its multicast list.
    //

    ULONG PacketFilter;
    ULONG MulticastCount;
    UCHAR Multicast[WLAN_MULTICAST_LIST][ETH_LENGTH_OF_ADDRESS];
    BOOLEAN FilterChanged;

    //
    // The thread.
    //

    PVOID Thread;
    KEVENT WorkEvent;
    BOOLEAN Halting;
    BOOLEAN TimerSet;
    BOOLEAN Initialized;

    //
    // The log, C:\WLAN.LOG.
    //

    HANDLE LogFile;
    ULONG LogLines;

    //
    // The statistics.
    //

    ULONG TransmitOk;
    ULONG ReceiveOk;
    ULONG TransmitError;
    ULONG ReceiveError;
    ULONG ReceiveNoBuffer;
} WLAN_ADAPTER, *PWLAN_ADAPTER;

//
// The log (bcmwlan.c).
//

VOID
WlanLog(
    IN PWLAN_ADAPTER Adapter,
    IN PCHAR Format,
    ...
    );

VOID
WlanLogBytes(
    IN PWLAN_ADAPTER Adapter,
    IN PCHAR Title,
    IN PUCHAR Data,
    IN ULONG Length
    );

//
// Files of the driver (bcmwlan.c): read a whole file into pool.
//

PUCHAR
WlanReadFile(
    IN PWLAN_ADAPTER Adapter,
    IN PWCHAR Path,
    OUT PULONG Length
    );

//
// A received frame or a change of the link, for the timer to indicate
// (bcmwlan.c, from the thread).
//

VOID
WlanQueueReceive(
    IN PWLAN_ADAPTER Adapter,
    IN PUCHAR Frame,
    IN ULONG Length
    );

BOOLEAN
WlanNextTransmit(
    IN PWLAN_ADAPTER Adapter,
    OUT PWLAN_SLOT *Slot
    );

VOID
WlanTransmitDone(
    IN PWLAN_ADAPTER Adapter,
    IN BOOLEAN Ok
    );

VOID
WlanSetLink(
    IN PWLAN_ADAPTER Adapter,
    IN BOOLEAN Up
    );

//
// The SDIO host (sdio.c).
//

BOOLEAN
SdioInitialize(
    IN PWLAN_ADAPTER Adapter,
    IN ULONG TransferClock
    );

BOOLEAN
SdioCmd52(
    IN PWLAN_ADAPTER Adapter,
    IN ULONG Function,
    IN ULONG Address,
    IN BOOLEAN Write,
    IN UCHAR Data,
    OUT PUCHAR Value OPTIONAL
    );

BOOLEAN
SdioRead8(
    IN PWLAN_ADAPTER Adapter,
    IN ULONG Function,
    IN ULONG Address,
    OUT PUCHAR Value
    );

BOOLEAN
SdioWrite8(
    IN PWLAN_ADAPTER Adapter,
    IN ULONG Function,
    IN ULONG Address,
    IN UCHAR Value
    );

BOOLEAN
SdioCmd53(
    IN PWLAN_ADAPTER Adapter,
    IN ULONG Function,
    IN ULONG Address,
    IN PVOID Buffer,
    IN ULONG Length,
    IN BOOLEAN Write,
    IN BOOLEAN Increment
    );

BOOLEAN
SdioSetBlockSize(
    IN PWLAN_ADAPTER Adapter,
    IN ULONG Function,
    IN ULONG Size
    );

BOOLEAN
SdioInterruptPending(
    IN PWLAN_ADAPTER Adapter
    );

VOID
SdioEnableInterrupt(
    IN PWLAN_ADAPTER Adapter,
    IN BOOLEAN Enable
    );

BOOLEAN
SdioIsr(
    IN PWLAN_ADAPTER Adapter
    );

VOID
SdioShutdown(
    IN PWLAN_ADAPTER Adapter
    );

//
// The chip (chip.c).
//

NDIS_STATUS
ChipStart(
    IN PWLAN_ADAPTER Adapter
    );

VOID
ChipStop(
    IN PWLAN_ADAPTER Adapter
    );

BOOLEAN
ChipJoin(
    IN PWLAN_ADAPTER Adapter
    );

BOOLEAN
ChipService(
    IN PWLAN_ADAPTER Adapter
    );

BOOLEAN
ChipSend(
    IN PWLAN_ADAPTER Adapter,
    IN PWLAN_SLOT Slot
    );

VOID
ChipSetFilter(
    IN PWLAN_ADAPTER Adapter
    );

VOID
ChipCheckLink(
    IN PWLAN_ADAPTER Adapter
    );

#endif // _BCMWLAN_
