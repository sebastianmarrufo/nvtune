/*
 * nvtunedrv ioctl interface.
 *
 * Shared verbatim between the kernel driver and the usermode tool. Plain C,
 * fixed-width types, no WDK or Win32 dependencies beyond the CTL_CODE macro,
 * so it can be included from either side (and from the Linux-hosted unit
 * tests, which is how the range allowlist gets exercised).
 */

#ifndef NVTUNEDRV_IOCTL_H
#define NVTUNEDRV_IOCTL_H

/*
 * Fixed-width types. In a kernel build (_KERNEL_MODE) ntddk.h has already set
 * up the WDK types and we must NOT pull in the usermode <stdint.h> -- doing so
 * drags vcruntime.h into the /kernel compile and collides with the WDK CRT
 * (the "_VCRUNTIME_DISABLED_WARNINGS" / "_CRT_STRINGIZE redefinition" errors).
 * Usermode and the Linux host tests use the real <stdint.h>.
 */
#if defined(_KERNEL_MODE) || defined(_NTDDK_) || defined(_WDMDDK_)
  typedef ULONG32 nvt_u32;
  typedef ULONG64 nvt_u64;
#else
  #include <stdint.h>
  typedef uint32_t nvt_u32;
  typedef uint64_t nvt_u64;
#endif

#define NVTUNEDRV_NT_DEVICE_NAME   L"\\Device\\nvtunedrv"
#define NVTUNEDRV_DOS_DEVICE_NAME  L"\\DosDevices\\nvtunedrv"
#define NVTUNEDRV_USER_PATH        "\\\\.\\nvtunedrv"
#define NVTUNEDRV_SERVICE_NAME     "nvtunedrv"

/* Bump when the struct layout below changes incompatibly. */
#define NVTUNEDRV_ABI_VERSION 1

#ifndef CTL_CODE
/* Mirrors the Win32 definition so this header stands alone under test. */
#define NVT_METHOD_BUFFERED 0
#define NVT_FILE_READ_ACCESS  0x0001
#define NVT_FILE_WRITE_ACCESS 0x0002
#define CTL_CODE(DeviceType, Function, Method, Access) \
    (((DeviceType) << 16) | ((Access) << 14) | ((Function) << 2) | (Method))
#define FILE_DEVICE_UNKNOWN 0x00000022
#define METHOD_BUFFERED     NVT_METHOD_BUFFERED
#define FILE_READ_ACCESS    NVT_FILE_READ_ACCESS
#define FILE_WRITE_ACCESS   NVT_FILE_WRITE_ACCESS
#endif

#define IOCTL_NVTUNE_GET_VERSION \
    CTL_CODE(FILE_DEVICE_UNKNOWN, 0x800, METHOD_BUFFERED, FILE_READ_ACCESS)
#define IOCTL_NVTUNE_MAP_BAR0 \
    CTL_CODE(FILE_DEVICE_UNKNOWN, 0x801, METHOD_BUFFERED, FILE_READ_ACCESS)
#define IOCTL_NVTUNE_UNMAP_BAR0 \
    CTL_CODE(FILE_DEVICE_UNKNOWN, 0x802, METHOD_BUFFERED, FILE_READ_ACCESS)
#define IOCTL_NVTUNE_READ32 \
    CTL_CODE(FILE_DEVICE_UNKNOWN, 0x803, METHOD_BUFFERED, FILE_READ_ACCESS)
#define IOCTL_NVTUNE_WRITE32 \
    CTL_CODE(FILE_DEVICE_UNKNOWN, 0x804, METHOD_BUFFERED, FILE_WRITE_ACCESS)
#define IOCTL_NVTUNE_READ_BLOCK \
    CTL_CODE(FILE_DEVICE_UNKNOWN, 0x805, METHOD_BUFFERED, FILE_READ_ACCESS)

/* Largest block read the driver will service, in dwords. */
#define NVTUNE_MAX_BLOCK_DWORDS 1024u

/* NVIDIA BAR0 has been 16 MiB on every part from Fermi through Ada. The
 * driver clamps to this regardless of what usermode asks for. */
#define NVTUNE_MAX_BAR0_LENGTH 0x1000000u
#define NVTUNE_MIN_BAR0_LENGTH 0x0400000u

#pragma pack(push, 4)

typedef struct _NVTUNE_VERSION_OUT {
    nvt_u32 abi_version;
    nvt_u32 driver_version;
    nvt_u32 max_devices;
    nvt_u32 reserved;
} NVTUNE_VERSION_OUT;

typedef struct _NVTUNE_MAP_IN {
    nvt_u32 bus;
    nvt_u32 device;
    nvt_u32 function;
    /* Advisory. Clamped to [NVTUNE_MIN_BAR0_LENGTH, NVTUNE_MAX_BAR0_LENGTH].
     * Zero means "use the maximum". The physical base is never taken from
     * usermode -- the driver reads it out of PCI config space itself. */
    nvt_u32 length_hint;
} NVTUNE_MAP_IN;

typedef struct _NVTUNE_MAP_OUT {
    nvt_u32 handle;
    nvt_u32 length;
    nvt_u64 phys_base;
    nvt_u32 vendor_id;
    nvt_u32 device_id;
    nvt_u32 subsystem_vendor_id;
    nvt_u32 subsystem_device_id;
    nvt_u32 class_code;      /* 24-bit class/subclass/progif */
    nvt_u32 revision;
} NVTUNE_MAP_OUT;

typedef struct _NVTUNE_HANDLE_IN {
    nvt_u32 handle;
    nvt_u32 reserved;
} NVTUNE_HANDLE_IN;

typedef struct _NVTUNE_READ_IN {
    nvt_u32 handle;
    nvt_u32 offset;
} NVTUNE_READ_IN;

typedef struct _NVTUNE_READ_OUT {
    nvt_u32 value;
    nvt_u32 reserved;
} NVTUNE_READ_OUT;

typedef struct _NVTUNE_WRITE_IN {
    nvt_u32 handle;
    nvt_u32 offset;
    nvt_u32 value;
    nvt_u32 reserved;
} NVTUNE_WRITE_IN;

typedef struct _NVTUNE_READ_BLOCK_IN {
    nvt_u32 handle;
    nvt_u32 offset;
    nvt_u32 count;      /* dwords, <= NVTUNE_MAX_BLOCK_DWORDS */
    nvt_u32 reserved;
} NVTUNE_READ_BLOCK_IN;

#pragma pack(pop)

#endif /* NVTUNEDRV_IOCTL_H */
