"""Execute delivered ST with simulated sockets/inputs; never contact field devices."""
import unittest
import zlib

from test_bridge import Mailbox, FtpStub, Cabinet
from st_runtime import World, Pointer

PROJECT = 'pcjob_bound_fixed.sr'
FILES = ('_project.srd', 'capture_home.srd', 'capture_home.srp', 'go_home.srd',
         'go_home.srp', 'helix_25mms.srd', 'helix_25mms.srp')


def configured_box():
    box = Mailbox()
    for name, value in {'xfixedmappingconfirmed': True, 'sfixedproject': PROJECT,
                        'uifixedcapture': 1, 'uifixedpath': 2, 'uifixedreturn': 3}.items():
        box.gvl.set(name, value)
    robot = box.world.globals['gvl_robotbridge']
    robot.set('uifixedjobguardversion', 120)
    for name, value in {'xrobotlinkok': True, 'xrobotstopped': True}.items(): robot.set(name, value)
    box.scan()
    return box


class FixedMailboxTests(unittest.TestCase):
    def test_fixed_configuration_crc_and_numbers_published(self):
        box = configured_box(); s = box.gvl.get('astatus')
        self.assertEqual(s[49:55], [1, 0, 0, 1, 2, 3])
        self.assertEqual((s[55] << 16) | s[56], zlib.crc32(PROJECT.encode()))
        self.assertEqual(s[59], 1)

    def test_only_exact_locally_confirmed_target_and_seven_names(self):
        cases = [('pcjob_wrong_project.sr', FILES[0], 7), (PROJECT, 'probe.txt', 7),
                 (PROJECT, '../bad.srp', 7), (PROJECT, FILES[0], 6)]
        for project, name, count in cases:
            with self.subTest(project=project, name=name, count=count):
                box = configured_box(); box.start(name=name, count=count, mode=2, project=project)
                self.assertNotEqual(box.prg.get('wreject'), 0)
                self.assertEqual(box.prg.get('fbftp').starts, 0)
                self.assertFalse(box.gvl.get('xfixedupdatebusy'))

    def test_unconfirmed_mapping_and_enabled_or_moving_robot_refuse(self):
        cases = [('gvl_filebridge', 'xfixedmappingconfirmed', False),
                 ('gvl_robotbridge', 'xrobotenabled', True), ('gvl_robotbridge', 'xrobotrunning', True),
                 ('gvl_robotbridge', 'xrobotpaused', True), ('gvl_robotbridge', 'xrobotstopped', False),
                 ('gvl_robotbridge', 'xrobotlinkok', False), ('gvl_robotbridge', 'xbusy', True)]
        for namespace, name, value in cases:
            with self.subTest(name=name):
                box = configured_box(); box.world.globals[namespace].set(name, value)
                box.start(name=FILES[0], count=7, mode=2, project=PROJECT)
                self.assertEqual(box.prg.get('wreject'), 0xE015)
                self.assertFalse(box.gvl.get('xfixedupdatebusy'))

    def test_original_create_mode_cannot_target_reserved_fixed_directory(self):
        box = configured_box(); box.start(name=FILES[0], count=7, mode=1, project=PROJECT)
        self.assertEqual(box.prg.get('wreject'), 0xE015)

    def test_missing_robot_guard_does_not_advertise_a_configured_fixed_target(self):
        box = configured_box()
        box.world.globals['gvl_robotbridge'].set('uifixedjobguardversion', 0)
        box.start(name=FILES[0], count=7, mode=2, project=PROJECT)
        self.assertEqual(box.prg.get('wreject'), 0xE015)
        self.assertEqual(box.gvl.get('astatus')[49], 0)

    def test_last_scan_state_change_cannot_finish_and_release(self):
        box = configured_box(); self.upload(box)
        box.world.globals['gvl_robotbridge'].set('xrobotenabled', True)
        box.command(5)
        self.assertNotEqual(box.prg.get('uistate'), 400)
        self.assertFalse(box.gvl.get('xfixedready'))

    def upload(self, box):
        for ordinal, name in enumerate(FILES, 1):
            content = b'' if name == 'capture_home.srd' else name.encode()
            box.start(content, name, 7, ordinal, mode=2, project=PROJECT)
            self.assertTrue(box.gvl.get('xfixedupdatebusy'))
            self.assertFalse(box.gvl.get('xfixedready'))
            if content: box.command(2, payload=content)
            box.command(3); box.scan(4)
            self.assertEqual(box.prg.get('uistate'), 200)

    def test_all_seven_readbacks_then_explicit_finish_release_numbers(self):
        box = configured_box(); self.upload(box)
        self.assertFalse(box.gvl.get('xfixedready'))
        box.command(5)
        self.assertEqual(box.prg.get('uistate'), 400)
        self.assertTrue(box.gvl.get('xfixedready'))
        self.assertFalse(box.gvl.get('xfixedupdatebusy'))
        self.assertEqual(box.prg.get('fbftp').starts, 7)
        box.command(6)
        self.assertTrue(box.gvl.get('xfixedready'))

    def test_failed_partial_update_and_local_reset_never_release_numbers(self):
        box = configured_box(); box.start(name=FILES[0], count=7, mode=2, project=PROJECT)
        box.command(2, payload=b'bad')  # expected CRC is for abc
        box.command(3)
        self.assertEqual(box.prg.get('uistate'), 900)
        self.assertFalse(box.gvl.get('xfixedready'))
        box.gvl.set('xreset', True); box.scan()
        self.assertFalse(box.gvl.get('xfixedready'))

    def test_number_or_project_changes_invalidate_ready_and_abort_active_session(self):
        box = configured_box(); self.upload(box); box.command(5)
        box.gvl.set('uifixedpath', 8); box.scan()
        self.assertFalse(box.gvl.get('xfixedready'))
        for field, value in [('uifixedpath', 8), ('sfixedproject', 'pcjob_other.sr')]:
            box = configured_box(); box.start(name=FILES[0], count=7, mode=2, project=PROJECT)
            box.gvl.set(field, value); box.scan()
            self.assertEqual(box.prg.get('uistate'), 900)
            self.assertFalse(box.gvl.get('xfixedready'))

    def test_robot_ownership_change_mid_update_aborts(self):
        box = configured_box(); box.start(name=FILES[0], count=7, mode=2, project=PROJECT)
        box.world.globals['gvl_robotbridge'].set('xrobotenabled', True); box.scan()
        self.assertEqual(box.prg.get('uistate'), 900)
        self.assertFalse(box.gvl.get('xfixedready'))

    def test_reboot_drops_previous_ready(self):
        box = configured_box(); self.upload(box); box.command(5)
        box.prg = box.world.instance('prg_filebridge'); box.scan()
        self.assertFalse(box.gvl.get('xfixedready'))


class FixedFtpTests(unittest.TestCase):
    def run_ftp(self, server, *, confirmed=True, wrong_target=False):
        world = World(native_factory=server.native)
        env = world.globals['gvl_filebridge']
        env.set('xfixedmappingconfirmed', confirmed); env.set('xfixedidle', True)
        env.set('sfixedproject', PROJECT)
        world.globals['gvl_robotbridge'].set('uifixedjobguardversion', 120)
        env.values['abuffer'][:3] = b'abc'
        fb = world.instance('fb_fileftp')
        args = dict(xstart=True, xabort=False, pbuffer=Pointer(env, 'abuffer'), udisize=3,
                    sproject='pcjob_wrong.sr' if wrong_target else PROJECT, sfile='helix_25mms.srp',
                    xcreateproject=True, xreplacefixed=True, suser='test', spassword='local_test')
        for _ in range(4000):
            fb.call(args)
            if fb.get('xdone'): return fb
        self.fail('FTP mock did not terminate')

    def test_existing_exact_fixed_directory_may_be_updated_and_read_back(self):
        server = Cabinet(existing=True); fb = self.run_ftp(server)
        self.assertTrue(fb.get('xtestpassed'))
        self.assertIn('STOR helix_25mms.srp', server.commands)
        self.assertIn('RETR helix_25mms.srp', server.commands)
        self.assertFalse(any(command.startswith('MKD') for command in server.commands))

    def test_missing_bound_project_never_creates_a_new_directory(self):
        server = Cabinet(); fb = self.run_ftp(server)
        self.assertFalse(fb.get('xtestpassed')); self.assertEqual(fb.get('werrorid'), 0xF20B)
        self.assertFalse(any(command.startswith(('MKD', 'STOR')) for command in server.commands))

    def test_direct_backend_call_cannot_bypass_local_target_allowlist(self):
        for options in ({'confirmed': False}, {'wrong_target': True}):
            server = Cabinet(existing=True); fb = self.run_ftp(server, **options)
            self.assertFalse(fb.get('xtestpassed')); self.assertEqual(fb.get('werrorid'), 0xF20C)
            self.assertFalse(server.commands)


class FixedRobotInterlockTests(unittest.TestCase):
    def setUp(self):
        self.box = configured_box(); self.world = self.box.world
        self.gvl = self.world.globals['gvl_robotbridge']
        self.robot = self.world.instance('prg_robotbridge')
        for name in ('xcommissioned', 'xgdiprogrammappingconfirmed', 'xremotepermit'): self.gvl.set(name, True)
        self.gvl.values['arobotrx'][0] = 0x90  # stopped + external auto, disabled
        self.robot.call({}, 10); self.robot.set('uihbedges', 2)
        self.boot = self.world.globals['gvl_robotbridgeretain'].get('udibootcounter')
        self.seq = 0; self.command(1)

    def command(self, command, parameter=0):
        self.seq += 1
        self.gvl.values['ahostcmd'][:] = [2, command, 0, self.seq, parameter, 0, 0, 123,
                                           self.boot >> 16, self.boot & 65535, 0, 0, 0, 0, 0, self.seq]
        for _ in range(4): self.robot.call({}, 10)

    def test_incomplete_fixed_job_refuses_load(self):
        self.command(4, 2)
        self.assertEqual(self.robot.get('wreject'), 9)
        self.assertEqual(self.robot.get('uistep'), 0)
        self.assertFalse(self.gvl.get('xloadoperatorconfirmed'))

    def test_update_blocks_enable_load_run_but_stop_still_available(self):
        for command, parameter in ((2, 0), (4, 2), (5, 0)):
            with self.subTest(command=command):
                self.box.gvl.set('xfixedupdatebusy', True)
                self.command(command, parameter)
                self.assertEqual(self.robot.get('wreject'), 9)
        self.command(7)
        self.assertEqual(self.robot.get('wreject'), 0)
        self.assertEqual(self.robot.get('wpulse'), 0x800)

    def test_begin_update_clears_old_manual_load_confirmation(self):
        self.gvl.set('xloadoperatorconfirmed', True)
        self.box.gvl.set('xfixedupdatebusy', True)
        self.robot.call({}, 10)
        self.assertFalse(self.gvl.get('xloadoperatorconfirmed'))

    def test_ready_job_can_be_loaded_but_is_not_automatically_started(self):
        self.box.gvl.set('xfixedready', True)
        self.command(4, 2)
        self.assertEqual(self.robot.get('wreject'), 0)
        self.assertEqual(self.robot.get('wpulse'), 0x20)
        self.assertFalse(self.gvl.get('xloadoperatorconfirmed'))
        self.assertFalse(self.gvl.get('xrobotenabled'))


if __name__ == '__main__': unittest.main()
