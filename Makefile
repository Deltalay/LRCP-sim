PREFIX = arm-none-eabi-
CC = $(PREFIX)gcc
OBJCOPY = $(PREFIX)objcopy

TARGET = firmware
# SRCS = main.c uECC.c aes.c
SRCS = src/main.c src/uECC.c
LDSCRIPT = linker.ld

MCU = -mcpu=cortex-m4 -mthumb -mfpu=fpv4-sp-d16 -mfloat-abi=hard
CFLAGS = $(MCU) -O3 -Wall -ffunction-sections -fdata-sections -nostdlib -DuECC_OPTIMIZATION_LEVEL=3 -DuECC_SQUARE_FUNC=1 
LDFLAGS = -T $(LDSCRIPT) -Wl,--gc-sections


all: $(TARGET).bin

$(TARGET).elf: $(SRCS) $(LDSCRIPT)
	$(CC) $(CFLAGS) $(LDFLAGS) $(SRCS) -o $(TARGET).elf

$(TARGET).bin: $(TARGET).elf
	$(OBJCOPY) -O binary $(TARGET).elf $(TARGET).bin

clean:
	rm -f $(TARGET).elf $(TARGET).bin

flash: $(TARGET).bin
	openocd -f pi.cfg -f target/stm32f4x.cfg -c "reset_config none" -c "program $(TARGET).bin verify reset exit 0x08000000"

.PHONY: all clean flash
