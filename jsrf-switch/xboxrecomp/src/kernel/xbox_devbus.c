/*
 * xbox_devbus.c -- what a device model needs from the kernel, and no more
 *
 * The APU and the OHCI model raise interrupt lines and resolve bus addresses
 * through the contiguous arena. Both answers belong to the kernel, but these
 * few functions are their own library on purpose: taking them from the
 * kernel's own objects pulls in the bridge, and with it recomp_lookup, which
 * only a title provides. A device model is linked alone -- tests/apu_mixdown
 * does exactly that -- so it depends on xbox_devbus, and xbox_kernel feeds it.
 */

#include "kernel.h"

/* ── Contiguous arena, as the bus sees it ─────────────────────
 *
 * xbox_ContiguousAlloc (xbox_memory_layout.c) publishes the physical range it
 * has handed out so far, [first, end): arena memory with physical address P
 * is at XBOX_CONTIG_BASE + P. Empty before the first allocation. */
static struct {
    volatile uint32_t first, end;
} g_contig_handed_out;

void xbox_ContiguousSetPhysicalRange(uint32_t first, uint32_t end)
{
    g_contig_handed_out.first = first;
    g_contig_handed_out.end = end;
}

/* Is this bus address arena memory (reached at XBOX_CONTIG_BASE + it)?
 * Otherwise it is a pass-through VA (see g_contig_start in
 * xbox_memory_layout.c). */
int xbox_ContiguousIsPhysical(uint32_t bus)
{
    return g_contig_handed_out.first <= bus && bus < g_contig_handed_out.end;
}

/* The VA behind a tagged pass-through bus address (XBOX_BUS_PASSTHRU, set by
 * xbox_MmGetPhysicalAddress), or 0 if bus is not one. Check this before
 * xbox_ContiguousIsPhysical, and before any masking to RAM size: the tag is
 * what keeps a pool or heap buffer from resolving into the window. */
uint32_t xbox_BusPassthroughVa(uint32_t bus)
{
    return ((bus & 0xF8000000u) == XBOX_BUS_PASSTHRU)
         ? bus - XBOX_BUS_PASSTHRU : 0u;
}

/* Device interrupt lines, level-triggered, one bit per vector.
 *
 * A device model (the APU on vector 5) asserts its line; the timer thread
 * wakes at once and calls the connected ISR, and keeps calling it each tick
 * while the line stays up. The ISR acknowledges in the device's own status
 * register, which is what lowers the line.
 *
 * ponytail: ISRs run on the timer thread with no IRQL and no interrupt
 * spinlock, like the vblank in kernel_bridge.c; KeSynchronizeExecution takes no lock
 * either. Add the interrupt object's lock if a driver is seen racing its ISR.
 */
static volatile LONG g_lines_up;        /* bit n: vector n asserted */
static HANDLE        g_line_raised;     /* auto-reset: wakes the timer thread */

/* Made on first use; two threads racing here keep one event. */
HANDLE xbox_irq_line_event(void)
{
    HANDLE made;

    if (g_line_raised != NULL)
        return g_line_raised;
    made = CreateEventA(NULL, FALSE, FALSE, NULL);
    if (InterlockedCompareExchangePointer(&g_line_raised, made, NULL) != NULL)
        CloseHandle(made);
    return g_line_raised;
}

void xbox_set_irq_line(uint32_t vector, int level)
{
    LONG bit;

    if (vector > 31)
        return;
    bit = (LONG)(1u << vector);
    if (!level) {
        InterlockedAnd(&g_lines_up, ~bit);
        return;
    }
    InterlockedOr(&g_lines_up, bit);
    SetEvent(xbox_irq_line_event());
}

uint32_t xbox_irq_lines(void)
{
    return (uint32_t)g_lines_up;
}
