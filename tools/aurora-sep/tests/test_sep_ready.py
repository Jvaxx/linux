#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""aurora-sep-ready reports ready only once the sensor attached.

On boot 0ab402d0 the open of /dev/sep-bio succeeded although the sensor
attach had failed, and the script logged "Touch ID is ready after 0 s". Run
it against a synthetic diag directory and /dev/null as the device.
"""

import os
import subprocess
import tempfile
import threading
import time
import unittest
from pathlib import Path

SCRIPT = Path(__file__).resolve().parents[1] / "aurora-sep-ready"


class SepReadyTest(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.devices = Path(self.tmp.name)
        self.diag = self.devices / "1234.sep" / "diag"
        self.diag.mkdir(parents=True)

    def tearDown(self):
        self.tmp.cleanup()

    def state(self, **attrs):
        # Atomically, as a sysfs read sees one value or the other.
        for name, value in attrs.items():
            staged = self.diag / (".staged-" + name)
            staged.write_text(value + "\n")
            os.replace(staged, self.diag / name)

    def run_ready(self, wait=3, dev="/dev/null"):
        env = dict(os.environ, AURORA_SEP_DEV=dev,
                   AURORA_SEP_SYSFS=str(self.devices),
                   AURORA_SEP_READY_WAIT=str(wait))
        done = subprocess.run(["sh", str(SCRIPT)], env=env, capture_output=True,
                              text=True, timeout=wait + 20)
        return done.returncode, done.stderr

    def test_attach_failure_is_not_reported_ready(self):
        # Boot 0ab402d0: activation worked, the attach did not.
        self.state(touchid="not-ready", sensor="failed",
                   sensor_attach="not-ready", restore="unknown")
        rc, log = self.run_ready()
        self.assertEqual(rc, 1)
        self.assertIn("NOT ready", log)
        self.assertIn("sensor attach incomplete", log)
        self.assertIn("sensor failed", log)
        self.assertNotIn("is ready", log)

    def test_ready_after_restore(self):
        self.state(touchid="ready", sensor="online",
                   sensor_attach="attached", restore="restored")
        rc, log = self.run_ready()
        self.assertEqual(rc, 0)
        self.assertIn("is ready", log)
        self.assertIn("restored", log)

    def test_ready_with_nothing_enrolled(self):
        self.state(touchid="ready", sensor="online",
                   sensor_attach="attached", restore="empty")
        rc, log = self.run_ready()
        self.assertEqual(rc, 0)
        self.assertIn("no fingerprint is enrolled", log)

    def test_unproven_restore_is_not_ready(self):
        self.state(touchid="ready", sensor="online",
                   sensor_attach="attached", restore="unproven")
        rc, log = self.run_ready()
        self.assertEqual(rc, 1)
        self.assertIn("were not restored", log)

    def test_waits_for_the_late_retry(self):
        self.state(touchid="not-ready", sensor="failed",
                   sensor_attach="retrying", restore="unknown")

        def attach():
            time.sleep(1.5)
            self.state(touchid="ready", sensor="online", restore="restored")
            self.state(sensor_attach="attached")

        writer = threading.Thread(target=attach)
        writer.start()
        began = time.monotonic()
        rc, log = self.run_ready(wait=10)
        writer.join()
        self.assertEqual(rc, 0, log)
        self.assertGreaterEqual(time.monotonic() - began, 1.5)
        self.assertIn("is ready", log)

    def test_a_late_retry_that_fails_is_not_ready(self):
        self.state(touchid="not-ready", sensor="failed",
                   sensor_attach="retrying", restore="unknown")

        def give_up():
            time.sleep(1)
            self.state(sensor_attach="not-ready")

        writer = threading.Thread(target=give_up)
        writer.start()
        rc, log = self.run_ready(wait=10)
        writer.join()
        self.assertEqual(rc, 1)
        self.assertIn("every automatic bring-up fell short", log)

    def test_gives_up_waiting(self):
        self.state(touchid="not-ready", sensor="failed",
                   sensor_attach="retrying", restore="unknown")
        rc, log = self.run_ready(wait=2)
        self.assertEqual(rc, 1)
        self.assertIn("still retrying", log)

    def test_activation_failure(self):
        self.state(touchid="failed", keybag="failed", keystore="open",
                   sensor_attach="unknown")
        rc, log = self.run_ready()
        self.assertEqual(rc, 1)
        self.assertIn("activation failed", log)
        self.assertIn("keybag did not load", log)

    def test_not_provisioned_is_not_a_failure(self):
        self.state(touchid="failed", keybag="missing", keystore="open",
                   sensor_attach="unknown")
        rc, log = self.run_ready()
        self.assertEqual(rc, 0)
        self.assertIn("not set up", log)

    def test_kernel_with_diag_abi_1(self):
        self.state(touchid="ready", sensor="online")
        rc, log = self.run_ready()
        self.assertEqual(rc, 0)
        self.assertIn("does not report the fingerprint restore", log)
        self.state(touchid="not-ready", sensor="failed")
        rc, log = self.run_ready()
        self.assertEqual(rc, 1)
        self.assertIn("sensor attach incomplete", log)

    def test_kernel_without_diag(self):
        for name in os.listdir(self.diag):
            os.unlink(self.diag / name)
        self.diag.rmdir()
        rc, log = self.run_ready()
        self.assertEqual(rc, 0)
        self.assertIn("does not report", log)
        self.assertNotIn("is ready", log)

    def test_no_device(self):
        rc, log = self.run_ready(dev=str(self.devices / "sep-bio"))
        self.assertEqual(rc, 0)
        self.assertIn("is not there", log)

    @unittest.skipUnless(os.path.exists("/dev/mem") and not os.access("/dev/mem", os.R_OK),
                         "needs a character device this user cannot open")
    def test_open_failure_is_not_ready(self):
        self.state(touchid="failed", keybag="present", keystore="open")
        rc, log = self.run_ready(dev="/dev/mem")
        self.assertEqual(rc, 1)
        self.assertIn("could not open", log)


if __name__ == "__main__":
    unittest.main()
