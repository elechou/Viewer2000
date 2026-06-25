//=============================================================================
// v2k_user_runtime.c - resettable user lifecycle support
//=============================================================================

#include "v2k_user_runtime.h"
#include "v2k_crc32_prime.h"
#include "../board/v2k_board.h"

#include <crc_defines.h>
#include <crc_tbl.h>

#define V2K_LINKER_VALUE(symbol) ((uint32_t)&(symbol))

extern uint16_t V2K_UserDataLoadStart;
extern uint16_t V2K_UserDataLoadEnd;
extern uint16_t V2K_UserDataLoadSize;
extern uint16_t V2K_UserDataRunStart;
extern uint16_t V2K_UserDataRunEnd;
extern uint16_t V2K_UserDataRunSize;
extern uint16_t V2K_UserBssStart;
extern uint16_t V2K_UserBssEnd;
extern uint16_t V2K_UserBssSize;
extern uint16_t V2K_UserDataCrcPresent;
extern CRC_TABLE V2K_UserDataCrcTable;

volatile v2k_io_t v2k_io;
volatile uint16_t g_v2k_app_enabled;
volatile uint32_t g_v2k_user_reset_count;
volatile uint16_t g_v2k_user_reset_error;
volatile uint16_t g_v2k_user_reset_active;
volatile uint32_t g_v2k_user_crc_expected;
volatile uint32_t g_v2k_user_crc_actual;

static void v2k_user_set_neutral_output(void)
{
    v2k_io.pwm.duty_a = V2K_DUTY_NEUTRAL;
    v2k_io.pwm.duty_b = V2K_DUTY_NEUTRAL;
    v2k_io.pwm.duty_c = V2K_DUTY_NEUTRAL;
}

void v2k_pwm_apply(float duty_a, float duty_b, float duty_c)
{
    v2k_io.pwm.duty_a = duty_a;
    v2k_io.pwm.duty_b = duty_b;
    v2k_io.pwm.duty_c = duty_c;
    v2k_board_pwm_apply_command(&v2k_io.pwm);
}

#pragma WEAK(setup)
void setup(void)
{
}

#pragma WEAK(control)
void control(void)
{
}

static void v2k_copy_words(uint16_t *dst, const uint16_t *src, uint32_t words)
{
    uint32_t i;
    for (i = 0uL; i < words; i++)
    {
        dst[i] = src[i];
    }
}

static void v2k_clear_words(uint16_t *dst, uint32_t words)
{
    uint32_t i;
    for (i = 0uL; i < words; i++)
    {
        dst[i] = 0u;
    }
}

static uint16_t v2k_user_layout(uint32_t *load_start,
                                uint32_t *run_start,
                                uint32_t *data_words,
                                uint32_t *bss_start,
                                uint32_t *bss_words)
{
    uint32_t load_end = V2K_LINKER_VALUE(V2K_UserDataLoadEnd);
    uint32_t load_size = V2K_LINKER_VALUE(V2K_UserDataLoadSize);
    uint32_t run_end = V2K_LINKER_VALUE(V2K_UserDataRunEnd);
    uint32_t run_size = V2K_LINKER_VALUE(V2K_UserDataRunSize);
    uint32_t bss_end = V2K_LINKER_VALUE(V2K_UserBssEnd);
    uint32_t bss_size = V2K_LINKER_VALUE(V2K_UserBssSize);

    *load_start = V2K_LINKER_VALUE(V2K_UserDataLoadStart);
    *run_start = V2K_LINKER_VALUE(V2K_UserDataRunStart);
    *data_words = run_size;
    *bss_start = V2K_LINKER_VALUE(V2K_UserBssStart);
    *bss_words = bss_size;

    if ((load_size != run_size) ||
        (load_end < *load_start) || ((load_end - *load_start) != load_size) ||
        (run_end < *run_start) || ((run_end - *run_start) != run_size) ||
        (bss_end < *bss_start) || ((bss_end - *bss_start) != bss_size))
    {
        g_v2k_user_reset_error = V2K_USER_RESET_ERR_LAYOUT;
        return 0u;
    }
    if ((run_size != 0uL) &&
        !((load_end <= *run_start) || (run_end <= *load_start)))
    {
        g_v2k_user_reset_error = V2K_USER_RESET_ERR_LAYOUT;
        return 0u;
    }
    if ((run_size != 0uL) && (bss_size != 0uL) &&
        (run_end > *bss_start) && (bss_end > *run_start))
    {
        g_v2k_user_reset_error = V2K_USER_RESET_ERR_LAYOUT;
        return 0u;
    }
    return 1u;
}

static uint16_t v2k_user_crc_record(uint32_t load_start,
                                    uint32_t data_words,
                                    uint32_t *expected)
{
    uint32_t crc_present = V2K_LINKER_VALUE(V2K_UserDataCrcPresent);

    if (data_words == 0uL)
    {
        if (crc_present != 0uL)
        {
            g_v2k_user_reset_error = V2K_USER_RESET_ERR_CRC_TABLE;
            return 0u;
        }
        *expected = 0uL;
        return 1u;
    }

    if ((crc_present != 1uL) ||
        (V2K_UserDataCrcTable.num_recs != 1u) ||
        (V2K_UserDataCrcTable.rec_size != sizeof(CRC_RECORD)) ||
        (V2K_UserDataCrcTable.recs[0].crc_alg_ID != CRC32_PRIME) ||
        (V2K_UserDataCrcTable.recs[0].addr != load_start) ||
        (V2K_UserDataCrcTable.recs[0].size != data_words))
    {
        g_v2k_user_reset_error = V2K_USER_RESET_ERR_CRC_TABLE;
        return 0u;
    }
    *expected = V2K_UserDataCrcTable.recs[0].crc_value;
    return 1u;
}

static uint16_t v2k_user_restore(uint16_t count_reset)
{
    uint32_t load_start;
    uint32_t run_start;
    uint32_t data_words;
    uint32_t bss_start;
    uint32_t bss_words;
    uint32_t expected;
    uint32_t actual;

    if (v2k_user_layout(&load_start, &run_start, &data_words,
                        &bss_start, &bss_words) == 0u)
    {
        return 0u;
    }
    if (v2k_user_crc_record(load_start, data_words, &expected) == 0u)
    {
        return 0u;
    }

    g_v2k_user_crc_expected = expected;
    if (data_words != 0uL)
    {
        actual = v2k_crc32_prime((const uint16_t *)load_start, data_words);
        g_v2k_user_crc_actual = actual;
        if (actual != expected)
        {
            g_v2k_user_reset_error = V2K_USER_RESET_ERR_GOLDEN_CRC;
            return 0u;
        }

        v2k_copy_words((uint16_t *)run_start,
                       (const uint16_t *)load_start,
                       data_words);
        actual = v2k_crc32_prime((const uint16_t *)run_start, data_words);
        g_v2k_user_crc_actual = actual;
        if (actual != expected)
        {
            g_v2k_user_reset_error = V2K_USER_RESET_ERR_RUN_CRC;
            return 0u;
        }
    }
    else
    {
        g_v2k_user_crc_actual = 0uL;
    }

    v2k_clear_words((uint16_t *)bss_start, bss_words);
    if (count_reset != 0u)
    {
        g_v2k_user_reset_count++;
    }
    g_v2k_user_reset_error = V2K_USER_RESET_OK;
    return 1u;
}

void v2k_user_runtime_init(void)
{
    g_v2k_app_enabled = 0u;
    g_v2k_user_reset_active = 1u;
    v2k_user_set_neutral_output();
    (void)v2k_user_restore(0u);
    g_v2k_user_reset_active = 0u;
}

uint16_t v2k_user_prepare_start(void)
{
    g_v2k_app_enabled = 0u;
    g_v2k_user_reset_active = 1u;
    v2k_user_set_neutral_output();

    if (v2k_user_restore(1u) == 0u)
    {
        g_v2k_user_reset_active = 0u;
        return 0u;
    }

    setup();
    v2k_user_set_neutral_output();
    g_v2k_user_reset_active = 0u;
    g_v2k_app_enabled = 1u;
    return 1u;
}

void v2k_user_disable(void)
{
    g_v2k_app_enabled = 0u;
    v2k_user_set_neutral_output();
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
