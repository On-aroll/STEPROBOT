"""Execute the delivered ST with virtual timers/IO; not an InoProShop compiler."""
from pathlib import Path
import importlib.util, sys, unittest

ROOT=Path(__file__).parents[3]
spec=importlib.util.spec_from_file_location('manual_st_runtime',ROOT/'docs/plc_fixed_job_v1_2/tests/st_runtime.py')
rt=importlib.util.module_from_spec(spec);spec.loader.exec_module(rt)
rt.ROOT=ROOT/'docs/auto_job_r6_4/validation/st_fixture'
rt.TOKEN=rt.re.compile(rt.TOKEN.pattern.replace('UDINT|DINT','UDINT|DINT|INT').replace('=<>]', '=/<>]'),rt.re.I)
rt.Parser.precedence['/']=5
old_eval=rt.Instance.evaluate
def evaluate(self,node):
    # The delivered slash expression divides UINT operands, so its result is integral.
    if node[0]=='binary' and node[1]=='/':
        return self.evaluate(node[2]) // self.evaluate(node[3])
    if node[0]=='function' and node[1].lower() in ('int_to_dint','word_to_dint'):
        return self.evaluate(node[2][0])
    if node[0]=='function' and node[1].lower()=='word_to_int':
        value=self.evaluate(node[2][0]);return value-65536 if value>=32768 else value
    return old_eval(self,node)
rt.Instance.evaluate=evaluate

class Box:
    def __init__(self):
        self.world=rt.World();self.g=self.world.globals['gvl_manualbridge'];self.b=self.world.globals['gvl_robotbridge']
        self.p=self.world.instance('prg_manualbridge');self.time=0;self.seq=0;self.owner=0x12345678;self.started=False;self.origin=0
        self.g.set('xinstalled',True);self.g.set('xservicebindingconfirmed',True)
        for name in ['xhostregistered','xhostalive','xcommissioned','xgdiprogrammappingconfirmed','xremotepermit',
                     'xrobotlinkok','xrobotexternalauto','xrobotenabled','xloadoperatorconfirmed','xrobotstopped']:
            self.b.set(name,True)
        self.world.globals['gvl_robotbridgeretain'].set('udibootcounter',1)
        self.b.get('ahoststatus')[13]=10;self.b.get('ahoststatus')[15:17]=[0x1234,0x5678]
        self.scan()
    def frame(self,op,action=0,arg=0,speed=100,seq=None):
        if seq is None:self.seq+=1;seq=self.seq
        w=[0]*16;w[:2]=[0x4d35,op];w[2:4]=[seq>>16,seq&65535];w[4:8]=[2,action,arg&65535,speed]
        w[8:12]=[self.owner>>16,self.owner&65535,0,1];w[12]=10
        w[13]=0xa55a
        for x in w[:13]:w[13]^=x
        w[14]=int(2<=op<=4 or op>=7);w[15]=(self.g.get('acommand')[15]+1)&65535
        self.g.set('acommand',w)
    def scan(self,ms=10,hb=True,service=True,complete=True):
        for _ in range(ms//10):
            self.time+=10
            if hb and self.time%100==0:self.g.get('acommand')[15]=(self.g.get('acommand')[15]+1)&65535
            if service:
                if self.g.get('xstartrequest'):self.started=True
                if self.g.get('xstoprequest'):self.started=False
                self.b.set('xrobotrunning',self.started)
                self.b.set('xrobotstopped',not self.started)
                if self.started:
                    rx=self.b.get('arobotrx');rx[4]=1001+(self.time//20%62)*16+(4 if self.origin else 0);rx[7]=self.origin
                    if self.p.get('uiphase') in (40,60,70):
                        rx[5]=self.p.get('wtoken')
                        if complete and (self.p.get('wackop')!=9 or self.p.get('uiphase')==70):rx[6:8]=[self.p.get('wtoken'),self.origin]
            self.p.call({},10)
    def enter(self):
        self.frame(1);self.scan(1000)
        assert self.p.get('xentered') and self.p.get('wresult')==2
    def tx(self):return self.world.globals['gvl_paramprobe'].get('aparamtx')[:]
    def position_packet(self,frame,index,value,ms=50,tail=None):
        whole=value//1000;fraction=value-whole*1000;header=28000+frame*32+index
        self.b.get('arobotrx')[4:8]=[header,whole&65535,fraction,header if tail is None else tail]
        self.scan(ms,service=False)
    def snapshot(self,frame=1,values=None):
        values=values or [-1,123456,-999,0,45000,-179999,2000125,-999,2105400,-180000,45000,5]
        for i,value in enumerate(values,1):self.position_packet(frame,i,value)
        return values
    def stage(self,kind=1,ident=41):
        self.frame(7,23,kind,ident);self.scan(650)
        assert self.p.get('wresult')==2
        for n in range(6):
            self.frame(7,24+n,0,2000);self.scan(650)
            assert self.p.get('wresult')==2

class Tests(unittest.TestCase):
    def test_retained_position_contents_are_invalid_on_first_scan(self):
        world=rt.World()
        block=world.globals['gvl_manualposition']
        block.set('astatus',[65535]*40)
        world.globals['gvl_robotbridgeretain'].set('udibootcounter',42)
        program=world.instance('prg_manualbridge')
        program.call({},10)
        out=block.get('astatus')
        self.assertEqual(out[:2],[0x5053,1])
        self.assertFalse(out[2]&1)
        self.assertEqual(out[3:8],[0,0,42,0,0])
        self.assertEqual(out[10:34],[0]*24)
        self.assertFalse(world.globals['gvl_manualbridge'].get('xstartrequest'))
        self.assertEqual(world.globals['gvl_paramprobe'].get('aparamtx'),[0]*4)

    def test_complete_position_snapshot_is_coherent_signed_and_scaled(self):
        b=Box();b.enter();values=b.snapshot();out=b.world.globals['gvl_manualposition'].get('astatus')
        self.assertEqual(out[:2],[0x5053,1]);self.assertTrue(out[2]&1);self.assertEqual(out[6:8],[0,1])
        result=[]
        for i in range(12):
            bits=(out[10+2*i]<<16)|out[11+2*i];result.append(bits-2**32 if bits>=2**31 else bits)
        self.assertEqual(result,values);self.assertEqual(out[36:39],[0x0603,12,1000])
    def test_partial_or_different_snapshot_cannot_publish(self):
        b=Box();b.enter();b.position_packet(1,1,123)
        b.position_packet(2,2,999);b.position_packet(2,12,123)
        self.assertEqual(b.p.get('udipositioncounter'),0);self.assertFalse(b.p.get('xpositionvalid'))
    def test_mismatched_header_payload_and_invalid_fraction_are_ignored(self):
        b=Box();b.enter();b.position_packet(1,1,123,tail=28034)
        self.assertEqual(b.p.get('uipositioncount'),0)
        b.b.get('arobotrx')[4:8]=[28033,0,1000,28033];b.scan(80,service=False)
        self.assertEqual(b.p.get('uipositioncount'),0)
    def test_guard_zero_keeps_service_edges_but_not_position_data(self):
        b=Box();b.enter();b.snapshot();b.b.get('arobotrx')[4:8]=[0,0,0,0]
        b.scan(100,service=False);self.assertTrue(b.p.get('xentered'))
        self.assertTrue(b.p.get('xservicealive'))
    def test_motion_invalidates_position_reference_and_payload_cannot_ack_motion(self):
        b=Box();b.enter();b.snapshot();b.frame(2,1,100,100);b.scan(450,service=False,complete=False)
        self.assertFalse(b.p.get('xpositionvalid'));token=b.p.get('wtoken')
        b.b.get('arobotrx')[4:8]=[28034,token,token,28034];b.scan(80,service=False,complete=False)
        self.assertEqual(b.p.get('uiphase'),40);self.assertEqual(b.p.get('wresult'),1)
    def test_position_age_disconnect_and_new_capture_flags(self):
        b=Box();b.enter();b.snapshot()
        b.scan(8100);self.assertFalse(b.p.get('xpositionvalid'))
        b.snapshot(2);self.assertTrue(b.p.get('xpositionvalid'))
        b.b.set('xrobotlinkok',False);b.scan(20,service=False)
        self.assertFalse(b.world.globals['gvl_manualposition'].get('astatus')[2]&1)
    def test_r6_capabilities_and_v1_frame_rejection(self):
        b=Box();self.assertEqual(b.g.get('astatus')[28:30],[31,0x0603])
        b.frame(1);w=b.g.get('acommand');w[4]=1;w[13]^=3;b.scan(100)
        self.assertEqual(b.p.get('werror'),1);self.assertFalse(b.g.get('xactive'))
    def test_new_ten_unit_single_step_limit(self):
        b=Box();b.enter();b.frame(2,11,-10000,500);b.scan(650)
        self.assertEqual(b.p.get('wresult'),2);self.assertEqual(b.tx()[1],55536)
    def test_jog_is_one_held_request_and_completes_only_on_release(self):
        b=Box();b.enter();b.frame(9,1,100,500);b.scan(6000)
        self.assertEqual(b.p.get('uiphase'),60);self.assertEqual(b.p.get('wresult'),1)
        self.assertEqual(b.tx()[2],33268);token=b.tx()[0]
        b.frame(6);b.scan(40,service=False)
        self.assertEqual(b.tx()[0],0);self.assertGreater(b.tx()[3],0)
        b.scan(650)
        self.assertEqual(b.p.get('wresult'),2);self.assertTrue(b.p.get('xentered'))
        self.assertTrue(b.g.get('xactive'));self.assertTrue(b.b.get('xloadoperatorconfirmed'))
        self.assertNotEqual(token,0)
        b.frame(9,1,-5,100);b.scan(650);self.assertEqual(b.p.get('uiphase'),60)
    def test_jog_large_segments_rejected(self):
        b=Box();b.enter();b.frame(9,11,501,100);b.scan(100)
        self.assertEqual(b.p.get('werror'),7)
    def test_full_speed_joints_and_release_preserve_service(self):
        for joint in range(1,7):
            b=Box();b.enter();b.frame(9,joint,1000,10000);b.scan(650)
            self.assertEqual(b.p.get('uiphase'),60)
            self.assertEqual(b.tx()[1:3],[1000,42768])
            b.frame(6);b.scan(650)
            self.assertEqual(b.p.get('wresult'),2)
            self.assertTrue(b.p.get('xentered'))
            self.assertTrue(b.g.get('xactive'))
            self.assertTrue(b.b.get('xloadoperatorconfirmed'))
    def test_speed_dependent_joint_segment_limits(self):
        for speed,limit in [(500,100),(501,250),(1000,250),(1001,500),(1500,500),(1501,1000),(10000,1000)]:
            b=Box();b.enter();b.frame(9,6,-limit,speed);b.scan(650)
            self.assertEqual(b.p.get('uiphase'),60)
            self.assertEqual(b.tx()[2],32768|speed)
            b=Box();b.enter();b.frame(9,6,limit+1,speed);b.scan(100)
            self.assertEqual(b.p.get('werror'),7)
    def test_speed_overflow_is_rejected_for_each_coordinate_type(self):
        for op,action,arg,speed in [(2,1,100,10001),(9,6,1000,10001),(2,11,100,32768),(9,14,100,32768)]:
            b=Box();b.enter();b.frame(op,action,arg,speed);b.scan(100)
            self.assertEqual(b.p.get('werror'),7)
        for kind,speed in [(1,10001),(2,32768)]:
            b=Box();b.enter();b.stage(kind);b.frame(8,30,41,speed);b.scan(100)
            self.assertEqual(b.p.get('werror'),7)
    def test_world_single_step_and_held_speed_use_full_range_without_marker_collision(self):
        b=Box();b.enter();b.frame(2,11,100,32767);b.scan(650)
        self.assertEqual(b.p.get('wresult'),2);self.assertEqual(b.tx()[2],32767)
        b.frame(9,16,-10000,32767);b.scan(650)
        self.assertEqual(b.p.get('uiphase'),60);self.assertEqual(b.tx()[1:3],[55536,65535])
        b.frame(6);b.scan(650)
        self.assertEqual(b.p.get('wresult'),2);self.assertTrue(b.p.get('xentered'))
    def test_world_segment_profile_limits_and_units(self):
        for speed,limit in [(1,100),(50,100),(51,500),(250,500),(251,1000),(1000,1000),(1001,5000),(5000,5000),(5001,10000),(32767,10000)]:
            b=Box();b.enter();b.frame(9,11,-limit,speed);b.scan(650)
            self.assertEqual(b.p.get('uiphase'),60)
            self.assertEqual(b.tx()[1:3],[(-limit)&65535,32768|speed])
            b=Box();b.enter();b.frame(9,11,limit+1,speed);b.scan(100)
            self.assertEqual(b.p.get('werror'),7)
    def test_absolute_motion_accepts_full_speed_for_the_staged_kind(self):
        for kind,speed in [(1,10000),(2,32767)]:
            b=Box();b.enter();b.stage(kind);b.frame(8,30,41,speed);b.scan(650)
            self.assertEqual(b.p.get('wresult'),2)
            self.assertEqual(b.tx()[2],speed)
            self.assertEqual(b.p.get('uistagekind'),0)
    def test_return_accepts_full_joint_speed_and_retains_long_move_timeout(self):
        b=Box();b.enter();b.origin=17;b.scan(100)
        b.frame(4,21,17,10000);b.scan(650,complete=False)
        self.assertEqual(b.p.get('uiphase'),40);self.assertEqual(b.tx()[1:3],[17,10000])
        b.scan(25000,complete=False)
        self.assertEqual(b.p.get('uiphase'),40);self.assertFalse(b.g.get('xstoprequest'))
        b.scan(650)
        self.assertEqual(b.p.get('wresult'),2);self.assertTrue(b.p.get('xentered'))
    def test_slow_jog_segment_does_not_use_idle_heartbeat_deadline(self):
        b=Box();b.enter();b.frame(9,1,100,10);b.scan(650)
        b.scan(2500,service=False)
        self.assertEqual(b.p.get('uiphase'),60);self.assertFalse(b.g.get('xstoprequest'))
        b.scan(18000,service=False)
        self.assertTrue(b.g.get('xstoprequest'));self.assertEqual(b.tx(),[0,0,0,0])
    def test_jog_release_before_token_commit_keeps_service(self):
        b=Box();b.enter();b.frame(9,2,100,100);b.scan(80)
        b.frame(6);b.scan(700)
        self.assertTrue(b.p.get('xentered'));self.assertEqual(b.p.get('wresult'),2)
    def test_jog_release_timeout_escalates_to_ordinary_stop(self):
        b=Box();b.enter();b.frame(9,2,100,100);b.scan(650)
        b.frame(6);b.scan(3200,service=False)
        self.assertTrue(b.g.get('xstoprequest'));self.assertEqual(b.tx(),[0,0,0,0])
        self.assertFalse(b.p.get('xentered'))
    def test_held_jog_lease_loss_stops_with_no_auto_restart(self):
        b=Box();b.enter();b.frame(9,11,100,100);b.scan(650)
        b.scan(700,hb=False,service=False)
        self.assertTrue(b.g.get('xstoprequest'));self.assertEqual(b.tx(),[0,0,0,0])
        self.assertEqual(b.p.get('werror'),8)
    def test_stage_is_nonmotion_and_move_requires_six_fields(self):
        b=Box();b.enter();b.frame(7,23,1,41);b.scan(650)
        self.assertEqual(b.p.get('uistagekind'),1)
        self.assertEqual(b.tx()[0]%32,23)
        b.frame(8,30,41,100);b.scan(100)
        self.assertEqual(b.p.get('werror'),14);self.assertEqual(b.tx()[0]%32,23)
    def test_complete_staged_move_consumes_target_once(self):
        b=Box();b.enter();b.stage();self.assertEqual(b.p.get('uistagecount'),6)
        b.frame(8,30,41,100);b.scan(650)
        self.assertEqual(b.p.get('wresult'),2);self.assertEqual(b.p.get('uistagecount'),0)
        token=b.tx()[0];b.frame(8,30,41,100);b.scan(100)
        self.assertEqual(b.p.get('werror'),14);self.assertEqual(b.tx()[0],token)
    def test_staged_negative_fraction_and_large_xyz(self):
        b=Box();b.enter();b.frame(7,23,2,99);b.scan(650)
        for action,whole,fraction in [(24,-1,999),(25,-2584,125),(26,9999,999),(27,-180,500),(28,0,2000),(29,360,0)]:
            b.frame(7,action,whole,fraction);b.scan(650)
            self.assertEqual(b.p.get('wresult'),2)
            self.assertEqual(b.tx()[1:3],[whole&65535,fraction])
    def test_stage_out_of_order_invalidates_prior_target(self):
        b=Box();b.enter();b.stage();b.frame(7,25,0,0);b.scan(100)
        self.assertEqual(b.p.get('werror'),14);self.assertEqual(b.p.get('uistagecount'),0)
    def test_target_wrong_id_rejected(self):
        b=Box();b.enter();b.stage();b.frame(8,30,42,100);b.scan(100)
        self.assertEqual(b.p.get('werror'),14)
    def test_stage_bounds_and_keep_current_sentinel(self):
        for whole,frac in [(360,1),(-361,999),(0,1000),(1,2000)]:
            b=Box();b.enter();b.frame(7,23,1,41);b.scan(650)
            b.frame(7,24,whole,frac);b.scan(100)
            self.assertEqual(b.p.get('werror'),7)
    def test_cancel_invalidates_staged_target(self):
        b=Box();b.enter();b.stage();b.frame(6);b.scan(100)
        self.assertTrue(b.p.get('xentered'));self.assertEqual(b.p.get('uistagecount'),0)
        b.frame(8,30,41,100);b.scan(100);self.assertEqual(b.p.get('werror'),14)
    def test_motion_completion_waits_for_new_service_edges(self):
        b=Box();b.enter();b.stage();b.frame(8,30,41,100);b.scan(400,complete=False)
        b.scan(4000,service=False)
        self.assertEqual(b.p.get('uiphase'),40)
        b.b.get('arobotrx')[5:8]=[b.p.get('wtoken'),b.p.get('wtoken'),0]
        b.scan(30,service=False);self.assertEqual(b.p.get('uiphase'),50)
        self.assertEqual(b.p.get('wresult'),1)
        b.scan(120);self.assertEqual(b.p.get('wresult'),2);self.assertTrue(b.p.get('xentered'))
    def test_parse_both_delivered_programs(self):
        b=Box();b.world.instance('prg_robotbridge')
    def test_stale_startup_request_ignored(self):
        b=Box();b.frame(1);b.p= b.world.instance('prg_manualbridge');b.scan(100)
        self.assertFalse(b.g.get('xactive'));self.assertEqual(b.tx(),[0,0,0,0])
    def test_enter_starts_no_motion_then_syncs(self):
        b=Box();b.enter();self.assertEqual(b.p.get('wtoken')%32,22)
        self.assertEqual(b.p.get('wargument'),0);self.assertEqual(b.p.get('wspeed'),0)
    def test_one_command_and_negative_argument(self):
        b=Box();b.enter();b.frame(2,2,-100,100);b.scan(600)
        self.assertEqual(b.p.get('wresult'),2);self.assertEqual(b.tx()[1],65436)
        token=b.tx()[0];b.scan(1000);self.assertEqual(b.tx()[0],token)
    def test_oversize_delta_rejected(self):
        b=Box();b.enter();b.frame(2,1,10001,100);b.scan(100)
        self.assertEqual(b.p.get('wresult'),4);self.assertEqual(b.p.get('werror'),7)
        self.assertEqual(b.tx()[0]%32,22)
    def test_duplicate_seq_cannot_change_arguments(self):
        b=Box();b.enter();b.frame(2,1,100,100);b.scan(500)
        token=b.tx()[0];b.frame(2,6,-1000,500,seq=b.seq);b.scan(100)
        self.assertEqual(b.tx()[0],token);self.assertEqual(b.tx()[1],100)
    def test_competing_request_does_not_mutate_active_motion(self):
        b=Box();b.enter();b.frame(2,1,100,100);b.scan(400,complete=False)
        token=b.p.get('wtoken');b.frame(2,6,-1000,500);b.scan(100,complete=False)
        self.assertEqual(b.p.get('wtoken'),token);self.assertEqual(b.tx()[1:3],[100,100])
    def test_malformed_checksum_never_commits(self):
        b=Box();b.enter();b.frame(2,1,100,100);b.g.get('acommand')[6]=1000;b.scan(100)
        self.assertEqual(b.p.get('werror'),1);self.assertEqual(b.tx()[0]%32,22)
    def test_cancel_preempts_with_zero_parameters(self):
        b=Box();b.enter();b.frame(2,1,1000,10);b.scan(400,complete=False)
        b.frame(6);b.scan(30,service=False)
        self.assertTrue(b.g.get('xstoprequest'));self.assertEqual(b.tx(),[0,0,0,0])
        b.b.set('xrobotrunning',False);b.b.set('xrobotstopped',True);b.scan(600,service=False)
        self.assertEqual(b.p.get('wresult'),2);self.assertFalse(b.g.get('xactive'))
        self.assertFalse(b.b.get('xloadoperatorconfirmed'))
    def test_lost_extended_heartbeat_stops_with_main_heartbeat_alive(self):
        b=Box();b.enter();b.frame(2,1,1000,10);b.scan(400,complete=False)
        b.scan(700,hb=False,service=False)
        self.assertTrue(b.g.get('xstoprequest'));self.assertEqual(b.tx(),[0,0,0,0]);self.assertEqual(b.p.get('werror'),8)
    def test_idle_cancel_does_not_restart_or_end_service(self):
        b=Box();b.enter();token=b.tx()[0];b.frame(6);b.scan(100)
        self.assertTrue(b.p.get('xentered'));self.assertEqual(b.tx()[0],token);self.assertFalse(b.g.get('xstartrequest'))
    def test_return_requires_matching_saved_origin(self):
        b=Box();b.enter();b.frame(4,21,25,100);b.scan(100)
        self.assertEqual(b.p.get('werror'),12)
    def test_permit_loss_stops_without_waiting_pc(self):
        b=Box();b.enter();b.b.set('xremotepermit',False);b.scan(20,service=False)
        self.assertTrue(b.g.get('xstoprequest'));self.assertEqual(b.tx(),[0,0,0,0])
    def test_new_registered_owner_may_restart_sequence_from_one(self):
        b=Box();b.enter();b.frame(5);b.scan(700)
        b.owner=0x23456789;b.b.get('ahoststatus')[15:17]=[0x2345,0x6789]
        b.b.set('xrobotrunning',False);b.b.set('xrobotstopped',True);b.frame(1,seq=1);b.scan(100)
        self.assertNotEqual(b.p.get('werror'),3)
    def test_late_stopped_feedback_clears_stopping_without_restart(self):
        b=Box();b.enter();b.frame(5);b.scan(4100,service=False)
        self.assertTrue(b.p.get('xstopping'));self.assertEqual(b.p.get('werror'),13)
        b.b.set('xrobotrunning',False);b.b.set('xrobotstopped',True);b.scan(600,service=False)
        self.assertFalse(b.g.get('xactive'));self.assertFalse(b.p.get('xstopping'))
        self.assertEqual(b.p.get('wresult'),5);self.assertEqual(b.tx(),[0,0,0,0])
    def test_cancel_during_start_holds_stop_before_accepting_old_stopped_feedback(self):
        b=Box();b.frame(1);b.scan(50,service=False);b.frame(6);b.scan(100,service=False)
        self.assertTrue(b.g.get('xstoprequest'));self.assertTrue(b.g.get('xactive'))
        b.scan(500,service=False);self.assertFalse(b.g.get('xactive'))

if __name__=='__main__':unittest.main(verbosity=2)
