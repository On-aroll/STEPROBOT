"""Execute the candidate ST with simulated registers and sockets; no device access."""
from pathlib import Path
import sys, struct, unittest, zlib

ROOT = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(ROOT/'docs/plc_fixed_job_v1_2/tests'))
import st_runtime as rt
import test_bridge as legacy
import fast_st_runtime
fast_st_runtime.install(legacy)

rt.ROOT = ROOT/'docs/auto_job_r6_4/plc'
PROJECT = 'pcjob_manual_r63.sr'
TAG = (123, 456, 789)


class Box(legacy.Mailbox):
    def __init__(self):
        self.tag = TAG
        super().__init__()
        self.gvl.set('xfixedmappingconfirmed', True)
        robot = self.world.globals['gvl_robotbridge']
        robot.set('uifixedjobguardversion', 140)
        robot.set('xrobotlinkok', True)
        robot.set('xrobotstopped', True)
        robot.values['ahoststatus'][13]=11
        self.scan()

    def send(self, words):
        if words[2] == 1 and words[18] == 2:
            words[19], words[116], words[117] = self.tag
            legacy.put32(words, 118, zlib.crc32(struct.pack('>118H', *words[:118])))
        return super().send(words)

    def upload(self, count=1, names=None):
        if names is None:
            names = ['auto_main.srd', 'auto_main.srp']
            for n in range(1, count+1):
                names += [f'auto_{n:02d}.srd', f'auto_{n:02d}.srp']
        for ordinal, name in enumerate(names, 1):
            data = b'' if name.endswith('.srd') else name.encode()
            self.start(data, name, len(names), ordinal, mode=2, project=PROJECT)
            if data:
                self.command(2, payload=data)
            self.command(3)
            self.scan(4)
            if self.prg.get('uistate') != 200:
                raise AssertionError((name, self.prg.get('uistate'), self.prg.get('wreject')))


class AutoMailboxTests(unittest.TestCase):
    def test_empty_file_deploy_advertises_new_capability_revision(self):
        box=legacy.Mailbox();box.start(b'')
        self.assertEqual(box.prg.get('uistate'),100)
        self.assertEqual(box.prg.get('udirunningcrc') ^ 0xFFFFFFFF,0)
        box.command(3);self.assertEqual(box.prg.get('fbftp').starts,1)
        box.scan(4);self.assertEqual(box.prg.get('uiverified'),1)
        box.command(5);status=box.gvl.get('astatus')
        self.assertEqual(status[2],400);self.assertEqual(status[47],13);self.assertEqual(status[48],0x0104)
    def test_one_many_and_maximum_jobs_finish_only_after_all_readbacks(self):
        for count in (1, 4, 15):
            with self.subTest(count=count):
                box = Box();box.upload(count)
                self.assertFalse(box.gvl.get('xfixedready'))
                box.command(5)
                self.assertEqual(box.prg.get('uistate'), 400)
                self.assertTrue(box.gvl.get('xfixedready'))
                self.assertEqual(box.gvl.get('aautotag'), list(TAG))
                self.assertEqual(box.world.globals['gvl_paramprobe'].get('aparamtx')[:3], list(TAG))

    def test_protected_files_wrong_project_odd_count_and_invalid_tags_refuse(self):
        for name, project, count, tag in (
            ('_project.srd', PROJECT, 4, TAG), ('manual_service.srp', PROJECT, 4, TAG),
            ('auto_main.srp', 'pcjob_wrong.sr', 4, TAG), ('auto_main.srp', PROJECT, 5, TAG),
            ('auto_main.srp', PROJECT, 4, (0,2,3)), ('auto_main.srp', PROJECT, 4, (32768,2,3)),
        ):
            with self.subTest(name=name, tag=tag):
                box=Box();box.tag=tag;box.start(name=name,count=count,mode=2,project=project)
                self.assertEqual(box.prg.get('wreject'),0xE015)
                self.assertEqual(box.prg.get('fbftp').starts,0)

    def test_missing_main_or_noncontiguous_child_refuses_finish(self):
        for names in (['auto_main.srd','auto_main.srp','auto_02.srd','auto_02.srp'],
                      ['auto_01.srd','auto_01.srp','auto_02.srd','auto_02.srp']):
            box=Box();box.upload(names=names);box.command(5)
            self.assertFalse(box.gvl.get('xfixedready'))
            self.assertEqual(box.prg.get('wreject'),0xE012)

    def test_tag_must_be_identical_for_every_file(self):
        box=Box();box.upload();box.command(5)
        box.tag=(2,3,4);box.seq=0
        box.start(b'', 'auto_main.srd',4,1,mode=2,project=PROJECT)
        box.command(3);box.scan(4);box.tag=(3,4,5)
        box.start(b'x','auto_main.srp',4,2,mode=2,project=PROJECT)
        self.assertEqual(box.prg.get('wreject'),0xE015)
        self.assertFalse(box.gvl.get('xfixedready'))

    def test_stop_enable_link_manual_ownership_and_guard_must_allow_update(self):
        cases=[('gvl_robotbridge','xrobotenabled',True),('gvl_robotbridge','xrobotrunning',True),
               ('gvl_robotbridge','xrobotpaused',True),('gvl_robotbridge','xrobotlinkok',False),
               ('gvl_robotbridge','uifixedjobguardversion',0),('gvl_manualbridge','xactive',True)]
        for namespace,name,value in cases:
            box=Box();box.world.globals[namespace].set(name,value)
            box.start(name='auto_main.srp',count=4,mode=2,project=PROJECT)
            self.assertEqual(box.prg.get('wreject'),0xE015)

    def test_partial_failure_reset_and_configuration_change_keep_job_unavailable(self):
        box=Box();box.start(name='auto_main.srp',count=4,mode=2,project=PROJECT)
        box.command(2,payload=b'bad');box.command(3)
        self.assertFalse(box.gvl.get('xfixedready'))
        box.gvl.set('xreset',True);box.scan()
        self.assertFalse(box.gvl.get('xfixedready'))
        box=Box();box.upload();box.command(5);box.gvl.set('uifixedpath',12);box.scan()
        self.assertFalse(box.gvl.get('xfixedready'))

    def test_manual_parameter_owner_is_preserved(self):
        box=Box();box.upload();box.command(5)
        manual=box.world.globals['gvl_manualbridge'];params=box.world.globals['gvl_paramprobe']
        manual.set('xactive',True);params.values['aparamtx'][:]=[7,8,9,10];box.scan()
        self.assertEqual(params.get('aparamtx'),[7,8,9,10])

    def test_ordinary_mode_cannot_write_reserved_manual_project(self):
        box=Box();box.start(name='_project.srd',count=3,mode=1,project=PROJECT)
        self.assertEqual(box.prg.get('wreject'),0xE015)


class AutoFtpTests(unittest.TestCase):
    def ftp(self, server, name='auto_main.srp'):
        world=rt.World(native_factory=server.native);env=world.globals['gvl_filebridge']
        env.set('xfixedmappingconfirmed',True);env.set('xfixedidle',True)
        world.globals['gvl_robotbridge'].set('uifixedjobguardversion',140)
        env.values['abuffer'][:3]=b'abc';fb=world.instance('fb_fileftp')
        args=dict(xstart=True,xabort=False,pbuffer=rt.Pointer(env,'abuffer'),udisize=3,
                  sproject=PROJECT,sfile=name,xcreateproject=True,xreplacefixed=True,suser='test',spassword='local_test')
        for _ in range(4000):
            fb.call(args)
            if fb.get('xdone'):return fb
        self.fail('FTP mock timed out')

    def test_existing_project_updates_auto_file_and_reads_back_without_mkdir(self):
        server=legacy.Cabinet(existing=True);fb=self.ftp(server)
        self.assertTrue(fb.get('xtestpassed'))
        self.assertIn('STOR auto_main.srp',server.commands)
        self.assertIn('RETR auto_main.srp',server.commands)
        self.assertFalse(any(s.startswith('MKD') for s in server.commands))

    def test_direct_ftp_cannot_overwrite_origin_or_manual_service(self):
        for name in ('_project.srd','manual_service.srp','capture_home.srp'):
            server=legacy.Cabinet(existing=True);fb=self.ftp(server,name)
            self.assertEqual(fb.get('werrorid'),0xF20C)
            self.assertFalse(server.commands)

    def test_missing_bound_project_is_not_created(self):
        server=legacy.Cabinet();fb=self.ftp(server)
        self.assertEqual(fb.get('werrorid'),0xF20B)
        self.assertFalse(any(s.startswith(('MKD','STOR')) for s in server.commands))


class AutoRobotTests(unittest.TestCase):
    def setUp(self):
        self.box=Box();self.world=self.box.world;self.g=self.world.globals['gvl_robotbridge']
        self.robot=self.world.instance('prg_robotbridge')
        for name in ('xcommissioned','xgdiprogrammappingconfirmed','xremotepermit'):self.g.set(name,True)
        self.g.values['arobotrx'][0]=0x90
        self.robot.call({},10);self.robot.set('uihbedges',2)
        self.boot=self.world.globals['gvl_robotbridgeretain'].get('udibootcounter');self.seq=0;self.command(1)

    def command(self,command,parameter=0):
        self.seq+=1
        self.g.values['ahostcmd'][:]=[2,command,0,self.seq,parameter,0,0,123,self.boot>>16,self.boot&65535,0,0,0,0,0,self.seq]
        for _ in range(4):self.robot.call({},10)

    def test_incomplete_auto_cannot_load_but_manual_ten_can(self):
        self.command(4,11);self.assertEqual(self.robot.get('wreject'),9)
        self.command(4,10);self.assertEqual(self.robot.get('wreject'),0)

    def test_upload_clears_confirmation_and_blocks_motion_commands(self):
        self.g.set('xloadoperatorconfirmed',True);self.box.gvl.set('xfixedupdatebusy',True)
        for cmd,param in ((2,0),(4,11),(5,0),(12,11)):
            self.command(cmd,param);self.assertEqual(self.robot.get('wreject'),9)
        self.assertFalse(self.g.get('xloadoperatorconfirmed'))
        self.command(7);self.assertEqual(self.robot.get('wreject'),0)

    def test_complete_auto_accepts_load_without_automatically_starting(self):
        self.box.gvl.set('xfixedready',True);self.command(4,11)
        self.assertEqual(self.robot.get('wreject'),0);self.assertEqual(self.robot.get('wpulse'),0x20)
        self.assertFalse(self.g.get('xloadoperatorconfirmed'));self.assertFalse(self.g.get('xrobotenabled'))


if __name__=='__main__':
    # Replace the old capability/revision assertion with its R6.4 counterpart above.
    old=unittest.defaultTestLoader.loadTestsFromModule(legacy)
    replaced=('test_zero_length_file_crc_and_mailbox_deploy','test_ftp_data_indices_cross_65535_in_delivered_source')
    legacy_tests=[test for group in old for test in group if not test.id().endswith(replaced)]
    suite=unittest.TestSuite(legacy_tests+[unittest.defaultTestLoader.loadTestsFromModule(sys.modules[__name__])])
    result=unittest.TextTestRunner(verbosity=2).run(suite);sys.exit(not result.wasSuccessful())
