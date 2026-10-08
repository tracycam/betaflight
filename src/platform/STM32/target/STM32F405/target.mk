TARGET_MCU        := STM32F405xx
TARGET_MCU_FAMILY := STM32F4

# MH2425 variant uses CCM-aware linker script (128K CCM + .ccm_code section)
ifeq ($(CONFIG),F405-BT-v1.5-MH)
  LD_SCRIPT := $(LINKER_DIR)/stm32_flash_f405_mh.ld
endif
