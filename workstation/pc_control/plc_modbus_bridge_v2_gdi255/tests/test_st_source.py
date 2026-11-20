"""Behavioral tests execute PRG_RobotBridge.st through the strict subset harness."""
import unittest
from st_scan_harness import ScanHarness


class STSourceTests(unittest.TestCase):
    def fixture(self):
        harness = ScanHarness()
        harness.ready()
        return harness

    def test_stale_register_on_boot_is_ignored(self):
        h = ScanHarness()
        h.frame(1, 1, boot=1)
        h.scans(2300)
        self.assertFalse(h.get("gvl_robotbridge.xhostregistered"))
        self.assertEqual(h.get("gvl_robotbridge.wrobottxcontrol"), 0)

    def test_register_and_metadata(self):
        h = self.fixture()
        status = h.get("gvl_robotbridge.ahoststatus")
        self.assertEqual(status[:2], [0x4D42, 2])
        self.assertEqual(status[4:9], [0, 1, 1, 2, 0])
        self.assertEqual(status[15:17], [0x1234, 0x5678])

    def test_enable_has_one_pulse_and_requires_feedback(self):
        h = self.fixture()
        h.frame(2, 2)
        trace = []
        for _ in range(230):
            h.scan(robot_base=0x0094 if h.time > 3500 else 0x0090)
            trace.append(bool(h.get("gvl_robotbridge.wrobottxcontrol") & 0x10))
        rises = sum(value and (i == 0 or not trace[i-1]) for i, value in enumerate(trace))
        self.assertEqual(rises, 1)
        self.assertGreaterEqual(sum(trace), 50)
        self.assertEqual(h.get("wackresult"), 2)
        h.scans(1500, robot_base=0x0094)
        self.assertFalse(h.get("gvl_robotbridge.wrobottxcontrol") & 0x10)

    def test_already_enabled_does_not_toggle_power(self):
        h = self.fixture()
        h.frame(2, 2); h.scans(100, robot_base=0x0094)
        self.assertEqual(h.get("wackresult"), 2)
        self.assertEqual(h.get("gvl_robotbridge.wrobottxcontrol") & 0x10, 0)

    def test_program3_is_binary_gdi_and_load_is_unconfirmed(self):
        h = self.fixture()
        h.frame(4, 2, 3); h.scans(1800)
        self.assertEqual(h.get("gvl_robotbridge.wrobottxcontrol") & 0xF, 0)
        self.assertEqual(h.get("gvl_robotbridge.arobottx"), [0, 3])
        self.assertEqual(h.get("wackresult"), 3)
        self.assertFalse(h.get("gvl_robotbridge.xloadoperatorconfirmed"))
        h.frame(5, 3); h.scans(80, robot_base=0x0094)
        self.assertEqual(h.get("wackresult"), 4)
        self.assertEqual(h.get("wackerror"), 9)

    def test_confirm_load_and_run_feedback(self):
        h = self.fixture()
        h.frame(4, 2, 1); h.scans(1800)
        h.frame(12, 3, 1); h.scans(80)
        self.assertTrue(h.get("gvl_robotbridge.xloadoperatorconfirmed"))
        h.frame(5, 4); h.scans(1100, robot_base=0x0094)
        h.scans(800, robot_base=0x0085)
        self.assertEqual(h.get("wackresult"), 2)
        self.assertEqual(h.get("wackcmd"), 5)

    def test_stop_preempts_and_has_own_full_low_high_cycle(self):
        h = self.fixture()
        h.frame(2, 2); h.scans(700)
        self.assertEqual(h.get("uistep"), 20)
        h.frame(7, 3); h.scans(80)
        self.assertEqual(h.get("udiinterruptedseq"), 2)
        self.assertEqual(h.get("wackcmd"), 7)
        self.assertEqual(h.get("gvl_robotbridge.wrobottxcontrol") & 0x0810, 0)
        h.scans(1800)
        self.assertEqual(h.get("wackresult"), 2)

    def test_busy_rejection_does_not_replace_inflight_ack(self):
        h = self.fixture()
        h.frame(2, 2); h.scans(100)
        h.frame(4, 3, 1); h.scans(80)
        self.assertEqual(h.get("udirejectedseq"), 3)
        self.assertEqual(h.get("wlastreject"), 5)
        self.assertEqual(h.get("udiackseq"), 2)
        self.assertEqual(h.get("wackcmd"), 2)

    def test_same_sequence_changed_payload_rejected(self):
        h = self.fixture()
        h.frame(4, 2, 1); h.scans(1800)
        h.frame(4, 2, 2); h.scans(80)
        self.assertEqual(h.get("wlastreject"), 4)
        self.assertEqual(h.get("uiselectedprogram"), 1)

    def test_wrong_owner_and_boot_cannot_control(self):
        h = self.fixture()
        h.frame(2, 2, owner=99); h.scans(80)
        self.assertEqual(h.get("wlastreject"), 2)
        h.frame(2, 3, boot=999); h.scans(80)
        self.assertEqual(h.get("wlastreject"), 3)
        self.assertEqual(h.get("gvl_robotbridge.wrobottxcontrol"), 0)

    def test_pc_loss_stops_and_does_not_re_register_old_frame(self):
        h = self.fixture()
        h.frame(2, 2); h.scans(1900, robot_base=0x0094)
        h.scans(3500, host_heartbeat=False, robot_base=0x0085)
        self.assertFalse(h.get("gvl_robotbridge.xhostregistered"))
        self.assertTrue(h.get("gvl_robotbridge.xstopinterlock"))
        self.assertEqual(h.get("gvl_robotbridge.wrobottxcontrol") & 0x800, 0x800)
        h.scans(500, robot_base=0x0090)
        self.assertFalse(h.get("gvl_robotbridge.xhostregistered"))

    def test_robot_loss_sets_interlock_and_needs_explicit_rearm(self):
        h = self.fixture()
        h.frame(2, 2); h.scans(1900, robot_base=0x0094)
        h.scans(4500, robot_heartbeat=False)
        self.assertFalse(h.get("gvl_robotbridge.xrobotlinkok"))
        self.assertTrue(h.get("gvl_robotbridge.xstopinterlock"))
        h.scans(2500)
        self.assertTrue(h.get("gvl_robotbridge.xrobotlinkok"))
        self.assertTrue(h.get("gvl_robotbridge.xstopinterlock"))
        h.frame(11, 3); h.scans(80)
        self.assertFalse(h.get("gvl_robotbridge.xstopinterlock"))

    def test_stop_allowed_when_remote_permit_is_revoked(self):
        h = self.fixture()
        h.frame(2, 2); h.scans(1900, robot_base=0x0094)
        h.values["gvl_robotbridge.xremotepermit"] = False
        h.frame(7, 3); h.scans(1800)
        self.assertTrue(h.get("gvl_robotbridge.xstopinterlock"))
        self.assertEqual(h.get("wackresult"), 2)

    def test_feedback_timeout_does_not_repeat_enable_toggle(self):
        h = self.fixture()
        h.frame(2, 2); h.scans(10500)
        self.assertEqual(h.get("wackresult"), 5)
        self.assertEqual(h.get("wackerror"), 10)
        self.assertTrue(h.get("gvl_robotbridge.xstopinterlock"))
        self.assertEqual(h.get("gvl_robotbridge.wrobottxcontrol") & 0x10, 0)

    def test_uncommissioned_and_no_permit_cannot_enable(self):
        h = self.fixture()
        h.values["gvl_robotbridge.xcommissioned"] = False
        h.frame(2, 2); h.scans(80)
        self.assertEqual(h.get("wackerror"), 13)
        self.assertEqual(h.get("gvl_robotbridge.wrobottxcontrol"), 0)
        h.values["gvl_robotbridge.xcommissioned"] = True
        h.values["gvl_robotbridge.xremotepermit"] = False
        h.frame(2, 3); h.scans(80)
        self.assertEqual(h.get("wackerror"), 6)
        self.assertEqual(h.get("gvl_robotbridge.wrobottxcontrol"), 0)

    def test_static_heartbeat_other_status_changes_do_not_prove_link(self):
        h = ScanHarness()
        for n in range(600):
            h.scan(robot_heartbeat=False, robot_base=0x90 | (n % 2) * 4)
        self.assertFalse(h.get("gvl_robotbridge.xrobotlinkok"))

    def test_all_32_robot_words_are_mirrored(self):
        h = self.fixture()
        raw = h.get("gvl_robotbridge.arobotrx")
        for n in range(1, 32): raw[n] = 2000 + n
        h.scan()
        self.assertEqual(h.get("gvl_robotbridge.ahoststatus")[20:52], raw)
        self.assertEqual(len(h.get("gvl_robotbridge.ahostcmd")), 16)
        self.assertEqual(len(h.get("gvl_robotbridge.ahoststatus")), 64)

    def test_leaving_external_auto_invalidates_load_confirmation(self):
        h = self.fixture()
        h.frame(4, 2, 1); h.scans(1800)
        h.frame(12, 3, 1); h.scans(80)
        self.assertTrue(h.get("gvl_robotbridge.xloadoperatorconfirmed"))
        h.scans(50, robot_base=0x0030)
        self.assertFalse(h.get("gvl_robotbridge.xloadoperatorconfirmed"))

    def test_invalid_parameter_and_reserved_fields_rejected(self):
        h = self.fixture()
        h.frame(2, 2, 100); h.scans(80)
        self.assertEqual(h.get("wlastreject"), 8)
        h.frame(2, 3)
        h.get("gvl_robotbridge.ahostcmd")[10] = 1
        h.scans(80)
        self.assertEqual(h.get("wlastreject"), 14)
        self.assertEqual(h.get("gvl_robotbridge.wrobottxcontrol"), 0)

    def test_wrong_owner_heartbeat_does_not_keep_session_alive(self):
        h = self.fixture()
        h.frame(2, 2); h.scans(1800, robot_base=0x0094)
        h.frame(2, 3, owner=99)
        h.scans(3500, robot_base=0x0085)
        self.assertFalse(h.get("gvl_robotbridge.xhostregistered"))
        self.assertTrue(h.get("gvl_robotbridge.xstopinterlock"))


    def test_all_255_program_numbers_are_exact_and_never_run(self):
        h = self.fixture()
        for number in range(1, 256):
            h.frame(4, number + 1, number)
            h.scans(1700)
            tx = h.get("gvl_robotbridge.arobottx")
            self.assertEqual(tx[1], number, number)
            self.assertEqual(tx[0] & 0x10F, 0, number)
            self.assertEqual(h.get("wackresult"), 3, number)
            self.assertFalse(h.get("gvl_robotbridge.xloadoperatorconfirmed"))
            status = h.get("gvl_robotbridge.ahoststatus")
            self.assertEqual(status[13], number)
            self.assertEqual(status[57:62], [1, 255, number, 49, 56])
            self.assertEqual(status[63], 2)

    def test_zero_256_and_65535_do_not_change_selected_program(self):
        h = self.fixture()
        h.frame(4, 2, 255); h.scans(1700)
        for seq, number in enumerate((0, 256, 65535), 3):
            h.frame(4, seq, number); h.scans(80)
            self.assertEqual(h.get("wackerror"), 8)
            self.assertEqual(h.get("gvl_robotbridge.arobottx"), [0, 255])

    def test_program16_is_stable_before_single_load_edge(self):
        h = self.fixture()
        h.frame(4, 2, 16)
        states = []
        for _ in range(180):
            h.scan()
            states.append(tuple(h.get("gvl_robotbridge.arobottx")))
        high = [i for i, (control, _) in enumerate(states) if control & 0x20]
        self.assertTrue(high)
        first = high[0]
        self.assertGreaterEqual(first, 50)
        self.assertTrue(all(value == 16 for _, value in states[first-50:first+1]))
        self.assertEqual(sum(bool(c & 0x20) and (i == 0 or not states[i-1][0] & 0x20)
                             for i, (c, _) in enumerate(states)), 1)
        self.assertTrue(all(not control & 0xF10F for control, _ in states))

    def test_gdi_mapping_is_local_and_required(self):
        h = self.fixture()
        h.values["gvl_robotbridge.xgdiprogrammappingconfirmed"] = False
        h.frame(4, 2, 16); h.scans(80)
        self.assertEqual(h.get("wackerror"), 13)
        self.assertEqual(h.get("gvl_robotbridge.arobottx"), [0, 0])
        self.assertFalse(h.get("gvl_robotbridge.ahoststatus")[2] & 1024)

    def test_gdi_mapping_loss_latches_stop_and_clears_confirmation(self):
        h = self.fixture()
        h.frame(4, 2, 255); h.scans(1700)
        h.frame(12, 3, 255); h.scans(80)
        self.assertTrue(h.get("gvl_robotbridge.xloadoperatorconfirmed"))
        h.values["gvl_robotbridge.xgdiprogrammappingconfirmed"] = False
        h.scan()
        self.assertTrue(h.get("gvl_robotbridge.xstopinterlock"))
        self.assertFalse(h.get("gvl_robotbridge.xloadoperatorconfirmed"))
        self.assertEqual(h.get("gvl_robotbridge.arobottx"), [0x800, 0])

    def test_v1_client_is_rejected_without_output(self):
        h = ScanHarness()
        h.values["gvl_robotbridge.xcommissioned"] = True
        h.values["gvl_robotbridge.xgdiprogrammappingconfirmed"] = True
        h.scans(2200)
        h.frame(1, 1)
        h.get("gvl_robotbridge.ahostcmd")[0] = 1
        h.scans(80)
        self.assertEqual(h.get("wlastreject"), 1)
        self.assertFalse(h.get("gvl_robotbridge.xhostregistered"))
        self.assertEqual(h.get("gvl_robotbridge.arobottx"), [0, 0])


if __name__ == "__main__":
    unittest.main()
