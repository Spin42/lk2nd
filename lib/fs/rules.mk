LOCAL_DIR := $(GET_LOCAL_DIR)

MODULES += \
	lib/fs/ext2 \
	lib/fs/fat

OBJS += \
	$(LOCAL_DIR)/fs.o \
	$(LOCAL_DIR)/debug.o
