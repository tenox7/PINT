/*++

Module Name:

    myfault.c

Abstract:

    Stops the system with a STOP screen on demand, to test crash dumps
    (crash_dump=1 and total_mem= of config.txt). The bugcheck is the
    kernel's, so a driver makes it (drivers/myfault, myfault.sys in
    System32\Drivers): this program installs it as the service Myfault if it
    is not, starts it and sends it the request. The STOP code is 0xDEADDEAD.
    (Named after the crash tool everyone knows; the code is PINT's own.)

    What is not yet written to the disk is lost: close the programs first.

    myfault now

--*/

#include <windows.h>
#include <winioctl.h>
#include <stdio.h>
#include <string.h>

//
// The request of the driver (drivers/myfault/myfault.c has the same).
//

#define IOCTL_MYFAULT_STOP  CTL_CODE(FILE_DEVICE_UNKNOWN, 0x800, METHOD_BUFFERED, FILE_ANY_ACCESS)

int __cdecl
main(
    int argc,
    char **argv
    )
{
    SC_HANDLE Manager, Service;
    HANDLE Device;
    DWORD Returned;

    if (argc != 2 || _stricmp(argv[1], "now") != 0) {
        printf("\nusage: myfault now\n\n"
               "Stops Windows NT with a STOP screen (0xDEADDEAD), at once: unsaved work\n"
               "is lost. For testing crash dumps.\n");
        return 1;
    }

    Manager = OpenSCManager(NULL, NULL, SC_MANAGER_ALL_ACCESS);
    if (Manager == NULL) {
        printf("myfault: cannot open the service control manager (error %lu)\n", GetLastError());
        return 1;
    }

    Service = OpenService(Manager, "Myfault", SERVICE_START);
    if (Service == NULL) {
        Service = CreateService(Manager, "Myfault", "Myfault", SERVICE_START, SERVICE_KERNEL_DRIVER,
                                SERVICE_DEMAND_START, SERVICE_ERROR_IGNORE, "System32\\Drivers\\myfault.sys",
                                NULL, NULL, NULL, NULL, NULL);
    }

    if (Service == NULL) {
        printf("myfault: cannot install the driver (error %lu)\n", GetLastError());
        return 1;
    }

    if (!StartService(Service, 0, NULL) && GetLastError() != ERROR_SERVICE_ALREADY_RUNNING) {
        printf("myfault: cannot start the driver, System32\\Drivers\\myfault.sys (error %lu)\n", GetLastError());
        return 1;
    }

    Device = CreateFile("\\\\.\\Myfault", GENERIC_READ | GENERIC_WRITE, 0, NULL, OPEN_EXISTING, 0, NULL);
    if (Device == INVALID_HANDLE_VALUE) {
        printf("myfault: cannot open the driver (error %lu)\n", GetLastError());
        return 1;
    }

    DeviceIoControl(Device, IOCTL_MYFAULT_STOP, NULL, 0, NULL, 0, &Returned, NULL);
    printf("myfault: the system did not stop (error %lu)\n", GetLastError());
    return 1;
}
