//#############################################################################
// cpu2.c - CPU2 comms core (Phase 3.5 SCI data pump)
//
// Current responsibilities:
//   1. Rendezvous with CPU1 through IPC_sync, wait for descriptor magic, then
//      verify the contract version handshake (v2k_command.h).
//   2. Own the CPU2 plane and CPU2->CPU1 MSGRAM; service the parameter, scope,
//      and command planes.
//   3. Let the board pipe collect octets; run COBS/CRC/message services and TX
//      in the super-loop.
//   4. Acknowledge IPC ping-pong and blink the local status LED. Pad config and
//      CSEL ownership are assigned by CPU1; CPU2 only writes the data register.
//
// CPU2 does not own control time. Block timestamps, sampling epochs, and
// control scheduling all come from CPU1. The local low-rate heartbeat proves
// only that the comms core is still running; it must not enter sampling or
// control timing.
//#############################################################################

#include <string.h>
#include "../common/v2k_planes.h"
#include "v2k_cpu2_board.h"
#include "v2k_sci_service.h"

#define V2K_CPU2_SERVICE_DELAY_US 20uL
#define V2K_CPU2_HEARTBEAT_TICKS  (1000uL / V2K_CPU2_SERVICE_DELAY_US)
#define V2K_CPU2_LED_TOGGLE_TICKS (250000uL / V2K_CPU2_SERVICE_DELAY_US)

V2K_STATIC_ASSERT((1000uL % V2K_CPU2_SERVICE_DELAY_US) == 0uL);
V2K_STATIC_ASSERT((250000uL % V2K_CPU2_SERVICE_DELAY_US) == 0uL);
V2K_STATIC_ASSERT(V2K_CPU2_HEARTBEAT_TICKS <= 0xFFFFuL);
V2K_STATIC_ASSERT(V2K_CPU2_LED_TOGGLE_TICKS <= 0xFFFFuL);

//-----------------------------------------------------------------------------
// Shared-memory entities. Section -> physical-region mapping is in the CPU2
// 28p65x_generic_* linker command files.
//-----------------------------------------------------------------------------
#pragma DATA_SECTION(g_v2k_cpu2_plane, V2K_SECT_CPU2_PLANE)
v2k_cpu2_plane_t g_v2k_cpu2_plane;

#pragma DATA_SECTION(g_v2k_msg_2to1, V2K_SECT_MSG_2TO1)
v2k_msg_2to1_t g_v2k_msg_2to1;

//-----------------------------------------------------------------------------
// Observables for CCS Expressions.
//-----------------------------------------------------------------------------
uint32_t g_pong_cnt;          // Completed ping acknowledgements.
uint16_t g_handshake_state;   // 0=sync, 1=descriptor, 2=bad contract, 3=running.

void main(void)
{
    uint16_t led_count = 0u;
    uint16_t heartbeat_count = 0u;

    v2k_cpu2_board_init_device();
    v2k_cpu2_board_assert_layout(&g_v2k_cpu2_plane, &g_v2k_msg_2to1);

    memset(&g_v2k_cpu2_plane, 0, sizeof(g_v2k_cpu2_plane));
    memset(&g_v2k_msg_2to1, 0, sizeof(g_v2k_msg_2to1));

    //
    // Board-owned NMI backstop must be in place before the IPC rendezvous (an
    // NMI can arrive any time after reset). Both own vendor registers, so they
    // live below the seam.
    //
    v2k_cpu2_board_boot_init_interrupts();
    v2k_cpu2_board_boot_sync();

    //
    // Wait for descriptor publication. Single-writer publish protocol: the
    // table is readable only after magic appears.
    //
    g_handshake_state = 1u;
    while (V2K_CPU1_PLANE_RO->desc_table.hdr.magic != V2K_DESC_MAGIC) { }

    //
    // Contract version handshake. A mismatch means CPU1/CPU2 were flashed from
    // different firmware generations; shared layout is unsafe, so stop in a
    // diagnosable failure state.
    //
    if ((V2K_MSG_1TO2_RO->cpu1_status.contract_ver != V2K_CONTRACT_VER) ||
        (V2K_CPU1_PLANE_RO->desc_table.hdr.contract_ver   != V2K_CONTRACT_VER))
    {
        g_handshake_state = 2u;
        v2k_cpu2_board_panic_halt();
    }
    g_handshake_state = 3u;
    v2k_sci_init();

    for (;;)
    {
        // Ping-pong acknowledgement: acknowledge each observed ping.
        if (v2k_cpu2_board_ipc_pong_ack())
        {
            g_pong_cnt++;
        }

        // The board pipe only collects octets. Protocol parsing, shared-plane
        // services, and TX all run in the super-loop.
        v2k_sci_service();

        // Local diagnostic heartbeat does not enter control time or scope
        // timestamps. Keep the service cadence shorter than the SCI TX FIFO
        // drain time at 3125000 baud so push frames do not inherit a forced
        // 100 us gap between FIFO refills.
        v2k_cpu2_board_delay_us((uint16_t)V2K_CPU2_SERVICE_DELAY_US);
        heartbeat_count++;
        if (heartbeat_count >= (uint16_t)V2K_CPU2_HEARTBEAT_TICKS)
        {
            heartbeat_count = 0u;
            g_v2k_msg_2to1.cpu2_status.heartbeat++;
        }

        // Toggle roughly every 250 ms, for visual diagnostics only.
        led_count++;
        if (led_count >= (uint16_t)V2K_CPU2_LED_TOGGLE_TICKS)
        {
            led_count = 0u;
            v2k_cpu2_board_toggle_status_led();
        }
    }
}
