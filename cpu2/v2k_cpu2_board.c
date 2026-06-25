//=============================================================================
// v2k_cpu2_board.c - F28P65x CPU2 physical board seam
//=============================================================================

#include "driverlib.h"
#include "device.h"
#include "board.h"
#include "v2k_cpu2_board.h"

#define V2K_CPU2_STATUS_LED_PIN 13u

void v2k_cpu2_board_panic_halt(void)
{
    for (;;) { ESTOP0; }
}

void v2k_cpu2_board_init_device(void)
{
    Device_init();
}

void v2k_cpu2_board_assert_layout(const volatile v2k_gs4_plane_t *gs4,
                                  const volatile v2k_msg_2to1_t *msg_2to1)
{
    if (((uint32_t)gs4 != V2K_GS4_BASE) ||
        ((uint32_t)msg_2to1 != V2K_MSGRAM_2TO1_BASE))
    {
        v2k_cpu2_board_panic_halt();
    }
}

void v2k_cpu2_board_init_sci_pipe(void)
{
    SysCtl_enablePeripheral(SYSCTL_PERIPH_CLK_SCIA);
    SCIA_BASE_init();
}

void v2k_cpu2_board_delay_100us(void)
{
    DEVICE_DELAY_US(100);
}

void v2k_cpu2_board_toggle_status_led(void)
{
    GPIO_togglePin(V2K_CPU2_STATUS_LED_PIN);
}
