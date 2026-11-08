# Event group construction and context contracts

Run `make test-eventgroup-contracts` and
`make test-eventgroup-contracts-sanitize`. CTest runs the provider, context and
POSIX integration observers in both heap modes; each fails through its exit
status. The provider observers compile the actual initialization paths against
inert SDK spies. `context_models.py` builds EventGroup-only, EventGroup plus
TIME and feature-off context models in both heap modes.
