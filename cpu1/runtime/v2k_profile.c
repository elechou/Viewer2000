//=============================================================================
// v2k_profile.c - bounded ISR aggregation and background publication
//=============================================================================

#include "v2k_profile.h"
#include "v2k_timebase.h"

#ifndef V2K_PROFILE_WINDOW_SAMPLES
#define V2K_PROFILE_WINDOW_SAMPLES V2K_ISR_HZ
#endif

V2K_STATIC_ASSERT(V2K_PROFILE_WINDOW_SAMPLES > 0u);

typedef struct {
    uint64_t load_sum;
    uint32_t sample_count;
    uint32_t load_peak;
    uint32_t ctrl_at_peak;
    uint32_t scope_at_peak;
    uint16_t lat_at_peak;
    v2k_tick_t peak_tick;
} v2k_profile_window_t;

volatile uint32_t g_v2k_prof_seq;
volatile uint32_t g_v2k_cycle_budget = V2K_ISR_BUDGET_CYCLES;
volatile uint32_t g_v2k_load_avg;
volatile uint32_t g_v2k_load_peak;
volatile uint32_t g_v2k_ctrl_at_peak;
volatile uint32_t g_v2k_scope_at_peak;
volatile uint16_t g_v2k_lat_at_peak;
volatile v2k_tick_t g_v2k_peak_tick;

static v2k_profile_window_t s_active;
static volatile v2k_profile_window_t s_pending;
static volatile uint32_t s_pending_seq;
static uint32_t s_published_seq;

static void v2k_profile_clear(v2k_profile_window_t *window)
{
    window->load_sum = 0u;
    window->sample_count = 0u;
    window->load_peak = 0u;
    window->ctrl_at_peak = 0u;
    window->scope_at_peak = 0u;
    window->lat_at_peak = 0u;
    window->peak_tick = 0u;
}

void v2k_profile_init(void)
{
    v2k_profile_clear(&s_active);
    s_pending.load_sum = 0u;
    s_pending.sample_count = 0u;
    s_pending.load_peak = 0u;
    s_pending.ctrl_at_peak = 0u;
    s_pending.scope_at_peak = 0u;
    s_pending.lat_at_peak = 0u;
    s_pending.peak_tick = 0u;
    s_pending_seq = 0u;
    s_published_seq = 0u;
    g_v2k_prof_seq = 0u;
    g_v2k_cycle_budget = V2K_ISR_BUDGET_CYCLES;
    g_v2k_load_avg = 0u;
    g_v2k_load_peak = 0u;
    g_v2k_ctrl_at_peak = 0u;
    g_v2k_scope_at_peak = 0u;
    g_v2k_lat_at_peak = 0u;
    g_v2k_peak_tick = 0u;
}

void v2k_profile_sample(uint32_t isr_cycles,
                        uint32_t control_cycles,
                        uint32_t scope_cycles,
                        uint16_t latency,
                        v2k_tick_t tick)
{
    uint32_t next_seq;
    // isr_cycles is CPUTIMER1 ticks, latency is ePWM TBCTR ticks; adding them is
    // only valid while both share one 5 ns tick (pinned by the static assert in
    // v2k_timebase.h). load = ADC/EOC latency + ISR duration = control-cycle occupancy.
    uint32_t load_cycles = isr_cycles + (uint32_t)latency;

    s_active.load_sum += load_cycles;
    s_active.sample_count++;
    if (load_cycles > s_active.load_peak)
    {
        s_active.load_peak = load_cycles;
        s_active.ctrl_at_peak = control_cycles;
        s_active.scope_at_peak = scope_cycles;
        s_active.lat_at_peak = latency;
        s_active.peak_tick = tick;
    }
    if (s_active.sample_count < V2K_PROFILE_WINDOW_SAMPLES)
    {
        return;
    }

    s_pending.load_sum = s_active.load_sum;
    s_pending.sample_count = s_active.sample_count;
    s_pending.load_peak = s_active.load_peak;
    s_pending.ctrl_at_peak = s_active.ctrl_at_peak;
    s_pending.scope_at_peak = s_active.scope_at_peak;
    s_pending.lat_at_peak = s_active.lat_at_peak;
    s_pending.peak_tick = s_active.peak_tick;
    next_seq = s_pending_seq + 1u;
    s_pending_seq = (next_seq != 0u) ? next_seq : 1u;
    v2k_profile_clear(&s_active);
}

void v2k_profile_service(void)
{
    v2k_profile_window_t candidate;
    uint32_t seq_before = s_pending_seq;
    uint32_t seq_after;

    if ((seq_before == 0u) || (seq_before == s_published_seq))
    {
        return;
    }
    candidate.load_sum = s_pending.load_sum;
    candidate.sample_count = s_pending.sample_count;
    candidate.load_peak = s_pending.load_peak;
    candidate.ctrl_at_peak = s_pending.ctrl_at_peak;
    candidate.scope_at_peak = s_pending.scope_at_peak;
    candidate.lat_at_peak = s_pending.lat_at_peak;
    candidate.peak_tick = s_pending.peak_tick;
    seq_after = s_pending_seq;
    if ((seq_before != seq_after) || (candidate.sample_count == 0u))
    {
        return;
    }

    g_v2k_load_avg = (uint32_t)(candidate.load_sum / candidate.sample_count);
    g_v2k_load_peak = candidate.load_peak;
    g_v2k_ctrl_at_peak = candidate.ctrl_at_peak;
    g_v2k_scope_at_peak = candidate.scope_at_peak;
    g_v2k_lat_at_peak = candidate.lat_at_peak;
    g_v2k_peak_tick = candidate.peak_tick;
    s_published_seq = seq_before;
    g_v2k_prof_seq = seq_before;
}

void v2k_profile_publish_status(volatile v2k_cpu1_status_t *status)
{
    // CPU2 reads this MSGRAM struct concurrently and brackets the profiler
    // region with prof_seq (wire 42) / prof_seq_end (wire 80). Two rules turn
    // that bracket into a real seqlock instead of a single trailing marker:
    //   1. publish only when a new one-second window has been serviced, so the
    //      struct is otherwise immutable while CPU2 reads it (this also keeps
    //      the lifetime counters coherent with the window they ride with, and
    //      drops a per-background-loop 11-field rewrite);
    //   2. clear prof_seq to 0 (the host's "collecting" sentinel) before the
    //      fields and write the real sequence last, so a cross-core read that
    //      straddles the update cannot observe a torn snapshot under a pair of
    //      equal, non-zero sequences.
    static uint32_t s_ipc_seq = 0xFFFFFFFFu; // != any real sequence: forces the first publish
    uint32_t seq = g_v2k_prof_seq;

    if (seq == s_ipc_seq)
    {
        return;
    }
    status->prof_seq = 0u;
    status->cycle_budget = g_v2k_cycle_budget;
    status->load_avg = g_v2k_load_avg;
    status->load_peak = g_v2k_load_peak;
    status->ctrl_at_peak = g_v2k_ctrl_at_peak;
    status->scope_at_peak = g_v2k_scope_at_peak;
    status->lat_at_peak = g_v2k_lat_at_peak;
    status->prof_reserved = 0u;
    status->peak_tick = g_v2k_peak_tick;
    status->budget_violations = g_v2k_isr_budget_violation_cnt;
    status->isr_overflows = g_v2k_isr_ovf_cnt;
    status->prof_seq = seq;
    s_ipc_seq = seq;
}
