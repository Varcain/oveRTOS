zephyr_library()
zephyr_library_sources(display_shmem.c)
zephyr_library_include_directories(
    ${OVE_DIR}/boards/qemu-mps2-an500/src
)
