/*
 * nvtunedrv - MMIO accessor for NVIDIA GPU BAR0.
 *
 * Legacy (non-PnP) WDM driver. It does not attach to the GPU device stack and
 * does not fight the NVIDIA driver for ownership; it simply maps the same
 * physical MMIO range a second time with matching cache attributes, which is
 * permitted and is how every hardware-monitoring tool on Windows works.
 *
 * Design constraints, in priority order:
 *
 *   1. Usermode never supplies a physical address. It supplies a PCI
 *      bus/device/function; the driver reads BAR0 out of config space and
 *      refuses anything that is not an NVIDIA display controller.
 *   2. Every read and write is validated against the allowlist in
 *      nvtune_ranges.h, in kernel mode, on every call. Writes are restricted
 *      to the FBPA apertures plus one ROM-visibility register.
 *   3. The device object is created with an ACL admitting only SYSTEM and
 *      Administrators.
 *
 *
 * Build: see build.cmd (EWDK or WDK). Test-sign and load: see
 * ../scripts/install-driver.ps1.
 */

#include <ntddk.h>
#include <wdmsec.h>

#include "nvtunedrv_ioctl.h"
#include "nvtune_ranges.h"

#define NVT_POOL_TAG      'nuvN'
#define NVT_MAX_DEVICES   8u
#define NVT_DRIVER_VERSION 0x00010000

/* PCI config space offsets we care about. */
#define PCI_CFG_VENDOR_ID     0x00
#define PCI_CFG_DEVICE_ID     0x02
#define PCI_CFG_REVISION      0x08
#define PCI_CFG_CLASS_CODE    0x09
#define PCI_CFG_BAR0          0x10
#define PCI_CFG_BAR1          0x14
#define PCI_CFG_SUBSYS_VENDOR 0x2C
#define PCI_CFG_SUBSYS_DEVICE 0x2E

#define PCI_VENDOR_NVIDIA     0x10DE
#define PCI_CLASS_DISPLAY     0x03

typedef struct _NVT_MAPPING {
    BOOLEAN          InUse;
    ULONG            Bus;
    ULONG            Device;
    ULONG            Function;
    PHYSICAL_ADDRESS PhysBase;
    ULONG            Length;
    PVOID            Va;
} NVT_MAPPING;

static NVT_MAPPING    g_Mappings[NVT_MAX_DEVICES];
static FAST_MUTEX     g_Lock;
static PDEVICE_OBJECT g_DeviceObject = NULL;

DRIVER_INITIALIZE DriverEntry;
static DRIVER_UNLOAD          NvtUnload;
static DRIVER_DISPATCH        NvtCreateClose;
static DRIVER_DISPATCH        NvtDeviceControl;

#ifdef ALLOC_PRAGMA
#pragma alloc_text(INIT, DriverEntry)
#pragma alloc_text(PAGE, NvtUnload)
#pragma alloc_text(PAGE, NvtCreateClose)
#pragma alloc_text(PAGE, NvtDeviceControl)
#endif

/* ------------------------------------------------------------------ *
 *  PCI config space
 * ------------------------------------------------------------------ */

static NTSTATUS
NvtReadConfig(ULONG Bus, ULONG Device, ULONG Function,
              ULONG Offset, PVOID Buffer, ULONG Length)
{
    PCI_SLOT_NUMBER slot;
    ULONG read;

    RtlZeroMemory(&slot, sizeof(slot));
    slot.u.bits.DeviceNumber   = Device;
    slot.u.bits.FunctionNumber = Function;

    /*
     * C4996: HalGetBusDataByOffset is marked deprecated in favour of the
     * IRP_MN_QUERY_INTERFACE / BUS_INTERFACE_STANDARD (GetBusData) path. That
     * replacement only works for a PnP driver that owns a device object in the
     * target's stack -- it sends an IRP down that stack. This is a deliberately
     * non-PnP legacy driver: it never attaches to the GPU, so there is no stack
     * to query and the "modern" API is not applicable. HalGetBusDataByOffset
     * remains the supported way to read config space from an unattached driver,
     * so the deprecation is suppressed here, narrowly, at the single call site.
     */
#pragma warning(push)
#pragma warning(disable : 4996)
    read = HalGetBusDataByOffset(PCIConfiguration, Bus, slot.u.AsULONG,
                                 Buffer, Offset, Length);
#pragma warning(pop)
    if (read != Length) {
        return STATUS_DEVICE_DOES_NOT_EXIST;
    }
    return STATUS_SUCCESS;
}


/*
 * Read BAR0's physical base straight from config space.
 *
 * Note that we never write the BAR to probe its size: the all-ones size probe
 * is destructive and the NVIDIA driver is live on this device. The length is
 * taken from the (clamped) usermode hint instead, and every subsequent access
 * is bounds-checked against it anyway.
 */
static NTSTATUS
NvtQueryDevice(ULONG Bus, ULONG Device, ULONG Function,
               PHYSICAL_ADDRESS* PhysBase, NVTUNE_MAP_OUT* Info)
{
    NTSTATUS status;
    USHORT vendor = 0, devid = 0, subvendor = 0, subdevice = 0;
    UCHAR  revision = 0;
    UCHAR  classcode[3] = { 0, 0, 0 };
    ULONG  bar0 = 0, bar1 = 0;
    ULONGLONG base;

    status = NvtReadConfig(Bus, Device, Function, PCI_CFG_VENDOR_ID,
                           &vendor, sizeof(vendor));
    if (!NT_SUCCESS(status)) return status;

    if (vendor != PCI_VENDOR_NVIDIA) {
        return STATUS_INVALID_DEVICE_REQUEST;
    }

    status = NvtReadConfig(Bus, Device, Function, PCI_CFG_DEVICE_ID,
                           &devid, sizeof(devid));
    if (!NT_SUCCESS(status)) return status;

    status = NvtReadConfig(Bus, Device, Function, PCI_CFG_REVISION,
                           &revision, sizeof(revision));
    if (!NT_SUCCESS(status)) return status;

    /* classcode[2] is the base class. */
    status = NvtReadConfig(Bus, Device, Function, PCI_CFG_CLASS_CODE,
                           classcode, sizeof(classcode));
    if (!NT_SUCCESS(status)) return status;

    if (classcode[2] != PCI_CLASS_DISPLAY) {
        return STATUS_INVALID_DEVICE_REQUEST;
    }

    status = NvtReadConfig(Bus, Device, Function, PCI_CFG_BAR0,
                           &bar0, sizeof(bar0));
    if (!NT_SUCCESS(status)) return status;

    /* Must be a memory BAR, not I/O. */
    if ((bar0 & 0x1u) != 0u) {
        return STATUS_INVALID_DEVICE_REQUEST;
    }

    base = (ULONGLONG)(bar0 & 0xFFFFFFF0u);

    /* Type 10b means the BAR is 64-bit and continues in the next slot. */
    if (((bar0 >> 1) & 0x3u) == 0x2u) {
        status = NvtReadConfig(Bus, Device, Function, PCI_CFG_BAR1,
                               &bar1, sizeof(bar1));
        if (!NT_SUCCESS(status)) return status;
        base |= ((ULONGLONG)bar1) << 32;
    }

    if (base == 0ull) {
        return STATUS_DEVICE_NOT_READY;
    }

    (void)NvtReadConfig(Bus, Device, Function, PCI_CFG_SUBSYS_VENDOR,
                        &subvendor, sizeof(subvendor));
    (void)NvtReadConfig(Bus, Device, Function, PCI_CFG_SUBSYS_DEVICE,
                        &subdevice, sizeof(subdevice));

    PhysBase->QuadPart = (LONGLONG)base;

    Info->vendor_id           = vendor;
    Info->device_id           = devid;
    Info->subsystem_vendor_id = subvendor;
    Info->subsystem_device_id = subdevice;
    Info->revision            = revision;
    Info->class_code          = ((ULONG)classcode[2] << 16) |
                                ((ULONG)classcode[1] << 8) |
                                 (ULONG)classcode[0];
    return STATUS_SUCCESS;
}

/* ------------------------------------------------------------------ *
 *  Mapping table
 * ------------------------------------------------------------------ */

static NVT_MAPPING*
NvtLookup(ULONG Handle)
{
    if (Handle >= NVT_MAX_DEVICES) return NULL;
    if (!g_Mappings[Handle].InUse) return NULL;
    return &g_Mappings[Handle];
}

static void
NvtReleaseAll(void)
{
    ULONG i;
    for (i = 0; i < NVT_MAX_DEVICES; ++i) {
        if (g_Mappings[i].InUse && g_Mappings[i].Va != NULL) {
            MmUnmapIoSpace(g_Mappings[i].Va, g_Mappings[i].Length);
        }
        RtlZeroMemory(&g_Mappings[i], sizeof(g_Mappings[i]));
    }
}

static NTSTATUS
NvtMapBar0(const NVTUNE_MAP_IN* in, NVTUNE_MAP_OUT* out)
{
    NTSTATUS status;
    PHYSICAL_ADDRESS phys;
    ULONG length, slot, i;
    PVOID va;

    RtlZeroMemory(out, sizeof(*out));
    phys.QuadPart = 0;

    status = NvtQueryDevice(in->bus, in->device, in->function, &phys, out);
    if (!NT_SUCCESS(status)) return status;

    length = in->length_hint ? in->length_hint : NVTUNE_MAX_BAR0_LENGTH;
    if (length > NVTUNE_MAX_BAR0_LENGTH) length = NVTUNE_MAX_BAR0_LENGTH;
    if (length < NVTUNE_MIN_BAR0_LENGTH) length = NVTUNE_MIN_BAR0_LENGTH;
    length = (length + PAGE_SIZE - 1) & ~((ULONG)PAGE_SIZE - 1);

    /* Already mapped? Hand back the existing handle rather than double-map. */
    for (i = 0; i < NVT_MAX_DEVICES; ++i) {
        if (g_Mappings[i].InUse &&
            g_Mappings[i].PhysBase.QuadPart == phys.QuadPart) {
            out->handle    = i;
            out->length    = g_Mappings[i].Length;
            out->phys_base = (UINT64)g_Mappings[i].PhysBase.QuadPart;
            return STATUS_SUCCESS;
        }
    }

    slot = NVT_MAX_DEVICES;
    for (i = 0; i < NVT_MAX_DEVICES; ++i) {
        if (!g_Mappings[i].InUse) { slot = i; break; }
    }
    if (slot == NVT_MAX_DEVICES) return STATUS_INSUFFICIENT_RESOURCES;

    /*
     * MmNonCached matches how the NVIDIA driver maps this range. Mapping the
     * same physical page twice with *different* cache attributes is what
     * triggers ATTEMPTED_EXECUTE_OF_NOEXECUTE_MEMORY-class bugchecks, so this
     * is not a detail to get creative with.
     */
    va = MmMapIoSpace(phys, length, MmNonCached);
    if (va == NULL) return STATUS_INSUFFICIENT_RESOURCES;

    g_Mappings[slot].InUse    = TRUE;
    g_Mappings[slot].Bus      = in->bus;
    g_Mappings[slot].Device   = in->device;
    g_Mappings[slot].Function = in->function;
    g_Mappings[slot].PhysBase = phys;
    g_Mappings[slot].Length   = length;
    g_Mappings[slot].Va       = va;

    out->handle    = slot;
    out->length    = length;
    out->phys_base = (UINT64)phys.QuadPart;
    return STATUS_SUCCESS;
}

static NTSTATUS
NvtUnmapBar0(ULONG Handle)
{
    NVT_MAPPING* m = NvtLookup(Handle);
    if (m == NULL) return STATUS_INVALID_HANDLE;
    if (m->Va != NULL) MmUnmapIoSpace(m->Va, m->Length);
    RtlZeroMemory(m, sizeof(*m));
    return STATUS_SUCCESS;
}

/* ------------------------------------------------------------------ *
 *  Dispatch
 * ------------------------------------------------------------------ */

static NTSTATUS
NvtCreateClose(PDEVICE_OBJECT DeviceObject, PIRP Irp)
{
    UNREFERENCED_PARAMETER(DeviceObject);
    PAGED_CODE();

    Irp->IoStatus.Status = STATUS_SUCCESS;
    Irp->IoStatus.Information = 0;
    IoCompleteRequest(Irp, IO_NO_INCREMENT);
    return STATUS_SUCCESS;
}

static NTSTATUS
NvtDeviceControl(PDEVICE_OBJECT DeviceObject, PIRP Irp)
{
    PIO_STACK_LOCATION stack;
    NTSTATUS status = STATUS_INVALID_DEVICE_REQUEST;
    ULONG_PTR info = 0;
    PVOID buffer;
    ULONG inLen, outLen, code;

    UNREFERENCED_PARAMETER(DeviceObject);
    PAGED_CODE();

    stack  = IoGetCurrentIrpStackLocation(Irp);
    buffer = Irp->AssociatedIrp.SystemBuffer;
    inLen  = stack->Parameters.DeviceIoControl.InputBufferLength;
    outLen = stack->Parameters.DeviceIoControl.OutputBufferLength;
    code   = stack->Parameters.DeviceIoControl.IoControlCode;

    ExAcquireFastMutex(&g_Lock);

    switch (code) {

    case IOCTL_NVTUNE_GET_VERSION: {
        NVTUNE_VERSION_OUT* out = (NVTUNE_VERSION_OUT*)buffer;
        if (outLen < sizeof(*out)) { status = STATUS_BUFFER_TOO_SMALL; break; }
        out->abi_version    = NVTUNEDRV_ABI_VERSION;
        out->driver_version = NVT_DRIVER_VERSION;
        out->max_devices    = NVT_MAX_DEVICES;
        out->reserved       = 0;
        info = sizeof(*out);
        status = STATUS_SUCCESS;
        break;
    }

    case IOCTL_NVTUNE_MAP_BAR0: {
        NVTUNE_MAP_IN  in;
        NVTUNE_MAP_OUT out;
        if (inLen < sizeof(in) || outLen < sizeof(out)) {
            status = STATUS_BUFFER_TOO_SMALL; break;
        }
        RtlCopyMemory(&in, buffer, sizeof(in));
        status = NvtMapBar0(&in, &out);
        if (NT_SUCCESS(status)) {
            RtlCopyMemory(buffer, &out, sizeof(out));
            info = sizeof(out);
        }
        break;
    }

    case IOCTL_NVTUNE_UNMAP_BAR0: {
        NVTUNE_HANDLE_IN in;
        if (inLen < sizeof(in)) { status = STATUS_BUFFER_TOO_SMALL; break; }
        RtlCopyMemory(&in, buffer, sizeof(in));
        status = NvtUnmapBar0(in.handle);
        break;
    }

    case IOCTL_NVTUNE_READ32: {
        NVTUNE_READ_IN   in;
        NVTUNE_READ_OUT  out;
        NVT_MAPPING*     m;
        if (inLen < sizeof(in) || outLen < sizeof(out)) {
            status = STATUS_BUFFER_TOO_SMALL; break;
        }
        RtlCopyMemory(&in, buffer, sizeof(in));
        m = NvtLookup(in.handle);
        if (m == NULL) { status = STATUS_INVALID_HANDLE; break; }
        if (!NvtRangeAllowed(in.offset, 4u, 0, m->Length)) {
            status = STATUS_ACCESS_DENIED; break;
        }
        out.value = READ_REGISTER_ULONG(
            (PULONG)((PUCHAR)m->Va + in.offset));
        out.reserved = 0;
        RtlCopyMemory(buffer, &out, sizeof(out));
        info = sizeof(out);
        status = STATUS_SUCCESS;
        break;
    }

    case IOCTL_NVTUNE_WRITE32: {
        NVTUNE_WRITE_IN in;
        NVT_MAPPING*    m;
        if (inLen < sizeof(in)) { status = STATUS_BUFFER_TOO_SMALL; break; }
        RtlCopyMemory(&in, buffer, sizeof(in));
        m = NvtLookup(in.handle);
        if (m == NULL) { status = STATUS_INVALID_HANDLE; break; }
        if (!NvtRangeAllowed(in.offset, 4u, 1, m->Length)) {
            status = STATUS_ACCESS_DENIED; break;
        }
        WRITE_REGISTER_ULONG((PULONG)((PUCHAR)m->Va + in.offset), in.value);
        status = STATUS_SUCCESS;
        break;
    }

    case IOCTL_NVTUNE_READ_BLOCK: {
        NVTUNE_READ_BLOCK_IN in;
        NVT_MAPPING*         m;
        ULONG                bytes;
        ULONG                i;
        PULONG               dst;
        if (inLen < sizeof(in)) { status = STATUS_BUFFER_TOO_SMALL; break; }
        RtlCopyMemory(&in, buffer, sizeof(in));
        if (in.count == 0u || in.count > NVTUNE_MAX_BLOCK_DWORDS) {
            status = STATUS_INVALID_PARAMETER; break;
        }
        /* count <= 1024, so bytes <= 4096: the u32 multiply cannot overflow. */
        bytes = in.count * 4u;
        if (outLen < bytes) { status = STATUS_BUFFER_TOO_SMALL; break; }
        m = NvtLookup(in.handle);
        if (m == NULL) { status = STATUS_INVALID_HANDLE; break; }
        if (!NvtRangeAllowed(in.offset, bytes, 0, m->Length)) {
            status = STATUS_ACCESS_DENIED; break;
        }
        dst = (PULONG)buffer;
        for (i = 0; i < in.count; ++i) {
            dst[i] = READ_REGISTER_ULONG(
                (PULONG)((PUCHAR)m->Va + in.offset + i * 4u));
        }
        info = bytes;
        status = STATUS_SUCCESS;
        break;
    }

    default:
        break;
    }

    ExReleaseFastMutex(&g_Lock);

    Irp->IoStatus.Status = status;
    Irp->IoStatus.Information = info;
    IoCompleteRequest(Irp, IO_NO_INCREMENT);
    return status;
}

static VOID
NvtUnload(PDRIVER_OBJECT DriverObject)
{
    UNICODE_STRING dosName;
    PAGED_CODE();

    ExAcquireFastMutex(&g_Lock);
    NvtReleaseAll();
    ExReleaseFastMutex(&g_Lock);

    RtlInitUnicodeString(&dosName, NVTUNEDRV_DOS_DEVICE_NAME);
    IoDeleteSymbolicLink(&dosName);

    if (DriverObject->DeviceObject != NULL) {
        IoDeleteDevice(DriverObject->DeviceObject);
    }
    g_DeviceObject = NULL;
}

NTSTATUS
DriverEntry(PDRIVER_OBJECT DriverObject, PUNICODE_STRING RegistryPath)
{
    NTSTATUS status;
    UNICODE_STRING ntName, dosName, sddl;

    UNREFERENCED_PARAMETER(RegistryPath);

    RtlZeroMemory(g_Mappings, sizeof(g_Mappings));
    ExInitializeFastMutex(&g_Lock);

    RtlInitUnicodeString(&ntName,  NVTUNEDRV_NT_DEVICE_NAME);
    RtlInitUnicodeString(&dosName, NVTUNEDRV_DOS_DEVICE_NAME);

    /*
     * SYSTEM and Administrators, full access. Nobody else gets a handle.
     * Without this the device object would inherit a default DACL that lets
     * any authenticated user open it -- which would hand every process on the
     * box a path to the GPU's registers.
     */
    RtlInitUnicodeString(&sddl, L"D:P(A;;GA;;;SY)(A;;GA;;;BA)");

    status = IoCreateDeviceSecure(DriverObject,
                                  0,
                                  &ntName,
                                  FILE_DEVICE_UNKNOWN,
                                  FILE_DEVICE_SECURE_OPEN,
                                  FALSE,
                                  &sddl,
                                  NULL,
                                  &g_DeviceObject);
    if (!NT_SUCCESS(status)) return status;

    status = IoCreateSymbolicLink(&dosName, &ntName);
    if (!NT_SUCCESS(status)) {
        IoDeleteDevice(g_DeviceObject);
        g_DeviceObject = NULL;
        return status;
    }

    DriverObject->MajorFunction[IRP_MJ_CREATE]         = NvtCreateClose;
    DriverObject->MajorFunction[IRP_MJ_CLOSE]          = NvtCreateClose;
    DriverObject->MajorFunction[IRP_MJ_DEVICE_CONTROL] = NvtDeviceControl;
    DriverObject->DriverUnload                         = NvtUnload;

    g_DeviceObject->Flags |= DO_BUFFERED_IO;
    g_DeviceObject->Flags &= ~DO_DEVICE_INITIALIZING;

    return STATUS_SUCCESS;
}
