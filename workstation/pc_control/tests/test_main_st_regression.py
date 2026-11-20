"""Run original V2 behavioral suite against the actual R5 patched main ST."""
from pathlib import Path
import importlib.util, sys, unittest

ROOT=Path(__file__).parents[3]
folder=ROOT/'docs/plc_modbus_bridge_v2_gdi255/tests'
sys.path.insert(0,str(folder))
import st_scan_harness as rt
rt.ROOT=ROOT/'docs/auto_job_r6_4/validation/st_fixture'
old_init=rt.ScanHarness.__init__
def init(self):
    old_init(self)
    for name in ['xactive','xstartrequest','xstoprequest']:
        self.values['gvl_manualbridge.'+name]=False
        self.kinds['gvl_manualbridge.'+name]='BOOL'
    for name in ['xfixedupdatebusy','xfixedmappingconfirmed','xfixedready']:
        self.values['gvl_filebridge.'+name]=False
        self.kinds['gvl_filebridge.'+name]='BOOL'
    self.values['gvl_filebridge.uifixedpath']=11
    self.kinds['gvl_filebridge.uifixedpath']='UINT'
rt.ScanHarness.__init__=init
old_ready=rt.ScanHarness.ready
def ready(self):
    # Existing GDI sweep tests exercise the ready state. Unready auto11 is
    # covered separately in test_auto_st.py against the actual candidate ST.
    self.values['gvl_filebridge.xfixedmappingconfirmed']=True
    self.values['gvl_filebridge.xfixedready']=True
    old_ready(self)
rt.ScanHarness.ready=ready
spec=importlib.util.spec_from_file_location('main_regression',folder/'test_st_source.py')
suite=importlib.util.module_from_spec(spec);spec.loader.exec_module(suite)

class ManualOwnershipTests(unittest.TestCase):
    def fixture(self):
        h=rt.ScanHarness();h.ready();return h
    def test_active_service_rejects_load_enable_run_but_allows_stop(self):
        h=self.fixture();h.values['gvl_manualbridge.xactive']=True
        for seq,command in enumerate([2,4,5],2):
            h.frame(command,seq,10 if command==4 else 0);h.scans(100)
            self.assertEqual(h.get('wlastreject'),9)
            self.assertEqual(h.get('gvl_robotbridge.wrobottxcontrol')&0x110,0)
        h.frame(7,6);h.scans(1800);self.assertEqual(h.get('wackcmd'),7)
        self.assertEqual(h.get('wackresult'),2)
    def test_manual_requests_use_same_control_array_and_stop_wins(self):
        h=self.fixture();h.values['gvl_robotbridge.xloadoperatorconfirmed']=True
        h.values['gvl_manualbridge.xstartrequest']=True;h.scans(40,robot_base=0x94)
        self.assertEqual(h.get('gvl_robotbridge.arobottx')[0]&0x100,0x100)
        h.values['gvl_manualbridge.xstoprequest']=True;h.scans(40,robot_base=0x94)
        self.assertEqual(h.get('gvl_robotbridge.arobottx')[0]&0x900,0x800)

class AlarmRecoveryTests(unittest.TestCase):
    def fixture(self):
        h=rt.ScanHarness();h.ready()
        h.values['xhascontrolledrobot']=True
        h.values['gvl_robotbridge.xstopinterlock']=True
        return h
    def trace(self,h,milliseconds,base):
        result=[]
        for _ in range(milliseconds//10):
            h.scan(robot_base=base)
            result.append(h.get('gvl_robotbridge.wrobottxcontrol'))
        return result
    def test_latched_alarm_clear_has_one_pulse_keeps_stop_and_never_moves(self):
        h=self.fixture();h.values['gvl_manualbridge.xactive']=True
        h.frame(8,2)
        trace=self.trace(h,1200,0x92)+self.trace(h,1000,0x90)
        levels=[bool(w&0x200) for w in trace]
        self.assertEqual(sum(v and (i==0 or not levels[i-1]) for i,v in enumerate(levels)),1)
        self.assertGreaterEqual(sum(levels),50)
        self.assertTrue(all(w&0x800 for w in trace))
        self.assertTrue(all((w&0x1F0)==0 for w in trace))
        self.assertEqual(h.get('wackresult'),2)
        self.assertTrue(h.get('gvl_robotbridge.xstopinterlock'))
        self.assertFalse(h.get('gvl_robotbridge.xloadoperatorconfirmed'))
    def test_summary_low_acknowledgement_still_emits_clear(self):
        h=self.fixture();h.frame(8,2)
        trace=self.trace(h,2000,0x90)
        self.assertTrue(any(w&0x200 for w in trace))
        self.assertEqual(h.get('wackresult'),2)
        self.assertTrue(h.get('gvl_robotbridge.xstopinterlock'))
    def test_persistent_alarm_times_out_once_and_blocks_rearm(self):
        h=self.fixture();h.frame(8,2)
        trace=self.trace(h,11500,0x92)
        levels=[bool(w&0x200) for w in trace]
        self.assertEqual(sum(v and (i==0 or not levels[i-1]) for i,v in enumerate(levels)),1)
        self.assertEqual((h.get('wackresult'),h.get('wackerror')),(5,10))
        h.frame(11,3);h.scans(80,robot_base=0x92)
        self.assertEqual(h.get('wackresult'),4)
        self.assertTrue(h.get('gvl_robotbridge.xstopinterlock'))
        h.frame(11,4);h.scans(80,robot_base=0x90)
        self.assertEqual(h.get('wackresult'),2)
        self.assertFalse(h.get('gvl_robotbridge.xstopinterlock'))
        self.assertEqual(h.get('gvl_robotbridge.wrobottxcontrol')&0x1F0,0)
    def test_clearing_never_allowed_during_running_paused_or_emergency(self):
        for base in [0x83,0x8A,0x192]:
            with self.subTest(base=base):
                h=self.fixture();h.frame(8,2)
                trace=self.trace(h,2000,base)
                self.assertEqual(h.get('wackresult'),4)
                self.assertTrue(all((w&0x3F0)==0 for w in trace))
    def test_later_alarm_after_clear_latches_stop_again(self):
        h=self.fixture();h.frame(8,2);h.scans(2000)
        self.assertEqual(h.get('wackresult'),2)
        h.values['gvl_robotbridge.xstopinterlock']=False
        h.scans(60,robot_base=0x92)
        self.assertTrue(h.get('gvl_robotbridge.xstopinterlock'))

if __name__=='__main__':
    tests=unittest.TestSuite([unittest.defaultTestLoader.loadTestsFromModule(suite),unittest.defaultTestLoader.loadTestsFromTestCase(ManualOwnershipTests),unittest.defaultTestLoader.loadTestsFromTestCase(AlarmRecoveryTests)])
    result=unittest.TextTestRunner(verbosity=2).run(tests);sys.exit(not result.wasSuccessful())
