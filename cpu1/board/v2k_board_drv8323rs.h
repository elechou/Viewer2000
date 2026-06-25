//=============================================================================
// v2k_board_drv8323rs.h - private DRV8323RS GPIO/SPI driver inside the board package
//=============================================================================
#ifndef V2K_BOARD_DRV8323RS_H
#define V2K_BOARD_DRV8323RS_H

#include <stdint.h>

void v2k_board_drv8323rs_disable(void);
void v2k_board_drv8323rs_enable(void);
uint16_t v2k_board_drv8323rs_wake_elapsed(void);
uint16_t v2k_board_drv8323rs_sleep_elapsed(void);
uint16_t v2k_board_drv8323rs_poll(void);
uint16_t v2k_board_drv8323rs_configure_and_verify(void);
uint16_t v2k_board_drv8323rs_has_fault(void);
volatile uint16_t *v2k_board_drv8323rs_status1_address(void);
volatile uint16_t *v2k_board_drv8323rs_status2_address(void);
volatile uint32_t *v2k_board_drv8323rs_spi_error_count_address(void);
volatile uint16_t *v2k_board_drv8323rs_config_valid_address(void);
volatile uint16_t *v2k_board_drv8323rs_control_readback_address(void);
volatile uint16_t *v2k_board_drv8323rs_gate_hs_readback_address(void);
volatile uint16_t *v2k_board_drv8323rs_gate_ls_readback_address(void);
volatile uint16_t *v2k_board_drv8323rs_ocp_readback_address(void);
volatile uint16_t *v2k_board_drv8323rs_csa_readback_address(void);
uint16_t v2k_board_drv8323rs_fault_source_is_released(void);

#endif // V2K_BOARD_DRV8323RS_H
