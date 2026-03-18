arm_ss = ss.source_set()
arm_common_ss = ss.source_set()
arm_common_ss.add(when: 'CONFIG_ARM_VIRT', if_true: files('virt.c'))
arm_common_ss.add(when: 'CONFIG_ACPI', if_true: files('virt-acpi-build.c'))
arm_common_ss.add(when: 'CONFIG_DIGIC', if_true: files('digic_boards.cpp'))
arm_common_ss.add(when: 'CONFIG_EMCRAFT_SF2', if_true: files('msf2-som.cpp'))
arm_common_ss.add(when: 'CONFIG_HIGHBANK', if_true: files('highbank.cpp'))
arm_common_ss.add(when: 'CONFIG_INTEGRATOR', if_true: files('integratorcp.c'))
arm_common_ss.add(when: 'CONFIG_MICROBIT', if_true: files('microbit.cpp'))
arm_common_ss.add(when: 'CONFIG_MPS3R', if_true: files('mps3r.cpp'))
arm_common_ss.add(when: 'CONFIG_MUSICPAL', if_true: [files('musicpal.c')])
arm_common_ss.add(when: 'CONFIG_NETDUINOPLUS2', if_true: files('netduinoplus2.cpp'))
arm_common_ss.add(when: 'CONFIG_OLIMEX_STM32_H405', if_true: files('olimex-stm32-h405.cpp'))
arm_common_ss.add(when: 'CONFIG_NPCM7XX', if_true: files('npcm7xx.cpp', 'npcm7xx_boards.cpp'))
arm_common_ss.add(when: 'CONFIG_NPCM8XX', if_true: files('npcm8xx.cpp', 'npcm8xx_boards.cpp'))
arm_common_ss.add(when: 'CONFIG_REALVIEW', if_true: files('realview.c'))
arm_common_ss.add(when: 'CONFIG_SBSA_REF', if_true: files('sbsa-ref.c'))
arm_common_ss.add(when: 'CONFIG_STELLARIS', if_true: files('stellaris.c'))
arm_common_ss.add(when: 'CONFIG_STM32VLDISCOVERY', if_true: files('stm32vldiscovery.cpp'))
arm_common_ss.add(when: 'CONFIG_ZYNQ', if_true: files('xilinx_zynq.c'))
arm_common_ss.add(when: 'CONFIG_SABRELITE', if_true: files('sabrelite.cpp'))

arm_common_ss.add(when: 'CONFIG_ARM_V7M', if_true: files('armv7m.c'))
arm_common_ss.add(when: 'CONFIG_EXYNOS4', if_true: files('exynos4210.cpp'))
arm_common_ss.add(when: 'CONFIG_DIGIC', if_true: files('digic.cpp'))
arm_common_ss.add(when: 'CONFIG_OMAP', if_true: files('omap1.c'))
arm_common_ss.add(when: 'CONFIG_ALLWINNER_A10', if_true: files('allwinner-a10.cpp', 'cubieboard.cpp'))
arm_common_ss.add(when: 'CONFIG_ALLWINNER_H3', if_true: files('allwinner-h3.cpp', 'orangepi.cpp'))
arm_common_ss.add(when: 'CONFIG_ALLWINNER_R40', if_true: files('allwinner-r40.cpp', 'bananapi_m2u.cpp'))
arm_common_ss.add(when: 'CONFIG_MAX78000_SOC', if_true: files('max78000_soc.cpp'))
arm_ss.add(when: 'CONFIG_RASPI', if_true: files('bcm2836.cpp', 'raspi.cpp'))
arm_common_ss.add(when: ['CONFIG_RASPI', 'TARGET_AARCH64'], if_true: files('bcm2838.cpp', 'raspi4b.cpp'))
arm_common_ss.add(when: 'CONFIG_STM32F100_SOC', if_true: files('stm32f100_soc.cpp'))
arm_common_ss.add(when: 'CONFIG_STM32F205_SOC', if_true: files('stm32f205_soc.cpp'))
arm_common_ss.add(when: 'CONFIG_STM32F405_SOC', if_true: files('stm32f405_soc.cpp'))
arm_common_ss.add(when: 'CONFIG_B_L475E_IOT01A', if_true: files('b-l475e-iot01a.cpp'))
arm_common_ss.add(when: 'CONFIG_STM32L4X5_SOC', if_true: files('stm32l4x5_soc.cpp'))
arm_common_ss.add(when: 'CONFIG_XLNX_ZYNQMP_ARM', if_true: files('xlnx-zynqmp.c', 'xlnx-zcu102.c'))
arm_common_ss.add(when: 'CONFIG_XLNX_VERSAL', if_true: files('xlnx-versal.c', 'xlnx-versal-virt.c'))
arm_common_ss.add(when: 'CONFIG_FSL_IMX25', if_true: files('fsl-imx25.cpp', 'imx25_pdk.cpp'))
arm_common_ss.add(when: 'CONFIG_FSL_IMX31', if_true: files('fsl-imx31.cpp', 'kzm.cpp'))
arm_common_ss.add(when: 'CONFIG_FSL_IMX6', if_true: files('fsl-imx6.cpp'))
arm_ss.add(when: 'CONFIG_ASPEED_SOC', if_true: files(
  'aspeed.c',
  'aspeed_soc_common.cpp',
  'aspeed_ast2400.cpp',
  'aspeed_ast2400_palmetto.cpp',
  'aspeed_ast2400_quanta-q71l.cpp',
  'aspeed_ast2400_supermicrox11.cpp',
  'aspeed_ast2500_evb.cpp',
  'aspeed_ast2500_fp5280g2.cpp',
  'aspeed_ast2500_g220a.cpp',
  'aspeed_ast2500_romulus.cpp',
  'aspeed_ast2500_sonorapass.cpp',
  'aspeed_ast2500_supermicro-x11spi.cpp',
  'aspeed_ast2500_tiogapass.cpp',
  'aspeed_ast2500_witherspoon.cpp',
  'aspeed_ast2500_yosemitev2.cpp',
  'aspeed_ast2600.cpp',
  'aspeed_ast2600_bletchley.cpp',
  'aspeed_ast2600_catalina.cpp',
  'aspeed_ast2600_evb.cpp',
  'aspeed_ast2600_fby35.cpp',
  'aspeed_ast2600_fuji.cpp',
  'aspeed_ast2600_gb200nvl.cpp',
  'aspeed_ast2600_qcom-dc-scm-v1.cpp',
  'aspeed_ast2600_qcom-firework.cpp',
  'aspeed_ast2600_rainier.cpp',
  'aspeed_ast10x0.cpp',
  'aspeed_ast10x0_evb.cpp',
  'fby35.cpp'))
arm_common_ss.add(when: ['CONFIG_ASPEED_SOC', 'TARGET_AARCH64'], if_true: files(
  'aspeed_ast27x0.cpp',
  'aspeed_ast27x0_evb.cpp',
  'aspeed_ast27x0-fc.cpp',
  'aspeed_ast27x0-ssp.cpp',
  'aspeed_ast27x0-tsp.cpp',
  'aspeed_coprocessor_common.cpp'))
arm_common_ss.add(when: 'CONFIG_MPS2', if_true: files('mps2.cpp'))
arm_common_ss.add(when: 'CONFIG_MPS2', if_true: files('mps2-tz.c'))
arm_common_ss.add(when: 'CONFIG_MSF2', if_true: files('msf2-soc.cpp'))
arm_common_ss.add(when: 'CONFIG_MUSCA', if_true: files('musca.cpp'))
arm_common_ss.add(when: 'CONFIG_ARMSSE', if_true: files('armsse.c'))
arm_common_ss.add(when: 'CONFIG_FSL_IMX7', if_true: files('fsl-imx7.cpp', 'mcimx7d-sabre.cpp'))
arm_common_ss.add(when: 'CONFIG_FSL_IMX8MP', if_true: files('fsl-imx8mp.c'))
arm_common_ss.add(when: 'CONFIG_FSL_IMX8MP_EVK', if_true: files('imx8mp-evk.cpp'))
arm_common_ss.add(when: 'CONFIG_ARM_SMMUV3', if_true: files('smmuv3.c'))
arm_common_ss.add(when: 'CONFIG_FSL_IMX6UL', if_true: files('fsl-imx6ul.cpp', 'mcimx6ul-evk.cpp'))
arm_common_ss.add(when: 'CONFIG_NRF51_SOC', if_true: files('nrf51_soc.cpp'))
arm_common_ss.add(when: 'CONFIG_XEN', if_true: files(
  'xen-stubs.cpp',
  'xen-pvh.cpp',
))

arm_common_ss.add(when: 'CONFIG_ARM_SMMUV3', if_true: files('smmu-common.c'))
arm_common_ss.add(when: 'CONFIG_COLLIE', if_true: files('collie.cpp'))
arm_common_ss.add(when: 'CONFIG_EXYNOS4', if_true: files('exynos4_boards.cpp'))
arm_common_ss.add(when: 'CONFIG_MAX78000FTHR', if_true: files('max78000fthr.cpp'))
arm_common_ss.add(when: 'CONFIG_NETDUINO2', if_true: files('netduino2.cpp'))
arm_common_ss.add(when: 'CONFIG_RASPI', if_true: files('bcm2835_peripherals.cpp'))
arm_common_ss.add(when: 'CONFIG_RASPI', if_true: files('bcm2838_peripherals.cpp'))
arm_common_ss.add(when: 'CONFIG_STRONGARM', if_true: files('strongarm.c'))
arm_common_ss.add(when: 'CONFIG_SX1', if_true: files('omap_sx1.cpp'))
arm_common_ss.add(when: 'CONFIG_VERSATILE', if_true: files('versatilepb.cpp'))
arm_common_ss.add(when: 'CONFIG_VEXPRESS', if_true: files('vexpress.c'))

arm_common_ss.add(files('boot.c'))

hw_arch += {'arm': arm_ss}
hw_common_arch += {'arm': arm_common_ss}
