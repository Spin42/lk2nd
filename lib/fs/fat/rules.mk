LOCAL_DIR := $(GET_LOCAL_DIR)

MODULES += \
	lib/fs \
	lib/bio

OBJS += \
	$(LOCAL_DIR)/fat.o \
	$(LOCAL_DIR)/ff.o \
	$(LOCAL_DIR)/ffunicode.o
