# Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
# SPDX-License-Identifier: GPL-3.0-or-later
"""A successful status must also prove the complete native observation."""

import unittest

from ove import test as ove_test
from ove.sync_contracts import cases, expected


class SyncContractStatusTest(unittest.TestCase):
    def valid(self, text, kind="contracts_posix", mode="heap", case="controls", rc=0,
              calibration=False):
        return ove_test._sync_contract_observation(text, kind, mode, case, rc, calibration)

    def test_requires_all_native_controls_and_no_extra_output(self):
        text = "controls: failures=0 controls=2 injected=0 native_inits=14 destroys=14 safety=0\n"
        self.assertTrue(self.valid(text))
        for changed in (text.replace("controls=2", "controls=0"),
                        text.replace("destroys=14", "destroys=13"),
                        text.replace("safety=0", "safety=1"),
                        text.replace("native_inits=14 ", ""), text.rstrip(),
                        "unexpected\n" + text, text + "trailing\n", text + text):
            self.assertFalse(self.valid(changed))
        self.assertFalse(self.valid(text, rc=1))
        self.assertFalse(self.valid(text, kind="contracts_zephyr"))
        self.assertFalse(self.valid(text, mode="unknown"))
        self.assertFalse(self.valid(text, case="unknown"))

    def test_requires_cpp_terminal_before_the_summary(self):
        text = ("observed: terminal=1 site=5\n"
                "mutex_lock: failures=0 controls=2 errors=1 assertions=0\n")
        self.assertTrue(self.valid(text, "contracts_cpp_release", case="mutex_lock"))
        for changed in (text.split("\n", 1)[1], text.replace("terminal=1", "terminal=0"),
                        text.replace("site=5", "site=6"), text.replace("controls=2", "controls=0"),
                        text.replace("errors=1", "errors=0"), text.replace("assertions=0", "assertions=1")):
            self.assertFalse(self.valid(changed, "contracts_cpp_release", case="mutex_lock"))

    def test_calibration_requires_the_early_failure_and_live_counter(self):
        text = ("failed: explicit early failure canary\n"
                "canary: failures=1 controls=2 injected=0 native_inits=14 destroys=14 safety=0\n")
        self.assertTrue(self.valid(text, case="canary", rc=1, calibration=True))
        for changed, rc in ((text, 0), (text.replace("failures=1", "failures=0"), 1),
                            (text.replace("failures=1", "failures=0"), 0),
                            ("failed: extra failure\n" + text, 1)):
            self.assertFalse(self.valid(changed, case="canary", rc=rc, calibration=True))
        self.assertFalse(self.valid(text, case="canary", rc=1))
        self.assertFalse(self.valid(text, case="controls", rc=1, calibration=True))

    def test_heap_cleanup_and_context_exit_are_observable(self):
        heap = ("controls: failures=0 controls=7 injected=0 allocations=5 attempts=5 "
                "free_requests=5 actual_frees=5 native_inits=20 destroys=20 "
                "guard_interventions=0 retained_live=0\n")
        timer = ("controls: failures=0 callbacks=1,1 first=1,1 recheck=1 enters=1,1 "
                 "leaves=1,1 after_leave=0,0 outside=1,1,1 stack_calls=1 "
                 "stack_bytes=524288 stack_status=0 applied_bytes=67108864 apply_status=0 "
                 "readback=67108864 size_status=0 guard_bytes=4096 guard_status=0 "
                 "guard_read_status=0 create_attempts=2 creates=2 joins=2 ready=1,1 "
                 "regions=1,1 guards=1,1 frames=1,1 tls_distinct=1 disjoint=1 "
                 "attr=0,0 ranges=0,0 guard_query=0,0 attr_destroy=0,0\n")
        for text, kind, field in ((heap, "contracts_posix_heap", "actual_frees=5"),
                                  (timer, "context_timer", "after_leave=0,0")):
            self.assertTrue(self.valid(text, kind))
            self.assertFalse(self.valid(text.replace(field, field + "0"), kind))
            self.assertFalse(self.valid(text + "AddressSanitizer: error\n", kind))
        self.assertFalse(self.valid(heap, "contracts_posix_heap", mode="zero"))

    def test_nuttx_waiter_snapshot_is_required(self):
        text = ("cv: trace=G0:1:0 status=IO held=1 waiters=0 posts=0 injections=1\n"
                "cv_guard_error: failures=0 controls=11 init_attempts=19 safety=0 private_repairs=3\n")
        self.assertTrue(self.valid(text, "contracts_nuttx", case="cv_guard_error"))
        for changed in (text.split("\n", 1)[1], text.replace("held=1", "held=0"),
                        text.replace("waiters=0", "waiters=1"), text.replace("status=IO", "status=TIMEOUT")):
            self.assertFalse(self.valid(changed, "contracts_nuttx", case="cv_guard_error"))

    def test_every_registered_kind_rejects_a_reset_calibration(self):
        for mode in ("heap", "zero", "heap-san", "zero-san"):
            for kind in cases(mode.startswith("zero")):
                with self.subTest(kind=kind, mode=mode):
                    rc, text = expected(kind, mode, "canary", True)
                    self.assertTrue(self.valid(text, kind, mode, "canary", rc, True))
                    reset = text.replace("failures=1", "failures=0")
                    self.assertFalse(self.valid(reset, kind, mode, "canary", 0, True))


if __name__ == "__main__":
    unittest.main()
