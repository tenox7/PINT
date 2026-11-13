/*++

Module Name:

    myfault.c

Abstract:

    Stops the system with a STOP screen on demand, to test crash dumps
    (crash_dump=1 and total_mem= of config.txt): it raises a hard error with
    the shutdown option, which the kernel answers, for a caller with the
    shutdown privilege, with the bugcheck FATAL_UNHANDLED_HARD_ERROR (STOP
    0x0000004C), the status given here as its first parameter; the screen
    names the status ("STOP: deaddead Unknown Hard Error"). It is what
    the session manager does when a system process dies. No driver is
    needed. (Named after the crash tool everyone knows; the code is PINT's
    own.)

    What is not yet written to the disk is lost: close the programs first.

    myfault now

--*/

#include <nt.h>
#include <ntrtl.h>
#include <nturtl.h>
#include <windows.h>
#include <stdio.h>
#include <string.h>

#define MYFAULT_STATUS ((NTSTATUS)0xDEADDEADL)

int __cdecl
main(
    int argc,
    char **argv
    )
{
    BOOLEAN WasEnabled;
    NTSTATUS Status;
    ULONG Response;

    if (argc != 2 || _stricmp(argv[1], "now") != 0) {
        printf("\nusage: myfault now\n\n"
               "Stops Windows NT with a STOP screen (STOP: deaddead), at once:\n"
               "unsaved work is lost. For testing crash dumps.\n");
        return 1;
    }

    Status = RtlAdjustPrivilege(SE_SHUTDOWN_PRIVILEGE, TRUE, FALSE, &WasEnabled);
    if (!NT_SUCCESS(Status)) {
        printf("myfault: no shutdown privilege (%08lX)\n", Status);
        return 1;
    }

    Status = NtRaiseHardError(MYFAULT_STATUS, 0, 0, NULL, OptionShutdownSystem, &Response);
    printf("myfault: the system did not stop (%08lX)\n", Status);
    return 1;
}
