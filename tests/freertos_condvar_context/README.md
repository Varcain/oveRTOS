# FreeRTOS condition-variable context validation

Run `make test-freertos-cv-context`, `make test-freertos-cv-context-sanitize`
and `make test-freertos-cv-context-arm`. CTest runs the host observers, which
include the actual adapter against initialized task, mutex and register models
for 16/32-bit ticks, ordinary and MPU V1 callers and both heap modes. The ARM
gate runs ordinary, V1, raw-critical and trace profiles of `arm/` on the real
FreeRTOS kernel under QEMU.
