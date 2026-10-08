#!/bin/bash
# Boot a qemu-mps2-an500 firmware headless for a few seconds and check its
# console for signs that the app did not come up: a fault, a stack overflow,
# a panic, an assertion, or an error-level log line.
#
# It is a smoke test, not a functional one — it tells an app that runs from
# one that dies at start-up.  The docs workflow uses it to keep broken demos
# off the site.
#
# Usage: qemu-smoke.sh <firmware.elf> [seconds]
#
# Exit status: 0 the app started and logged no failure, 1 otherwise (the
# offending console lines are printed).

set -uo pipefail

ELF="${1:?Usage: $0 <firmware.elf> [seconds]}"
SECONDS_TO_RUN="${2:-8}"

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
RUN="${SCRIPT_DIR}/../../boards/qemu-mps2-an500/qemu-run.sh"

LOG="$(mktemp -t ove-smoke.XXXXXX)"
trap 'rm -f "${LOG}"' EXIT

# The app never exits on its own; the timeout is the normal way out.
"${RUN}" "${ELF}" --headless --no-gdb --no-net --timeout "${SECONDS_TO_RUN}" \
    > "${LOG}" 2>&1 < /dev/null || true

if ! grep -aq "ove: starting" "${LOG}"; then
    echo "smoke: no start-up banner"
    head -5 "${LOG}"
    exit 1
fi

FAILURES="$(grep -aiE 'hard ?fault|usage ?fault|bus ?fault|mem ?fault|stack overflow|panic|assert|\[E\]' "${LOG}" | head -5)"
if [ -n "${FAILURES}" ]; then
    echo "smoke: the app reported a failure"
    echo "${FAILURES}"
    exit 1
fi

exit 0
