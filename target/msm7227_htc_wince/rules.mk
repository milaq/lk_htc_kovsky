LOCAL_DIR := $(GET_LOCAL_DIR)

INCLUDES += -I$(LOCAL_DIR)/include -I$(LK_TOP_DIR)/platform/msm_shared/include

PLATFORM := msm7227

MEMBASE := 0x00000000 # SMI
MEMSIZE := 0x00080000 # 0.5MB

BASE_ADDR        := 0x00200000

TAGS_ADDR        := BASE_ADDR+0x00000100
KERNEL_ADDR      := BASE_ADDR+0x00008000
RAMDISK_ADDR     := BASE_ADDR+0x00800000
SCRATCH_ADDR     := BASE_ADDR+0x01000000

KEYS_USE_GPIO_KEYPAD := 0

DEFINES += DISPLAY_TYPE_MDDI=1
DEFINES += DISPLAY_TYPE_LCDC=0
DEFINES += DISPLAY_SPLASH_SCREEN=0
DEFINES += WITH_DEV_FBCON=1
DEFINES += DEX_DEBUG=0
DEFINES += WITH_STATIC_HEAP=1
DEFINES += HEAP_START=0x00100000
DEFINES += HEAP_LEN=0x00100000

DEVS += fbcon
MODULES += dev/fbcon
MODULES += dev/keys
MODULES += lib/ptable

DEFINES += \
	SDRAM_SIZE=$(MEMSIZE) \
	MEMBASE=$(MEMBASE) \
	BASE_ADDR=$(BASE_ADDR) \
	TAGS_ADDR=$(TAGS_ADDR) \
	KERNEL_ADDR=$(KERNEL_ADDR) \
	RAMDISK_ADDR=$(RAMDISK_ADDR) \
	SCRATCH_ADDR=$(SCRATCH_ADDR)

OBJS += \
	$(LOCAL_DIR)/init.o \
	$(LOCAL_DIR)/board_htcphoton.o \
	$(LOCAL_DIR)/dex_comm.o \
	$(LOCAL_DIR)/atags.o