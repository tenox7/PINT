/*++

Module Name:

    crashdmp.c

Abstract:

    The crash dump setup of the SD card driver. NT 3.5 writes a crash dump
    (all of memory) through a private copy of the driver of the boot disk,
    which diskdump.sys drives by polling, into the pagefile of the boot
    partition, which must hold all of memory and a page; it reads whether to
    from Control\CrashControl right after the boot drivers start. This driver
    is one: with the CRASHDUMP load option, which the ARC firmware adds for
    crash_dump=1 of config.txt, it sets CrashDumpEnabled and a pagefile on C:
    of the size of memory (keeping the pagefile setting it replaces, which it
    puts back when the option goes); without it, dumps are off. After a crash
    the pagefile holds the dump, which savedump copies to C:\WINNT\MEMORY.DMP
    at the next start.

    The firmware passes CRASHDUMP only with total_mem= (the Pi firmware's
    memory limit) too, so that the dump fits, and gives that limit with it
    (CRASHDUMP=256, in megabytes): the kernel of NT 3.5 exports nothing a
    driver could ask for the size of memory, so the pagefile is sized from it.

Environment:

    kernel mode only

--*/

#include <ntddk.h>

#define CONTROL_PATH        L"\\Registry\\Machine\\System\\CurrentControlSet\\Control"
#define CRASH_CONTROL_PATH  CONTROL_PATH L"\\CrashControl"
#define MEMORY_PATH         CONTROL_PATH L"\\Session Manager\\Memory Management"
#define SAVED_PAGING_FILES  L"PintSavedPagingFiles"

VOID
BcmEmmcSetCrashDump(
    VOID
    );

//
// A value of a key, read whole (the caller frees it), or NULL.
//

static PKEY_VALUE_PARTIAL_INFORMATION
ReadValue(
    IN PWSTR Path,
    IN PWSTR Name
    )
{
    UNICODE_STRING KeyName;
    UNICODE_STRING ValueName;
    OBJECT_ATTRIBUTES Attributes;
    PKEY_VALUE_PARTIAL_INFORMATION Value = NULL;
    HANDLE Key;
    ULONG Length = 0;

    RtlInitUnicodeString(&KeyName, Path);
    RtlInitUnicodeString(&ValueName, Name);
    InitializeObjectAttributes(&Attributes, &KeyName, OBJ_CASE_INSENSITIVE, NULL, NULL);
    if (!NT_SUCCESS(ZwOpenKey(&Key, KEY_READ, &Attributes))) {
        return NULL;
    }

    ZwQueryValueKey(Key, &ValueName, KeyValuePartialInformation, NULL, 0, &Length);
    if (Length != 0) {
        Value = ExAllocatePool(PagedPool, Length + sizeof(WCHAR) * 2);
        if (Value != NULL) {
            RtlZeroMemory(Value, Length + sizeof(WCHAR) * 2);
            if (!NT_SUCCESS(ZwQueryValueKey(Key, &ValueName, KeyValuePartialInformation, Value, Length, &Length))) {
                ExFreePool(Value);
                Value = NULL;
            }
        }
    }

    ZwClose(Key);
    return Value;
}

static VOID
WriteValue(
    IN PWSTR Path,
    IN PWSTR Name,
    IN ULONG Type,
    IN PVOID Data,
    IN ULONG Length
    )
{
    RtlWriteRegistryValue(RTL_REGISTRY_ABSOLUTE, Path, Name, Type, Data, Length);
}

static BOOLEAN
SameValue(
    IN PKEY_VALUE_PARTIAL_INFORMATION Value,
    IN PVOID Data,
    IN ULONG Length
    )
{
    return Value != NULL && Value->DataLength == Length && RtlCompareMemory(Value->Data, Data, Length) == Length;
}

//
// Whether the load options (SystemStartOptions) hold CRASHDUMP, and the
// megabytes of memory given with it (CRASHDUMP=256), 0 if none are.
//

static BOOLEAN
CrashDumpOption(
    OUT PULONG Megabytes
    )
{
    PKEY_VALUE_PARTIAL_INFORMATION Value = ReadValue(CONTROL_PATH, L"SystemStartOptions");
    PWCHAR Text;
    PWCHAR Word;
    BOOLEAN Found = FALSE;
    ULONG Index;

    *Megabytes = 0;
    if (Value == NULL) {
        return FALSE;
    }

    if (Value->Type == REG_SZ) {
        Text = (PWCHAR)Value->Data;
        while (*Text != 0 && !Found) {
            while (*Text == L' ' || *Text == L'/') {
                Text += 1;
            }

            Word = Text;
            while (*Text != 0 && *Text != L' ') {
                Text += 1;
            }

            if (Text - Word >= 9) {
                for (Index = 0; Index < 9 && RtlUpcaseUnicodeChar(Word[Index]) == L"CRASHDUMP"[Index]; Index += 1) {
                }

                if (Index == 9 && Text - Word == 9) {
                    Found = TRUE;
                } else if (Index == 9 && Word[9] == L'=') {
                    Found = TRUE;
                    for (Word += 10; Word < Text && *Word >= L'0' && *Word <= L'9'; Word += 1) {
                        *Megabytes = *Megabytes * 10 + (*Word - L'0');
                    }
                }
            }
        }
    }

    ExFreePool(Value);
    return Found;
}

VOID
BcmEmmcSetCrashDump(
    VOID
    )

/*++

Routine Description:

    This routine sets crash dumps on or off for this boot from the CRASHDUMP
    load option, before the I/O system reads Control\CrashControl (right after
    the boot drivers) and the session manager the pagefiles.

Arguments:

    None.

Return Value:

    None.

--*/

{
    PKEY_VALUE_PARTIAL_INFORMATION Current;
    PKEY_VALUE_PARTIAL_INFORMATION Saved;
    WCHAR PagingFiles[64];
    ULONG Enabled;
    ULONG Megabytes;
    ULONG Length;
    BOOLEAN Dump = CrashDumpOption(&Megabytes);

    Enabled = Dump ? 1 : 0;
    Current = ReadValue(CRASH_CONTROL_PATH, L"CrashDumpEnabled");
    if (!SameValue(Current, &Enabled, sizeof(Enabled))) {
        WriteValue(CRASH_CONTROL_PATH, L"CrashDumpEnabled", REG_DWORD, &Enabled, sizeof(Enabled));
    }

    if (Current != NULL) {
        ExFreePool(Current);
    }

    Current = ReadValue(MEMORY_PATH, L"PagingFiles");
    Saved = ReadValue(CRASH_CONTROL_PATH, SAVED_PAGING_FILES);

    if (Dump) {

        //
        // A pagefile on C: of all of memory (the limit the firmware gave; 64 MB
        // without one), a page and a megabyte to spare, in megabytes (a
        // REG_MULTI_SZ: the string and an empty one).
        //

        Megabytes = (Megabytes == 0 || Megabytes > 4000 ? 64 : Megabytes) + 2;

        RtlZeroMemory(PagingFiles, sizeof(PagingFiles));
        swprintf(PagingFiles, L"C:\\pagefile.sys %u %u", Megabytes, Megabytes);
        Length = (wcslen(PagingFiles) + 2) * sizeof(WCHAR);
        if (!SameValue(Current, PagingFiles, Length)) {
            if (Saved == NULL && Current != NULL) {
                WriteValue(CRASH_CONTROL_PATH, SAVED_PAGING_FILES, Current->Type, Current->Data, Current->DataLength);
            }

            WriteValue(MEMORY_PATH, L"PagingFiles", REG_MULTI_SZ, PagingFiles, Length);
        }
    } else if (Saved != NULL) {

        //
        // Dumps off: the pagefile setting the dump one replaced.
        //

        WriteValue(MEMORY_PATH, L"PagingFiles", Saved->Type, Saved->Data, Saved->DataLength);
        RtlDeleteRegistryValue(RTL_REGISTRY_ABSOLUTE, CRASH_CONTROL_PATH, SAVED_PAGING_FILES);
    }

    if (Current != NULL) {
        ExFreePool(Current);
    }

    if (Saved != NULL) {
        ExFreePool(Saved);
    }
}

