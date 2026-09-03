/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 Sebastian Marrufo */

/*
 * nvtunedrv MMIO range allowlist.
 *
 * This is the security boundary. A kernel driver that maps arbitrary physical
 * memory on behalf of usermode is a privilege-escalation primitive -- that is
 * precisely why WinRing0, RTCore64 and friends ended up on Microsoft's
 * vulnerable-driver blocklist. This driver instead exposes only the specific
 * register windows the tool actually needs, validated in kernel mode against
 * the table below. Usermode never supplies a physical address; it supplies a
 * PCI bus/device/function, and the driver reads the BAR out of config space
 * itself.
 *
 * Header-only, freestanding C so the same code compiles into the driver and
 * into the Linux-hosted unit tests. Do not add anything here that pulls in the
 * WDK or the CRT.
 */

/*
 * Fixed-width types.
 *
 * This header is compiled in three contexts, each of which supplies exact-width
 * integers differently:
 *
 *   - kernel driver (/kernel, _KERNEL_MODE): ntddk.h is already included and
 *     provides ULONG32/ULONG64/size_t. Pulling in the usermode
 *     <stdint.h>/<stddef.h> here would drag vcruntime.h into a kernel compile
 *     and collide with the WDK's km\crt -- the "expected ')'; found
 *     _VCRUNTIME_DISABLED_WARNINGS" / "_CRT_STRINGIZE macro redefinition"
 *     storm. So kernel-side we take the CRT headers out of the picture
 *     entirely and alias the WDK's own types.
 *   - Win32 usermode and the Linux host tests: plain <stdint.h>/<stddef.h>.
 *
 * Everything below uses the nvt_u32 / nvt_u64 / nvt_size aliases so the choice
 * lives in exactly one place.
 */
#ifndef NVTUNE_RANGES_H
#define NVTUNE_RANGES_H

#if defined(_KERNEL_MODE) || defined(_NTDDK_) || defined(_WDMDDK_)
  typedef ULONG32   nvt_u32;
  typedef ULONG64   nvt_u64;
  typedef SIZE_T    nvt_size;
#else
  #include <stddef.h>
  #include <stdint.h>
  typedef uint32_t  nvt_u32;
  typedef uint64_t  nvt_u64;
  typedef size_t    nvt_size;
#endif

typedef struct _NVT_RANGE {
    nvt_u32     start;
    nvt_u32     length;
    int         writable;
    const char* name;
} NVT_RANGE;

/*
 * Offsets are BAR0-relative.
 *
 * Read-only entries are topology/identification registers the tool needs to
 * identify the chip and enumerate partitions. The read-write entries are the
 * FBPA memory-controller apertures documented in NVIDIA's gp100-fbpa.txt --
 * the whole point of the tool.
 *
 * 0x001850 (PBUS ROM access control) is writable because reading the VBIOS
 * mirror at PROM requires clearing it first; it is restored immediately after.
 * It is a single dword and controls nothing but ROM visibility.
 */
static const NVT_RANGE kNvtRanges[] = {
    /* --- identification and topology, read-only --------------------- */
    { 0x000000u, 0x000004u, 0, "PMC_BOOT_0" },
    { 0x021C14u, 0x000004u, 0, "FUSE_STATUS_OPT_FBIO" },
    { 0x021D70u, 0x000040u, 0, "FUSE_STATUS_OPT_ROP_L2_FBP" },
    { 0x02243Cu, 0x000004u, 0, "PTOP_SCAL_NUM_FBPAS" },
    { 0x022458u, 0x000004u, 0, "PTOP_SCAL_NUM_FBPA_PER_FBP" },
    { 0x100800u, 0x000004u, 0, "PFB_FBHUB_NUM_ACTIVE_FBPS" },

    /* --- VBIOS mirror ------------------------------------------------ */
    { 0x001850u, 0x000004u, 1, "PBUS_ROM_ACCESS" },
    { 0x300000u, 0x020000u, 0, "PROM (VBIOS mirror)" },

    /* --- clock tree (PBUS/PCLOCK), read-only ------------------------- */
    /* PLL coefficients and dividers for gpc/xbar/system/video/rop domains.
     * Read-only: this tool reads and decodes clocks, it does not reprogram
     * them (core-clock writes are a separate, opt-in future capability). */
    { 0x132000u, 0x001000u, 0, "PFB memory PLL region (clocks, RO)" },
    { 0x137000u, 0x001000u, 0, "PCLOCK PLL/divider region (clocks, RO)" },

#if defined(NVTUNE_DISCOVERY)
    /*
     * Discovery mode: expose the FULL FBPA apertures READ-ONLY, so
     * `probe --watch` can survey registers beyond the documented timing block
     * (0x290..0x2A7) -- e.g. hunting the Turing shadow/commit registers.
     *
     * This is READ ONLY on purpose. It widens what you can observe, never what
     * you can write; the writable entries below are unchanged. Build the driver
     * with NVTUNE_DISCOVERY defined only while reverse-engineering, then rebuild
     * without it for normal use. The whole 0x9A0000 window overlaps the
     * writable broadcast entry below, which is fine: NvtRangeAllowed grants a
     * write only if the containing range is writable, and it checks every entry
     * -- a read here plus a write there both resolve correctly.
     */
    { 0x100000u, 0x001000u, 0, "PFB_FBPA broadcast region (discovery, RO)" },
    { 0x110000u, 0x010000u, 0, "PFB_FBPA unicast region legacy (discovery, RO)" },
    { 0x900000u, 0x0A0000u, 0, "PFB_FBPA unicast+MC region (discovery, RO)" },
    { 0x9A0000u, 0x004000u, 0, "PFB_FBPA broadcast full window (discovery, RO)" },
#endif

    /* --- FBPA apertures, pre-Pascal (Fermi / Kepler / Maxwell) ------- */
    { 0x10F000u, 0x001000u, 1, "PFB_FBPA broadcast (legacy)" },
    { 0x110000u, 0x008000u, 1, "PFB_FBPA[0..7] unicast (legacy)" },
    { 0x11D000u, 0x003000u, 1, "PFB_FBPA_MC[0..2] (legacy)" },

    /* --- FBPA apertures, Pascal and later ---------------------------- */
    { 0x900000u, 0x040000u, 1, "PFB_FBPA[0..15] unicast" },
    { 0x980000u, 0x00C000u, 1, "PFB_FBPA_MC[0..2]" },
    { 0x9A0000u, 0x004000u, 1, "PFB_FBPA broadcast" },
};

#define NVT_RANGE_COUNT (sizeof(kNvtRanges) / sizeof(kNvtRanges[0]))

/*
 * Is [offset, offset+length) entirely inside one permitted range?
 *
 * Deliberately requires containment in a *single* entry rather than accepting
 * a span that happens to be covered by two adjacent ones -- adjacency in this
 * table is incidental, not a guarantee.
 *
 * Returns 1 if allowed, 0 otherwise. Never traps: all arithmetic is done in
 * 64-bit to make overflow impossible.
 */
#if defined(_MSC_VER)
#pragma warning(push)
#pragma warning(disable : 4505)
#elif defined(__GNUC__)
__attribute__((unused))
#endif
static int NvtRangeAllowed(nvt_u32 offset, nvt_u32 length, int for_write,
                           nvt_u32 bar_length)
{
    nvt_u64 end;
    nvt_size i;

    if (length == 0u) return 0;
    if ((offset & 3u) != 0u) return 0;      /* must be dword aligned */
    if ((length & 3u) != 0u) return 0;

    end = (nvt_u64)offset + (nvt_u64)length;
    if (end > (nvt_u64)bar_length) return 0;

    /*
     * Grant if SOME single entry contains the whole span with sufficient
     * access. We must scan all entries, not stop at the first container: with
     * overlapping ranges (e.g. a read-only discovery window laid over the
     * writable broadcast aperture) the first container might be read-only while
     * a later one grants the write. Stopping early would wrongly deny a legal
     * write. A read needs any containing entry; a write needs a containing
     * entry whose writable flag is set.
     */
    for (i = 0; i < NVT_RANGE_COUNT; ++i) {
        nvt_u64 rstart = (nvt_u64)kNvtRanges[i].start;
        nvt_u64 rend   = rstart + (nvt_u64)kNvtRanges[i].length;
        if ((nvt_u64)offset < rstart) continue;
        if (end > rend) continue;
        if (for_write && !kNvtRanges[i].writable) continue;
        return 1;
    }
    return 0;
}
#if defined(_MSC_VER)
#pragma warning(pop)
#endif

/* Name of the range containing `offset`, or 0. Diagnostics only.
 *
 * A translation unit that includes this header but never calls this would draw
 * C4505 (unreferenced local function) under MSVC /W4 /WX, and -Wunused-function
 * under gcc/clang. It is genuinely used by the unit tests; suppress the warning
 * for includers that don't call it rather than forcing a bogus reference. */
#if defined(_MSC_VER)
#pragma warning(push)
#pragma warning(disable : 4505)
#elif defined(__GNUC__)
__attribute__((unused))
#endif
static const char* NvtRangeName(nvt_u32 offset)
{
    nvt_size i;
    for (i = 0; i < NVT_RANGE_COUNT; ++i) {
        if (offset >= kNvtRanges[i].start &&
            offset <  kNvtRanges[i].start + kNvtRanges[i].length) {
            return kNvtRanges[i].name;
        }
    }
    return 0;
}
#if defined(_MSC_VER)
#pragma warning(pop)
#endif

#endif /* NVTUNE_RANGES_H */
