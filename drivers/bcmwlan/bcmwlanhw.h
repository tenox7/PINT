/*++

Module Name:

    bcmwlanhw.h

Abstract:

    The on-board wireless chip of the Raspberry Pi (a Broadcom BCM43430 or
    Cypress CYW43436 or CYW43455 on SDIO) as the firmware describes it: the
    NetworkController BCMWLAN_ID of the internal bus, after the Ethernet
    controller. Its configuration data holds the interrupt and the registers
    of the EMMC (the Arasan SD host controller the chip is wired to on GPIO
    34-39), then the board's revision code (which NVRAM file the chip takes)
    and the base clock of the EMMC. The firmware describes it only when an
    SDIO card answers on the EMMC: on a board with the chip, whose card slot
    is on another controller (the SDHOST of a BCM2836 or BCM2837, the EMMC2
    of a BCM2711).

--*/

#ifndef _BCMWLANHW_
#define _BCMWLANHW_

#define BCMWLAN_ID              "BCM43XX-WLAN"

typedef struct _CM_BCMWLAN_DEVICE_DATA {
    USHORT Version;
    USHORT Revision;
    ULONG BoardRevision;
    ULONG BaseClock;
} CM_BCMWLAN_DEVICE_DATA, *PCM_BCMWLAN_DEVICE_DATA;

#endif // _BCMWLANHW_
