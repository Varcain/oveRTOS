# QEMU device selection for the oveRTOS browser emulator (arm-softmmu).
#
# Used with --without-default-devices by config/scripts/build-qemu-wasm.sh,
# so only what is listed here (and what it selects) is built.  This halves
# the size of the resulting qemu-system-arm.wasm.

# The MPS2 FPGA boards: mps2-an500 (Cortex-M7) and mps2-an521 (Cortex-M33).
CONFIG_MPS2=y

# -device loader: the Linux personality loads its rootfs into PSRAM with it.
CONFIG_GENERIC_LOADER=y

# target/arm references the GICv5 device model whether or not a board
# selects it; without this QEMU 11.1 fails to link.
CONFIG_ARM_GICV5=y
