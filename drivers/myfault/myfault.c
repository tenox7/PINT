/*++

Module Name:

    myfault.c

Abstract:

    The driver of MYFAULT (apps/myfault): it stops the system with a bugcheck
    when the program asks, to test crash dumps. A bugcheck is the kernel's
    own (KeBugCheckEx), which stops the other processors and writes the dump
    as any STOP does; a program has no call for it. The program installs the
    driver as the service Myfault and starts it when it is run; nothing loads
    it otherwise.

--*/

#include <ntddk.h>

#define MYFAULT_DEVICE      L"\\Device\\Myfault"
#define MYFAULT_LINK        L"\\DosDevices\\Myfault"

//
// The request of the program (apps/myfault/myfault.c has the same), and the
// code of the STOP screen.
//

#define IOCTL_MYFAULT_STOP  CTL_CODE(FILE_DEVICE_UNKNOWN, 0x800, METHOD_BUFFERED, FILE_ANY_ACCESS)
#define MYFAULT_STOP_CODE   0xDEADDEAD

static NTSTATUS
MyfaultDispatch(
    IN PDEVICE_OBJECT DeviceObject,
    IN PIRP Irp
    )
{
    PIO_STACK_LOCATION Stack = IoGetCurrentIrpStackLocation(Irp);
    NTSTATUS Status = STATUS_SUCCESS;

    if (Stack->MajorFunction == IRP_MJ_DEVICE_CONTROL) {
        if (Stack->Parameters.DeviceIoControl.IoControlCode == IOCTL_MYFAULT_STOP) {
            KeBugCheckEx(MYFAULT_STOP_CODE, 0, 0, 0, 0);
        }

        Status = STATUS_INVALID_DEVICE_REQUEST;
    }

    Irp->IoStatus.Status = Status;
    Irp->IoStatus.Information = 0;
    IoCompleteRequest(Irp, IO_NO_INCREMENT);
    return Status;
}

static VOID
MyfaultUnload(
    IN PDRIVER_OBJECT DriverObject
    )
{
    UNICODE_STRING Link;

    RtlInitUnicodeString(&Link, MYFAULT_LINK);
    IoDeleteSymbolicLink(&Link);
    IoDeleteDevice(DriverObject->DeviceObject);
}

NTSTATUS
DriverEntry(
    IN PDRIVER_OBJECT DriverObject,
    IN PUNICODE_STRING RegistryPath
    )
{
    UNICODE_STRING Name, Link;
    PDEVICE_OBJECT DeviceObject;
    NTSTATUS Status;

    RtlInitUnicodeString(&Name, MYFAULT_DEVICE);
    Status = IoCreateDevice(DriverObject, 0, &Name, FILE_DEVICE_UNKNOWN, 0, FALSE, &DeviceObject);
    if (!NT_SUCCESS(Status)) {
        return Status;
    }

    RtlInitUnicodeString(&Link, MYFAULT_LINK);
    Status = IoCreateSymbolicLink(&Link, &Name);
    if (!NT_SUCCESS(Status)) {
        IoDeleteDevice(DeviceObject);
        return Status;
    }

    DriverObject->MajorFunction[IRP_MJ_CREATE] = MyfaultDispatch;
    DriverObject->MajorFunction[IRP_MJ_CLOSE] = MyfaultDispatch;
    DriverObject->MajorFunction[IRP_MJ_DEVICE_CONTROL] = MyfaultDispatch;
    DriverObject->DriverUnload = MyfaultUnload;
    return STATUS_SUCCESS;
}
