//=============================================================================
// wire.h - readable L0-to-L1 seam for the CPU1 runtime
//
// L1 runtime code calls this interface for timing, safe output application, background
// device service, and protection. User code may directly read completed
// peripheral results through TI DriverLib, but it does not configure timing,
// output, interrupt, ownership, or protection resources.
//=============================================================================
#ifndef WIRE_H
#define WIRE_H

#include "../v2k.h"

typedef void (*wire_isr_handler_t)(void);

#define WIRE_POWERSTAGE_MODE_POWERED 0u
#define WIRE_POWERSTAGE_MODE_DRY_RUN 1u

#define WIRE_START_PENDING 0u
#define WIRE_START_READY   1u
#define WIRE_START_FAILED  2u

#define WIRE_START_BLOCK_NONE              0x0000u
#define WIRE_START_BLOCK_CONFIG_UNAPPROVED 0x0001u
#define WIRE_START_BLOCK_NFAULT            0x0002u
#define WIRE_START_BLOCK_SPI               0x0004u
#define WIRE_START_BLOCK_DRV_CONFIG        0x0008u
#define WIRE_START_BLOCK_DRV_STATUS        0x0010u
#define WIRE_START_BLOCK_CURRENT_PROTECTION 0x0020u

// Control time base and fixed ISR fast path.
void wire_timebase_init(wire_isr_handler_t adc_isr);
void wire_timebase_start(void);
void wire_apply(const volatile v2k_io_out_t *out);
uint32_t wire_cycle_count(void);
uint16_t wire_pwm_counter(void);
uint16_t wire_isr_ack(void);
void wire_gpio_probe_set(uint16_t value);

// Bounded background work and platform variable enumeration.
void wire_background_service(void);
void wire_powerstage_start_begin(void);
uint16_t wire_powerstage_start_poll(void);
void wire_powerstage_cancel_start(void);
void wire_register_ports(uint16_t fast_prescaler);

// Protection lifecycle. These are the only output lock/release operations.
void wire_fault_pre_board_lock_outputs(void);
void wire_fault_init_trip_isr(wire_isr_handler_t tz_isr);
void wire_fault_disable_irq(void);
void wire_fault_enable_irq(void);
void wire_fault_clear_interrupt_flag(void);
void wire_fault_force_output_lock(void);
uint16_t wire_fault_release_output_lock(void);
void wire_fault_defer_driver_shutdown(void);
uint16_t wire_fault_driver_shutdown_is_complete(void);
uint16_t wire_fault_output_is_locked(void);
uint16_t wire_fault_source_is_released(void);
uint16_t wire_fault_trip_is_current(void);
void wire_fault_ack_isr(void);

void wire_panic_halt(void);

#endif // WIRE_H
