# FreeRTOS IRQ boundary checks

`make test-freertos-irq-masks` and its `-sanitize` variant compile the actual
FreeRTOS IRQ adapter against initialized scalar register models for the
ordinary and MPU V1 ports, both erratum settings and both allocation modes; each
model case is a CTest case that fails through its exit status.
`make test-freertos-irq-bindings` and its `-sanitize` variant link the actual
Rust `SpinMutex::with` and `critical_section::with` paths to the same model.
`target.py` and `arm/` are a manual native fixture and not part of these targets.
