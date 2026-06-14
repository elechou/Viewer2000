//=============================================================================
// v2k_sci_service.h - Phase 3.5 SCIA/XDS110 协议数据泵
//=============================================================================
#ifndef V2K_SCI_SERVICE_H
#define V2K_SCI_SERVICE_H

#include <stdint.h>

// 波特率 / 帧格式 / FIFO 启停由 cpu2 sysconfig 的 SCI 实例落地，对应
// SCIA_BASE_BAUDRATE 等宏在生成的 board.h；改 baud 走 sysconfig，本服务
// 不再单独维护一份常量。

extern volatile uint32_t g_v2k_sci_rx_octets;
extern volatile uint32_t g_v2k_sci_tx_octets;
extern volatile uint32_t g_v2k_sci_rx_overflow;
extern volatile uint32_t g_v2k_sci_bad_frames;
extern volatile uint32_t g_v2k_sci_good_frames;

void v2k_sci_init(void);
void v2k_sci_service(void);

#endif // V2K_SCI_SERVICE_H
