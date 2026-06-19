//=============================================================================
// v2k_user_runtime.c - resettable user lifecycle support
//=============================================================================

#include "v2k_user_runtime.h"

#define V2K_USER_DATA_SNAPSHOT_WORDS 256u

extern uint16_t V2K_UserDataStart;
extern uint16_t V2K_UserDataEnd;
extern uint16_t V2K_UserBssStart;
extern uint16_t V2K_UserBssEnd;

volatile v2k_io_t v2k_io;
volatile uint16_t g_v2k_app_enabled;
volatile uint32_t g_v2k_user_reset_count;
volatile uint16_t g_v2k_user_reset_error;
volatile uint16_t g_v2k_user_reset_active;

static uint16_t s_user_data_snapshot[V2K_USER_DATA_SNAPSHOT_WORDS];
static uint16_t s_user_snapshot_ready;
static uint16_t s_user_data_words;

#pragma WEAK(setup)
void setup(void)
{
}

#pragma WEAK(control)
void control(void)
{
}

static uint16_t v2k_user_section_words(const uint16_t *start, const uint16_t *end)
{
    return (uint16_t)((uint32_t)end - (uint32_t)start);
}

static void v2k_copy_words(uint16_t *dst, const uint16_t *src, uint16_t words)
{
    uint16_t i;
    for (i = 0u; i < words; i++)
    {
        dst[i] = src[i];
    }
}

static void v2k_clear_words(uint16_t *dst, uint16_t words)
{
    uint16_t i;
    for (i = 0u; i < words; i++)
    {
        dst[i] = 0u;
    }
}

void v2k_user_runtime_init(void)
{
    s_user_data_words =
        v2k_user_section_words(&V2K_UserDataStart, &V2K_UserDataEnd);
    if (s_user_data_words > V2K_USER_DATA_SNAPSHOT_WORDS)
    {
        g_v2k_user_reset_error = 1u;
        s_user_snapshot_ready = 0u;
        return;
    }

    v2k_copy_words(s_user_data_snapshot, &V2K_UserDataStart, s_user_data_words);
    s_user_snapshot_ready = 1u;
    v2k_io.out.duty_a = V2K_DUTY_A_SAFE;
}

static uint16_t v2k_user_reset_sections(void)
{
    uint16_t bss_words;

    if (s_user_snapshot_ready == 0u)
    {
        g_v2k_user_reset_error = 2u;
        return 0u;
    }

    v2k_copy_words(&V2K_UserDataStart, s_user_data_snapshot, s_user_data_words);

    bss_words = v2k_user_section_words(&V2K_UserBssStart, &V2K_UserBssEnd);
    v2k_clear_words(&V2K_UserBssStart, bss_words);

    g_v2k_user_reset_count++;
    return 1u;
}

uint16_t v2k_user_prepare_start(void)
{
    g_v2k_app_enabled = 0u;
    g_v2k_user_reset_active = 1u;
    v2k_io.out.duty_a = V2K_DUTY_A_SAFE;

    if (v2k_user_reset_sections() == 0u)
    {
        g_v2k_user_reset_active = 0u;
        return 0u;
    }

    setup();
    v2k_io.out.duty_a = V2K_DUTY_A_SAFE;
    g_v2k_user_reset_active = 0u;
    g_v2k_app_enabled = 1u;
    return 1u;
}

void v2k_user_disable(void)
{
    g_v2k_app_enabled = 0u;
    v2k_io.out.duty_a = V2K_DUTY_A_SAFE;
}

uint16_t v2k_user_is_enabled(void)
{
    return g_v2k_app_enabled;
}

uint16_t v2k_user_reset_is_active(void)
{
    return g_v2k_user_reset_active;
}

void v2k_user_control_tick(void)
{
    if (g_v2k_app_enabled != 0u)
    {
        control();
    }
}
