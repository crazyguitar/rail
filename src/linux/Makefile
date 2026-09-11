obj-m := railfs.o
railfs-y := $(patsubst $(src)/%.c,%.o,$(sort $(wildcard $(src)/railfs-*.c)))
include $(src)/railfs-compat.mk
