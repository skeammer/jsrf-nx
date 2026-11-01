/**
 * Xbox Memory Layout Implementation
 *
 * Maps the XBE data sections to their expected virtual addresses on Windows.
 * This is critical for the recompiled code which references globals by
 * absolute address (e.g., mov eax, [0x004D532C]).
 *
 * Implementation:
 * 1. VirtualAlloc a contiguous region at XBOX_BASE_ADDRESS
 * 2. Copy .rdata and initialized .data from the XBE
 * 3. Zero-fill the BSS region
 * 4. Set memory protection (read-only for .rdata)
 */

#include "xbox_memory_layout.h"
#include "kernel.h"
#include <stdio.h>
#if !defined(_WIN32)
#include <sched.h>
#include <time.h>
#endif
#include <stdlib.h>
#include <string.h>
#include <setjmp.h>
#if !defined(_WIN32)
#include <unistd.h>   /* _exit */
#endif
#if defined(__linux__)
#include <sys/mman.h>
#endif

/* XBE header field offsets (per xboxdevwiki.net/Xbe) */
#define XBE_MAGIC_OFFSET        0x0000
#define XBE_BASE_ADDR_OFFSET    0x0104
#define XBE_HEADER_SIZE_OFFSET  0x0108
#define XBE_SECTION_COUNT_OFFSET 0x011C
#define XBE_SECTION_HEADERS_OFFSET 0x0120
#define XBE_TLS_ADDR_OFFSET     0x012C

/* XBE section header layout (56 bytes each) */
#define SECTHDR_FLAGS       0x00
#define SECTHDR_VA          0x04
#define SECTHDR_VSIZE       0x08
#define SECTHDR_RAW_OFFSET  0x0C
#define SECTHDR_RAW_SIZE    0x10
#define SECTHDR_NAME_ADDR   0x14
#define SECTHDR_SIZE        56

static void *g_memory_base = NULL;
static size_t g_memory_size = 0;
static ptrdiff_t g_memory_offset = 0;  /* actual_base - XBOX_BASE_ADDRESS */

/* Actual mapped RAM for this run; see the header. Default retail 64 MB. */
size_t g_xbox_total_ram = XBOX_TOTAL_RAM;
size_t g_xbox_map_size = 0;   /* 0 = same as RAM */

void xbox_SetTotalRam(size_t bytes)
{
    g_xbox_total_ram = bytes;
}


void xbox_SetMapSize(size_t bytes)
{
    g_xbox_map_size = bytes;
}

void xbox_EnableSeparateReserveSpace(size_t bytes)
{
    if (bytes > 0)
        xbox_SetMapSize(g_xbox_total_ram + bytes);
}

BOOL xbox_EnablePhysicalHeapAlias(void)
{
    return TRUE;
}

/* File mapping handle for the Xbox memory region.
 * Using CreateFileMapping + MapViewOfFileEx allows mirror views to alias
 * the same physical pages as the base region, so writes to mirror addresses
 * (which wrap modulo 64 MB on real Xbox hardware) correctly modify the
 * underlying data. */
static HANDLE g_mapping_handle = NULL;

/* Mirror view pointers for cleanup */
static void *g_mirror_views[XBOX_NUM_MIRRORS] = {0};

/* The base view and its 28 mirrors occupy one contiguous span. Reserving that
 * span up front is what makes the mirrors placeable at all: each one sits at
 * base + N * 64 MB, and on a host that chose the base for us, those addresses
 * run through whatever the loader already owns. Placing them one at a time
 * means ~3 of 28 collide, and *which* three changes with ASLR. Claiming the
 * whole range first, then carving views out of ground we hold, removes the
 * question. */
static void *g_span_base = NULL;
static size_t g_span_size = 0;
static void *g_tiled_view = NULL;

/* Contiguous / physical memory window (see MemoryLayoutInit).
 * XBOX_CONTIG_BASE / XBOX_CONTIG_SIZE come from kernel.h - the bridges need
 * the same numbers for MmClaimGpuInstanceMemory. */
static void *g_contig_memory = NULL;

/* NV2A GPU register aperture (see MemoryLayoutInit). Backed as plain RAM so
 * that D3D8 code linked into the title can poke it without faulting. */
#define XBOX_NV2A_BASE 0xFD000000u
#define XBOX_NV2A_SIZE (16u * 1024u * 1024u)
static void *g_nv2a_memory = NULL;

/* MCPX southbridge register span: APU 0xFE800000 through NIC 0xFEF00000. */
#define XBOX_MCPX_BASE 0xFE800000u
#define XBOX_MCPX_SIZE (8u * 1024u * 1024u)
static void *g_mcpx_memory = NULL;

/* Flash ROM. The console's 256 KB flash is mirrored through the top of the
 * address space, and the MCPX span above stops one page short of it -- so a
 * title that touches it faulted on an address that is perfectly ordinary on
 * hardware.
 *
 * The Xbox Dashboard does, from two directions at once: its XIP workers hash
 * 64 KB from 0xFF000000 (it verifies archives against digests), and its render
 * path writes to 0xFF000040. Both are hard faults today, and they kill the
 * process a few dozen lines after its first frame clears.
 *
 * Plain memory, like the other two apertures, and mapped for the same stated
 * reason: a read of zero is survivable, a fault is not. Zeros are not the
 * console's BIOS, so a digest taken over this will not match one taken over
 * real flash -- that is a separate question from whether the access should
 * fault, and this is the half that has an obviously right answer. */
#define XBOX_FLASH_BASE 0xFF000000u
#define XBOX_FLASH_SIZE (1u * 1024u * 1024u)
static void *g_flash_memory = NULL;

/* Two small, genuinely unbacked gaps sit between the apertures above: NV2A
 * ends at 0xFE000000 and MCPX starts at 0xFE800000 (8 MB between them), and
 * Flash ends at 0xFF100000 with nothing mapped from there to the top of the
 * 32-bit address space (~15 MB). Neither is a real device span -- they are
 * just the arithmetic remainder of laying the four apertures out at their
 * real hardware addresses.
 *
 * A wild guest pointer derived from (or near) any of NV2A/MCPX/Flash is one
 * bad add away from landing in one of these, and on a host with no fault
 * handling (Switch) that is a hard crash rather than the garbage read/write
 * it would be on hardware. A JSRF crash log (exception 257, guest VA
 * 0xFE3941B0) landed in the first gap; the JSRF Mac port's own unresolved
 * wild-pointer investigation separately recorded a crash in the second
 * (0xFF100000-0xFFFFFFFF). Backing both as plain zeroed memory is the same
 * mitigation already applied to NV2A/MCPX/Flash, for the same reason: a read
 * or write that goes nowhere useful is survivable, a fault is not. This does
 * not fix whatever still computes the wild pointer in the first place. */
#define XBOX_NV2A_MCPX_GAP_BASE 0xFE000000u
#define XBOX_NV2A_MCPX_GAP_SIZE (8u * 1024u * 1024u)
static void *g_nv2a_mcpx_gap_memory = NULL;

#define XBOX_POST_FLASH_GAP_BASE 0xFF100000u
#define XBOX_POST_FLASH_GAP_SIZE (15u * 1024u * 1024u)
static void *g_post_flash_gap_memory = NULL;
/* The contiguous window's backing section. It is a file mapping rather than
 * plain committed memory for one reason: the tiled aperture has to be a
 * second view of the very same bytes, and only a mapping can be mapped
 * twice. See the tiled aperture below for why that matters.
 */
static HANDLE g_contig_mapping = NULL;
/* How much of the tiled aperture can exist.
 *
 * Two ceilings, both below the mapped RAM size once that is large:
 *
 *   - it starts at 0xF0000000 in a 32-bit guest address space, so it can
 *     never reach past 0x100000000; and
 *   - the NV2A register aperture sits at 0xFD000000, which is where the
 *     window really ends on hardware.
 *
 * Asking for the full RAM size overlapped both and MapViewOfFileEx failed
 * with ERROR_INVALID_ADDRESS -- a warning at startup and then a fault on the
 * title's first surface write, with nothing connecting the two. */
/* How much guest address space is mapped.
 *
 * For anything that dereferences an address it read out of guest memory --
 * a descriptor pointer a device model follows, say. Those are attacker-ish
 * input in the only sense that matters here: the title can leave one
 * uninitialised, and 0xCCCCCCCC dereferenced is a crash in the runtime
 * rather than a fault the title would have taken. */
size_t xbox_GetMappedSize(void)
{
    return g_memory_size;
}

static size_t xbox_TiledApertureSize(void)
{
    uint64_t end = XBOX_NV2A_BASE < 0x100000000ULL
                 ? XBOX_NV2A_BASE : 0x100000000ULL;
    size_t max = (size_t)(end - XBOX_TILED_BASE);
    return g_memory_size < max ? g_memory_size : max;
}

/* RECOMP_NO_ALIAS=1: lay memory out the way a host that cannot alias it
 * (Switch) has to -- no RAM mirrors, a tiled aperture of its own -- so a
 * desktop run reproduces what the console sees. */
static int host_no_alias(void)
{
    const char *e = getenv("RECOMP_NO_ALIAS");
    return e && *e && *e != '0';
}

static HANDLE g_nv2a_ack_thread = NULL;
static volatile LONG g_nv2a_ack_stop = 0;

/*
 * NV2A busy-bit acknowledgement.
 *
 * D3D8 talks to the GPU through set-a-bit / wait-for-hardware-to-clear-it
 * handshakes. Against plain RAM the bit is set and nothing ever clears it, so
 * the title spins forever. Halo hangs in the push-buffer kick at 0x001EF930:
 *
 *     mov  [eax+0x100410], edx     ; set 0x10000
 *   L: test [eax+0x100410], 0x10000
 *     jne  L                       ; wait for the GPU
 *
 * Clearing those bits from a thread is not a hack around the handshake, it is
 * the handshake: on hardware the GPU clears them asynchronously, which is
 * exactly what this does. Work that would have been submitted is being done by
 * the D3D11 layer instead, so acknowledging immediately is honest.
 *
 * Only registers listed here are touched. Blanket-zeroing the aperture would
 * also wipe registers holding real state.
 *
 * ponytail: table-driven, extend as more handshakes turn up. A spin on a bit
 * that is not listed still hangs -- run the title and the watchdog sample will
 * name the register.
 */
static const struct { uint32_t offset; uint32_t busy_mask; } NV2A_ACK[] = {
    { 0x100410, 0x00010000u },  /* PFB flush kick, Halo 0x001EF930 */

    /* Interrupt status registers. These are write-1-to-clear on hardware, so
     * an ISR "clearing" one writes the pending bit back -- against plain RAM
     * that sets it instead, the interrupt stays pending forever, and the
     * service routine re-enters until the stack is gone. Halo dies exactly
     * that way: CMiniport::ServiceGrInterrupt writes 0x1000 to PGRAPH_INTR to
     * acknowledge, reads it back still pending, and recurses into a native
     * stack overflow.
     *
     * Holding them at zero is correct rather than convenient: nothing here
     * ever raises a GPU interrupt, so "none pending" is the truth.
     *
     * JSRF-NX: do not carve VBLANK / the PCRTC summary out of these. The
     * reference port can, because it traps the page and models PCRTC_INTR_0
     * as write-1-to-clear. Here it is plain RAM: D3D's init and its vblank
     * handler acknowledge by writing 1, which then stuck forever, and no
     * vblank was ever delivered again (every D3D thread parked in
     * BlockUntilVerticalBlank, 0x0018CE50). The vblank hold below already
     * keeps a raised vblank alive until the DPC has read it. */
    { 0x000100, 0xFFFFFFFFu },  /* PMC_INTR_0    */
    { 0x001100, 0xFFFFFFFFu },  /* PBUS_INTR_0   */
    { 0x002100, 0xFFFFFFFFu },  /* PFIFO_INTR_0  */
    { 0x400100, 0xFFFFFFFFu },  /* PGRAPH_INTR   */
    { 0x600100, 0xFFFFFFFFu },  /* PCRTC_INTR_0  */
};

/*
 * Bits that must always read as SET. The mirror image of the table above:
 * where an interrupt-pending bit is false because nothing raises interrupts,
 * a queue-empty bit is true because nothing is queued.
 *
 * Halo's CMiniport::TilingUpdateIdle spins until the PFIFO caches report
 * empty (0x001F5CD1). Zeroed RAM says "not empty" forever, so tile setup
 * during CDevice::InitializeFrameBuffers never completes.
 *
 * Note 0x003220 is deliberately absent -- that one exits on the bit being
 * CLEAR, which zeroed memory already gives.
 */
static const struct { uint32_t offset; uint32_t idle_mask; } NV2A_IDLE[] = {
    { 0x002400, 0x00000010u },  /* PFIFO_RUNOUT_STATUS  LOW_MARK (empty) */
    { 0x003214, 0x00000010u },  /* PFIFO_CACHE1_STATUS  LOW_MARK (empty) */
};

/*
 * PFIFO channel DMA pointers. Software writes DMA_PUT and spins until the GPU
 * advances DMA_GET to match -- "you have consumed everything I submitted".
 * Halo's wait is at 0x001F3948:
 *
 *   L: call BusyLoop
 *      ecx = [[dev+0x2304] + 0x44]   ; DMA_GET
 *      edx = [dev]                   ; DMA_PUT
 *      test (edx ^ ecx), 0xfffffff
 *      jne L
 *
 * [dev+0x2304] is 0xFD800000, so the channel's USER area sits at aperture
 * offset 0x800000 and the two pointers are at +0x40 / +0x44. Copying PUT to
 * GET is the acknowledgement; the commands are not executed from the push
 * buffer here -- the D3D11 layer draws -- so reporting them consumed is the
 * truthful answer.
 *
 * This was written once, removed, and restored. It was removed because
 * [dev+0x2304] read as 0x0080F7FF, i.e. no register to acknowledge -- but that
 * garbage was a downstream symptom of ordinal 47 having no stdcall arg size,
 * which walked esp 8 bytes off and made D3D initialise the DMA channel with
 * `this` = 1. With that fixed the pointer is correct and so is this.
 */
#define NV2A_USER_DMA_PUT 0x800040u
#define NV2A_USER_DMA_GET 0x800044u

/*
 * The same channel's pointers on the PFIFO side of the aperture.
 *
 * The USER area above is the window software writes through; PFIFO holds the
 * engine's own copy, and D3D reads it back on the path where the USER pointer
 * is not usable. The title's channel context switch saves and restores all
 * four of these as one block (DDS9 0x002FE2xx), which is what identifies them:
 *
 *   0x3240 CACHE1_DMA_PUT          0x3248 CACHE1_REF
 *   0x3244 CACHE1_DMA_GET          0x324C CACHE1_DMA_SUBROUTINE
 *
 * DMA_SUBROUTINE matters because it is not a flag: bits 31:1 are the offset
 * the engine returns to when a pushbuffer subroutine ends, and bit 0 says
 * whether one is running. DDS9's free-space calculation (sub_002F6CC0) reads
 * the USER GET first and falls back to this register's return offset when
 * that lands outside the ring -- i.e. "the GPU is off in a subroutine, so ask
 * where it will come back to". Zeroed RAM answers 0 to both, which is below
 * the ring base, and the free-space subtraction then goes negative and is
 * clamped to zero. The reserve wants 0x2000 bytes, gets 0, and spins.
 *
 * Not acknowledged here, only reported. DDS9 reads the USER pair and never
 * reaches the fallback, so every value in this block is zero for the one
 * title that was traced -- mirroring PUT to GET would be a guess dressed as
 * a handshake. The watchdog prints them so the next title to spin here is
 * diagnosed from data instead.
 */
#define NV2A_PFIFO_DMA_PUT        0x003240u
#define NV2A_PFIFO_DMA_GET        0x003244u
#define NV2A_PFIFO_REF            0x003248u
#define NV2A_PFIFO_DMA_SUBROUTINE 0x00324Cu

/*
 * Free-running counters in the MCPX aperture.
 *
 * Some hardware registers are clocks, not flags: software reads them and waits
 * until the value passes a target. Against zeroed RAM the value never moves and
 * the wait is forever. DirectSound's CMcpxCore::SetupVoiceProcessor spins on
 * the APU sample counter at 0xFE820010 exactly this way, which is where Halo
 * stopped once input initialisation started working.
 *
 * Ticking it is the honest model: on hardware this counter advances on its own
 * whether or not anything is listening.
 *
 * ponytail: the rate is "as fast as this thread loops", not 48 kHz. Nothing
 * paces audio off it yet. Derive it from a real clock if timing starts to
 * matter.
 */
/* AC'97 bus-master reset.
 *
 * Each bus-master channel has a control byte at NABM + 0x0B whose bit 1, RR
 * ("reset registers"), software sets and then waits for the controller to
 * clear. DDS9 does it inside DirectSoundCreate. The NABM block is plain
 * memory here, so the ack thread clears RR in every channel it finds it set
 * in (RECOMP_AC97_READY=plain). That wins against a wait that re-reads the
 * register. Against one whose load the compiler hoisted out of the loop --
 * DDS9's reads it once -- only a write trap would, and the Switch has no
 * fault handling to build one on. */
#define AC97_NABM_OFFSET  0x400000u   /* 0xFEC00000 within the MCPX aperture */
#define AC97_RR           0x02u

/* The NABM block, cleared from the ack thread (RECOMP_AC97_READY). */
static void *g_ac97_poll_page = NULL;

static void ac97_clear_reset_bits_at(void *page)
{
    /* Every bus-master channel, not the three a PC AC'97 has.
     *
     * The generic controller has PCM In, PCM Out and Mic In at NABM +0x00,
     * +0x10 and +0x20; the MCPX has more, and DDS9 walks a table of channel
     * offsets rather than naming them. It reset the channel at +0x00 first
     * and then one at +0x60 -- which a three-entry list did not cover, so it
     * spun on the second exactly as it had on the first. Sweeping the whole
     * NABM block is both simpler and right: +0x0B is the control byte of
     * whatever channel lives there, and RR is the same bit in all of them. */
    uint32_t off;

    for (off = 0x10B; off < 0x180; off += 0x10) {
        volatile uint8_t *r = (volatile uint8_t *)((char *)page + off);
        if (*r & AC97_RR)
            *r = (uint8_t)(*r & ~AC97_RR);
    }
}

/*
 * Command words the DSP stub completes instantly. A bring-up probe, not a
 * model.
 *
 * The GP and EP DSPs in src/apu are stubs -- effects bypass, encode
 * passthrough -- and a stub that never completes is worse for a title than
 * one that completes at once, because the title cannot get past it at all.
 * DDS9 posts a command and waits for the DSP to clear it:
 *
 *     mov  [ebx], 3            ; ebx = scratch + 0x810
 *   L: cmp  dword [ebx], 0
 *     jne  L
 *
 * Unlike the AC'97 reset bit, this one re-reads every iteration, so clearing
 * it from here is a race this side wins rather than loses.
 *
 * Why an environment variable rather than a registration API: the word's
 * address is reached as *(*(*(this+8)+0x10)) + 0x810 from an object with no
 * global anchor, and the APU never sees that address directly -- it reaches
 * the block through the scatter-gather descriptors the title programmed. So
 * the honest fix is for the GP stub to follow those descriptors, which is DSP
 * work. This exists to answer, in one run and without that work, whether
 * completing the command is in fact all the title is waiting for.
 *
 * RECOMP_DSP_ACK=0x804A8810[,...] -- up to 8 words, zeroed whenever non-zero.
 */
#define XBOX_MAX_DSP_ACK 8
static uint32_t g_dsp_ack[XBOX_MAX_DSP_ACK];
static int g_dsp_ack_count = 0;

/* ── Device-aware accesses (MMIO_RD/MMIO_WR in recomp_types.h) ──────────
 *
 * Functions lifted with tools.recomp --mmio-sections send every access at or
 * above 0xFD000000 here instead of touching memory. A device model registers
 * the range it answers; anything else in the apertures is the plain memory
 * they are backed with, exactly what an unrewritten access would have seen.
 * No fault handling is involved, so this works the same on every host. */
#define XBOX_MAX_MMIO_HANDLERS 8

typedef struct {
    uint32_t first, end;                /* [first, end) */
    uint32_t (*read)(void *opaque, uint32_t offset, unsigned size);
    void     (*write)(void *opaque, uint32_t offset, uint32_t val, unsigned size);
    void     *opaque;
} MmioDevice;

static MmioDevice g_mmio[XBOX_MAX_MMIO_HANDLERS];
static int        g_mmio_count;

int xbox_MmioRegister(uint32_t lo, uint32_t hi,
                      uint32_t (*read)(void *, uint32_t, unsigned),
                      void (*write)(void *, uint32_t, uint32_t, unsigned),
                      void *opaque)
{
    MmioDevice *device;

    if (g_mmio_count == XBOX_MAX_MMIO_HANDLERS)
        return -1;
    device = &g_mmio[g_mmio_count++];
    device->first = lo;
    device->end = hi;
    device->read = read;
    device->write = write;
    device->opaque = opaque;
    fprintf(stderr, "  MMIO: 0x%08X..0x%08X routed to a device model\n", lo, hi);
    return 0;
}

/* The device answering `va`, or NULL for plain aperture memory. */
static const MmioDevice *mmio_device(uint32_t va)
{
    int i;

    for (i = 0; i < g_mmio_count; i++)
        if (va - g_mmio[i].first < g_mmio[i].end - g_mmio[i].first)
            return &g_mmio[i];
    return NULL;
}

uint32_t xbox_mmio_read(uint32_t va, unsigned size)
{
    const MmioDevice *device = mmio_device(va);
    volatile uint8_t *memory;

    if (device != NULL)
        return device->read(device->opaque, va - device->first, size);
    memory = (volatile uint8_t *)((uintptr_t)va + g_memory_offset);
    switch (size) {
    case 1:  return *memory;
    case 2:  return *(volatile uint16_t *)memory;
    default: return *(volatile uint32_t *)memory;
    }
}

void xbox_mmio_write(uint32_t va, uint32_t val, unsigned size)
{
    const MmioDevice *device = mmio_device(va);
    volatile uint8_t *memory;

    if (device != NULL) {
        device->write(device->opaque, va - device->first, val, size);
        return;
    }
    memory = (volatile uint8_t *)((uintptr_t)va + g_memory_offset);
    switch (size) {
    case 1:  *memory = (uint8_t)val; break;
    case 2:  *(volatile uint16_t *)memory = (uint16_t)val; break;
    default: *(volatile uint32_t *)memory = val; break;
    }
}

/* The same, registered by a game project that knows where its title's
 * command word lives (it is usually reached through the DirectSound object,
 * so only the title's code can find it). Idempotent. Returns 0, or -1 when
 * the table is full. */
int xbox_ApuDspAckWord(uint32_t va)
{
    int i;

    for (i = 0; i < g_dsp_ack_count; i++)
        if (g_dsp_ack[i] == va)
            return 0;                   /* already known */
    if (g_dsp_ack_count == XBOX_MAX_DSP_ACK)
        return -1;
    g_dsp_ack[g_dsp_ack_count] = va;
    g_dsp_ack_count++;
    fprintf(stderr, "  DSP ack: command word 0x%08X will be completed at once\n", va);
    return 0;
}

static void dsp_ack_init(void)
{
    const char *spec = getenv("RECOMP_DSP_ACK");
    char buf[128], *q, *end;

    if (!spec || !*spec)
        return;
    strncpy(buf, spec, sizeof buf - 1);
    buf[sizeof buf - 1] = 0;
    for (q = buf; *q && g_dsp_ack_count < XBOX_MAX_DSP_ACK; ) {
        unsigned long va = strtoul(q, &end, 0);
        if (end == q)
            break;
        g_dsp_ack[g_dsp_ack_count++] = (uint32_t)va;
        q = (*end == ',') ? end + 1 : end;
    }
    if (g_dsp_ack_count)
        fprintf(stderr, "  DSP ack: %d command word(s) will be completed"
                        " immediately\n", g_dsp_ack_count);
}

static int fence_readable(uint32_t va, uint32_t bytes);  /* defined below */

/* Hold a guest global at a value. A bring-up probe, like the DSP ack.
 *
 * There is exactly one reason this exists: to answer "is the title waiting on
 * this?" in one run, before spending a day making the thing that would set it
 * honestly. It is not a fix and must not be mistaken for one -- whatever it
 * holds, nothing in the guest is producing, so the state it fakes is
 * inconsistent with everything downstream of it by construction.
 *
 * RECOMP_POKE=0x30F234:1,0x30F238:1
 */
#define XBOX_MAX_POKE 8
static struct { uint32_t va, value; } g_poke[XBOX_MAX_POKE];
static int g_poke_count;

static void poke_init(void)
{
    const char *spec = getenv("RECOMP_POKE");
    char buf[192], *q, *end;

    if (!spec || !*spec)
        return;
    strncpy(buf, spec, sizeof buf - 1);
    buf[sizeof buf - 1] = 0;
    for (q = buf; *q && g_poke_count < XBOX_MAX_POKE; ) {
        unsigned long va = strtoul(q, &end, 0);
        unsigned long val = 0;
        if (end == q)
            break;
        if (*end == ':')
            val = strtoul(end + 1, &end, 0);
        g_poke[g_poke_count].va    = (uint32_t)va;
        g_poke[g_poke_count].value = (uint32_t)val;
        g_poke_count++;
        q = (*end == ',') ? end + 1 : end;
    }
    if (g_poke_count)
        fprintf(stderr, "  POKE: holding %d guest global(s) -- bring-up probe,"
                        " not a fix\n", g_poke_count);
}

static void poke_tick(void)
{
    int i;

    for (i = 0; i < g_poke_count; i++) {
        if (!fence_readable(g_poke[i].va, 4))
            continue;
        {
            volatile uint32_t *w = (volatile uint32_t *)
                ((uintptr_t)g_poke[i].va + g_memory_offset);
            if (*w != g_poke[i].value)
                *w = g_poke[i].value;
        }
    }
}

static void dsp_ack_tick(void)
{
    int i;

    for (i = 0; i < g_dsp_ack_count; i++) {
        /* fence_readable rather than a bare bounds test: these land in the
         * contiguous window, which a plain size check against the main map
         * rejects. */
        if (!fence_readable(g_dsp_ack[i], 4))
            continue;
        {
            volatile uint32_t *w = (volatile uint32_t *)
                ((uintptr_t)g_dsp_ack[i] + g_memory_offset);
            if (*w)
                *w = 0;
        }
    }
}

static const uint32_t MCPX_COUNTERS[] = {
    0x020010,   /* APU GP sample counter, DirectSound SetupVoiceProcessor */
};

static void *g_mcpx_regs = NULL;

/*
 * GPU completion fences the title waits on in guest memory rather than in the
 * aperture. See xbox_Nv2aMirrorFence in the header for why this is the same
 * acknowledgement the NV2A_ACK table makes, and why the address has to be
 * followed through the device struct instead of being a constant.
 */
#define XBOX_MAX_FENCE_MIRRORS 4

static struct {
    uint32_t device_ptr_va;
    uint32_t put_off;
    uint32_t get_ptr_off;
} g_fence_mirrors[XBOX_MAX_FENCE_MIRRORS];
static int g_fence_mirror_count = 0;

int xbox_Nv2aMirrorFence(uint32_t device_ptr_va,
                         uint32_t put_off, uint32_t get_ptr_off)
{
    if (g_fence_mirror_count >= XBOX_MAX_FENCE_MIRRORS)
        return -1;
    g_fence_mirrors[g_fence_mirror_count].device_ptr_va = device_ptr_va;
    g_fence_mirrors[g_fence_mirror_count].put_off = put_off;
    g_fence_mirrors[g_fence_mirror_count].get_ptr_off = get_ptr_off;
    g_fence_mirror_count++;
    fprintf(stderr, "  NV2A fence mirror: device at 0x%08X,"
            " PUT +0x%X -> *(GET +0x%X)\n",
            device_ptr_va, put_off, get_ptr_off);
    return 0;
}
/* A guest address is usable only once the window is mapped and it lands
 * inside it; the chain is followed fresh every poll because the title may not
 * have built it yet. */
static int fence_readable(uint32_t va, uint32_t bytes)
{
    /* Page zero is unmapped, so the bound is the first mapped page rather than
     * just "not null": the device pointer is zero until the title creates the
     * device, and this thread polls from before that. Rejecting only 0 let
     * dev + get_ptr_off through as 0x34 and faulted on the very first tick. */
    if (g_memory_base == NULL || va < XBOX_FS_BASE)
        return 0;
    /* The contiguous window is mapped separately and sits far above the main
     * range, so a size check against g_memory_size rejects it. The fence a
     * title waits on is exactly the kind of block that lives there --
     * MmAllocateContiguousMemory is where a GPU-written semaphore comes
     * from -- so a chain ending in that window has to be followed, not
     * discarded. */
    if (va >= XBOX_CONTIG_BASE
            && (uint64_t)va + bytes <= (uint64_t)XBOX_CONTIG_BASE + XBOX_CONTIG_SIZE)
        return g_contig_memory != NULL;
    return (size_t)va + bytes <= g_memory_size;
}

/*
 * Two counters inside the device, one of which the GPU owns.
 *
 * D3D's swap throttle is a pair: the title bumps "frames submitted" itself and
 * waits for "frames completed", which on hardware only the GPU moves. The Xbox
 * dashboard's is exactly that, at guest 0x000AF121 --
 *
 *     eax = [esi+0x2518]        ; completed
 *     ecx = [esi+0x2B60]        ; submitted
 *     ecx = ecx - eax
 *     if (ecx < 2) proceed      ; else spin on a 400-iteration delay loop
 *
 * -- and with nothing moving completed it spins there forever once two frames
 * are outstanding. That delay loop was 99.8 million of the dashboard's calls,
 * against 35 thousand for the next function down.
 *
 * This differs from xbox_Nv2aFrameCounter, which advances a counter on a 60 Hz
 * clock, in the way that matters for this pair: a free-running counter can
 * pass submitted, and then submitted - completed underflows to about four
 * billion, which is >= 2, and the spin never ends again. Mirroring cannot do
 * that, because completed is only ever whatever submitted already is.
 *
 * It is also simply true here. The pushbuffer is executed at submit, so by the
 * time the title asks whether the frame is finished, it is.
 */
#define XBOX_MAX_COUNTER_MIRRORS 4

static struct {
    uint32_t device_ptr_va;
    uint32_t src_off, dst_off;
} g_counter_mirrors[XBOX_MAX_COUNTER_MIRRORS];
static int g_counter_mirror_count = 0;

int xbox_Nv2aMirrorCounter(uint32_t device_ptr_va,
                           uint32_t src_off, uint32_t dst_off)
{
    if (g_counter_mirror_count >= XBOX_MAX_COUNTER_MIRRORS)
        return -1;
    g_counter_mirrors[g_counter_mirror_count].device_ptr_va = device_ptr_va;
    g_counter_mirrors[g_counter_mirror_count].src_off = src_off;
    g_counter_mirrors[g_counter_mirror_count].dst_off = dst_off;
    g_counter_mirror_count++;
    fprintf(stderr, "  NV2A counter mirror: device at 0x%08X,"
            " +0x%X -> +0x%X\n", device_ptr_va, src_off, dst_off);
    return 0;
}

static void counter_mirrors_tick(void)
{
    for (int i = 0; i < g_counter_mirror_count; i++) {
        uint32_t dev;

        if (!fence_readable(g_counter_mirrors[i].device_ptr_va, 4))
            continue;
        dev = *(volatile uint32_t *)((uintptr_t)g_counter_mirrors[i].device_ptr_va
                                     + g_memory_offset);
        if (!fence_readable(dev + g_counter_mirrors[i].src_off, 4)
                || !fence_readable(dev + g_counter_mirrors[i].dst_off, 4))
            continue;
        {
            volatile uint32_t *dst =
                (volatile uint32_t *)((uintptr_t)(dev + g_counter_mirrors[i].dst_off)
                                      + g_memory_offset);
            uint32_t src =
                *(volatile uint32_t *)((uintptr_t)(dev + g_counter_mirrors[i].src_off)
                                       + g_memory_offset);
            if (*dst != src)
                *dst = src;
        }
    }
}

/*
 * Frame counters the title polls to pace itself.
 *
 * D3D keeps a swap count inside the device and bumps it once per presented
 * frame; a title that wants to wait a frame reads it and spins until it moves.
 * Wreckless does exactly that at guest 0x000DC5E0 -- "loop while the counter
 * has advanced by less than 2" -- so a counter that never moves is not a
 * dropped frame, it is a hang with a full asset load behind it.
 *
 * Nothing here presents, so nothing would ever move it. Advancing it on a
 * clock is what makes the wait terminate, and 60 Hz is the rate the title
 * expects the display to run at. Followed through the device pointer for the
 * same reason the fence is: the device is allocated at runtime.
 */
#define XBOX_MAX_FRAME_COUNTERS 4
#define XBOX_FRAME_PERIOD_MS    16      /* ~60 Hz */

static struct {
    uint32_t device_ptr_va;
    uint32_t counter_off;
} g_frame_counters[XBOX_MAX_FRAME_COUNTERS];
static int   g_frame_counter_count = 0;
static DWORD g_frame_counter_last_ms = 0;

int xbox_Nv2aFrameCounter(uint32_t device_ptr_va, uint32_t counter_off)
{
    if (g_frame_counter_count >= XBOX_MAX_FRAME_COUNTERS)
        return -1;
    g_frame_counters[g_frame_counter_count].device_ptr_va = device_ptr_va;
    g_frame_counters[g_frame_counter_count].counter_off   = counter_off;
    g_frame_counter_count++;
    fprintf(stderr, "  Frame counter: device at 0x%08X, count +0x%X @ %d Hz\n",
            device_ptr_va, counter_off, 1000 / XBOX_FRAME_PERIOD_MS);
    return 0;
}

/* A real swap happened: advance every registered counter, and remember when.
 *
 * The timer below exists for a title nothing presents for. Once the
 * pushbuffer executor is actually running flips, the timer is the wrong
 * clock and an actively harmful one: Half-Life 2's loader paces its intro on
 * this count, so a 62 Hz timer against an executor managing a fraction of a
 * frame per second ran the video forward in virtual time far faster than it
 * could be drawn. Only every few hundredth frame was ever presented, each one
 * sampled part way through its own decode -- which looks exactly like a
 * stalling, blocky video rather than a clock running away.
 */
static DWORD g_frame_counter_flip_ms;

void xbox_Nv2aFrameCounterFlip(void)
{
    int i;

    g_frame_counter_flip_ms = GetTickCount();
    if (!g_frame_counter_flip_ms)
        g_frame_counter_flip_ms = 1;          /* 0 means "never" */
    for (i = 0; i < g_frame_counter_count; i++) {
        uint32_t dev;

        if (!fence_readable(g_frame_counters[i].device_ptr_va, 4))
            continue;
        dev = *(volatile uint32_t *)((uintptr_t)g_frame_counters[i].device_ptr_va
                                     + g_memory_offset);
        if (!fence_readable(dev + g_frame_counters[i].counter_off, 4))
            continue;
        *(volatile uint32_t *)((uintptr_t)(dev + g_frame_counters[i].counter_off)
                               + g_memory_offset) += 1;
    }
}

static void frame_counters_tick(void)
{
    DWORD now = GetTickCount();
    int i;

    if (!g_frame_counter_count)
        return;
    if (g_frame_counter_last_ms
            && (now - g_frame_counter_last_ms) < XBOX_FRAME_PERIOD_MS)
        return;
    /* Something is presenting: let it drive the count instead. Two seconds,
     * because the executor's flips are not evenly spaced and a title that
     * genuinely stops presenting still has to be got moving again. */
    if (g_frame_counter_flip_ms && (now - g_frame_counter_flip_ms) < 2000) {
        g_frame_counter_last_ms = now;
        return;
    }
    g_frame_counter_last_ms = now;

    for (i = 0; i < g_frame_counter_count; i++) {
        uint32_t dev;

        if (!fence_readable(g_frame_counters[i].device_ptr_va, 4))
            continue;
        dev = *(volatile uint32_t *)((uintptr_t)g_frame_counters[i].device_ptr_va
                                     + g_memory_offset);
        if (!fence_readable(dev + g_frame_counters[i].counter_off, 4))
            continue;
        *(volatile uint32_t *)((uintptr_t)(dev + g_frame_counters[i].counter_off)
                               + g_memory_offset) += 1;
    }
}

/* With RECOMP_PB_EXEC the executor is the GPU, and the fence word is what its
 * BACK_END_WRITE_SEMAPHORE_RELEASE (0x1D70) writes as it gets there. Copying
 * the title's counter in instead says every fence has passed: D3D's
 * make-space and resource waits return at once, it writes over ring space
 * and vertex data the executor has not read, and the executor decodes them
 * as commands ("[PB] bad target", JSRF's garbage NOP trap selector). So in
 * that mode the mirror only hands the word's address to the executor, as
 * NFSU2 does from its fence wait (d3d8ltcg-device-context.md). */
static void fence_mirrors_tick(void)
{
    static int exec = -1;
    static uint32_t sem_registered;

    if (exec < 0)
        exec = getenv("RECOMP_PB_EXEC") != NULL;
    for (int i = 0; i < g_fence_mirror_count; i++) {
        uint32_t dev, get_ptr;

        if (!fence_readable(g_fence_mirrors[i].device_ptr_va, 4))
            continue;
        dev = *(volatile uint32_t *)((uintptr_t)g_fence_mirrors[i].device_ptr_va
                                     + g_memory_offset);
        if (!fence_readable(dev + g_fence_mirrors[i].get_ptr_off, 4)
                || !fence_readable(dev + g_fence_mirrors[i].put_off, 4))
            continue;
        get_ptr = *(volatile uint32_t *)((uintptr_t)(dev + g_fence_mirrors[i].get_ptr_off)
                                         + g_memory_offset);
        if (!fence_readable(get_ptr, 4))
            continue;
        if (exec) {
            if (get_ptr != sem_registered) {
                extern void nv2a_pb_set_semaphore_target(uint32_t guest_va);
                nv2a_pb_set_semaphore_target(get_ptr);
                sem_registered = get_ptr;
                fprintf(stderr, "  [D3D] GPU semaphore at 0x%08X (fence word"
                        " 0x%08X, counter 0x%08X): executor releases drive it,"
                        " mirror off\n", get_ptr,
                        *(volatile uint32_t *)((uintptr_t)get_ptr + g_memory_offset),
                        *(volatile uint32_t *)((uintptr_t)(dev + g_fence_mirrors[i].put_off)
                                               + g_memory_offset));
            }
            continue;
        }
        {
            volatile uint32_t *fence =
                (volatile uint32_t *)((uintptr_t)get_ptr + g_memory_offset);
            uint32_t put =
                *(volatile uint32_t *)((uintptr_t)(dev + g_fence_mirrors[i].put_off)
                                       + g_memory_offset);
            if (*fence != put)
                *fence = put;
        }
    }
}

static int s_nv2a_trace = 0;
int xbox_log_quiet(void);           /* kernel_bridge.c */

/* The display framebuffer, as reported by AvSetDisplayMode. Checksummed once a
 * second so a run can answer the only question that matters before building a
 * presenter: is the guest putting pixels anywhere at all, and do they change
 * from frame to frame. */
static uint32_t s_fb_va, s_fb_pitch, s_fb_height = 480;

void xbox_SetDisplayFramebuffer(uint32_t fb_va, uint32_t pitch)
{
    s_fb_va = fb_va;
    s_fb_pitch = pitch;
}

/* Where the title last said its framebuffer is, for anything that wants to read
 * guest pixels directly. There was only a setter, so every such reader carried
 * its own hardcoded address instead -- and a title that moves its framebuffer
 * (which is most of them, once it owns one) left that constant pointing at
 * uninitialised memory. A dump taken there is not empty, it is noise, which
 * reads as "the title drew garbage" rather than "you read the wrong page".
 *
 * This is the resolved address, not the physical one AvSetDisplayMode states:
 * the caller resolves before storing, because a physical framebuffer address
 * read directly lands in the loaded image. Getting that wrong is the same bug
 * twice over -- once in the probe below, once in a caller of this. */
uint32_t xbox_GetDisplayFramebuffer(uint32_t *pitch)
{
    if (pitch)
        *pitch = s_fb_pitch;
    return s_fb_va;
}

static void framebuffer_probe_tick(void)
{
    static DWORD last_ms;
    static uint32_t last_sum;
    DWORD now = GetTickCount();
    uint32_t sum = 0, nonzero = 0, i, n;
    const uint32_t *p;

    if (!s_nv2a_trace || !s_fb_va || !s_fb_pitch)
        return;
    if (last_ms && (now - last_ms) < 1000)
        return;
    last_ms = now;
    if ((size_t)s_fb_va + s_fb_pitch * s_fb_height > g_memory_size)
        return;
    p = (const uint32_t *)((uintptr_t)s_fb_va + g_memory_offset);
    n = (s_fb_pitch * s_fb_height) / 4;
    for (i = 0; i < n; i++) {
        sum = sum * 33u + p[i];
        if (p[i]) nonzero++;
    }
    fprintf(stderr, "  [FB] 0x%08X sum=%08X nonzero=%u/%u %s\n",
            s_fb_va, sum, nonzero, n,
            sum != last_sum ? "CHANGED" : "same");
    last_sum = sum;
    fflush(stderr);
}

/* Pushbuffer executor load over the last second, in percent. */
static int s_exec_busy_pct;
int nv2a_exec_busy_percent(void) { return s_exec_busy_pct; }
/* And in all, in microseconds: time inside nv2a_pb_scan (reading the stream,
 * drawing, and waiting on traps and flips), for per-frame reports. */
static volatile uint64_t s_exec_busy_us;
uint64_t nv2a_exec_busy_us(void) { return s_exec_busy_us; }

/* Non-zero while a device model has raised a GPU interrupt the title has not
 * serviced yet. The table above holds the interrupt-status registers at zero
 * because "nothing here raises GPU interrupts" -- which stops being true the
 * moment the pushbuffer executor traps a software method, and zeroing its
 * status bits a microsecond after it sets them means the ISR never sees
 * them. While held, the PGRAPH status is left alone. */
static volatile LONG g_nv2a_intr_hold;

void xbox_Nv2aHoldInterrupts(int on)
{
    if (on)
        InterlockedIncrement(&g_nv2a_intr_hold);
    else
        InterlockedDecrement(&g_nv2a_intr_hold);
}

/* The vblank bits, likewise. The runtime raises vblank on the timer thread:
 * the tick sets the bits and calls D3D's ISR, which masks the interrupt and
 * queues a DPC, and the DPC reads PMC_INTR again to decide what happened.
 * Cleared on this thread's next pass (up to a millisecond later) they were
 * usually gone before the DPC had the guest lock, so the DPC saw nothing and
 * D3D's vblank count stood still -- a flip then never retired, and NFSU2 hung
 * in PersistDisplay at the start of a race. Held until the DPC starts, plus a
 * grace period for it to read them; its own vblank handler then spins until
 * bit 24 drops, which it does on the next pass after that.
 *
 * 0: not held. -1: held. Otherwise the monotonic microsecond it ends. */
static volatile LONGLONG g_vblank_hold;
#define VBLANK_GRACE_US 300

#define VBLANK_HOLD_OFF   0
#define VBLANK_HOLD_ON  (-1)
#define NV2A_PMC_INTR_0_REG    0x000100u
#define NV2A_PCRTC_INTR_0_REG  0x600100u
#define NV2A_PGRAPH_INTR_REG   0x400100u
#define PMC_SOURCE_PGRAPH      0x00001000u
#define PMC_SOURCE_PCRTC       0x01000000u
#define PCRTC_VBLANK           0x00000001u

static LONGLONG vblank_now_us(void)
{
    struct timespec now;

    clock_gettime(CLOCK_MONOTONIC, &now);
    return (LONGLONG)now.tv_sec * 1000000 + now.tv_nsec / 1000;
}

/* An atomic 64-bit store, from the compare-exchange the platform has. */
static void vblank_set(LONGLONG state)
{
    LONGLONG seen = g_vblank_hold;
    LONGLONG was;

    while ((was = InterlockedCompareExchange64(&g_vblank_hold, state, seen)) != seen)
        seen = was;
}

void xbox_Nv2aVblankHold(void)
{
    vblank_set(VBLANK_HOLD_ON);
}

/* The DPC has its bits: they are kept a grace period longer, then go. */
void xbox_Nv2aVblankRelease(void)
{
    InterlockedCompareExchange64(&g_vblank_hold, vblank_now_us() + VBLANK_GRACE_US,
                                 VBLANK_HOLD_ON);
}

void xbox_Nv2aVblankDrop(void)
{
    vblank_set(VBLANK_HOLD_OFF);
}

/* The vblank's handler is about to run, and whoever called it has read the
 * bits already: they are cleared now. The handler ends by acknowledging
 * PCRTC and spinning until PMC_INTR bit 24 drops, which left to the ack
 * thread's next pass took a millisecond or more per vblank, holding the
 * dispatch lock. */
void xbox_Nv2aVblankTaken(void)
{
    volatile uint32_t *nv = (volatile uint32_t *)((uint8_t *)g_memory_offset + 0xFD000000u);

    vblank_set(VBLANK_HOLD_OFF);
    InterlockedAnd((volatile LONG *)&nv[NV2A_PMC_INTR_0_REG / 4], ~(LONG)PMC_SOURCE_PCRTC);
    InterlockedAnd((volatile LONG *)&nv[NV2A_PCRTC_INTR_0_REG / 4], ~(LONG)PCRTC_VBLANK);
}

/* Are the vblank's bits held? A grace period that has run out ends the hold. */
static int vblank_held(void)
{
    LONGLONG state = InterlockedCompareExchange64(&g_vblank_hold, 0, 0);

    if (state == VBLANK_HOLD_OFF)
        return 0;
    if (state != VBLANK_HOLD_ON && vblank_now_us() >= state) {
        InterlockedCompareExchange64(&g_vblank_hold, VBLANK_HOLD_OFF, state);
        return 0;
    }
    return 1;
}

/* apply 0: only say whether there is anything to acknowledge.
 *
 * Applying, the PGRAPH bits are cleared under the executor's trap lock, with
 * "held" read again inside it. Read once at the top of the pass, a trap posted
 * after that read was wiped here: the handler then found no PGRAPH_INTR, did
 * nothing, and was still reported as having taken it (the wrapper goes by
 * NSOURCE), so D3D's fence event was never set. The clears are atomic: the
 * executor and the vblank tick set bits in the same words from other
 * threads. */
extern int  nv2a_pb_trap_trylock(void);
extern void nv2a_pb_trap_unlock(void);

/* The bits of interrupt word `offset` the scan may clear, given what is held:
 * a held trap keeps PGRAPH_INTR whole and the PGRAPH source in PMC_INTR; a
 * held vblank keeps the PCRTC source and the vblank bit. Everything else is
 * acknowledged -- freezing all of PMC_INTR deadlocked, since D3D's vblank DPC
 * acknowledges PCRTC and spins until PMC bit 24 drops, on the thread that
 * also has to deliver the held trap. */
static uint32_t ack_mask(uint32_t offset, uint32_t mask, int trap_held, int vblank)
{
    if (offset == NV2A_PGRAPH_INTR_REG && trap_held)
        return 0;
    if (offset == NV2A_PMC_INTR_0_REG) {
        if (trap_held)
            mask &= ~PMC_SOURCE_PGRAPH;
        if (vblank)
            mask &= ~PMC_SOURCE_PCRTC;
    }
    if (offset == NV2A_PCRTC_INTR_0_REG && vblank)
        mask &= ~PCRTC_VBLANK;
    return mask;
}

static int nv2a_ack_scan(volatile uint32_t *regs, int apply)
{
    int trap_held = InterlockedCompareExchange(&g_nv2a_intr_hold, 0, 0) != 0;
    int vblank = vblank_held();
    int locked = 0, pending = 0;
    size_t i;

    if (apply) {
        /* Busy (a trap being posted or handled): the PGRAPH bits wait for
         * the next pass, as if held. Held is read again under the lock. */
        locked = nv2a_pb_trap_trylock();
        trap_held = !locked || InterlockedCompareExchange(&g_nv2a_intr_hold, 0, 0) != 0;
    }
    for (i = 0; i < sizeof NV2A_ACK / sizeof NV2A_ACK[0]; i++) {
        volatile uint32_t *word = (volatile uint32_t *)((char *)regs + NV2A_ACK[i].offset);
        uint32_t mask = ack_mask(NV2A_ACK[i].offset, NV2A_ACK[i].busy_mask, trap_held, vblank);

        if ((*word & mask) == 0)
            continue;
        pending = 1;
        if (apply)
            InterlockedAnd((volatile LONG *)word, (LONG)~mask);
    }
    if (locked)
        nv2a_pb_trap_unlock();
    for (i = 0; i < sizeof NV2A_IDLE / sizeof NV2A_IDLE[0]; i++) {
        volatile uint32_t *word = (volatile uint32_t *)((char *)regs + NV2A_IDLE[i].offset);
        uint32_t idle = NV2A_IDLE[i].idle_mask;

        if ((*word & idle) == idle)
            continue;
        pending = 1;
        if (apply)
            *word |= idle;
    }
    return pending;
}

static void nv2a_ack_flags(volatile uint32_t *regs)
{
    (void)nv2a_ack_scan(regs, 1);
}

/* Busy/idle flags, on a thread of their own.
 *
 * Software sets a request bit and spins until "hardware" clears it -- XDK
 * D3D's CDevice::KickOff does this on every kickoff ([nv2a + 0x100410] bit
 * 16, the write-combine flush). The thread below this one also runs the
 * pushbuffer executor and the render back end, so one pass of it can take a
 * whole frame; with the flags cleared only there, every kickoff waited a
 * frame. X-Men Legends' mission load, thousands of kickoffs long, looked
 * frozen on its loading screen. Clearing the flags is a few loads and stores,
 * so it gets a loop that does nothing else. */
/* Between passes of the two register threads.
 *
 * Sleep(0) is a bare yield: with nothing else runnable it returns at once,
 * and each thread spun a core flat out -- two of a Switch's three. But a
 * fixed sleep delays every kickoff the title then waits on. So they sleep
 * until woken, for at most a millisecond: the title's own poll loops wake
 * them (RECOMP_SPIN_HINT -> recomp_spin_wake), which is exactly when an
 * answer is wanted. While the pushbuffer has work they only yield. */
#include <pthread.h>
static pthread_mutex_t   s_hw_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t    s_hw_cond = PTHREAD_COND_INITIALIZER;
static volatile int      s_hw_sleepers;     /* threads in hw_sleep */
static volatile unsigned s_hw_wakes;        /* bumped by every wake */

#define NV2A_PFB_WC_FLUSH      0x100410u   /* the kickoff's write-combine flush */
#define NV2A_PFB_WC_FLUSH_BUSY 0x00010000u

/* The kickoff's write-combine flush (NV2A_ACK's first entry), answered on the
 * thread that polls for it. Waking the flag thread for it and waiting for
 * Horizon to schedule that cost ~12% of NFSU2's main thread in a race. Only
 * the title sets the bit, so clearing it here loses no request. */
static void spin_ack_flush(void)
{
    volatile uint32_t *flush = (volatile uint32_t *)
        ((uint8_t *)xbox_GetMemoryOffset() + 0xFD000000u + NV2A_PFB_WC_FLUSH);

    if ((*flush & NV2A_PFB_WC_FLUSH_BUSY) == 0)
        return;
    __atomic_thread_fence(__ATOMIC_SEQ_CST);    /* its pushbuffer writes first */
    *flush &= ~NV2A_PFB_WC_FLUSH_BUSY;
}

/* The DMA_PUT the executor has walked up to, and whether it is asleep in
 * nv2a_thread_pause waiting for a new one (see nv2a_kick). */
static volatile uint32_t s_put_done = 0xFFFFFFFFu;
static volatile int      s_exec_asleep;
static volatile uint32_t s_kicks;

/* Wake every thread in hw_sleep. */
static void hw_wake(int is_kick)
{
    pthread_mutex_lock(&s_hw_lock);
    s_hw_wakes++;
    if (is_kick)
        s_kicks++;
    pthread_cond_broadcast(&s_hw_cond);
    pthread_mutex_unlock(&s_hw_lock);
}

/* A guest poll loop's periodic wake (RECOMP_SPIN_HINT). Only when a sleeper
 * has something to do: the executor (it runs the register mirrors as well,
 * so any poll may be waiting on it), or the flag thread with a flag to
 * acknowledge. Waking the flag thread every 16 turns regardless -- a lock, a
 * broadcast, an unlock -- was 19% of JSRF's main thread in its busiest
 * streets, inside XDK D3D's fence wait. */
void recomp_spin_wake(void)
{
    spin_ack_flush();
    if (__atomic_load_n(&s_hw_sleepers, __ATOMIC_RELAXED) == 0)
        return;
    if (__atomic_load_n(&s_exec_asleep, __ATOMIC_RELAXED)
        || (g_nv2a_memory && nv2a_ack_scan((volatile uint32_t *)g_nv2a_memory, 0)))
        hw_wake(0);
}

uint32_t nv2a_kick_count(void) { return s_kicks; }
/* Microseconds the executor has spent asleep between passes, in all. */
static volatile uint64_t s_exec_sleep_us;
uint64_t nv2a_exec_sleep_us(void) { return s_exec_sleep_us; }

/* Watching for the next DMA_PUT before sleeping, after a pass that had
 * work: microseconds (RECOMP_EXEC_POLL_US, 0 = never), and how often a new
 * PUT turned up in that time.
 *
 * XDK D3D's fence wait makes the title and the executor take turns: a
 * vertex buffer locked again waits until the executor has passed the draw
 * that last read it (JSRF's streets do this hundreds of times a frame).
 * Each turn the executor had gone to sleep, and was woken -- a broadcast,
 * then Horizon scheduling it again -- for every one. */
static int s_exec_poll_us = -1;
static volatile uint32_t s_exec_poll_hits, s_exec_sleeps;
uint32_t nv2a_exec_poll_hits(void) { return s_exec_poll_hits; }
uint32_t nv2a_exec_sleeps(void) { return s_exec_sleeps; }

static int64_t mono_us(void)
{
    struct timespec now;

    clock_gettime(CLOCK_MONOTONIC, &now);
    return (int64_t)now.tv_sec * 1000000 + now.tv_nsec / 1000;
}

/* RECOMP_EXEC_POLL_US, read once: 300 unless set, 0 for never. */
static int exec_poll_window_us(void)
{
    if (s_exec_poll_us < 0) {
        const char *setting = getenv("RECOMP_EXEC_POLL_US");
        int us = setting != NULL ? atoi(setting) : 300;
        s_exec_poll_us = us < 0 ? 0 : us;
        fprintf(stderr, "  [NV2A] executor watches for new commands %d us before it sleeps"
                        " (RECOMP_EXEC_POLL_US)\n", s_exec_poll_us);
    }
    return s_exec_poll_us;
}

/* Spin for up to the window, watching DMA_PUT: 1 if a new one turned up.
 * A thread sharing the core gets a turn every 20 us. */
static int exec_poll(const volatile uint32_t *put)
{
    int window = exec_poll_window_us();
    int64_t started, last_yield;

    if (window == 0)
        return 0;
    started = last_yield = mono_us();
    for (;;) {
        int64_t now;
        int k;

        for (k = 0; k < 256; k++) {
            if (*put != s_put_done) {
                s_exec_poll_hits++;
                return 1;
            }
            __asm__ __volatile__("yield");
        }
        now = mono_us();
        if (now - started >= window)
            return 0;
        if (now - last_yield >= 20) {
            sched_yield();
            last_yield = now;
        }
    }
}

/* Sleep until woken, for at most a millisecond. `put`: this is the executor,
 * which says it is asleep and then looks at DMA_PUT once more -- nv2a_kick
 * stores PUT (the title did) and then reads "asleep", so one of the two sees
 * the other and no kick is lost. */
static void hw_sleep(const volatile uint32_t *put)
{
    struct timespec deadline, before, after;
    unsigned wakes_seen;
    int must_wait = 1;

    clock_gettime(CLOCK_REALTIME, &deadline);
    deadline.tv_nsec += 1000000L;
    if (deadline.tv_nsec >= 1000000000L) {
        deadline.tv_sec++;
        deadline.tv_nsec -= 1000000000L;
    }
    pthread_mutex_lock(&s_hw_lock);
    wakes_seen = s_hw_wakes;
    __atomic_add_fetch(&s_hw_sleepers, 1, __ATOMIC_RELAXED);
    if (put) {
        __atomic_store_n(&s_exec_asleep, 1, __ATOMIC_RELAXED);
        __atomic_thread_fence(__ATOMIC_SEQ_CST);
        if (*put != s_put_done)
            must_wait = 0;
        clock_gettime(CLOCK_MONOTONIC, &before);
        if (must_wait)
            s_exec_sleeps++;
    }
    while (must_wait && wakes_seen == s_hw_wakes)
        if (pthread_cond_timedwait(&s_hw_cond, &s_hw_lock, &deadline) != 0)
            break;
    if (put) {
        __atomic_store_n(&s_exec_asleep, 0, __ATOMIC_RELAXED);
        clock_gettime(CLOCK_MONOTONIC, &after);
        s_exec_sleep_us += (uint64_t)((after.tv_sec - before.tv_sec) * 1000000LL
                                      + (after.tv_nsec - before.tv_nsec) / 1000);
    }
    __atomic_sub_fetch(&s_hw_sleepers, 1, __ATOMIC_RELAXED);
    pthread_mutex_unlock(&s_hw_lock);
}

/* Between passes of the two register threads: a yield while the pushbuffer
 * has work, otherwise (after watching for a new PUT, if `poll`) a sleep the
 * title's poll loops and kernel waits cut short. `put` NULL: the flag
 * thread. */
static void nv2a_thread_pause(int busy, const volatile uint32_t *put, int poll)
{
    if (busy) {
        sched_yield();
        return;
    }
    if (put && poll && exec_poll(put))
        return;
    hw_sleep(put);
}

/* A guest thread is about to block (a kernel wait or sleep): if it left
 * commands the executor has not seen and the executor is asleep, wake it.
 * XDK D3D's kickoff polls only for its write-combine flush, before it writes
 * DMA_PUT, so the poll loops' wake came too early; and a fence wait
 * (BlockOnFence) then blocks on an event rather than spinning. The executor
 * slept out its millisecond with the fence in the ring while the title slept
 * waiting for it, a few times a frame in JSRF's streets. */
void nv2a_kick(void)
{
    const volatile uint32_t *put;

    if (!g_nv2a_memory)
        return;
    put = (const volatile uint32_t *)((char *)g_nv2a_memory + NV2A_USER_DMA_PUT);
    __atomic_thread_fence(__ATOMIC_SEQ_CST);    /* the title's PUT store first */
    if (*put != s_put_done && __atomic_load_n(&s_exec_asleep, __ATOMIC_RELAXED))
        hw_wake(1);
}

static DWORD WINAPI nv2a_flag_thread(LPVOID param)
{
    volatile uint32_t *regs = (volatile uint32_t *)param;
    while (!InterlockedCompareExchange(&g_nv2a_ack_stop, 0, 0)) {
        nv2a_ack_flags(regs);
        nv2a_thread_pause(0, NULL, 0);
    }
    return 0;
}

#define NV2A_REG32(regs, offset) (*(volatile uint32_t *)((char *)(regs) + (offset)))

/* Without the pushbuffer executor (RECOMP_PB_EXEC unset), GET simply follows
 * PUT. With it, GET moves only once the executor has walked up to PUT
 * (ack_execute): mirroring it first told D3D the commands were consumed
 * while still unread, D3D reused that ring space, and the executor read the
 * new data as old commands -- index data turned up as blend factors. */
static void ack_mirror_get(volatile uint32_t *regs)
{
    static int executing = -1;

    if (executing != 1)
        executing = getenv("RECOMP_PB_EXEC") != NULL;
    if (!executing && NV2A_REG32(regs, NV2A_USER_DMA_GET) != NV2A_REG32(regs, NV2A_USER_DMA_PUT))
        NV2A_REG32(regs, NV2A_USER_DMA_GET) = NV2A_REG32(regs, NV2A_USER_DMA_PUT);
}

/* The executor's share of wall time over the last second (s_exec_busy_pct,
 * see nv2a_exec_busy_percent) and its running total in microseconds. Near
 * 100% means the "GPU" is the frame-rate limit. */
static void ack_account_busy(LARGE_INTEGER began, LARGE_INTEGER ended)
{
    static LARGE_INTEGER frequency, window_start;
    static LONGLONG busy_in_window, sub_us_ticks;
    LONGLONG ticks;
    uint64_t us;

    if (frequency.QuadPart == 0) {
        QueryPerformanceFrequency(&frequency);
        window_start = began;
    }
    busy_in_window += ended.QuadPart - began.QuadPart;
    /* Whole microseconds, carrying the remainder (this thread alone writes
     * s_exec_busy_us). */
    ticks = ended.QuadPart - began.QuadPart + sub_us_ticks;
    us = (uint64_t)(ticks * 1000000 / frequency.QuadPart);
    sub_us_ticks = ticks - (LONGLONG)(us * frequency.QuadPart / 1000000);
    s_exec_busy_us += us;
    if (ended.QuadPart - window_start.QuadPart > frequency.QuadPart) {
        s_exec_busy_pct = (int)(100.0 * busy_in_window
                                / (double)(ended.QuadPart - window_start.QuadPart));
        busy_in_window = 0;
        window_start = ended;
    }
}

/* Carry out what the title submitted up to `put`, then hand back the space:
 * GET = PUT. DMA_PUT holds a physical address (XDK D3D writes VA &
 * 0x0FFFFFFF and reads GET | 0x80000000); nv2a_pb_scan walks it through the
 * contiguous window, which is the physical-address view. */
static void ack_execute(volatile uint32_t *regs, uint32_t put, int put_moved, DWORD now_ms)
{
    extern void nv2a_pb_scan(uint32_t);
    extern void nv2a_pb_scan_report(void);
    extern void nv2a_pb_resync(uint32_t);
    extern void nv2a_backend_queue_flush(void);
    static uint32_t get_written = 0xFFFFFFFFu;
    static DWORD last_report;
    uint32_t get_now = NV2A_REG32(regs, NV2A_USER_DMA_GET);

    /* GET is ours to move. Not what we last wrote: the title reset the ring.
     * The first time it is where D3D started the ring, and walking from
     * there keeps the one-time device state sent before the first PUT. */
    if (get_written == 0xFFFFFFFFu || get_now != get_written)
        nv2a_pb_resync(get_now);
    if (put_moved) {
        LARGE_INTEGER began, ended;

        QueryPerformanceCounter(&began);
        /* The semaphore is registered before the walk runs its first
         * release, or that one is lost and D3D waits for a value passed. */
        fence_mirrors_tick();
        nv2a_pb_scan(put);
        nv2a_backend_queue_flush();     /* caught up: the renderer has the rest */
        QueryPerformanceCounter(&ended);
        ack_account_busy(began, ended);
    }
    NV2A_REG32(regs, NV2A_USER_DMA_GET) = put;      /* consumed: reusable now */
    get_written = put;
    if (s_nv2a_trace && now_ms - last_report > 10000) {
        last_report = now_ms;
        nv2a_pb_scan_report();
    }
}

/* RECOMP_NV2A_TRACE: DMA_PUT and DMA_GET as they move (GET stuck behind PUT
 * is the shape of a pushbuffer-full hang), and PCRTC_START, the frame the
 * title believes is on screen. */
static void ack_trace_put(volatile uint32_t *regs, uint32_t put)
{
    uint32_t get = NV2A_REG32(regs, NV2A_USER_DMA_GET);

    if (!s_nv2a_trace || xbox_log_quiet())
        return;
    fprintf(stderr, "  [NV2A] DMA_PUT = 0x%08X  DMA_GET = 0x%08X%s\n", put, get,
            get == put ? "" : "  (GPU behind)");
    fflush(stderr);
}

static void ack_trace_scanout(volatile uint32_t *regs)
{
    static uint32_t shown = 0xFFFFFFFFu;
    uint32_t start;

    if (!s_nv2a_trace || xbox_log_quiet())
        return;
    start = NV2A_REG32(regs, 0x600800);
    if (start == shown)
        return;
    shown = start;
    fprintf(stderr, "  [NV2A] PCRTC_START = 0x%08X\n", start);
    fflush(stderr);
}

/* The free-running counters the title polls: MCPX's, and KeTickCount (also
 * written on a thread of its own, tick_count_thread, since this one stalls
 * on traps). A frozen KeTickCount broke every timeout polling it: Halo's
 * DHCP set-up spun for ever bringing up XNet. */
static void ack_tick_counters(void)
{
    size_t i;

    if (g_mcpx_regs)
        for (i = 0; i < sizeof MCPX_COUNTERS / sizeof MCPX_COUNTERS[0]; i++)
            (*(volatile uint32_t *)((char *)g_mcpx_regs + MCPX_COUNTERS[i]))++;
    *(volatile uint32_t *)((uintptr_t)(XBOX_KERNEL_DATA_BASE + KDATA_TICK_COUNT)
                           + g_memory_offset) = GetTickCount();
}

/* The pause at the end of a pass: a yield while the pushbuffer moves or has
 * unread commands; else a sleep, the first idle pass after work watching
 * for more first. */
static void ack_pause(volatile uint32_t *regs)
{
    static uint32_t put_before;
    static int idle_passes;
    uint32_t put = NV2A_REG32(regs, NV2A_USER_DMA_PUT);
    int busy = put != put_before || NV2A_REG32(regs, NV2A_USER_DMA_GET) != put;

    if (busy)
        idle_passes = 0;
    else if (idle_passes < 2)
        idle_passes++;
    nv2a_thread_pause(busy, &NV2A_REG32(regs, NV2A_USER_DMA_PUT), idle_passes == 1);
    put_before = put;
}

/* The NV2A's side of the register file, a pass at a time: the busy flags,
 * the device models' ticks, the pushbuffer, the counters. */
static DWORD WINAPI nv2a_ack_thread(LPVOID param)
{
    volatile uint32_t *regs = (volatile uint32_t *)param;
    DWORD put_moved_ms = 0;
    uint32_t put_seen = 0;

    while (!InterlockedCompareExchange(&g_nv2a_ack_stop, 0, 0)) {
        DWORD now_ms;
        uint32_t put;

        nv2a_ack_flags(regs);
        ack_mirror_get(regs);
        fence_mirrors_tick();
        dsp_ack_tick();
        if (g_ac97_poll_page)
            ac97_clear_reset_bits_at(g_ac97_poll_page);
        poke_tick();
        counter_mirrors_tick();
        frame_counters_tick();
        framebuffer_probe_tick();

        /* A new PUT, or 2 s since the last: the walk (nv2a_pb_scan returns
         * at once unless RECOMP_PB_EXEC or RECOMP_PB_SCAN asked for it). */
        now_ms = GetTickCount();
        put = NV2A_REG32(regs, NV2A_USER_DMA_PUT);
        if (put != put_seen || now_ms - put_moved_ms > 2000) {
            ack_execute(regs, put, put != put_seen, now_ms);
            NV2A_REG32(regs, NV2A_USER_DMA_GET) = put;
            put_seen = put;
            put_moved_ms = now_ms;
            s_put_done = put;           /* for nv2a_kick */
            ack_trace_put(regs, put);
        }
        ack_trace_scanout(regs);
        ack_tick_counters();
        ack_pause(regs);
    }
    return 0;
}

/* KeTickCount on its own thread. The ack thread's write above stalls
 * whenever that thread does: it runs the pushbuffer executor, which waits on
 * GPU traps and retired flips. NFSU2's EA audio scheduler sleeps until
 * "deadline - KeTickCount" with the deadline 10 ms further each turn, so a
 * frozen tick ran it at 2 Hz and the game never mixed a sound. */
static DWORD WINAPI tick_count_thread(LPVOID unused)
{
    (void)unused;
    while (!InterlockedCompareExchange(&g_nv2a_ack_stop, 0, 0)) {
        *(volatile uint32_t *)((uintptr_t)(XBOX_KERNEL_DATA_BASE + KDATA_TICK_COUNT)
                               + g_memory_offset) = GetTickCount();
        Sleep(1);
    }
    return 0;
}

static void xbox_Nv2aAckStart(void)
{
    dsp_ack_init();
    poke_init();
    g_nv2a_ack_stop = 0;
    g_nv2a_ack_thread = CreateThread(NULL, 0, nv2a_ack_thread,
                                     g_nv2a_memory, 0, NULL);
    /* ponytail: not joined at shutdown; it only reads and writes guest RAM
     * and stops with g_nv2a_ack_stop like the thread above. */
    CloseHandle(CreateThread(NULL, 0, nv2a_flag_thread, g_nv2a_memory, 0, NULL));
    CloseHandle(CreateThread(NULL, 0, tick_count_thread, NULL, 0, NULL));
    if (g_nv2a_ack_thread) {
        fprintf(stderr, "  NV2A busy-bit ack: %zu register(s) acknowledged\n",
                sizeof(NV2A_ACK) / sizeof(NV2A_ACK[0]));
    }
}

/* Separate allocation for Xbox kernel address space (0x80010000+).
 * Some RenderWare code reads the kernel PE header to detect features. */
static void *g_kernel_memory = NULL;

/* Global offset accessible by recompiled code (via recomp_types.h) */
ptrdiff_t g_xbox_mem_offset = 0;

/* Set once xbox_MemoryLayoutInit successfully reserves real host memory
 * for guest_vmem.c's extended-VMA range; see that reservation below. */
int g_xbox_extvma_reserved = 0;

/* Bounds of the title's executable sections, from its own XBE section table.
 *
 * RECOMP_ICALL uses these to decide whether an indirect-call target is code
 * before dispatching it. This used to be a hardcoded "0x00400000..0xFE000000 is
 * not code" test, which is true for Burnout 3 -- its .text ends at 0x002CC200,
 * so everything above 0x400000 really is data -- and false for any title with
 * more code than that. Half-Life 2's .text runs to 0x005F4A6C, so the constant
 * silently discarded every indirect call into the top two thirds of the game,
 * including the one that enters its main. No log, no crash: eax = 0 and carry
 * on, which looks exactly like a function that returned early.
 *
 * Zero until the layout is initialised, which the macro treats as "allow" so
 * nothing breaks before the title is loaded. */
uint32_t g_xbox_image_lo = 0;
uint32_t g_xbox_image_hi = 0;
uint32_t g_xbox_code_lo = 0;
uint32_t g_xbox_code_hi = 0;

/* Global registers for recompiled code (via recomp_types.h) */
/* Each guest thread's TIB. The first thread uses the one the loader built;
 * a spawned thread gets its own from xbox_AllocThreadTib(). */
RECOMP_TLS uint32_t g_fs_base = XBOX_TIB_MAIN;

/* The shape of the TLS block the loader built, so a new thread can be
 * given one just like it: where the initialised image data starts, how
 * big the block is, and how big the per-thread structure slot 0 points
 * at is. Zero total means the image had no TLS directory. */
static uint32_t g_tls_template_va, g_tls_total, g_tls_thread_size = 64;

/* REGISTER BLOCK. Lifted code rewritten by JSRF's regen/jsrf_reg_locals.py
 * keeps these registers in C locals and stores and loads them all around
 * every call, so on the Switch they are one block in a fixed order, reached
 * from g_eax's address (recomp_types.h, REGISTERS IN LOCALS):
 *
 *   eax ecx edx ebx esi edi esp fp_top | fp_stack[8]
 *    0   4   8   12  16  20  24   28      32 .. 95
 *
 * A compiler places separate thread-locals as it likes, so the block is laid
 * out here in assembly, zero-filled like the definitions it replaces. To
 * every other file these are ordinary thread-locals. The generated code's
 * recomp_fast_tls_selfcheck() checks the layout at boot. -DRECOMP_NO_REG_BLOCK
 * (for the generated code as well) goes back to separate definitions. */
#if defined(__SWITCH__) && defined(__aarch64__) && !defined(RECOMP_NO_REG_BLOCK)
#define RECOMP_REG_BLOCK_SYM(name, size)                                      \
    "\t.globl " #name "\n\t.type " #name ", %tls_object\n"                  \
    "\t.size " #name ", " #size "\n" #name ":\n\t.zero " #size "\n"
__asm__("\t.pushsection .tbss.recomp_regs,\"awT\",%nobits\n"
        "\t.balign 16\n"
        RECOMP_REG_BLOCK_SYM(g_eax, 4)
        RECOMP_REG_BLOCK_SYM(g_ecx, 4)
        RECOMP_REG_BLOCK_SYM(g_edx, 4)
        RECOMP_REG_BLOCK_SYM(g_ebx, 4)
        RECOMP_REG_BLOCK_SYM(g_esi, 4)
        RECOMP_REG_BLOCK_SYM(g_edi, 4)
        RECOMP_REG_BLOCK_SYM(g_esp, 4)
        RECOMP_REG_BLOCK_SYM(g_fp_top, 4)
        RECOMP_REG_BLOCK_SYM(g_fp_stack, 64)
        "\t.popsection\n");
#undef RECOMP_REG_BLOCK_SYM
extern RECOMP_TLS uint32_t g_eax, g_ecx, g_edx, g_esp;
extern RECOMP_TLS uint32_t g_ebx, g_esi, g_edi;
extern RECOMP_TLS double g_fp_stack[8];
extern RECOMP_TLS int g_fp_top;
#else
RECOMP_TLS uint32_t g_eax = 0, g_ecx = 0, g_edx = 0, g_esp = 0;
RECOMP_TLS uint32_t g_ebx = 0, g_esi = 0, g_edi = 0;
#define RECOMP_FP_DEFINED_HERE 1
#endif

#ifdef RECOMP_ABI_CHECK
/* Report a lifted function that returned without restoring ebx/esi/edi.
 *
 * Those are callee-saved on x86, and the recompiler keeps them in globals, so
 * a function whose epilogue was never lifted corrupts its caller rather than
 * itself -- an error with no crash and no message, just less work silently
 * done. Ranked by hit count so the routine breaking a hot loop stands out from
 * the one-offs; -DRECOMP_ABI_CHECK only, since it costs three compares on
 * every indirect call.
 */
extern volatile uint32_t g_icall_trace[16];
extern volatile uint32_t g_icall_trace_idx;

void recomp_abi_violation_log(uint32_t va, uint32_t ebx0, uint32_t esi0,
                              uint32_t edi0, uint32_t esp0)
{
    enum { SLOTS = 32 };
    static uint32_t seen[SLOTS];
    static uint64_t hits[SLOTS];
    static int count;
    int i;

    for (i = 0; i < count; i++)
        if (seen[i] == va)
            break;
    if (i == count) {
        if (count == SLOTS)
            return;
        seen[count] = va;
        hits[count] = 0;
        count++;
        fprintf(stderr, "[ABI] sub_%08X:%s%s%s%s\n"
                        "      ebx %08X->%08X esi %08X->%08X"
                        " edi %08X->%08X esp %08X->%08X\n",
                va,
                g_ebx != ebx0 ? " ebx" : "",
                g_esi != esi0 ? " esi" : "",
                g_edi != edi0 ? " edi" : "",
                g_esp < esp0 + 4 ? " esp(epilogue never ran)" : "",
                ebx0, g_ebx, esi0, g_esi, edi0, g_edi, esp0, g_esp);
        /* esp coming back too HIGH means some callee popped arguments that
         * were never pushed -- a convention mismatch the one-sided invariant
         * above cannot see. The most recent indirect targets are the usual
         * suspects, so name them. */
        {
            int t;
            fprintf(stderr, "      esp delta %+d, recent icall targets:",
                    (int)(g_esp - esp0));
            for (t = 4; t >= 1; t--)
                fprintf(stderr, " %08X",
                        g_icall_trace[(g_icall_trace_idx - t) & 15]);
            fputc('\n', stderr);
        }
        fflush(stderr);
    }
    hits[i]++;
}
#endif

/* SEH frame pointer bridge (see recomp_types.h for explanation) */
RECOMP_TLS uint32_t g_seh_ebp = 0;
#ifdef RECOMP_FP_DEFINED_HERE   /* else in the REGISTER BLOCK above */
RECOMP_TLS double g_fp_stack[8];
RECOMP_TLS int g_fp_top = 0;
#endif

/* Set once at startup. The generated code reads it at the ret of every
 * --force-return function, so it has to be cheap and it has to default to
 * off: a build carrying forced functions behaves normally until the
 * variable is set. */
int g_force_return = 0;
/* x87 control and status. The reset default masks every exception and
 * rounds to nearest, which is what the CRT expects before _control87. */
RECOMP_TLS uint16_t g_fp_control_word = 0x037Fu;
RECOMP_TLS int g_fp_cmp = 0;
RECOMP_TLS uint16_t g_fp_cc = 0x4000;

/* Defined below, with the other guest registers. */
extern RECOMP_TLS uint32_t g_ebp;
extern RECOMP_TLS uint32_t g_eax, g_ecx, g_edx, g_ebx, g_esi, g_edi;

/* ---- non-local jumps ---------------------------------------------------
 *
 * The native half of the guest's setjmp/longjmp. See recomp_types.h for why a
 * guest-only longjmp is not enough; in short, the recompiled frames are C
 * frames and something has to unwind them.
 *
 * Keyed by guest buffer address, per thread. Buffers nest, so jumping to an
 * outer one discards every inner entry -- those frames are gone.
 */
#define RECOMP_JMPBUF_SLOTS 32

typedef struct {
    uint32_t buf_va;
    jmp_buf  native;
} recomp_jmp_slot;

static RECOMP_TLS recomp_jmp_slot s_jmp[RECOMP_JMPBUF_SLOTS];
static RECOMP_TLS int             s_jmp_used;

jmp_buf *recomp_setjmp_slot(uint32_t buf_va)
{
    int i;

    for (i = 0; i < s_jmp_used; i++)
        if (s_jmp[i].buf_va == buf_va)
            return &s_jmp[i].native;      /* the same buffer, re-armed */
    if (s_jmp_used >= RECOMP_JMPBUF_SLOTS)
        s_jmp_used = RECOMP_JMPBUF_SLOTS - 1;   /* keep the deepest */
    s_jmp[s_jmp_used].buf_va = buf_va;
    return &s_jmp[s_jmp_used++].native;
}

int recomp_guest_longjmp(uint32_t buf_va, uint32_t value)
{
    const uint8_t *mem = (const uint8_t *)g_memory_offset;
    int i;

    for (i = s_jmp_used - 1; i >= 0; i--) {
        if (s_jmp[i].buf_va != buf_va)
            continue;

        /* The callee-saved registers and the stack, exactly as the CRT's
         * longjmp restores them: esp is the setjmp-time esp plus the return
         * address that setjmp's own ret would have popped. */
        g_ebx = *(const uint32_t *)(mem + buf_va + 0x04);
        g_edi = *(const uint32_t *)(mem + buf_va + 0x08);
        g_esi = *(const uint32_t *)(mem + buf_va + 0x0C);
        g_esp = *(const uint32_t *)(mem + buf_va + 0x10) + 4;

        /* ebp is a C local in every translated function, and a local modified
         * after setjmp is indeterminate once longjmp lands. Hand the resumed
         * frame its saved value back through the globals it already reads. */
        g_seh_ebp = *(const uint32_t *)(mem + buf_va + 0x00);
        g_ebp     = g_seh_ebp;

        s_jmp_used = i + 1;   /* the inner buffers died with their frames */
        longjmp(s_jmp[i].native, value ? (int)value : 1);
    }
    return 0;
}

/* Watchdog: dump the guest call stack if the title stops making progress.
 *
 * A hang gives nothing to work from -- no crash, no last log line, no native
 * stack that means anything, because the guest frames live in guest memory and
 * the native one only shows whichever translated function is spinning. Sampling
 * the guest stack from a second thread is the one view that says where the
 * title actually is. Same GS format the crash handler uses, so tools/
 * stackwalk.py reads either.
 *
 * Off unless RECOMP_WATCHDOG_SECS is set, so it costs a getenv in normal runs.
 */
/* Defined below, after the watchdog. */
extern volatile uint32_t g_icall_trace[16];
extern volatile uint32_t g_icall_trace_idx;
extern volatile uint64_t g_icall_count;

static uint32_t *s_watchdog_esp;
/* The other guest registers are thread-local too, so the watchdog has to be
 * handed the guest thread's copies rather than reading its own -- which are
 * always zero, and read as "every register is null" at exactly the moment the
 * registers are the thing being asked about. */
static uint32_t *s_watchdog_regs[6];
static unsigned  s_watchdog_secs;

/* Can RECOMP_PEEK dereference this guest address?
 *
 * It used to accept only the first 64 MB, which reads as "RAM" but is not the
 * question -- every window this file maps is mapped at va + g_memory_offset,
 * so the register apertures are just as dereferenceable as RAM is. Rejecting
 * them silently printed nothing for an address that was perfectly readable,
 * and a hang spinning on a GPU register is exactly the case where the value
 * that matters lives at 0xFD......  Peeking one is how the busy-wait in
 * DDS9's pushbuffer reserve was pinned to a DMA pointer rather than a flag.
 *
 * Every window is checked against its own pointer, because they are mapped
 * independently and any of them can be absent for this run. The 4 is the
 * width of the read below: an address one or two bytes short of the end is
 * inside the window and still faults. */
static int peek_readable(uint32_t va)
{
    struct { const void *mapped; uint32_t base; uint64_t size; } win[] = {
        { g_memory_base,   XBOX_BASE_ADDRESS, (uint64_t)g_memory_size },
        { g_contig_memory, XBOX_CONTIG_BASE,  XBOX_CONTIG_SIZE },
        { g_nv2a_memory,   XBOX_NV2A_BASE,    XBOX_NV2A_SIZE },
        { g_mcpx_memory,   XBOX_MCPX_BASE,    XBOX_MCPX_SIZE },
        { g_flash_memory,  XBOX_FLASH_BASE,   XBOX_FLASH_SIZE },
        { g_nv2a_mcpx_gap_memory, XBOX_NV2A_MCPX_GAP_BASE, XBOX_NV2A_MCPX_GAP_SIZE },
        { g_post_flash_gap_memory, XBOX_POST_FLASH_GAP_BASE, XBOX_POST_FLASH_GAP_SIZE },
    };
    size_t i;

    for (i = 0; i < sizeof(win) / sizeof(win[0]); i++) {
        if (!win[i].mapped || !win[i].size)
            continue;
        if (va >= win[i].base
                && (uint64_t)va + 4 <= (uint64_t)win[i].base + win[i].size)
            return 1;
    }
    return 0;
}

/* ---- RECOMP_WATCH: name the guest code that changes a guest dword -------
 *
 * A peek says a value changed between two samples. It does not say who
 * changed it, and for a value produced deep inside a middleware layer that
 * is the only question that matters -- reading the lifted C outwards from
 * the write is guesswork, and reading it inwards from the caller is worse.
 *
 * Same mechanism as the AC'97 trap above: make the page read-only, catch the
 * write, single-step it, then report. What it adds is the guest call chain,
 * scanned off the guest stack the way the watchdog does, which turns "the
 * mask became 4" into a list of addresses to go and read.
 *
 * Off unless RECOMP_WATCH is set. Costs a page fault per write to that page,
 * so it is a bring-up tool and says so.
 */
static uint32_t g_watch_va;
static void    *g_watch_page;
static uint32_t g_watch_last;
static void    *g_watch_veh;
static RECOMP_TLS int s_watch_stepping;

/* A plausible guest code address: inside the image's executable sections,
 * as recorded from the section headers at load. */
static int watch_is_code(uint32_t va)
{
    return va >= g_xbox_code_lo && va < g_xbox_code_hi;
}

static void watch_report(void)
{
    const uint8_t *mem = (const uint8_t *)g_memory_offset;
    uint32_t now = *(const uint32_t *)(mem + g_watch_va);
    uint32_t esp = g_esp, i, shown = 0;

    if (now == g_watch_last)
        return;
    fprintf(stderr, "[WATCH] [%08X] %08X -> %08X  (esp=%08X)\n",
            g_watch_va, g_watch_last, now, esp);
    g_watch_last = now;

    /* Return addresses the recompiled code pushed, innermost first. Values
     * that merely look like code get printed too -- the chain is a lead, not
     * a proof, and saying so is cheaper than a stack walk that cannot be
     * done without frame information the lift does not keep. */
    for (i = 0; esp && i < 256u && shown < 12u; i++) {
        uint32_t slot = esp + i * 4u;
        uint32_t v;
        if (!peek_readable(slot))
            break;
        v = *(const uint32_t *)(mem + slot);
        if (watch_is_code(v)) {
            fprintf(stderr, "         [esp+%-4u] %08X\n", i * 4u, v);
            shown++;
        }
    }

    /* RECOMP_WATCH_RAW also prints the frame unfiltered. The filtered chain
     * answers "who wrote this"; the raw frame answers "to what object", which
     * is the next question every time -- saved registers and pointer
     * arguments live there and look nothing like code. */
    if (getenv("RECOMP_WATCH_RAW")) {
        for (i = 0; esp && i < 24u; i++) {
            uint32_t slot = esp + i * 4u;
            if (!peek_readable(slot))
                break;
            fprintf(stderr, "         raw[esp+%-4u] %08X\n", i * 4u,
                    *(const uint32_t *)(mem + slot));
        }
    }
    fflush(stderr);
}

static LONG CALLBACK watch_veh(PEXCEPTION_POINTERS ep)
{
#ifdef _WIN32
    DWORD code = ep->ExceptionRecord->ExceptionCode;
    DWORD old;

    if (!g_watch_page)
        return EXCEPTION_CONTINUE_SEARCH;

    if (code == EXCEPTION_SINGLE_STEP && s_watch_stepping) {
        s_watch_stepping = 0;
        watch_report();
        VirtualProtect(g_watch_page, 4096, PAGE_READONLY, &old);
        ep->ContextRecord->EFlags &= ~0x100u;
        return EXCEPTION_CONTINUE_EXECUTION;
    }

    if (code == EXCEPTION_ACCESS_VIOLATION
            && ep->ExceptionRecord->ExceptionInformation[0] == 1) {
        uintptr_t fault = ep->ExceptionRecord->ExceptionInformation[1];

        if (fault >= (uintptr_t)g_watch_page
                && fault < (uintptr_t)g_watch_page + 4096) {
            if (!VirtualProtect(g_watch_page, 4096, PAGE_READWRITE, &old))
                return EXCEPTION_CONTINUE_SEARCH;
            s_watch_stepping = 1;
            ep->ContextRecord->EFlags |= 0x100u;
            return EXCEPTION_CONTINUE_EXECUTION;
        }
    }
#else
    /* No VEH on POSIX: AddVectoredExceptionHandler returns NULL there, so
     * the trap is never armed and this never runs. */
    (void)ep;
#endif
    return EXCEPTION_CONTINUE_SEARCH;
}

/* The target, which may be reached through pointers that do not exist yet.
 *
 * "[[0x006DF414]]+0x14" is two dereferences and an offset: the interesting
 * field of a heap object whose address changes run to run, but which is
 * always reachable from a static one. Without this the only way to watch such
 * a field is to learn its address from one run and hope the allocator repeats
 * it, which it does not. */
static unsigned g_watch_derefs;
static uint32_t g_watch_root;
static uint32_t g_watch_off;

static int watch_resolve(uint32_t *out)
{
    const uint8_t *mem = (const uint8_t *)g_memory_offset;
    uint32_t a = g_watch_root;
    unsigned k;

    for (k = 0; k < g_watch_derefs; k++) {
        if (!peek_readable(a))
            return 0;
        a = *(const uint32_t *)(mem + a);
        if (!a)
            return 0;
    }
    a += g_watch_off;
    if (!peek_readable(a))
        return 0;
    *out = a;
    return 1;
}

static int watch_arm(uint32_t va);

/* Poll until the chain resolves, then arm. Twenty milliseconds, because the
 * object appears once during bring-up and never again -- this thread exists
 * for a few seconds and then does nothing for the rest of the run. */
static DWORD WINAPI watch_resolver(LPVOID unused)
{
    unsigned tries;

    (void)unused;
    for (tries = 0; tries < 15000u && !g_watch_page; tries++) {
        uint32_t va;
        if (watch_resolve(&va) && watch_arm(va))
            return 0;
        Sleep(20);
    }
    if (!g_watch_page)
        fprintf(stderr, "  WATCH: %u-deep chain from 0x%08X never resolved\n",
                g_watch_derefs, g_watch_root);
    return 0;
}

void xbox_WatchInit(void)
{
    const char *spec = getenv("RECOMP_WATCH");
    const char *q;
    char *endp;

    if (!spec || !*spec || g_memory_base == NULL || g_watch_page)
        return;

    for (q = spec; *q == '['; q++)
        g_watch_derefs++;
    g_watch_root = (uint32_t)strtoul(q, &endp, 0);
    while (*endp == ']')
        endp++;
    if (*endp == '+')
        g_watch_off = (uint32_t)strtoul(endp + 1, NULL, 0);

    if (g_watch_derefs) {
        fprintf(stderr, "  WATCH: resolving %u-deep chain from 0x%08X "
                        "+0x%X\n", g_watch_derefs, g_watch_root, g_watch_off);
        CloseHandle(CreateThread(NULL, 0, watch_resolver, NULL, 0, NULL));
        return;
    }
    watch_arm(g_watch_root + g_watch_off);
}

static int watch_arm(uint32_t va)
{
    DWORD old;

    g_watch_va = va;
    if (!peek_readable(g_watch_va)) {
        fprintf(stderr, "  WATCH: 0x%08X is not in a mapped window; "
                        "not armed\n", g_watch_va);
        return 0;
    }
    g_watch_last = *(const uint32_t *)((const uint8_t *)g_memory_offset
                                       + g_watch_va);
    /* The page holding the guest dword, in host terms. */
    g_watch_page = (void *)(((uintptr_t)((const uint8_t *)g_memory_offset
                                         + g_watch_va)) & ~(uintptr_t)4095);
    g_watch_veh = AddVectoredExceptionHandler(1, watch_veh);
    if (!g_watch_veh
            || !VirtualProtect(g_watch_page, 4096, PAGE_READONLY, &old)) {
        if (g_watch_veh) {
            RemoveVectoredExceptionHandler(g_watch_veh);
            g_watch_veh = NULL;
        }
        g_watch_page = NULL;
        fprintf(stderr, "  WATCH: cannot trap 0x%08X; not armed\n",
                g_watch_va);
        return 0;
    }
    fprintf(stderr, "  WATCH: writes to the page of 0x%08X are trapped "
                    "(current %08X)\n", g_watch_va, g_watch_last);
    fflush(stderr);
    return 1;
}

/* Print the RECOMP_PEEK globals. Shared, because the two moments worth
 * sampling are a hang and an early exit, and only the first had it: a title
 * whose main() returns during init never reaches the watchdog, so the one
 * question that mattered -- which of its init calls failed -- was the one the
 * tooling could not answer. Silent unless RECOMP_PEEK is set. */
void xbox_PeekSample(const char *label)
{
    const uint8_t *mem = (const uint8_t *)g_memory_offset;
    const char *spec = getenv("RECOMP_PEEK");
    char buf[256], *q, *end;

    if (!spec || !*spec || g_memory_base == NULL)
        return;
    strncpy(buf, spec, sizeof buf - 1);
    buf[sizeof buf - 1] = 0;
    fprintf(stderr, "  %s:", label ? label : "peek");
    for (q = buf; *q; ) {
        /* "[[0x006DF414]]+0x14" follows two pointers and adds an offset.
         * The fields worth watching during bring-up are usually inside heap
         * objects whose addresses change run to run but which are always
         * reachable from a static one, and a peek that cannot follow a
         * pointer cannot see them at all. */
        unsigned derefs = 0, k;
        unsigned long va;
        uint32_t a;
        int ok = 1;

        while (*q == '[') { derefs++; q++; }
        va = strtoul(q, &end, 0);
        if (end == q)
            break;
        while (*end == ']')
            end++;
        a = (uint32_t)va;
        for (k = 0; k < derefs && ok; k++) {
            if (!peek_readable(a) || !(a = *(const uint32_t *)(mem + a)))
                ok = 0;
        }
        if (*end == '+')
            a += (uint32_t)strtoul(end + 1, &end, 0);
        if (ok && peek_readable(a))
            fprintf(stderr, " [%08X]=%08X", a, *(const uint32_t *)(mem + a));
        else
            fprintf(stderr, " [%08X]=??", a);
        q = (*end == ',') ? end + 1 : end;
    }
    fprintf(stderr, "\n");
    fflush(stderr);
}

/* The snapshot itself: the main guest thread's registers and stack, the
 * recent indirect calls, the pushbuffer pointers. Also taken on demand
 * (xbox_WatchdogDump), e.g. by a host that notices the title stopped
 * presenting. */
static void watchdog_dump(const char *why)
{
    const uint8_t *mem;
    uint32_t esp, i;

    mem = (const uint8_t *)g_memory_offset;
    esp = s_watchdog_esp ? *s_watchdog_esp : 0;
    fprintf(stderr, "[WATCHDOG] %s; guest esp=0x%08X\n"
            "  regs: eax=%08X ecx=%08X edx=%08X ebx=%08X esi=%08X edi=%08X\n",
            why, esp,
            s_watchdog_regs[0] ? *s_watchdog_regs[0] : 0,
            s_watchdog_regs[1] ? *s_watchdog_regs[1] : 0,
            s_watchdog_regs[2] ? *s_watchdog_regs[2] : 0,
            s_watchdog_regs[3] ? *s_watchdog_regs[3] : 0,
            s_watchdog_regs[4] ? *s_watchdog_regs[4] : 0,
            s_watchdog_regs[5] ? *s_watchdog_regs[5] : 0);
    /* The recent indirect-call targets name whatever is spinning: a stuck loop
     * inside a function reached through a pointer leaves no clue on the stack
     * beyond the return address of the call that entered it. */
    {
        uint32_t k;
        /* The running indirect-call total separates a hang from mere
         * slowness. Kernel calls cannot: a pure CPU loop makes none, so
         * "same count at 20s and 60s" proves nothing about it. */
        fprintf(stderr, "  icalls so far: %llu\n",
                (unsigned long long)g_icall_count);
        fprintf(stderr, "  recent ICALL targets:");
        for (k = 0; k < 16; k++)
            fprintf(stderr, " %08X",
                    g_icall_trace[(g_icall_trace_idx + k) & 15]);
        fprintf(stderr, "\n");
    }
    /* Guest globals worth seeing at the moment of the hang.
     *
     * RECOMP_PEEK is otherwise only sampled by the pushbuffer reporter, which
     * a title that hangs before rendering never reaches -- and a spin that
     * makes no kernel calls is invisible to RECOMP_KERNEL_WATCH too. A pure
     * CPU loop polling a global is exactly the case neither of those covers.
     */
    xbox_PeekSample("peek");
    /* The pushbuffer pointers, unconditionally.
     *
     * "Extend the table as more handshakes turn up -- run the title and the
     * watchdog sample will name the register" is only true if the sample
     * actually shows them. It did not: a title spinning on a DMA pointer made
     * no kernel calls and no indirect calls, so every other line the watchdog
     * prints was identical between two samples taken 40 seconds apart, and the
     * register that was stuck did not appear at all.
     *
     * Both sides of the channel, because which one the title consults is a
     * property of its D3D and not of the hardware: Halo waits on the USER
     * pair, DDS9 reads USER first and falls back to PFIFO's DMA_SUBROUTINE.
     * Printing only the pair that some other title used is how this stayed
     * invisible. */
    if (g_nv2a_memory) {
        const char *r = (const char *)g_nv2a_memory;
#define WD_NV2A(off) (*(const volatile uint32_t *)(r + (off)))
        fprintf(stderr, "  NV2A USER  PUT=%08X GET=%08X\n"
                        "  NV2A PFIFO PUT=%08X GET=%08X REF=%08X SUBR=%08X\n",
                WD_NV2A(NV2A_USER_DMA_PUT), WD_NV2A(NV2A_USER_DMA_GET),
                WD_NV2A(NV2A_PFIFO_DMA_PUT), WD_NV2A(NV2A_PFIFO_DMA_GET),
                WD_NV2A(NV2A_PFIFO_REF), WD_NV2A(NV2A_PFIFO_DMA_SUBROUTINE));
        /* The trap handshake: a fence wait that never ends is a NOP(5) trap
         * never posted, or posted and lost, and these tell which. */
        fprintf(stderr, "  NV2A PMC_INTR=%08X EN=%08X PGRAPH_INTR=%08X NSOURCE=%08X"
                        " TRAPPED_ADDR=%08X DATA=%08X FIFO=%08X hold=%d\n",
                WD_NV2A(0x000100), WD_NV2A(0x000140), WD_NV2A(0x400100),
                WD_NV2A(0x400108), WD_NV2A(0x400704), WD_NV2A(0x400708),
                WD_NV2A(0x400720), (int)g_nv2a_intr_hold);
#undef WD_NV2A
    }

    for (i = 0; i < 400 && esp; i++) {
        uint32_t a = esp + i * 4;
        if (a < XBOX_STACK_BASE || a >= XBOX_STACK_TOP) break;
        fprintf(stderr, "    GS %08X %08X\n", a,
                *(const uint32_t *)(mem + a));
    }
    fflush(stderr);
}

void xbox_WatchdogDump(const char *reason)
{
    watchdog_dump(reason != NULL ? reason : "snapshot");
}

static DWORD WINAPI xbox_watchdog_thread(LPVOID unused)
{
    char reason[64];
    const char *keep;

    (void)unused;
    Sleep(s_watchdog_secs * 1000u);
    snprintf(reason, sizeof reason, "still running after %us", s_watchdog_secs);
    watchdog_dump(reason);
    /* RECOMP_WATCHDOG_KEEP=1: a snapshot and nothing more -- on a slow host
     * a healthy boot can still be loading at the deadline. Otherwise the
     * run ends here. */
    keep = getenv("RECOMP_WATCHDOG_KEEP");
    fflush(stderr);
    if (keep != NULL && keep[0] == '1')
        return 0;
    _exit(3);
    return 0;
}

void xbox_WatchdogStart(void)
{
    const char *secs = getenv("RECOMP_WATCHDOG_SECS");
    HANDLE h;

    /* Taken on the guest thread: g_esp is thread-local, so the watchdog has to
     * be handed the address of the one that matters rather than reading its
     * own, which is always zero. Always, so xbox_WatchdogDump works without
     * the timed watchdog. */
    s_watchdog_esp = &g_esp;
    s_watchdog_regs[0] = &g_eax; s_watchdog_regs[1] = &g_ecx;
    s_watchdog_regs[2] = &g_edx; s_watchdog_regs[3] = &g_ebx;
    s_watchdog_regs[4] = &g_esi; s_watchdog_regs[5] = &g_edi;
    if (!secs || !*secs)
        return;
    s_watchdog_secs = (unsigned)atoi(secs);
    if (!s_watchdog_secs)
        return;
    h = CreateThread(NULL, 0, xbox_watchdog_thread, NULL, 0, NULL);
    if (h)
        CloseHandle(h);
}

/* SSE. 128 bits of architectural state, per-thread like the rest. */
RECOMP_TLS RecompMmx g_mm0, g_mm1, g_mm2, g_mm3;
RECOMP_TLS RecompMmx g_mm4, g_mm5, g_mm6, g_mm7;
RECOMP_TLS RecompXmm g_xmm0, g_xmm1, g_xmm2, g_xmm3;
RECOMP_TLS RecompXmm g_xmm4, g_xmm5, g_xmm6, g_xmm7;
/* Last frame established by `mov ebp, esp`. Read by frameless functions
 * that address their caller's frame through ebp. */
RECOMP_TLS uint32_t g_ebp = 0;

/* EFLAGS.DF. Zero means the string instructions walk forwards, which is the
 * ABI's resting state and what almost every one of them does -- so this is
 * almost always 0 and costs a predictable branch. The exceptions are the ones
 * that matter: MSVC's strrchr/wcsrchr scan backwards from the terminator with
 * `std; repne scasb`, and memmove goes backwards when its regions overlap the
 * wrong way. Thread-local, because `std` and the `cld` that undoes it can land
 * in different lifted bodies of the same guest routine. */
RECOMP_TLS int g_df = 0;

/* ICALL trace ring buffer */
volatile uint32_t g_icall_trace[16] = {0};
volatile uint32_t g_icall_trace_idx = 0;

/* One line for a periodic report (the Switch's [perf]): indirect and kernel
 * calls since the last sample and the latest indirect-call targets. A title
 * spinning in lifted code keeps the indirect count moving with the same few
 * targets; one blocked in the kernel moves neither. */
long long xbox_kernel_call_count(void);

void xbox_perf_sample(void)
{
    static uint64_t icalls_before;
    static long long kcalls_before;
    uint64_t icalls = g_icall_count;
    long long kcalls = xbox_kernel_call_count();
    uint32_t newest = g_icall_trace_idx;
    char targets[6 * 9 + 1];
    int k, len = 0;

    /* The six newest entries of the 16-slot ring, newest first. */
    for (k = 1; k <= 6; k++)
        len += snprintf(targets + len, sizeof targets - (size_t)len, " %08X",
                        g_icall_trace[(newest - (uint32_t)k) & 15]);
    fprintf(stderr, "[perf] guest: %llu indirect calls, %lld kernel calls; last targets%s\n",
            (unsigned long long)(icalls - icalls_before), kcalls - kcalls_before, targets);
    icalls_before = icalls;
    kcalls_before = kcalls;
}
volatile uint64_t g_icall_count = 0;

BOOL xbox_MemoryLayoutInit(const void *xbe_data, size_t xbe_size)
{
    g_force_return = getenv("RECOMP_FORCE_RETURN") != NULL;
    DWORD old_protect;
    const uint8_t *xbe = (const uint8_t *)xbe_data;

    if (g_memory_base) {
        fprintf(stderr, "xbox_MemoryLayoutInit: already initialized\n");
        return FALSE;
    }

    /*
     * Calculate the full range we need to map.
     * From XBOX_MAP_START (0x0) to the end of the furthest section.
     * This includes low memory (KPCR at 0x0-0xFF) which game code reads
     * from, the XBE sections, and the simulated stack.
     */
    /* Map the full Xbox address space (covers all sections + stack + heap).
     * Size is runtime-configurable: retail 64 MB, devkit debug builds 128 MB. */
    /* The mapped range, which is not necessarily RAM. Mirrors are placed
     * at multiples of this, so growing it is what stops a title's
     * above-RAM allocations from aliasing low memory. */
    g_memory_size = g_xbox_map_size ? g_xbox_map_size : g_xbox_total_ram;

    /*
     * Create a file mapping backed by the page file.
     *
     * Using file mapping instead of VirtualAlloc allows us to map the same
     * physical pages at multiple virtual addresses via MapViewOfFileEx.
     * This is critical for the Xbox RAM mirror: the Xbox memory controller
     * uses a 26-bit address bus, so ALL addresses wrap modulo 64 MB.
     * Code that writes to address 0x20000448 is really writing to 0x00000448.
     * With file mapping views, we create aliased mappings at 64 MB intervals
     * that all point to the same physical memory.
     */
    g_mapping_handle = CreateFileMappingA(
        INVALID_HANDLE_VALUE,   /* page file backed */
        NULL,                   /* default security */
        PAGE_READWRITE,         /* read-write access */
        0,                      /* high DWORD of size */
        (DWORD)g_memory_size,   /* low DWORD of size (64 MB) */
        NULL                    /* unnamed mapping */
    );
    if (!g_mapping_handle) {
        fprintf(stderr, "xbox_MemoryLayoutInit: CreateFileMapping failed (error %lu)\n",
                GetLastError());
        return FALSE;
    }

    /*
     * Map the base view at the desired virtual address.
     * Try the original Xbox base address first. If that fails (common on
     * Windows 11 where low addresses are often reserved), try page-aligned
     * addresses upward until we find a free region.
     */
    {
        static const uintptr_t try_bases[] = {
            XBOX_BASE_ADDRESS,      /* 0x00010000 - original Xbox address */
            0x00800000,             /* 8 MB - above typical PEB/TEB region */
            0x01000000,             /* 16 MB */
            0x02000000,             /* 32 MB */
            0x10000000,             /* 256 MB */
            0,                      /* sentinel - let OS choose */
        };

        /* Iterate the whole array, sentinel included. The old condition
         * (try_bases[i] != 0 || i == 0) stopped *at* the zero rather than
         * using it, so the "let the OS choose" fallback never ran: the loop
         * tried the fixed addresses and gave up. Invisible on Windows, where
         * one of the low bases succeeds -- fatal on arm64 macOS, where all of
         * them sit inside the 4 GB __PAGEZERO segment and none can. */
        /* Reserve base + mirrors as one range, and map the base at its head.
         * VirtualFree releases just the slice about to be used, so each view
         * replaces our own reservation rather than racing for free space. */
        g_span_size = g_memory_size * (size_t)(1 + XBOX_NUM_MIRRORS);
        if (g_span_size > (size_t)XBOX_CONTIG_BASE)
            g_span_size = (size_t)XBOX_CONTIG_BASE;
        {
            LPVOID span_hint = NULL;
#if defined(__linux__)
            /* A bare mmap lands just under the 47-bit ceiling, leaving no room
             * for the apertures at base + 0xFD000000. Pick a base with the full
             * 4 GB guest space free. */
            void *probe = mmap(NULL, 0x100000000ULL, PROT_NONE,
                               MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
            if (probe != MAP_FAILED) {
                munmap(probe, 0x100000000ULL);
                span_hint = probe;
            }
#endif
            g_span_base = VirtualAlloc(span_hint, g_span_size, MEM_RESERVE, PAGE_NOACCESS);
        }
        if (g_span_base) {
            VirtualFree(g_span_base, g_memory_size, MEM_RELEASE);
            g_memory_base = MapViewOfFileEx(g_mapping_handle,
                                            FILE_MAP_ALL_ACCESS, 0, 0,
                                            g_memory_size, g_span_base);
            if (!g_memory_base) {
                VirtualFree(g_span_base, g_span_size, MEM_RELEASE);
                g_span_base = NULL;
                g_span_size = 0;
            }
        }

        const size_t n_bases = sizeof(try_bases) / sizeof(try_bases[0]);
        for (size_t i = 0; !g_memory_base && i < n_bases; i++) {
            LPVOID hint = try_bases[i] ? (LPVOID)try_bases[i] : NULL;
            g_memory_base = MapViewOfFileEx(
                g_mapping_handle,
                FILE_MAP_ALL_ACCESS,
                0, 0,           /* offset into mapping */
                g_memory_size,  /* size */
                hint            /* desired base address */
            );
            if (g_memory_base) {
                if (try_bases[i] != 0 && (uintptr_t)g_memory_base != try_bases[i]) {
                    /* OS gave us a different address, retry */
                    UnmapViewOfFile(g_memory_base);
                    g_memory_base = NULL;
                    continue;
                }
                break;
            }
        }
    }

    if (!g_memory_base) {
        fprintf(stderr, "xbox_MemoryLayoutInit: failed to map base view (%zu KB)\n",
                g_memory_size / 1024);
        CloseHandle(g_mapping_handle);
        g_mapping_handle = NULL;
        return FALSE;
    }

    g_memory_offset = (uintptr_t)g_memory_base - XBOX_MAP_START;

    /* Guest page zero: no access.
     *
     * Nothing legitimate lives there -- every XBE's image base is 0x00010000
     * and the TIB now sits at XBOX_FS_BASE -- so any access is a null pointer
     * the title dereferenced. Left readable it did quiet damage: a null check
     * of the form `cmp byte [ecx], 0` read whatever happened to be at 0 and
     * decided the pointer was fine, and a store through a null pointer landed
     * on real memory and surfaced as corruption somewhere unrelated. Faulting
     * here turns both into one access violation at the instruction that made
     * the mistake, which the crash handler can name.
     *
     * Opt-in through RECOMP_TRAP_NULL, because it converts a class of bug the
     * title currently survives into a hard stop: a guest that dereferences null
     * and ignores the result keeps running while page zero reads as zero, and
     * stops dead once it faults. That is the right default for hunting one of
     * these and the wrong one for making progress past the rest, so it is a
     * switch rather than a policy.
     *
     * Note this is separate from moving the TIB off page zero, which is not
     * optional: with the TIB gone, address 0 reads as plain zero, so a null
     * check written as a load through the pointer now gets the answer it
     * expects whether or not the page is trapped.
     *
     * Best-effort: failing to protect it costs only the diagnostic. */
    if (XBOX_MAP_START == 0 && getenv("RECOMP_TRAP_NULL")) {
        DWORD old_protect;
        /* Protection is applied at host page granularity, and the host page
         * is not always the guest's 4 KB -- Apple Silicon uses 16 KB, so this
         * 0x1000 request actually covers guest 0..0x3FFF. That is why
         * XBOX_TIB_MAIN sits at 0x4000: the widest page any supported host
         * uses fits below the TIB, so the rounding costs nothing and the
         * guard installs everywhere.
         *
         * The check below is what remains of an earlier bug rather than dead
         * code. With the TIB at 0x1000 the rounding reached it, init wrote the
         * TIB moments later, and every run that asked for the guard died at
         * startup -- so the guard disabled itself on all of Apple Silicon and
         * the diagnostic silently did nothing. It stays as a floor for a host
         * with pages wider than the TIB offset, where skipping really is
         * better than breaking the run. */
#if defined(_WIN32)
        SYSTEM_INFO si;
        long host_page;
        GetSystemInfo(&si);
        host_page = (long)si.dwPageSize;
#else
#if defined(__SWITCH__)
        long host_page = 4096;          /* Horizon pages are 4 KB */
#else
        long host_page = sysconf(_SC_PAGESIZE);
#endif
#endif
        if (host_page > 0 && (uint32_t)host_page > XBOX_TIB_MAIN) {
            fprintf(stderr, "  RECOMP_TRAP_NULL: not available -- the host page "
                    "is %ld bytes, so trapping guest page zero would also trap "
                    "the TIB at 0x%08X\n", host_page, XBOX_TIB_MAIN);
        } else {
            if (VirtualProtect(g_memory_base, 0x1000, PAGE_NOACCESS, &old_protect)) {
                fprintf(stderr, "  guest page 0 is PAGE_NOACCESS"
                                " (null dereferences fault)\n");
            }
        }
    }

    if (g_memory_offset == 0) {
        fprintf(stderr, "xbox_MemoryLayoutInit: mapped %zu KB at 0x%08X (original Xbox address)\n",
                g_memory_size / 1024, XBOX_MAP_START);
    } else {
        fprintf(stderr, "xbox_MemoryLayoutInit: mapped %zu KB at 0x%p (offset %+td from Xbox base)\n",
                g_memory_size / 1024, g_memory_base, g_memory_offset);
    }

    /*
     * Helper macro: convert Xbox VA to actual mapped address.
     * When g_memory_offset == 0 (ideal case), this is identity.
     */
    #define XBOX_VA(va) ((void *)((uintptr_t)(va) + g_memory_offset))

    /*
     * Copy XBE header to base address.
     * The Xbox kernel maps the XBE image header at 0x00010000.
     * Game code reads kernel thunk table, certificate data, and
     * section info from this region.
     */
    {
        /* XBE header size is at file offset 0x0108 (SizeOfImageHeader) */
        DWORD header_size = 0;
        if (xbe_size >= 0x10C) {
            header_size = *(const DWORD *)(xbe + 0x0108);
        }
        if (header_size == 0 || header_size > 0x10000)
            header_size = 0x1000;  /* fallback: 4KB */
        if (header_size > xbe_size)
            header_size = (DWORD)xbe_size;
        memcpy(XBOX_VA(XBOX_BASE_ADDRESS), xbe, header_size);
        fprintf(stderr, "  XBE header: %u bytes at %p (Xbox VA 0x%08X)\n",
                header_size, XBOX_VA(XBOX_BASE_ADDRESS), XBOX_BASE_ADDRESS);
    }

    /*
     * Dynamically load ALL XBE sections by parsing the section headers.
     *
     * This replaces the old approach of hardcoding section addresses for
     * a specific game (Burnout 3). By reading the section table from the
     * XBE header, any game's sections are loaded automatically.
     *
     * Every section is copied to its original Xbox VA:
     * - .text: needed because memory walkers may scan code pages
     * - .rdata: constants, vtables, kernel thunk table
     * - .data: global variables (initialized portion from XBE, BSS zeroed)
     * - XDK library sections (D3D, DSOUND, WMADEC, XPP, etc.)
     * - DOLBY, BINK, XTIMAGE, etc.
     */
    {
        DWORD base_addr = *(const DWORD *)(xbe + XBE_BASE_ADDR_OFFSET);
        DWORD num_sections = *(const DWORD *)(xbe + XBE_SECTION_COUNT_OFFSET);
        DWORD sect_headers_va = *(const DWORD *)(xbe + XBE_SECTION_HEADERS_OFFSET);
        DWORD sect_headers_off = sect_headers_va - base_addr;
        int sections_loaded = 0;
        int sections_short = 0;
        size_t total_bytes = 0;

        if (num_sections > 64) num_sections = 64;  /* sanity cap */

        fprintf(stderr, "  XBE sections: %u (headers at file offset 0x%08X)\n",
                num_sections, sect_headers_off);

        for (DWORD si = 0; si < num_sections; si++) {
            if (sect_headers_off + (si + 1) * SECTHDR_SIZE > xbe_size) break;

            const uint8_t *sh = xbe + sect_headers_off + si * SECTHDR_SIZE;
            DWORD sec_va       = *(const DWORD *)(sh + SECTHDR_VA);
            DWORD sec_vsize    = *(const DWORD *)(sh + SECTHDR_VSIZE);
            DWORD sec_raw_off  = *(const DWORD *)(sh + SECTHDR_RAW_OFFSET);
            DWORD sec_raw_size = *(const DWORD *)(sh + SECTHDR_RAW_SIZE);
            DWORD sec_name_va  = *(const DWORD *)(sh + SECTHDR_NAME_ADDR);

            /* Read section name from XBE header */
            const char *sec_name = "?";
            DWORD name_off = sec_name_va - base_addr;
            if (name_off < xbe_size && name_off + 8 <= xbe_size)
                sec_name = (const char *)(xbe + name_off);

            /* Validate: section must fit within our 64MB mapped region */
            if (sec_va < XBOX_BASE_ADDRESS || sec_va + sec_vsize > XBOX_TOTAL_RAM)
                continue;

            /* Determine copy size (raw_size may exceed vsize due to alignment) */
            DWORD copy_size = (sec_raw_size < sec_vsize) ? sec_raw_size : sec_vsize;

            /* Zero the full virtual size first (handles BSS) */
            memset(XBOX_VA(sec_va), 0, sec_vsize);

            /*
             * Copy initialized data from XBE.
             *
             * A section whose raw data runs past the end of the buffer is a
             * truncated or corrupt image, not a BSS section, and it must not
             * be counted among the sections loaded. Reporting it as loaded is
             * how a 4MB title read through a 1MB buffer produced "Loaded
             * 17/17 sections" with every byte of every section still zero --
             * including the kernel thunk table, which then resolved 0 imports
             * and looked like a title that calls no kernel functions.
             */
            int have_data = (copy_size == 0) ||
                            (sec_raw_off + copy_size <= xbe_size);
            if (copy_size > 0 && have_data) {
                memcpy(XBOX_VA(sec_va), xbe + sec_raw_off, copy_size);
            } else if (!have_data) {
                fprintf(stderr,
                        "  WARNING: section %u (%s) raw data 0x%08X+%u runs past "
                        "the %zu-byte image -- left zeroed\n",
                        si, sec_name, sec_raw_off, copy_size, xbe_size);
                sections_short++;
            }

            /* Every loaded section, executable or not. Anything that writes
             * guest memory from outside the title -- the pushbuffer executor
             * clearing a surface, say -- needs to know where the title itself
             * lives, because scribbling on it is not a rendering artefact, it
             * is the title's code and globals gone. */
            if (!g_xbox_image_lo || sec_va < g_xbox_image_lo)
                g_xbox_image_lo = sec_va;
            if (sec_va + sec_vsize > g_xbox_image_hi)
                g_xbox_image_hi = sec_va + sec_vsize;

            /* Executable sections define the range indirect calls may target.
             * XBE section flag 0x04 is EXECUTABLE. */
            if (*(const DWORD *)(sh + SECTHDR_FLAGS) & 0x00000004u) {
                if (!g_xbox_code_lo || sec_va < g_xbox_code_lo)
                    g_xbox_code_lo = sec_va;
                if (sec_va + sec_vsize > g_xbox_code_hi)
                    g_xbox_code_hi = sec_va + sec_vsize;
            }

            if (have_data) {
                sections_loaded++;
                total_bytes += copy_size;
            }

            fprintf(stderr, "  [%2u] %-12s VA=0x%08X vsize=%-8u raw=0x%08X rsize=%-8u%s\n",
                    si, sec_name, sec_va, sec_vsize, sec_raw_off, sec_raw_size,
                    (sec_raw_size < sec_vsize) ? " (BSS)" : "");
        }

        fprintf(stderr, "  Loaded %d/%u sections (%zu bytes total)\n",
                sections_loaded, num_sections, total_bytes);
        if (sections_short) {
            fprintf(stderr,
                    "  ERROR: %d section(s) had no data in the image -- the XBE "
                    "is truncated or was read short; the title will not run\n",
                    sections_short);
        }
    }

    /*
     * Parse the kernel thunk table address from the XBE header.
     * The XBE stores KernelImageThunkAddress at offset 0x0158, XOR-encrypted.
     * The key differs between retail and debug XBEs, and there is no flag
     * saying which was used -- decode with both and keep whichever lands in
     * the mapped address range (this is what tools/xbe_parser does).
     *
     * Debug XBEs are not an edge case here: they are the builds most worth
     * recompiling, since they still carry assert strings and symbols. Halo's
     * cachebeta.xbe is one, and assuming the retail key decoded its thunk
     * table to 0xB4F98174 instead of 0x00253090, which silently fell back to
     * the compile-time default and resolved 0 of 378 kernel imports.
     */
    if (xbe_size >= 0x015C) {
        uint32_t thunk_raw = *(const uint32_t *)(xbe + 0x0158);
        uint32_t thunk_retail = thunk_raw ^ 0x5B6D40B6;  /* retail XOR key */
        uint32_t thunk_debug  = thunk_raw ^ 0xEFB1F152;  /* debug XOR key  */
        uint32_t thunk_va;

        if (thunk_retail >= XBOX_BASE_ADDRESS && thunk_retail < XBOX_TOTAL_RAM) {
            thunk_va = thunk_retail;
        } else {
            thunk_va = thunk_debug;
        }

        /* Validate: thunk VA should be within our mapped region */
        if (thunk_va >= XBOX_BASE_ADDRESS && thunk_va < XBOX_TOTAL_RAM) {
            /* Count thunk entries by scanning until we hit 0 */
            uint32_t thunk_count = 0;
            /* XBOX_KERNEL_THUNK_TABLE_SIZE, not 366: the kernel exports 378
             * slots, and kernel.h notes 366 is short by 12. A title importing
             * a high ordinal would have had its table truncated here. */
            for (uint32_t t = 0; t < XBOX_KERNEL_THUNK_TABLE_SIZE; t++) {
                uint32_t entry = *(volatile uint32_t *)((uintptr_t)(thunk_va + t * 4) + g_memory_offset);
                if (entry == 0) break;
                thunk_count++;
            }
            xbox_kernel_set_thunk_address(thunk_va, thunk_count);
            fprintf(stderr, "  Kernel thunks: %u entries at Xbox VA 0x%08X\n",
                    thunk_count, thunk_va);
        } else {
            fprintf(stderr, "  WARNING: kernel thunk VA 0x%08X out of range (raw=0x%08X)\n",
                    thunk_va, thunk_raw);
        }
    }

    /*
     * NOTE: .rdata is NOT set read-only.
     * VirtualProtect rounds to page boundaries, and the .rdata end (0x003B2454)
     * and .data start (0x003B2360) share the same 4KB page (0x003B2000-0x003B2FFF).
     * Making .rdata read-only also makes the first ~0xCA0 bytes of .data read-only,
     * which causes game initialization code to fault when writing to .data globals
     * in that overlap range.
     */
    (void)old_protect;

    #undef XBOX_VA

    /* Set the global offset for recompiled code MEM macros */
    g_xbox_mem_offset = g_memory_offset;

    /*
     * Initialize the Xbox stack for recompiled code.
     * The stack area lives at XBOX_STACK_BASE in Xbox address space.
     * g_esp is the global stack pointer shared by all translated functions.
     */
    g_esp = XBOX_STACK_TOP;
    fprintf(stderr, "  Stack: %u KB at Xbox VA 0x%08X (ESP = 0x%08X)\n",
            XBOX_STACK_SIZE / 1024, XBOX_STACK_BASE, g_esp);

    /*
     * Populate the fake Thread Information Block (TIB) at Xbox VA 0x0.
     *
     * The original Xbox code uses fs:[offset] to read per-thread data,
     * but the recompiler drops the fs: segment prefix and generates
     * MEM32(offset) instead. Since we mapped low memory (0x0-0xFFFF),
     * we populate the TIB fields that game code accesses:
     *
     *   fs:[0x00] = SEH exception list (-1 = end of chain)
     *   fs:[0x04] = stack base (top of stack)
     *   fs:[0x08] = stack limit (bottom of stack)
     *   fs:[0x18] = self pointer (TIB address)
     *   fs:[0x20] = KPCR Prcb pointer (→ fake structure)
     *   fs:[0x28] = TLS / RW engine context pointer
     *
     * We use free space in the BSS area for the fake structures.
     */
    {
        #define XBOX_VA(va) ((void *)((uintptr_t)(va) + g_memory_offset))
        #define MEM32_INIT(va, val) (*(uint32_t *)XBOX_VA(va) = (uint32_t)(val))

        /* Fake TIB at address 0x0 */
        MEM32_INIT(XBOX_FS_BASE + 0x00, 0xFFFFFFFF);       /* SEH: end of chain */
        MEM32_INIT(XBOX_FS_BASE + 0x04, XBOX_STACK_TOP);   /* Stack base (high address) */
        MEM32_INIT(XBOX_FS_BASE + 0x08, XBOX_STACK_BASE);  /* Stack limit (low address) */
        MEM32_INIT(XBOX_FS_BASE + 0x18, XBOX_FS_BASE);     /* Self pointer */

        /*
         * fs:[0x20] - On Xbox KPCR, this is the Prcb pointer.
         * Game code reads [fs:[0x20] + 0x250] which on the real Xbox
         * accesses a D3D cache structure. We set it to 0 so the read
         * at offset 0x250 returns 0, causing the cache init to be skipped.
         */
        /* A zeroed block rather than a null pointer. The read is
         * [fs:[0x20] + 0x250], and this used to be left at 0 so that read
         * landed on guest address 0x250 and returned zero by accident -- which
         * only worked while page zero was mapped. Pointing at real zeroed
         * memory says the same thing to the title and survives that page being
         * unmapped, which is what makes a genuine null dereference visible. */
        #define FAKE_PRCB_VA 0x00761000  /* zeroed KPCR Prcb stand-in */
        memset(XBOX_VA(FAKE_PRCB_VA), 0, 0x400);
        MEM32_INIT(XBOX_FS_BASE + 0x20, FAKE_PRCB_VA);
        #undef FAKE_PRCB_VA

        /*
         * fs:[0x28] - Thread local storage / RW engine context.
         * The RW engine reads [fs:[0x28] + 0x28] to get a pointer
         * to its data area. We allocate a fake structure at 0x00760000
         * (in the BSS area) and a data buffer at 0x00700000.
         */
        #define FAKE_TLS_VA     0x00760000  /* Fake TLS structure (in BSS) */
        #define FAKE_RWDATA_VA  0x00700000  /* RW engine data area (in BSS) */

        MEM32_INIT(XBOX_FS_BASE + 0x28, FAKE_TLS_VA);
        /* TLS[0x28] = pointer to RW data area */
        MEM32_INIT(FAKE_TLS_VA + 0x28, FAKE_RWDATA_VA);

        /*
         * XBE TLS directory.
         *
         * An image with __declspec(thread) data carries one, and on hardware
         * the loader acts on it. Nothing here did, so thread-local access read
         * whatever memory happened to be under fs:[4].
         *
         * Xbox reaches thread-local data through NtTib.StackBase -- fs:[4] --
         * not Win32's fs:[0x2C], and the block sits BELOW that pointer: the
         * image's entry point computes its own index, negative, as
         * -(blocksize/4). Wreckless does this at guest 0x000EB57E and arrives
         * at -5 for its 20-byte block, so [fs:[4] + index*4] is the block's
         * first dword. The rounding below mirrors that arithmetic exactly,
         * because fs:[4] has to land where the title's own index says it is.
         *
         * The index itself is deliberately NOT written here: the title
         * computes and stores it. What the loader owes it is a block in the
         * right place.
         *
         * Slot 0 holds a pointer to per-thread data -- XAPI's SetLastError is
         * [[fs:[4] + index*4] + 4] = err -- so it gets a zeroed block rather
         * than being left NULL, which had SetLastError writing the error code
         * over fs:[4] itself and the next call faulting at guest 0xFFFFFFEF.
         *
         * ponytail: one block for the whole process, not one per thread.
         * Every guest thread therefore shares LastError. Give this a per-thread
         * allocation when a title is observed to care.
         */
        #define FAKE_TLS_BLOCK_VA  0x00770000  /* image TLS data          */
        #define FAKE_TLS_THREAD_VA 0x00770200  /* what slot 0 points at   */
        {
            DWORD tls_dir_va = *(const DWORD *)(xbe + XBE_TLS_ADDR_OFFSET);

            if (tls_dir_va) {
                const uint32_t *tls = (const uint32_t *)XBOX_VA(tls_dir_va);
                uint32_t data_start = tls[0];
                uint32_t data_end   = tls[1];
                uint32_t zero_fill  = tls[4];
                uint32_t init_size  = (data_end > data_start)
                                    ? data_end - data_start : 0;
                uint32_t total      = ((init_size + zero_fill + 0xF) & ~0xFu) + 4;

                memset(XBOX_VA(FAKE_TLS_BLOCK_VA), 0, total);
                memset(XBOX_VA(FAKE_TLS_THREAD_VA), 0, 64);
                if (init_size)
                    memcpy(XBOX_VA(FAKE_TLS_BLOCK_VA),
                           XBOX_VA(data_start), init_size);

                MEM32_INIT(FAKE_TLS_BLOCK_VA, FAKE_TLS_THREAD_VA);
                MEM32_INIT(XBOX_FS_BASE + 0x04, FAKE_TLS_BLOCK_VA + total);

                g_tls_template_va = FAKE_TLS_BLOCK_VA;
                g_tls_total       = total;

                fprintf(stderr, "  TLS: %u-byte block at 0x%08X,"
                        " fs:[4] = 0x%08X (index will be %d)\n",
                        total, FAKE_TLS_BLOCK_VA, FAKE_TLS_BLOCK_VA + total,
                        -(int)(total / 4));
            }
        }
        #undef FAKE_TLS_BLOCK_VA
        #undef FAKE_TLS_THREAD_VA

        fprintf(stderr, "  TIB: fake TIB at VA 0x%X, TLS at 0x%08X, RW data at 0x%08X\n",
                XBOX_FS_BASE, FAKE_TLS_VA, FAKE_RWDATA_VA);

        #undef FAKE_TLS_VA
        #undef FAKE_RWDATA_VA
        #undef MEM32_INIT
        #undef XBOX_VA
    }

    /*
     * Contiguous / physical memory window at 0x80000000.
     *
     * MmAllocateContiguousMemory hands back addresses in this window: physical
     * page P is visible at 0x80000000 + P. Titles that pin buffers at fixed
     * physical addresses then use the whole range, so it has to be backed for
     * its full length - Halo pins 3.4 MB at 0x61000 and 22 MB at 0x3A6000, and
     * with only the fake kernel page mapped here a write walked off the end of
     * it a few pages in.
     *
     * Deliberately NOT a view of the 64 MB RAM mapping. On hardware this window
     * aliases physical RAM, but we load the XBE image into the low addresses of
     * that same region, so aliasing would put a title's pinned pools on top of
     * its own code. Separate storage costs an extra mapping and behaves
     * correctly; nothing here depends on the aliasing.
     *
     * Reserved before the kernel page below, which lives inside it.
     */
    {
        uintptr_t contig_native = XBOX_CONTIG_BASE + g_memory_offset;
        g_contig_mapping = CreateFileMappingW(
            INVALID_HANDLE_VALUE, NULL, PAGE_READWRITE,
            0, (DWORD)XBOX_CONTIG_SIZE, NULL);
        g_contig_memory = g_contig_mapping
            ? MapViewOfFileEx(g_contig_mapping, FILE_MAP_ALL_ACCESS,
                              0, 0, XBOX_CONTIG_SIZE, (LPVOID)contig_native)
            : NULL;
        if (!g_contig_memory)
            g_contig_memory = VirtualAlloc(
                (LPVOID)contig_native,
                XBOX_CONTIG_SIZE,
                MEM_RESERVE | MEM_COMMIT,
                PAGE_READWRITE
            );
        if (g_contig_memory) {
            fprintf(stderr, "  Contiguous window: %u MB at Xbox VA 0x%08X (native %p)\n",
                    XBOX_CONTIG_SIZE / (1024 * 1024), XBOX_CONTIG_BASE, g_contig_memory);
            /* Verify write accessibility */
            *(volatile uint32_t *)g_contig_memory = 0xA1B2C3D4u;
            if (*(volatile uint32_t *)g_contig_memory != 0xA1B2C3D4u) {
                fprintf(stderr, "  WARNING: contiguous window write test failed!\n");
            }
            *(volatile uint32_t *)g_contig_memory = 0;
        } else {
            fprintf(stderr, "  WARNING: contiguous window at 0x%08X failed "
                    "(error %lu); pinned physical allocations will fault\n",
                    XBOX_CONTIG_BASE, GetLastError());
        }
    }

    /*
     * NV2A hardware register aperture at 0xFD000000 (16 MB).
     *
     * The GPU's registers are memory-mapped here on real hardware. A title
     * that only calls D3D never notices, but the D3D8 library is linked into
     * the XBE rather than provided by the kernel, so once execution is inside
     * it the register pokes are just loads and stores in recompiled code.
     * Halo faults reading 0xFD001804 during rasterizer_preinitialize, a few
     * instructions after Direct3DCreate8 returns.
     *
     * Backed as ordinary zeroed RAM. That is enough to get through
     * initialisation, and reads returning zero are the benign answer for the
     * status and capability registers touched here.
     *
     * ponytail: plain memory, no register semantics. A spin loop waiting for
     * a bit to *set* would hang rather than fault -- if that shows up, the fix
     * is to bridge the D3D8 entry point that owns the loop, not to start
     * emulating NV2A. Nothing has needed that yet.
     */
    {
        uintptr_t nv2a_native = XBOX_NV2A_BASE + g_memory_offset;
        g_nv2a_memory = VirtualAlloc(
            (LPVOID)nv2a_native,
            XBOX_NV2A_SIZE,
            MEM_RESERVE | MEM_COMMIT,
            PAGE_READWRITE
        );
        /* The pushbuffer survey rides on the same poll, so either
         * variable arms it. */
        s_nv2a_trace = getenv("RECOMP_NV2A_TRACE") != NULL
                    || getenv("RECOMP_PB_SCAN") != NULL
                    || getenv("RECOMP_PB_EXEC") != NULL;
        if (g_nv2a_memory) {
            fprintf(stderr, "  NV2A register aperture: %u MB at Xbox VA "
                    "0x%08X (zeroed, no register semantics)\n",
                    XBOX_NV2A_SIZE / (1024 * 1024), XBOX_NV2A_BASE);
        } else {
            fprintf(stderr, "  WARNING: NV2A aperture at 0x%08X failed "
                    "(error %lu); D3D register access will fault\n",
                    XBOX_NV2A_BASE, GetLastError());
        }
    }

    /*
     * MCPX device apertures.
     *
     * The NV2A block above is not the only hardware the title touches
     * directly. The southbridge devices live higher up:
     *
     *   0xFE800000  APU (audio processing unit)
     *   0xFEC00000  AC97
     *   0xFED00000  USB0 / USB1
     *   0xFEF00000  NIC
     *
     * Halo faults reading 0xFED00000 during input initialisation -- the XDK's
     * USB code talks to the host controller's registers rather than going
     * through a driver. Back the whole span as plain RAM for the same reason
     * the NV2A aperture is backed: a read of zero is survivable, a fault is
     * not.
     *
     * ponytail: no register semantics anywhere in here. If something spins
     * waiting for a bit to set, extend the NV2A ack thread's table rather than
     * emulating the device.
     */
    {
        uintptr_t mcpx_native = XBOX_MCPX_BASE + g_memory_offset;
        g_mcpx_memory = VirtualAlloc(
            (LPVOID)mcpx_native,
            XBOX_MCPX_SIZE,
            MEM_RESERVE | MEM_COMMIT,
            PAGE_READWRITE
        );
        g_mcpx_regs = g_mcpx_memory;
        if (g_mcpx_memory) {
            /* AC'97 codec ready.
             *
             * DirectSound resets the codec by setting a bit in 0xFEC0012C and
             * then polls 0xFEC00130 for bit 8 a thousand times waiting for the
             * codec to come up. On zeroed registers that bit never appears, so
             * the wait times out and DirectSoundCreate returns DSERR_NODRIVER
             * (0x88780078).
             *
             * That failure is not confined to audio. Wreckless initialises its
             * whole engine object behind `if (DirectSoundCreate() >= 0)`, so a
             * failed create skips the initialisation, leaves the object's table
             * pointer null, and the null propagates: a null-derived divisor
             * produces a NaN transform matrix, which produces a garbage index,
             * which crashes. Reporting the codec as present is what lets the
             * engine initialise at all.
             *
             * The aperture is plain memory, so setting the bit once is enough:
             * nothing clears it, and the poll reads it on the first pass. */
            #define MCPX_AC97_CODEC_STATUS 0x00400130u   /* 0xFEC00130 */
            #define MCPX_AC97_CODEC_READY  0x00000100u
            /* Opt-in, and not because it is wrong.
             *
             * Reporting the codec is the correct answer -- DSERR_NODRIVER is
             * not what hardware returns -- but it is only correct as far as it
             * goes. DirectSound then hands the audio DSP a command block in
             * RAM and spins until the DSP clears it, and there is no DSP here,
             * so the title trades a late crash for an early hang: 44 assets
             * loaded and then a fault, versus one asset and a stall in audio
             * init. Until the DSP handshake is answered, the honest default is
             * the failure that gets further, with the correct behaviour one
             * variable away. */
            if (getenv("RECOMP_AC97_READY")) {
                /* Codec ready; everything else plain memory, with the
                 * channel-reset bits cleared by the ack thread (see
                 * ac97_clear_reset_bits_at). The APU stays unemulated here,
                 * and DirectSound creates, which is what the engine behind
                 * it needs. Any value of RECOMP_AC97_READY: the trapping
                 * variant needs fault handling the Switch does not have. */
                *(volatile uint32_t *)((char *)g_mcpx_memory
                                       + MCPX_AC97_CODEC_STATUS)
                    |= MCPX_AC97_CODEC_READY;
                g_ac97_poll_page = (char *)g_mcpx_memory + AC97_NABM_OFFSET;
                fprintf(stderr, "  AC97: codec reported ready at 0x%08X"
                                " (plain memory; reset bits polled)\n",
                        XBOX_MCPX_BASE + MCPX_AC97_CODEC_STATUS);
            }
            fprintf(stderr, "  MCPX device aperture: %u MB at Xbox VA "
                    "0x%08X (APU/AC97/USB/NIC, zeroed)\n",
                    XBOX_MCPX_SIZE / (1024 * 1024), XBOX_MCPX_BASE);
        } else {
            fprintf(stderr, "  WARNING: MCPX aperture at 0x%08X failed "
                    "(error %lu); USB/audio register access will fault\n",
                    XBOX_MCPX_BASE, GetLastError());
        }
    }

    /* Flash ROM aperture -- see XBOX_FLASH_BASE for why. */
    {
        uintptr_t flash_native = XBOX_FLASH_BASE + g_memory_offset;

        g_flash_memory = VirtualAlloc(
            (LPVOID)flash_native,
            XBOX_FLASH_SIZE,
            MEM_RESERVE | MEM_COMMIT,
            PAGE_READWRITE
        );
        if (g_flash_memory) {
            fprintf(stderr, "  Flash ROM aperture: %u MB at Xbox VA "
                    "0x%08X (zeroed, not a real BIOS image)\n",
                    XBOX_FLASH_SIZE / (1024 * 1024), XBOX_FLASH_BASE);
        } else {
            fprintf(stderr, "  WARNING: flash aperture at 0x%08X failed "
                    "(error %lu); a title reading flash will fault\n",
                    XBOX_FLASH_BASE, GetLastError());
        }
    }

    /* The two small gaps between NV2A/MCPX/Flash -- see their declarations
     * above for why these are backed the same way as the real apertures. */
    {
        uintptr_t gap_native = XBOX_NV2A_MCPX_GAP_BASE + g_memory_offset;
        g_nv2a_mcpx_gap_memory = VirtualAlloc(
            (LPVOID)gap_native,
            XBOX_NV2A_MCPX_GAP_SIZE,
            MEM_RESERVE | MEM_COMMIT,
            PAGE_READWRITE
        );
        if (g_nv2a_mcpx_gap_memory) {
            fprintf(stderr, "  NV2A/MCPX gap: %u MB at Xbox VA 0x%08X "
                    "(zeroed, wild-pointer guard)\n",
                    XBOX_NV2A_MCPX_GAP_SIZE / (1024 * 1024),
                    XBOX_NV2A_MCPX_GAP_BASE);
        } else {
            fprintf(stderr, "  WARNING: NV2A/MCPX gap at 0x%08X failed "
                    "(error %lu)\n",
                    XBOX_NV2A_MCPX_GAP_BASE, GetLastError());
        }
    }
    {
        uintptr_t gap_native = XBOX_POST_FLASH_GAP_BASE + g_memory_offset;
        g_post_flash_gap_memory = VirtualAlloc(
            (LPVOID)gap_native,
            XBOX_POST_FLASH_GAP_SIZE,
            MEM_RESERVE | MEM_COMMIT,
            PAGE_READWRITE
        );
        if (g_post_flash_gap_memory) {
            fprintf(stderr, "  Post-flash gap: %u MB at Xbox VA 0x%08X "
                    "(zeroed, wild-pointer guard)\n",
                    XBOX_POST_FLASH_GAP_SIZE / (1024 * 1024),
                    XBOX_POST_FLASH_GAP_BASE);
        } else {
            fprintf(stderr, "  WARNING: post-flash gap at 0x%08X failed "
                    "(error %lu)\n",
                    XBOX_POST_FLASH_GAP_BASE, GetLastError());
        }
    }

    if (g_nv2a_memory) {
        xbox_Nv2aAckStart();
    }

    /*
     * Allocate a page at Xbox kernel address space (0x80010000).
     *
     * RenderWare's Xbox driver code (xbcache.c) reads MEM32(0x8001003C)
     * to parse the Xbox kernel's PE header and find the INIT section for
     * CPU cache line sizing. On PC, we provide a minimal fake PE header
     * with 0 sections so the function gracefully skips the cache init.
     *
     * The actual native address is 0x80010000 + g_memory_offset.
     */
    {
        #define XBOX_KERNEL_BASE 0x80010000u
        #define KERNEL_PAGE_SIZE 4096
        uintptr_t kernel_native = XBOX_KERNEL_BASE + g_memory_offset;
        /* Already committed if the contiguous window above succeeded -
         * 0x80010000 sits inside it - so just use that storage. */
        g_kernel_memory = g_contig_memory
            ? (void *)kernel_native
            : VirtualAlloc((LPVOID)kernel_native, KERNEL_PAGE_SIZE,
                           MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
        if (g_kernel_memory) {
            /* Zero-fill then set e_lfanew = 0x80 (offset to PE header).
             * With the rest zeroed, NumberOfSections = 0 and the INIT
             * section search finds nothing, which is the safe path. */
            memset(g_kernel_memory, 0, KERNEL_PAGE_SIZE);
            *(uint32_t *)((uint8_t *)g_kernel_memory + 0x3C) = 0x80;  /* e_lfanew */
            fprintf(stderr, "  Kernel: fake PE header at Xbox VA 0x%08X (native %p)\n",
                    XBOX_KERNEL_BASE, g_kernel_memory);
        } else {
            fprintf(stderr, "  WARNING: could not map Xbox kernel VA 0x%08X\n",
                    XBOX_KERNEL_BASE);
        }
        #undef XBOX_KERNEL_BASE
        #undef KERNEL_PAGE_SIZE
    }

    /* Initialize the dynamic heap. */
    fprintf(stderr, "  Heap: %u MB at Xbox VA 0x%08X-0x%08X\n",
            (unsigned)((XBOX_HEAP_TOP - XBOX_HEAP_BASE) / (1024 * 1024)),
            XBOX_HEAP_BASE, XBOX_HEAP_TOP);

    /*
     * Map mirror views of the 64 MB region.
     *
     * On retail Xbox, physical RAM wraps at 64 MB due to the 26-bit
     * address bus. Address 0x04070000 reads the same data as 0x00070000.
     * The RenderWare engine's memory walker crosses 64 MB and accesses
     * mirrored data for an extended walk covering 256+ MB of virtual
     * addresses. Game init code also writes large data structures past
     * 64 MB that on real hardware wrap into physical RAM.
     *
     * We map additional views of the SAME file mapping section at 64 MB
     * intervals. All views alias the same physical pages, so reads and
     * writes at any mirror address correctly access the base data.
     */
    {
        int mirrors_ok = 0;
        /* The tiled aperture is a specific architectural alias -- physical RAM
         * a second time at 0xF0000000, which is where titles render -- while
         * these mirrors are a generic emulation of the address wrap. When the
         * mapped size is large enough that a mirror would cover 0xF0000000,
         * the mirror wins the address and the tiled mapping fails with
         * ERROR_INVALID_ADDRESS; Half-Life 2 then faults on its first surface
         * write. The specific alias is worth more than one wrap mirror, so
         * skip any that would overlap it.
         *
         * Guest addresses, not host: mirror m covers guest
         * (m + 1) * g_memory_size. */
        uint64_t tiled_lo = XBOX_TILED_BASE;
        uint64_t tiled_hi = tiled_lo + xbox_TiledApertureSize();
#if defined(__SWITCH__)
        /* Not on the Switch. Horizon reservations are bookkeeping only, so
         * the extended-VMA reservation below (from aliased_top, 0x74000000)
         * succeeds on top of the last mirrors, and a title's allocation up
         * there would be handed RAM-mirror pages -- silently overwriting low
         * RAM. Each mirror is also another 128 MB svcMapProcessMemory view
         * with page tables from the kernel's shared pool. Code memory cannot
         * map them anyway (0xD401), and JSRF runs without them (Switch run
         * 5, Linux RECOMP_NO_ALIAS=1). */
        const int skip_mirrors = 1;
        fprintf(stderr, "  RAM mirrors: not mapped on Switch\n");
#else
        const int skip_mirrors = 0;
#endif

        for (int m = 0; m < XBOX_NUM_MIRRORS; m++) {
            uintptr_t mirror_base = (uintptr_t)g_memory_base +
                                    (uintptr_t)(m + 1) * g_memory_size;
            uint64_t guest_lo = (uint64_t)(m + 1) * g_memory_size;
            uint64_t guest_hi = guest_lo + g_memory_size;

            if (host_no_alias() || skip_mirrors)
                continue;

            /* RAM mirrors emulate the 26-bit address wrap within Xbox user space.
             * They must never overlap or extend into the kernel contiguous window
             * (0x80000000) or any hardware MMIO / aperture range. */
            if (guest_lo >= (uint64_t)XBOX_CONTIG_BASE || guest_hi > (uint64_t)XBOX_CONTIG_BASE) {
                continue;
            }

            if (guest_lo < tiled_hi && tiled_lo < guest_hi) {
                fprintf(stderr, "  Mirror %d: skipped, overlaps the tiled"
                                " aperture at 0x%08X\n",
                        m + 1, (unsigned)XBOX_TILED_BASE);
                continue;
            }
            /* Inside the reservation this hands back the slice we are about
             * to use; outside it (no reservation) this is a no-op on an
             * address we never held. */
            if (g_span_base)
                VirtualFree((LPVOID)mirror_base, g_memory_size, MEM_RELEASE);
            g_mirror_views[m] = MapViewOfFileEx(
                g_mapping_handle,
                FILE_MAP_ALL_ACCESS,
                0, 0,
                g_memory_size,
                (LPVOID)mirror_base
            );
            if (g_mirror_views[m]) {
                mirrors_ok++;
            } else {
                fprintf(stderr, "  Mirror %d: FAILED at %p (error %lu)\n",
                        m + 1, (void *)mirror_base, GetLastError());
            }
        }
        fprintf(stderr, "  RAM mirror: %d/%d views mapped (covers %d MB)\n",
                mirrors_ok, XBOX_NUM_MIRRORS,
                (int)((mirrors_ok + 1) * g_memory_size / (1024 * 1024)));

        if (g_contig_memory) {
            /* Verify contiguous window survived mirror initialization */
            *(volatile uint32_t *)g_contig_memory = 0x5E6F7A8Bu;
            if (*(volatile uint32_t *)g_contig_memory != 0x5E6F7A8Bu) {
                fprintf(stderr, "  CRITICAL: contiguous window at 0x%08X lost during mirror mapping!\n",
                        XBOX_CONTIG_BASE);
            }
            *(volatile uint32_t *)g_contig_memory = 0;
        }
    }

    /*
     * Host backing for guest_vmem.c's extended-VMA tracker -- the range
     * above the RAM-mirror scheme (aliased_top, exactly where the last
     * mirror view above ends) up to the top of Xbox user address space
     * (0x7FFE0000). guest_vmem.c reserves/commits guest requests up here
     * for titles that ask NtAllocateVirtualMemory for a specific high
     * address, but until now nothing ever gave that range real host
     * memory -- confirmed live, a title's own allocation there succeeded
     * at the bookkeeping level and then took a real access violation the
     * first time it touched the memory, because g_xbox_mem_offset+guest_va
     * pointed at host address space nobody had reserved.
     *
     * Reserve it here, right after the mirrors, for the same reason the
     * mirrors themselves use an exact-address MapViewOfFileEx this early:
     * this point in the process's life is when a fixed host address is
     * most likely to still be free. Doing this lazily instead, the first
     * time a title actually asks for extended VMA (which can be deep into
     * boot, after the CRT heap, loaded DLLs, and other engine subsystems
     * have already claimed nearby address space), measurably fails --
     * that was the original, lazy version of this reservation, and it lost
     * the exact address to something else nearly every run.
     *
     * MEM_RESERVE only: guest_vmem_allocate's own commit path
     * (extvma_commit in guest_vmem.c) MEM_COMMITs the specific sub-ranges
     * a title actually uses, exactly like real Xbox reserve-then-commit.
     */
    {
        /* From the end of the RAM mirrors to the user-mode top, 0x7FFE0000. */
        const uint64_t user_top = 0x7FFE0000u;
        uint64_t from = (uint64_t)g_xbox_total_ram * (XBOX_NUM_MIRRORS + 1u);

        if (from < user_top) {
            uint64_t span = user_top - from;
            void *host = (void *)((uintptr_t)g_memory_base + (uintptr_t)from);
            unsigned long long mb = (unsigned long long)(span >> 20);

            g_xbox_extvma_reserved =
                VirtualAlloc(host, (SIZE_T)span, MEM_RESERVE, PAGE_NOACCESS) != NULL;
            if (g_xbox_extvma_reserved)
                fprintf(stderr, "  Extended VMA: reserved %llu MB at %p\n", mb, host);
            else
                fprintf(stderr, "  Extended VMA: no %llu MB at %p for it (error %lu); a title"
                                " reserving up there is refused instead\n", mb, host,
                        (unsigned long)GetLastError());
        }
    }

    /*
     * Tiled / write-combined aperture at 0xF0000000.
     *
     * The NV2A exposes physical RAM a second time here and titles render
     * through it. Wreckless's first surface write goes to guest 0xF1954000 --
     * the tiled alias of physical 0x01954000, already inside our RAM -- and
     * faulted because nothing was mapped there.
     *
     * A view of the same section rather than fresh storage: the title writes a
     * surface through the tiled address and reads it back through the normal
     * one, so the two have to be the same bytes. That is the whole reason the
     * RAM lives in a file mapping.
     */
    {
        uintptr_t tiled_native = XBOX_TILED_BASE + g_memory_offset;
        size_t tiled_size = xbox_TiledApertureSize();
        int tiled_is_view;
        /* A view of the CONTIGUOUS window, not of RAM.
         *
         * On hardware all three -- physical P, 0x80000000+P and 0xF0000000+P
         * -- are one and the same memory. Here they cannot be: the XBE image
         * is loaded at its own VA in the RAM mapping, so aliasing the
         * contiguous window onto RAM would drop a title's pinned physical
         * pools on top of its own code (Halo pins 3.4 MB at 0x61000, which is
         * inside its image). The contiguous window therefore has separate
         * storage, and the question becomes which of the two the tiled
         * aperture should be a view of.
         *
         * It is the contiguous one. A tiled address is a GPU surface address
         * by construction, and GPU surfaces come from
         * MmAllocateContiguousMemory -- so the pairing that has to hold is
         * tiled to contiguous. Against RAM instead, Half-Life 2's loader wrote
         * every decoded video frame through 0xF1C63000 while D3D sampled the
         * texture at 0x81C63000, and the sampler read zeros: 1.8 billion black
         * pixels rasterised, perfectly, from an empty texture.
         */
        if (tiled_size > XBOX_CONTIG_SIZE)
            tiled_size = XBOX_CONTIG_SIZE;
        g_tiled_view = (g_contig_mapping && !host_no_alias())
            ? MapViewOfFileEx(
                g_contig_mapping,
                FILE_MAP_ALL_ACCESS,
                0, 0,
                tiled_size,
                (LPVOID)tiled_native)
            : NULL;
        tiled_is_view = g_tiled_view != NULL;
        if (!g_tiled_view) {
            /* On hosts that cannot alias memory (such as Nintendo Switch Horizon OS),
             * MapViewOfFileEx fails for a second view. Back the tiled aperture
             * with independent memory so rendering writes do not fault. */
            g_tiled_view = VirtualAlloc(
                (LPVOID)tiled_native,
                tiled_size,
                MEM_RESERVE | MEM_COMMIT,
                PAGE_READWRITE
            );
            if (g_tiled_view) {
                fprintf(stderr, "  Tiled aperture: %u MB at Xbox VA 0x%08X (standalone memory)\n",
                        (unsigned)(tiled_size / (1024 * 1024)), (unsigned)XBOX_TILED_BASE);
            }
        }
        if (g_tiled_view) {
            /* Prove the alias rather than assert it. Everything the title
             * renders goes through this window and is read back through the
             * physical address, so if the two are not the same bytes the GPU
             * sees empty buffers and the screen stays black -- with nothing
             * anywhere to say why. One write and one read turns that into a
             * startup line. */
            {
                volatile uint32_t *via_tiled =
                    (volatile uint32_t *)((uintptr_t)(XBOX_TILED_BASE + 0x1000)
                                          + g_memory_offset);
                volatile uint32_t *via_contig =
                    (volatile uint32_t *)((uintptr_t)(XBOX_CONTIG_BASE + 0x1000)
                                          + g_memory_offset);
                uint32_t saved = *via_contig;

                *via_tiled = 0xA5C30F17u;
                if (*via_contig != 0xA5C30F17u)
                    fprintf(stderr, "  WARNING: tiled aperture does NOT alias"
                            " the contiguous window (wrote A5C30F17, read"
                            " %08X) -- the GPU will sample empty textures\n",
                            *via_contig);
                else
                    fprintf(stderr, "  Tiled aperture alias verified"
                            " (tiled 0x%08X == contiguous 0x%08X)\n",
                            XBOX_TILED_BASE, XBOX_CONTIG_BASE);
                *via_contig = saved;
            }
            if (tiled_is_view)
                fprintf(stderr, "  Tiled aperture: %u MB at Xbox VA 0x%08X"
                        " (view of the contiguous window)\n",
                        (unsigned)(tiled_size / (1024 * 1024)),
                        XBOX_TILED_BASE);
        } else {
            fprintf(stderr, "  WARNING: tiled aperture at 0x%08X failed"
                    " (error %lu); rendering writes will fault\n",
                    XBOX_TILED_BASE, GetLastError());
        }
    }

    xbox_WatchInit();
    fprintf(stderr, "xbox_MemoryLayoutInit: complete\n");
    return TRUE;
}

/*
 * Make every RAM mirror read-only, for finding writes that reach low memory
 * through an alias.
 *
 * Xbox RAM is visible at 28 virtual addresses that alias the same pages, so a
 * store to 0x04000004 changes Xbox VA 4 without ever touching VA 4. Both a
 * page-protection watchpoint and a DR0 hardware watchpoint on VA 4 therefore
 * report nothing while the memory demonstrably changes -- which is exactly
 * what happened chasing Halo's fs:[4] corruption.
 *
 * Debug aid, not part of normal startup: a title that legitimately writes
 * through a mirror will fault here too, and the fault address names the alias
 * and the code.
 */
void xbox_ProtectMirrorsForDebug(void)
{
    int n = 0;
    for (int m = 0; m < XBOX_NUM_MIRRORS; m++) {
        DWORD old;
        if (g_mirror_views[m] &&
            VirtualProtect(g_mirror_views[m], g_memory_size,
                           PAGE_READONLY, &old)) {
            n++;
        }
    }
    fprintf(stderr, "  Mirrors: %d/%d made read-only (debug)\n",
            n, XBOX_NUM_MIRRORS);
}

void xbox_MemoryLayoutShutdown(void)
{
    if (g_kernel_memory) {
        VirtualFree(g_kernel_memory, 0, MEM_RELEASE);
        g_kernel_memory = NULL;
    }
    if (g_nv2a_ack_thread) {
        InterlockedExchange(&g_nv2a_ack_stop, 1);
        WaitForSingleObject(g_nv2a_ack_thread, 1000);
        CloseHandle(g_nv2a_ack_thread);
        g_nv2a_ack_thread = NULL;
    }
    if (g_nv2a_memory) {
        VirtualFree(g_nv2a_memory, 0, MEM_RELEASE);
        g_nv2a_memory = NULL;
    }
    /* Unmap mirror views first */
    for (int m = 0; m < XBOX_NUM_MIRRORS; m++) {
        if (g_mirror_views[m]) {
            UnmapViewOfFile(g_mirror_views[m]);
            g_mirror_views[m] = NULL;
        }
    }
    /* Unmap base view */
    if (g_memory_base) {
        UnmapViewOfFile(g_memory_base);
        g_memory_base = NULL;
        g_memory_size = 0;
    }
    /* The apertures. Left mapped, a second init cannot place them: the first
     * run still owns 0x80000000, 0xFD000000, 0xFE800000, 0xFF000000 and the
     * tiled alias, and every one of those comes back as "failed" while init
     * still returns TRUE because they are best-effort. The result is a layout
     * that looks initialised and has no device apertures at all. */
    if (g_tiled_view) {
        UnmapViewOfFile(g_tiled_view);
        g_tiled_view = NULL;
    }
    if (g_contig_memory) {
        VirtualFree(g_contig_memory, 0, MEM_RELEASE);
        g_contig_memory = NULL;
    }
    if (g_mcpx_memory) {
        VirtualFree(g_mcpx_memory, 0, MEM_RELEASE);
        g_mcpx_memory = NULL;
    }
    if (g_flash_memory) {
        VirtualFree(g_flash_memory, 0, MEM_RELEASE);
        g_flash_memory = NULL;
    }
    if (g_nv2a_mcpx_gap_memory) {
        VirtualFree(g_nv2a_mcpx_gap_memory, 0, MEM_RELEASE);
        g_nv2a_mcpx_gap_memory = NULL;
    }
    if (g_post_flash_gap_memory) {
        VirtualFree(g_post_flash_gap_memory, 0, MEM_RELEASE);
        g_post_flash_gap_memory = NULL;
    }

    /* Close file mapping handle */
    if (g_mapping_handle) {
        CloseHandle(g_mapping_handle);
        g_mapping_handle = NULL;
    }

    /* Whatever is left of the base+mirrors reservation. The views carved out
     * of it are already unmapped above; this releases the range itself. */
    if (g_span_base) {
        VirtualFree(g_span_base, g_span_size, MEM_RELEASE);
        g_span_base = NULL;
        g_span_size = 0;
    }
    fprintf(stderr, "xbox_MemoryLayoutShutdown: released\n");
}

/* Bump allocator for pure address-space reservations, above RAM.
 *
 * A MEM_RESERVE costs no memory on real hardware -- it takes address space out
 * of a 4 GB range, not pages out of the 64 MB the console has -- so titles
 * reserve far more than exists and commit a fraction. Satisfying that out of
 * the RAM heap does not work: Half-Life 2 asks for 128 MB and then 200 MB, and
 * clamping those to what the heap can back left it sub-allocating across a
 * range it believed it owned, walking past the top of RAM and aliasing low
 * memory through the mirrors.
 *
 * So reservations come from the mapped space *above* RAM instead. Those pages
 * are already backed and distinct, nothing else hands them out, and a commit
 * inside one is a no-op because it is real memory already.
 *
 * Returns 0 when the mapping is no larger than RAM -- the default for titles
 * that never call xbox_SetMapSize -- which leaves the old behaviour untouched.
 *
 * ponytail: a bump allocator with no free. A reservation is address space, the
 * range is large, and a title that reserves and releases repeatedly would need
 * a real allocator; none has yet.
 */
static uint32_t g_reserve_next;

uint32_t xbox_ReserveAlloc(uint32_t size, uint32_t align)
{
    uint32_t base;

    if (g_memory_size <= g_xbox_total_ram || size == 0)
        return 0;
    if (!align)
        align = 4096;
    if (!g_reserve_next)
        g_reserve_next = (uint32_t)g_xbox_total_ram;

    base = (g_reserve_next + align - 1) & ~(align - 1);
    if ((size_t)base + size > g_memory_size)
        return 0;
    g_reserve_next = base + size;
    return base;
}

BOOL xbox_IsXboxAddress(uintptr_t address)
{
    return (address >= XBOX_BASE_ADDRESS &&
            address < XBOX_BASE_ADDRESS + g_memory_size);
}

void *xbox_GetMemoryBase(void)
{
    return g_memory_base;
}

ptrdiff_t xbox_GetMemoryOffset(void)
{
    return g_memory_offset;
}

/* ── Dynamic heap allocator ────────────────────────────────
 *
 * Simple bump allocator for MmAllocateContiguousMemory and similar.
 * Returns Xbox VAs within the mapped region so MEM32() works correctly.
 * No free support (bump-only for now).
 */
static uint32_t g_heap_next = XBOX_HEAP_BASE;

static int g_heap_alloc_count = 0;

/* Block table backing xbox_HeapFree. A bump pointer alone never reclaims,
 * which is fine for a title that allocates once and fatal for a debug build
 * that churns. Flat array rather than an intrusive list: allocations come back
 * in bump order, so index order is address order and coalescing is a
 * neighbour check. */
#define XBOX_HEAP_MAX_BLOCKS 65536
static struct { uint32_t addr; uint32_t size; uint8_t free; }
    g_heap_blocks[XBOX_HEAP_MAX_BLOCKS];
static int g_heap_block_count = 0;
/* Guest threads (DirectSound, CRI's movie threads, the game) allocate at
 * once; the table was unguarded. */
static SRWLOCK g_heap_lock = SRWLOCK_INIT;

/* Insert a free block at index `at`, keeping address order. Reuses an empty
 * slot (size 0, left behind by coalescing) when one is already there. */
static int heap_insert_free(int at, uint32_t addr, uint32_t size)
{
    if (!(at < g_heap_block_count && g_heap_blocks[at].size == 0)) {
        if (g_heap_block_count >= XBOX_HEAP_MAX_BLOCKS)
            return 0;
        memmove(&g_heap_blocks[at + 1], &g_heap_blocks[at],
                (size_t)(g_heap_block_count - at) * sizeof g_heap_blocks[0]);
        g_heap_block_count++;
    }
    g_heap_blocks[at].addr = addr;
    g_heap_blocks[at].size = size;
    g_heap_blocks[at].free = 1;
    return 1;
}

/*
 * Simulated stacks for spawned threads.
 *
 * The main thread owns the top of the XBOX_STACK region and grows down; worker
 * stacks are carved from the bottom upward so the two cannot meet until the
 * whole 8 MB is gone. Xbox VAs, not host memory: recompiled code addresses its
 * stack through MEM32() like any other Xbox pointer.
 */
uint32_t xbox_HeapAlloc(uint32_t size, uint32_t alignment);

#define XBOX_THREAD_STACK_SIZE  (512 * 1024)
#define XBOX_MAX_THREAD_STACKS  8

static int g_thread_stacks_used = 0;

/* A TIB and TLS block for a newly spawned guest thread.
 *
 * A TIB is per-thread on the console and was per-process here: one address,
 * 0x1000, for everyone. Two things live in it that must not be shared. fs:[0]
 * is the SEH chain head, so two threads unwinding at once walk each other's
 * frames. fs:[4] points at the image's TLS block, whose slot 0 is the CRT's
 * per-thread data -- errno, the locale, and the bookkeeping _lock() uses to
 * decide who owns which lock.
 *
 * Half-Life 2 deadlocked on the last of those: two threads inside _lock(),
 * each holding the CRT lock the other was waiting for, because "which thread
 * am I" was a single shared answer.
 *
 * The new block is a copy of the template the loader built, so a thread starts
 * with the image's initialised thread-local data rather than zeros, and its
 * own per-thread structure behind slot 0.
 */
uint32_t xbox_AllocThreadTib(void)
{
    /* XBOX_VA is scoped to the loader; the same arithmetic, spelled here. */
    #define TIB_VA(va) ((void *)((uintptr_t)(va) + g_memory_offset))
    const uint32_t tib_size = 0x40;
    uint32_t tib, block, thread_data, total;

    if (!g_tls_total)
        return 0;                    /* image has no TLS; nothing to copy */

    total = g_tls_total;
    tib = xbox_HeapAlloc(tib_size + total + g_tls_thread_size, 16);
    if (!tib)
        return 0;
    block       = tib + tib_size;
    thread_data = block + total;

    /* The TIB itself, copied so stack bounds and the fields the title filled
     * in are inherited, then the two that must not be. */
    memcpy(TIB_VA(tib), TIB_VA(XBOX_TIB_MAIN), tib_size);
    memcpy(TIB_VA(block), TIB_VA(g_tls_template_va), total);
    memset(TIB_VA(thread_data), 0, g_tls_thread_size);

    *(uint32_t *)TIB_VA(tib + 0x00) = 0xFFFFFFFFu;   /* own SEH chain    */
    *(uint32_t *)TIB_VA(block)      = thread_data;   /* slot 0           */
    *(uint32_t *)TIB_VA(tib + 0x04) = block + total; /* fs:[4], see above*/

    return tib;
    #undef TIB_VA
}

uint32_t xbox_AllocThreadStack(void)
{
    uint32_t base;

    if (g_thread_stacks_used >= XBOX_MAX_THREAD_STACKS) {
        return 0;
    }

    /* From the heap, not from XBOX_STACK_BASE.
     *
     * The stack region begins at 0x00780000, which is fine only while the
     * title's image ends below that. Half-Life 2's image runs to 0x009B68C0,
     * so the first thread stack (0x00780000..0x00800000) landed inside its
     * .rdata and .data: the worker spawned during engine init wrote its
     * frames over the game's own static data. Nothing faults -- the pages are
     * mapped and writable -- so it shows up later as globals that were
     * correct when written and wrong when read.
     *
     * The heap already starts above the image and knows how big it is, so
     * taking slices from it is correct for any image size instead of only
     * for small ones.
     */
    base = xbox_HeapAlloc(XBOX_THREAD_STACK_SIZE, 4096);
    if (!base)
        return 0;
    g_thread_stacks_used++;

    /* Top of the slice, 16-byte aligned, growing down. */
    return base + XBOX_THREAD_STACK_SIZE - 16;
}

/* Give a worker's stack back when the worker ends.
 *
 * The counter used to only ever go up, so a title that creates and destroys
 * threads ran the pool dry no matter how few were alive at once. The Xbox
 * Dashboard spawns one worker per ambient WAV and terminates it before loading
 * the next; after XBOX_MAX_THREAD_STACKS files the pool was empty and
 * PsCreateSystemThreadEx fell back to running the worker inline. That fallback
 * is a deadlock here rather than a slowdown: the worker ran to completion
 * before the caller reached its wait, so the main thread then waited forever on
 * events whose only signaller had already finished. It looked like an audio
 * hang, three layers away from the cause.
 *
 * Takes the value AllocThreadStack returned, so callers never do the arithmetic.
 */
void xbox_FreeThreadStack(uint32_t stack_top)
{
    if (!stack_top)
        return;
    xbox_HeapFree(stack_top + 16 - XBOX_THREAD_STACK_SIZE);
    if (g_thread_stacks_used > 0)
        g_thread_stacks_used--;
}

/* Bump allocator over the contiguous window mapped at XBOX_CONTIG_BASE.
 *
 * MmAllocateContiguousMemory hands back physical memory, and on Xbox physical
 * page P is visible at 0x80000000 + P. Drivers rely on that being an exact
 * round trip: Xbox D3D writes its pushbuffer position to the NV2A as
 * `VA & 0x0FFFFFFF` and reads the GPU's position back as `GET | 0x80000000`,
 * then compares the two. That holds for any address in this window and for
 * nothing in the general heap, whose position depends on what the title
 * reserved first -- Half-Life 2 reserves 128 MB and then 200 MB before D3D
 * allocates its pushbuffer, which put the buffer at 0x15782000 and left the
 * engine comparing 0x857844C0 against it forever.
 *
 * Grows up from the base; XBOX_GPU_INSTANCE_DEFAULT is carved off the top by
 * the GPU-instance bridge, so the two do not meet until the window is full.
 *
 * Freed blocks are reused (first fit, coalesced). The arena used to be a bump
 * pointer that MmFreeContiguousMemory never gave back -- it called
 * xbox_HeapFree, which does not know these addresses. NFSU2 allocates its
 * textures and frame buffers here (XPhysicalAlloc) and frees them on every
 * switch between the garage and the world; after ~20 minutes of Career the
 * 64 MB window was full, a 16 KB request failed while leaving the car lot and
 * the game hung on the loading screen. */
static uint32_t g_contig_next = XBOX_CONTIG_BASE;
static uint32_t g_contig_hwm = XBOX_CONTIG_BASE;   /* highest end ever handed out */

/* Where the arena starts: just above the loaded image, not at the window base.
 *
 * The window is separate storage from low RAM, but its physical addresses
 * (VA - 0x80000000) are the same numbers as low-RAM VAs, and
 * MmGetPhysicalAddress passes non-contiguous VAs through unchanged. With the
 * arena at physical 0, a DMA address below the image end was ambiguous: X-Men
 * Legends' USB stack hands the controller both contiguous descriptors
 * (physical 0x006DC6A0) and a static .data buffer (VA 0x005DCE24), and no rule
 * can tell those apart. Starting the arena above the image makes the two
 * ranges disjoint for image VAs. It does not for pool, heap or stack VAs,
 * which sit above the image in the same numbers the arena grows into (JSRF:
 * pool from 0x010DD000, heap from 0x00F81000, arena past 0x01E00000 within
 * 30 s). MmGetPhysicalAddress therefore tags those with XBOX_BUS_PASSTHRU,
 * and a device model checks xbox_BusPassthroughVa() before
 * xbox_ContiguousIsPhysical(). */
static uint32_t g_contig_start = XBOX_CONTIG_BASE;

/* The arena's bookkeeping: what is handed out and what was given back below
 * the top (g_contig_next), as two lists of extents in address order. A freed
 * extent that reaches the top is not kept: the top moves down instead. */
#define XBOX_CONTIG_MAX_BLOCKS 16384

typedef struct { uint32_t addr, size; } ContigExtent;

typedef struct {
    ContigExtent at[XBOX_CONTIG_MAX_BLOCKS];
    int          count;
} ContigList;

static ContigList g_contig_used, g_contig_free;
static SRWLOCK    g_contig_lock = SRWLOCK_INIT;

/* The first extent at or above `addr`. */
static int contig_lower_bound(const ContigList *list, uint32_t addr)
{
    int low = 0, high = list->count;

    while (low < high) {
        int middle = low + (high - low) / 2;
        if (list->at[middle].addr < addr)
            low = middle + 1;
        else
            high = middle;
    }
    return low;
}

static void contig_insert_at(ContigList *list, int index, uint32_t addr, uint32_t size)
{
    memmove(&list->at[index + 1], &list->at[index],
            (size_t)(list->count - index) * sizeof list->at[0]);
    list->at[index].addr = addr;
    list->at[index].size = size;
    list->count++;
}

static void contig_erase_at(ContigList *list, int index)
{
    memmove(&list->at[index], &list->at[index + 1],
            (size_t)(list->count - index - 1) * sizeof list->at[0]);
    list->count--;
}

/* Room for `extra` more entries in both lists. */
static int contig_room(int extra)
{
    return g_contig_used.count + g_contig_free.count + extra <= XBOX_CONTIG_MAX_BLOCKS;
}

/* [addr, addr + size) is free again (below the top): merged with the free
 * extents it touches. */
static void contig_release(uint32_t addr, uint32_t size)
{
    ContigList *list = &g_contig_free;
    int at = contig_lower_bound(list, addr);

    if (at > 0 && list->at[at - 1].addr + list->at[at - 1].size == addr) {
        at--;
        list->at[at].size += size;
    } else {
        contig_insert_at(list, at, addr, size);
    }
    if (at + 1 < list->count && list->at[at].addr + list->at[at].size == list->at[at + 1].addr) {
        list->at[at].size += list->at[at + 1].size;
        contig_erase_at(list, at + 1);
    }
}

/* First fit among the freed extents: where an aligned block of `size` goes,
 * with the extent split around it, or 0. */
static uint32_t contig_reuse(uint32_t size, uint32_t alignment)
{
    ContigList *list = &g_contig_free;
    int i;

    for (i = 0; i < list->count; i++) {
        uint32_t base = list->at[i].addr, end = base + list->at[i].size;
        uint32_t start = (base + alignment - 1) & ~(alignment - 1);
        int before, after;

        if (list->at[i].size < size || (uint64_t)start + size > end)
            continue;
        /* The first one that fits is the one, even if the lists are full
         * (the top is tried instead). */
        before = start != base;
        after = start + size != end;
        if (!contig_room(1 + before + after - 1))
            return 0;
        if (before && after) {
            list->at[i].size = start - base;
            contig_insert_at(list, i + 1, start + size, end - (start + size));
        } else if (before) {
            list->at[i].size = start - base;
        } else if (after) {
            list->at[i].addr = start + size;
            list->at[i].size = end - (start + size);
        } else {
            contig_erase_at(list, i);
        }
        contig_insert_at(&g_contig_used, contig_lower_bound(&g_contig_used, start), start, size);
        return start;
    }
    return 0;
}

/* A new block at the top; 0 if it would reach the GPU instance memory kept
 * at the window's end, or the lists are full. */
static uint32_t contig_bump(uint32_t size, uint32_t alignment)
{
    uint32_t gap_from = g_contig_next;
    uint32_t start = (g_contig_next + alignment - 1) & ~(alignment - 1);
    uint64_t limit = (uint64_t)XBOX_CONTIG_BASE + XBOX_CONTIG_SIZE - XBOX_GPU_INSTANCE_DEFAULT;

    if ((uint64_t)start + size > limit || !contig_room(2))
        return 0;
    if (start > gap_from)
        contig_release(gap_from, start - gap_from);     /* the alignment gap */
    contig_insert_at(&g_contig_used, g_contig_used.count, start, size);
    g_contig_next = start + size;
    if (g_contig_next > g_contig_hwm) {
        g_contig_hwm = g_contig_next;
        /* The published range only grows: a device may still hold the
         * physical address of a block that has since been freed. */
        xbox_ContiguousSetPhysicalRange(g_contig_start - XBOX_CONTIG_BASE,
                                        g_contig_hwm - XBOX_CONTIG_BASE);
    }
    return start;
}

/* Why an allocation failed, with the lock already let go. */
static void contig_report_full(uint32_t size, uint32_t top, uint32_t free_bytes,
                               int blocks, uint32_t largest)
{
    fprintf(stderr, "  [CONTIG] arena exhausted (%u requested, %u of %u used, "
            "%u free below the top in %d blocks, largest %u)\n",
            size, top, (unsigned)XBOX_CONTIG_SIZE, free_bytes, blocks, largest);
    fflush(stderr);
}

uint32_t xbox_ContiguousAlloc(uint32_t size, uint32_t alignment)
{
    uint32_t start;

    size = size == 0 ? 4096u : (size + 4095u) & ~4095u;
    alignment = alignment < 4096 ? 4096 : alignment;

    AcquireSRWLockExclusive(&g_contig_lock);
    if (g_contig_next == XBOX_CONTIG_BASE && g_xbox_image_hi) {
        /* First use: the arena starts above the image (see g_contig_start). */
        g_contig_start = XBOX_CONTIG_BASE + ((g_xbox_image_hi + 0xFFFFu) & ~0xFFFFu);
        g_contig_next = g_contig_hwm = g_contig_start;
    }
    start = contig_reuse(size, alignment);
    if (start == 0)
        start = contig_bump(size, alignment);
    if (start == 0) {
        uint32_t free_bytes = 0, largest = 0, top = g_contig_next - XBOX_CONTIG_BASE;
        int i, blocks = g_contig_used.count + g_contig_free.count;

        for (i = 0; i < g_contig_free.count; i++) {
            free_bytes += g_contig_free.at[i].size;
            largest = g_contig_free.at[i].size > largest ? g_contig_free.at[i].size : largest;
        }
        ReleaseSRWLockExclusive(&g_contig_lock);
        contig_report_full(size, top, free_bytes, blocks, largest);
        return 0;
    }
    ReleaseSRWLockExclusive(&g_contig_lock);

    memset((void *)((uintptr_t)start + g_memory_offset), 0, size);
    return start;
}

/* MmFreeContiguousMemory. 0 for an address the arena never handed out (a
 * pinned physical block, a heap pointer), which is then ignored. */
int xbox_ContiguousFree(uint32_t va)
{
    static unsigned frees;
    ContigList *used = &g_contig_used;
    uint32_t size, top;
    int at, blocks;

    AcquireSRWLockExclusive(&g_contig_lock);
    at = contig_lower_bound(used, va);
    if (at == used->count || used->at[at].addr != va) {
        ReleaseSRWLockExclusive(&g_contig_lock);
        return 0;
    }
    size = used->at[at].size;
    contig_erase_at(used, at);
    contig_release(va, size);
    /* Free space that now reaches the top goes back to the bump pointer. */
    if (g_contig_free.count != 0) {
        ContigExtent *last = &g_contig_free.at[g_contig_free.count - 1];
        if (last->addr + last->size == g_contig_next) {
            g_contig_next = last->addr;
            g_contig_free.count--;
        }
    }
    top = g_contig_next - XBOX_CONTIG_BASE;
    blocks = g_contig_used.count + g_contig_free.count;
    ReleaseSRWLockExclusive(&g_contig_lock);

    if (++frees <= 4 || frees % 256 == 0) {
        fprintf(stderr, "  [CONTIG] free #%u va=0x%08X (top %u KB, %d blocks)\n",
                frees, va, top / 1024, blocks);
        fflush(stderr);
    }
    return 1;
}

/* The size of the live contiguous block containing va, or 0. */
uint32_t xbox_ContiguousBlockSize(uint32_t va)
{
    const ContigList *used = &g_contig_used;
    uint32_t size = 0;
    int at;

    AcquireSRWLockShared(&g_contig_lock);
    /* The last block starting at or below va. */
    at = contig_lower_bound(used, va + 1) - 1;
    if (at >= 0 && va - used->at[at].addr < used->at[at].size)
        size = used->at[at].size;
    ReleaseSRWLockShared(&g_contig_lock);
    return size;
}

/* How much of the window has been handed out (the high-water mark).
 *
 * Lets a caller holding a physical address decide whether it names contiguous
 * memory this runtime allocated. The pushbuffer executor needs exactly that:
 * a surface offset is physical, and only the window makes it addressable. */
uint32_t xbox_ContiguousAllocatedBytes(void)
{
    return g_contig_hwm - XBOX_CONTIG_BASE;
}

/* xbox_ContiguousIsPhysical lives in xbox_devbus.c, where a device model can
 * reach it without linking the kernel; every change to the arena is published
 * there. */
void xbox_ContiguousSetPhysicalRange(uint32_t lo, uint32_t hi);


static uint32_t heap_alloc_locked(uint32_t size, uint32_t alignment);

/* Guest page protection and heap reuse.
 *
 * The title can make its own memory read-only or no-access, through
 * MmSetAddressProtect or NtProtectVirtualMemory, and on a POSIX host that is a
 * real mprotect of the guest pages. When it then frees the block, the heap
 * keeps no record of the protection, and the next allocation that reuses those
 * pages faults in the zero-fill below: JSRF on the Linux harness, about 40 s in,
 * reusing 0x0186A000 for a 64 KB NtAllocateVirtualMemory. On the console the
 * memory comes back read-write, as it does here now: once the title has
 * restricted any page, every block the heap hands out is made read-write
 * first (page-rounded). The Switch's VirtualProtect is a no-op, so there this
 * changes nothing. The first 32 restrictive calls are logged with their
 * caller, so the protection itself can be traced. */
static volatile int g_guest_protect_seen;

void xbox_NoteGuestProtect(const char *how, uint32_t va, uint32_t size,
                           uint32_t prot, uint32_t caller)
{
    static int shown;
    uint32_t p = prot & 0xFF;

    if (p != 0x01 && p != 0x02 && p != 0x10 && p != 0x20)
        return;                     /* writable: nothing for the heap to undo */
    g_guest_protect_seen = 1;
    if (shown++ < 32) {
        fprintf(stderr, "  [MEM] %s(0x%08X, %u, 0x%X) from 0x%08X: pages "
                        "the heap later reuses are made read-write again\n",
                how, va, size, prot, caller);
        fflush(stderr);
    }
}

static void heap_reopen(uint32_t start, uint32_t size)
{
#if defined(__SWITCH__)
    /* Horizon's VirtualProtect (win32_compat.c) is a no-op, so nothing here
     * was ever protected; newlib has no sysconf either. */
    (void)start; (void)size;
#elif !defined(_WIN32)
    static uintptr_t page;
    uintptr_t lo, hi;
    DWORD old;

    if (!g_guest_protect_seen || !size)
        return;
    if (!page) {
        long ps = sysconf(_SC_PAGESIZE);
        page = ps > 0 ? (uintptr_t)ps : 4096u;
    }
    lo = ((uintptr_t)start + g_memory_offset) & ~(page - 1);
    hi = ((uintptr_t)start + size + g_memory_offset + page - 1) & ~(page - 1);
    VirtualProtect((LPVOID)lo, (SIZE_T)(hi - lo), PAGE_READWRITE, &old);
#else
    DWORD old;

    if (!g_guest_protect_seen || !size)
        return;
    VirtualProtect((LPVOID)((uintptr_t)start + g_memory_offset), size,
                   PAGE_READWRITE, &old);
#endif
}

uint32_t xbox_HeapAlloc(uint32_t size, uint32_t alignment)
{
    uint32_t r;
    AcquireSRWLockExclusive(&g_heap_lock);
    r = heap_alloc_locked(size, alignment);
    ReleaseSRWLockExclusive(&g_heap_lock);
    return r;
}

static uint32_t heap_alloc_locked(uint32_t size, uint32_t alignment)
{
    uint32_t result;

    if (alignment < 4) alignment = 4;

    /* Enforce minimum allocation size.
     * The Xbox D3D8 code sometimes computes resource sizes from GPU
     * capabilities that return 0 (since we don't have real NV2A hardware),
     * resulting in zero-size allocations. With a bump allocator, these all
     * return the same address, causing overlapping structures. Enforce a
     * minimum of 4096 bytes so each allocation gets its own memory. */
    if (size < 16) size = 16;

    /* Reuse a freed block first. Without this the heap only ever grows: Halo's
     * debug build allocates and releases heavily through init, exhausted all
     * 48 MB in 4,726 allocations, and its second D3D CreateDevice then failed
     * with E_OUTOFMEMORY -- which the title reports by clearing
     * global_d3d_device, so the rasterizer asserts and startup stops. */
    /* ...and take only what the request needs from it. Reuse used to hand
     * over the whole block: a 16-byte request could take a freed 2 MB block,
     * and once audio streaming started churning allocations the 48 MB heap
     * drained in seconds (209 failed 2 MB requests, then no thread stack for
     * the next movie's decoder, which then ran inline on the main thread and
     * froze the game). The aligned piece is carved out; what is left in front
     * and behind stays free. */
    size = (size + 15) & ~15u;
    for (int i = 0; i < g_heap_block_count; i++) {
        uint32_t a, start, end, front, back;
        if (!g_heap_blocks[i].free || g_heap_blocks[i].size < size) {
            continue;
        }
        a = g_heap_blocks[i].addr;
        start = (a + alignment - 1) & ~(alignment - 1);
        end = a + g_heap_blocks[i].size;
        if (start + size > end || start + size < start) {
            continue;   /* not enough room once aligned */
        }
        front = start - a;
        back = end - (start + size);
        if (front && !heap_insert_free(i, a, front))
            continue;   /* table full: leave this block alone */
        if (front)
            i++;        /* the taken piece moved up one slot */
        g_heap_blocks[i].addr = start;
        g_heap_blocks[i].size = size;
        g_heap_blocks[i].free = 0;
        if (back && !heap_insert_free(i + 1, start + size, back))
            g_heap_blocks[i].size += back;   /* table full: keep it attached */
        heap_reopen(start, size);
        memset((void *)((uintptr_t)start + g_memory_offset), 0, size);
        return start;
    }

    /* Align the next pointer */
    result = (g_heap_next + alignment - 1) & ~(alignment - 1);

    if (result + size > XBOX_HEAP_TOP) {
        fprintf(stderr, "xbox_HeapAlloc: out of memory (requested %u, used %u/%u)\n",
                size, g_heap_next - XBOX_HEAP_BASE,
                (unsigned)(XBOX_HEAP_TOP - XBOX_HEAP_BASE));
        /* Who ate the heap? Group live blocks by size -- an exhausted heap is
         * nearly always one request size repeated, and the count names it. */
        {
            static int dumped = 0;
            static struct { uint32_t size; int n; } hist[256];
            if (!dumped) {
                int used = 0;
                dumped = 1;
                for (int i = 0; i < g_heap_block_count; i++) {
                    int j = 0;
                    if (g_heap_blocks[i].free || !g_heap_blocks[i].size) continue;
                    while (j < used && hist[j].size != g_heap_blocks[i].size) j++;
                    if (j == used) {
                        if (used == 256) continue;   /* ponytail: 256 distinct sizes is plenty */
                        hist[used].size = g_heap_blocks[i].size;
                        hist[used++].n = 0;
                    }
                    hist[j].n++;
                }
                for (int j = 0; j < used; j++) {
                    if ((uint64_t)hist[j].n * hist[j].size < 1024 * 1024) continue;
                    fprintf(stderr, "  [HEAP] %d live blocks of %u bytes (%u KB)\n",
                            hist[j].n, hist[j].size,
                            (unsigned)((uint64_t)hist[j].n * hist[j].size / 1024));
                }
                fflush(stderr);
            }
        }
        return 0;
    }

    g_heap_next = result + size;

    /* Zero-fill the allocated block (Xbox memory is always zeroed) */
    heap_reopen(result, size);
    memset((void *)((uintptr_t)result + g_memory_offset), 0, size);

    if (g_heap_block_count < XBOX_HEAP_MAX_BLOCKS) {
        g_heap_blocks[g_heap_block_count].addr = result;
        g_heap_blocks[g_heap_block_count].size = size;
        g_heap_blocks[g_heap_block_count].free = 0;
        g_heap_block_count++;
    }

    g_heap_alloc_count++;
    /* Rate-limited: a debug title makes thousands of these and the log is a
     * diagnostic, not a transaction record. */
    if (g_heap_alloc_count <= 32 || (g_heap_alloc_count % 512) == 0) {
        fprintf(stderr, "  [HEAP] #%d: size=%u align=%u → 0x%08X..0x%08X (used %u/%u)\n",
                g_heap_alloc_count, size, alignment, result, result + size,
                g_heap_next - XBOX_HEAP_BASE,
                (unsigned)(XBOX_HEAP_TOP - XBOX_HEAP_BASE));
        fflush(stderr);
    }

    return result;
}

/* How big is the block at this guest address?
 *
 * MmQueryAllocationSize and ExQueryPoolBlockSize both ask this, and both used
 * to answer 0 -- ExQueryPoolBlockSize by returning a literal, and
 * MmQueryAllocationSize by having no bridge at all. The host cannot answer it:
 * VirtualQuery on the translated address reports the size of the whole 64 MB
 * guest mapping, which is a worse answer than none. The block table already
 * has the real one, and it is the same table xbox_HeapFree matches against.
 *
 * Interior addresses count: a title that asks about a pointer it has walked
 * forward is asking about the block that contains it. Returns 0 for an address
 * this heap never handed out, which is what "not one of mine" has to look like.
 */
uint32_t xbox_HeapBlockSize(uint32_t xbox_va)
{
    int i;
    uint32_t r = 0;

    if (!xbox_va)
        return 0;
    AcquireSRWLockShared(&g_heap_lock);
    for (i = 0; i < g_heap_block_count; i++) {
        if (g_heap_blocks[i].free)
            continue;
        if (xbox_va >= g_heap_blocks[i].addr &&
            xbox_va <  g_heap_blocks[i].addr + g_heap_blocks[i].size) {
            r = g_heap_blocks[i].size - (xbox_va - g_heap_blocks[i].addr);
            break;
        }
    }
    ReleaseSRWLockShared(&g_heap_lock);
    return r;
}

void xbox_HeapFree(uint32_t xbox_va)
{
    static int frees = 0, matched = 0;

    if (!xbox_va) {
        return;
    }
    AcquireSRWLockExclusive(&g_heap_lock);
    frees++;
    if (frees <= 8) {
        fprintf(stderr, "  [HEAP] free #%d va=0x%08X blocks=%d\n",
                frees, xbox_va, g_heap_block_count);
        fflush(stderr);
    }
    for (int i = 0; i < g_heap_block_count; i++) {
        if (g_heap_blocks[i].addr != xbox_va || g_heap_blocks[i].free) {
            continue;
        }
        g_heap_blocks[i].free = 1;
        if (++matched % 512 == 0) {
            fprintf(stderr, "  [HEAP] frees=%d matched=%d blocks=%d\n",
                    frees, matched, g_heap_block_count);
            fflush(stderr);
        }

        /* Coalesce with neighbours. Index order is address order, and
         * adjacency is a simple end==start test -- stepping over the empty
         * slots (size 0) earlier merges left behind, which used to stop a
         * merge dead. Keeps large requests satisfiable after small churn. */
        {
            int n = i + 1, p = i - 1;
            while (n < g_heap_block_count && g_heap_blocks[n].size == 0) n++;
            while (p >= 0 && g_heap_blocks[p].size == 0) p--;
            if (n < g_heap_block_count && g_heap_blocks[n].free &&
                g_heap_blocks[i].addr + g_heap_blocks[i].size == g_heap_blocks[n].addr) {
                g_heap_blocks[i].size += g_heap_blocks[n].size;
                g_heap_blocks[n].size = 0;
                g_heap_blocks[n].addr = 0;
            }
            if (p >= 0 && g_heap_blocks[p].free &&
                g_heap_blocks[p].addr + g_heap_blocks[p].size == g_heap_blocks[i].addr) {
                g_heap_blocks[p].size += g_heap_blocks[i].size;
                g_heap_blocks[i].size = 0;
                g_heap_blocks[i].addr = 0;
            }
        }
        ReleaseSRWLockExclusive(&g_heap_lock);
        return;
    }
    ReleaseSRWLockExclusive(&g_heap_lock);
}

HANDLE xbox_GetMappingHandle(void)
{
    return g_mapping_handle;
}
