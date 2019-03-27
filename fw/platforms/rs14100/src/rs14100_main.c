/*
 * Copyright (c) 2014-2019 Cesanta Software Limited
 * All rights reserved
 *
 * Licensed under the Apache License, Version 2.0 (the ""License"");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an ""AS IS"" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "common/platform.h"

#include "mgos_hal_freertos_internal.h"
#include "mgos_core_dump.h"
#include "mgos_gpio.h"
#include "mgos_system.h"
#include "mgos_uart.h"

#include "rs14100_sdk.h"

#define LED_ON 0
#define LED1_PIN RS14100_ULP_GPIO(0) /* TRI LED green, 0 = on, 1 = off */
#define LED2_PIN RS14100_ULP_GPIO(2) /* TRI LED blue,  0 = on, 1 = off */

// 31: debug

extern void arm_exc_handler_top(void);
extern const void *flash_int_vectors[60];
static void (*int_vectors[256])(void)
    __attribute__((section(".ram_int_vectors")));

void rs14100_set_int_handler(int irqn, void (*handler)(void)) {
  int_vectors[irqn + 16] = handler;
}

#define ICACHE_BASE 0x20280000
#define ICACHE_CTRL_REG (*((volatile uint32_t *) (ICACHE_BASE + 0x14)))
#define ICACHE_XLATE_REG (*((volatile uint32_t *) (ICACHE_BASE + 0x24)))

uint32_t SystemCoreClockMHZ = 0;

void rs14100_enable_icache(void) {
  // Enable instruction cache.
  RSI_PS_M4ssPeriPowerUp(M4SS_PWRGATE_ULP_ICACHE);
  M4CLK->CLK_ENABLE_SET_REG1_b.ICACHE_CLK_ENABLE_b = 1;
  // M4CLK->CLK_ENABLE_SET_REG1_b.ICACHE_CLK_2X_ENABLE_b = 1;
  // Per HRM, only required for 120MHz+, but we do it always, just in case.
  ICACHE_XLATE_REG |= (1 << 21);
  ICACHE_CTRL_REG = 0x1;  // 4-Way assoc, enable.
}

enum mgos_init_result mgos_hal_freertos_pre_init(void) {
  return MGOS_INIT_OK;
}

IRAM void rs14100_qspi_clock_config(void) {
  // If CPU clock is less than 100 Mhz, make QSPI run in sync with it.
  if (system_clocks.soc_clock <= 100000000) {
    M4CLK->CLK_ENABLE_SET_REG3_b.QSPI_M4_SOC_SYNC_b = 1;
  } else {
    // Otherwise, use SoC PLL. If it's above 100 Mhz, add a divider.
    int div = 1, clk = system_clocks.soc_pll_clock;
    while (clk / div > 100000000) {
      div++;
    }
    M4CLK->CLK_CONFIG_REG1_b.QSPI_CLK_SWALLOW_SEL = 0;
    if (div % 2 == 0) {
      M4CLK->CLK_CONFIG_REG1_b.QSPI_CLK_DIV_FAC = div / 2;
      M4CLK->CLK_CONFIG_REG2_b.QSPI_ODD_DIV_SEL = 0;
    } else {
      M4CLK->CLK_CONFIG_REG1_b.QSPI_CLK_DIV_FAC = div;
      M4CLK->CLK_CONFIG_REG2_b.QSPI_ODD_DIV_SEL = 1;
    }
    M4CLK->CLK_CONFIG_REG1_b.QSPI_CLK_SEL = 3;  // SoC PLL
  }
}

void rs14100_clock_config(uint32_t cpu_freq) {
  RSI_IPMU_CommonConfig();
  RSI_IPMU_PMUCommonConfig();
  RSI_IPMU_M32rc_OscTrimEfuse();
  RSI_IPMU_M20rcOsc_TrimEfuse();
  RSI_IPMU_DBLR32M_TrimEfuse();
  RSI_IPMU_M20roOsc_TrimEfuse();
  RSI_IPMU_RO32khz_TrimEfuse();
  RSI_IPMU_RC16khz_TrimEfuse();
  RSI_IPMU_RC64khz_TrimEfuse();
  RSI_IPMU_RC32khz_TrimEfuse();
  RSI_IPMU_RO_TsEfuse();
  RSI_IPMU_Vbattstatus_TrimEfuse();
  RSI_IPMU_Vbg_Tsbjt_Efuse();
  RSI_IPMU_Auxadcoff_DiffEfuse();
  RSI_IPMU_Auxadcgain_DiffEfuse();
  RSI_IPMU_Auxadcoff_SeEfuse();
  RSI_IPMU_Auxadcgain_SeEfuse();
  RSI_IPMU_Bg_TrimEfuse();
  RSI_IPMU_Blackout_TrimEfuse();
  RSI_IPMU_POCbias_Efuse();
  RSI_IPMU_Buck_TrimEfuse();
  RSI_IPMU_Ldosoc_TrimEfuse();
  RSI_IPMU_Dpwmfreq_TrimEfuse();
  RSI_IPMU_Delvbe_Tsbjt_Efuse();
  RSI_IPMU_Xtal1bias_Efuse();
  RSI_IPMU_Xtal2bias_Efuse();
  RSI_IPMU_InitCalibData();

  RSI_CLK_PeripheralClkEnable3(M4CLK, M4_SOC_CLK_FOR_OTHER_ENABLE);
  RSI_CLK_M4ssRefClkConfig(M4CLK, ULP_32MHZ_RC_CLK);
  RSI_ULPSS_RefClkConfig(ULPSS_ULP_32MHZ_RC_CLK);

  RSI_IPMU_ClockMuxSel(1);
  RSI_PS_FsmLfClkSel(KHZ_RO_CLK_SEL);

  system_clocks.m4ss_ref_clk = DEFAULT_32MHZ_RC_CLOCK;
  system_clocks.ulpss_ref_clk = DEFAULT_32MHZ_RC_CLOCK;
  system_clocks.modem_pll_clock = DEFAULT_MODEM_PLL_CLOCK;
  system_clocks.modem_pll_clock2 = DEFAULT_MODEM_PLL_CLOCK;
  system_clocks.intf_pll_clock = DEFAULT_INTF_PLL_CLOCK;
  system_clocks.rc_32khz_clock = DEFAULT_32KHZ_RC_CLOCK;
  system_clocks.rc_32mhz_clock = DEFAULT_32MHZ_RC_CLOCK;
  system_clocks.ro_20mhz_clock = DEFAULT_20MHZ_RO_CLOCK;
  system_clocks.ro_32khz_clock = DEFAULT_32KHZ_RO_CLOCK;
  system_clocks.xtal_32khz_clock = DEFAULT_32KHZ_XTAL_CLOCK;
  system_clocks.doubler_clock = DEFAULT_DOUBLER_CLOCK;
  system_clocks.rf_ref_clock = DEFAULT_RF_REF_CLOCK;
  system_clocks.mems_ref_clock = DEFAULT_MEMS_REF_CLOCK;
  system_clocks.byp_rc_ref_clock = DEFAULT_32MHZ_RC_CLOCK;
  system_clocks.i2s_pll_clock = DEFAULT_I2S_PLL_CLOCK;

  // 32 MHz RC clk input for the PLLs (all of them, not just SoC).
  RSI_CLK_SocPllRefClkConfig(2);
  RSI_CLK_SetSocPllFreq(M4CLK, cpu_freq, 32000000);

  if (cpu_freq > 90000000) {
    RSI_PS_SetDcDcToHigerVoltage();
    RSI_PS_PowerStateChangePs3toPs4();
  } else {
    RSI_PS_PowerStateChangePs4toPs3();
    RSI_Configure_DCDC_LowerVoltage();
    RSI_PS_SetDcDcToLowerVoltage();
  }
  system_clocks.soc_pll_clock = cpu_freq;

  RSI_CLK_M4SocClkConfig(M4CLK, M4_SOCPLLCLK, 0);
  system_clocks.soc_clock = cpu_freq;

  rs14100_qspi_clock_config();
  SystemCoreClockUpdate();
}

void SystemCoreClockUpdate(void) {
  SystemCoreClock = system_clocks.soc_clock;
  SystemCoreClockMHZ = SystemCoreClock / 1000000;
}

int main(void) {
  /* Move int vectors to RAM. */
  for (int i = 0; i < (int) ARRAY_SIZE(int_vectors); i++) {
    int_vectors[i] = arm_exc_handler_top;
  }
  memcpy(int_vectors, flash_int_vectors, sizeof(flash_int_vectors));
  for (int i = 0; i < (int) ARRAY_SIZE(int_vectors); i++) {
    if (int_vectors[i] == NULL) int_vectors[i] = arm_exc_handler_top;
  }
  SCB->VTOR = (uint32_t) &int_vectors[0];

  SystemInit();
  rs14100_clock_config(180000000 /* cpu_freq */);

  RSI_PS_FsmHfClkSel(FSM_32MHZ_RC);
  RSI_PS_PmuUltraSleepConfig(1U);
  // For some reason cache just doesn't help, even slows things down a bit.
  // rs14100_enable_icache();

  // Set up region 7 (highest priority) to block first 256 bytes of SRAM (to
  // catch NULL derefs).
  MPU->RNR = 7;
  MPU->RBAR = 0;
  MPU->RASR = MPU_RASR_ENABLE_Msk |
              (7 << MPU_RASR_SIZE_Pos) |  // 256 bytes (2^(7+1)).
              (0 << MPU_RASR_AP_Pos) |    // No RW access.
              (1 << MPU_RASR_XN_Pos);     // No code execution.
  // We run in privileged mode all the time so PRIVDEFENA acts as
  // "allow everything else".
  MPU->CTRL = MPU_CTRL_ENABLE_Msk | MPU_CTRL_PRIVDEFENA_Msk;
  // MPU->CTRL = 0;

  // mgos_gpio_setup_output(LED1_PIN, !LED_ON);
  // mgos_gpio_setup_output(LED2_PIN, !LED_ON);

  NVIC_SetPriorityGrouping(3 /* NVIC_PRIORITYGROUP_4 */);
  NVIC_SetPriority(MemoryManagement_IRQn, 0);
  NVIC_SetPriority(BusFault_IRQn, 0);
  NVIC_SetPriority(UsageFault_IRQn, 0);
  NVIC_SetPriority(DebugMonitor_IRQn, 0);
  rs14100_set_int_handler(SVCall_IRQn, SVC_Handler);
  rs14100_set_int_handler(PendSV_IRQn, PendSV_Handler);
  rs14100_set_int_handler(SysTick_IRQn, SysTick_Handler);
  mgos_hal_freertos_run_mgos_task(true /* start_scheduler */);
  /* not reached */
  abort();
}
