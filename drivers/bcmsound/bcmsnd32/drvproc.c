/*++

Module Name:

    drvproc.c

Abstract:

    The multimedia driver of the Raspberry Pi's sound driver (bcmsnd32.dll),
    an installable driver as the Windows Sound System's sndsys32.dll is:
    winmm loads it for the wave, mixer, aux and MIDI devices (Drivers32), and
    NT's drvlib (wodMessage, mxdMessage, auxMessage, widMessage) and synthlib
    (modSynthMessage) turn their messages into the requests of the kernel
    driver's devices, which drvlib finds in the Parameters\Device0\Devices of
    the service Bcmsound. The kernel driver names the devices and the lines
    and controls of its mixer by the numbers of the strings of this DLL
    (bcmsnd32.rc); synthlib plays MIDI with the WSS's FM patches, also here.

    It has nothing to configure: the output is sound_output of C:\CONFIG.TXT.

--*/

#include <windows.h>
#include <mmsystem.h>
#include <drvlib.h>
#include <registry.h>
#include "bcmsndid.h"

//
// What drvlib and synthlib take from the driver: its service's name and its
// module (synthlib's patches and alerts).
//

TCHAR DriverName[] = BCMSOUND_SERVICE_NAME;
HMODULE ghModule;

LRESULT
DriverProc(
    DWORD DriverId,
    HDRVR Driver,
    UINT Message,
    LPARAM Parameter1,
    LPARAM Parameter2
    )
{
    switch (Message) {
    case DRV_LOAD:
        ghModule = GetDriverModuleHandle(Driver);
        return DrvLibInit(ghModule, DLL_PROCESS_ATTACH, NULL) ? 1L : 0L;

    case DRV_FREE:
        DrvLibInit(ghModule, DLL_PROCESS_DETACH, NULL);
        return 1L;

    case DRV_OPEN:
    case DRV_CLOSE:
    case DRV_ENABLE:
    case DRV_DISABLE:
        return 1L;

    case DRV_QUERYCONFIGURE:
        return 0L;

    case DRV_INSTALL:
    case DRV_REMOVE:
        return DRVCNF_OK;

    default:
        return DefDriverProc(DriverId, Driver, Message, Parameter1, Parameter2);
    }
}
