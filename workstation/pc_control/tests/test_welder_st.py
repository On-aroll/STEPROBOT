"""Execute delivered welder ST with virtual PDOs/timers. This is not an IEC compiler."""
from pathlib import Path
import importlib.util, tempfile, shutil, unittest, re

ROOT=Path(__file__).resolve().parents[3]
spec=importlib.util.spec_from_file_location('welder_st_runtime',ROOT/'docs/plc_fixed_job_v1_2/tests/st_runtime.py')
rt=importlib.util.module_from_spec(spec);spec.loader.exec_module(rt)
fixture=tempfile.TemporaryDirectory()
for source in (ROOT/'docs/welder_r6_4/plc').glob('*.st'):shutil.copy2(source,fixture.name)
for name in ('GVL_RobotBridge.st','GVL_RobotBridgeRetain.st'):shutil.copy2(ROOT/'docs/auto_job_r6_4/plc'/name,fixture.name)
rt.ROOT=Path(fixture.name)
rt.TOKEN=re.compile(rt.TOKEN.pattern.replace('|\\d+|',r'|16#[0-9a-f]+|\d+\.\d+|\d+|').replace('=<>]', '=/<>]'),re.I)
rt.Parser.precedence['/']=5
old_literal=rt.literal
def literal(s):
    if s.lower().startswith('16#'):return int(s[3:],16)
    if re.fullmatch(r'-?\d+\.\d+',s):return float(s)
    return old_literal(s)
rt.literal=literal
old_eval=rt.Instance.evaluate
def evaluate(self,node):
    if node[0]=='binary' and node[1]=='/':
        a,b=self.evaluate(node[2]),self.evaluate(node[3]);return a/b if isinstance(a,float) or isinstance(b,float) else a//b
    if node[0]=='function':
        name=node[1].lower();args=[self.evaluate(a) for a in node[2]]
        if name.endswith('_to_real'):return float(args[0])
        if name in ('uint_to_int','byte_to_uint'):return int(args[0])
        if name=='uint_to_usint':return int(args[0])&255
        if name=='limit':return min(max(args[1],args[0]),args[2])
        if name in ('real_to_uint','real_to_word'):return int(round(args[0]))&65535
    return old_eval(self,node)
rt.Instance.evaluate=evaluate

class Box:
    def __init__(self):
        self.world=rt.World();self.w=self.world.globals['gvl_welders'];self.g=self.world.globals['gvl_welderbridge'];self.b=self.world.globals['gvl_robotbridge'];self.r=self.world.globals['gvl_robotbridgeretain']
        self.p=self.world.instance('prg_welderbridge');self.io=self.world.instance('prg_welderio');self.p.values['prg_welderio']=self.io
        self.seq=0;self.owner=0x12345678;self.boot=123
        self.b.get('ahoststatus')[15:17]=[self.owner>>16,self.owner&65535];self.r.set('udibootcounter',self.boot)
        for field in ('xhostregistered','xhostalive','xrobotlinkok','xremotepermit','xrobotexternalauto'):self.b.set(field,True)
        for field in ('xmappingconfirmed','xweldpermit','xpdooperational'):self.g.set(field,True)
        self.w.set('g_byweldinmap01',2)
        self.scan(2)
    def scan(self,count=1,hb=True):
        for _ in range(count):
            if hb:self.g.set('wheartbeat',(self.g.get('wheartbeat')+1)&65535)
            self.p.call({},10)
    def command(self,op,controls=0,params=None,owner=None,boot=None,seq=None,bad=False):
        if seq is None:self.seq+=1;seq=self.seq
        owner=self.owner if owner is None else owner;boot=self.boot if boot is None else boot
        a=[0]*32;a[:2]=[0x5742,1];a[2:8]=[seq>>16,seq&65535,owner>>16,owner&65535,boot>>16,boot&65535];a[8:10]=[op,controls]
        if params is not None:a[10:30]=params
        a[31]=0xa55a
        for n in a[:31]:a[31]^=n
        if bad:a[31]^=1
        self.g.set('acommand',a);self.scan()
    def status(self):return self.g.get('astatus')
    def active(self):return self.status()[10]
    def arc(self):self.command(1,3);assert self.active()==3

class Tests(unittest.TestCase):
    def test_default_mapping_has_no_legacy_address_collision(self):
        base=ROOT/'docs/welder_r6_4/plc'
        self.assertNotRegex((base/'GVL_Welders.st').read_text('utf-8'),r'AT\s+%')
        self.assertEqual(re.findall(r'AT %MW(\d+)',(base/'GVL_WelderBridge.st').read_text('utf-8')),['7000','7064','7200'])
    def test_reused_default_parameters_and_pdo_byte_positions(self):
        b=Box();b.command(1,2)
        self.assertEqual(b.w.get('g_byweldoutmap01'),34) # prepare + DC pulse
        self.assertEqual([b.w.get('g_byweldoutmap0'+str(i)) for i in (3,4,5,6)],[50,150,232,3])
        self.assertEqual(b.status()[20:23],[0,1,1000])
    def test_arc_requires_explicit_frame_and_success_feedback_is_independent(self):
        b=Box();self.assertEqual(b.active(),0);b.arc();self.assertFalse(b.status()[11]&1)
        b.w.set('g_byweldinmap01',3);b.scan();self.assertTrue(b.status()[11]&1)
    def test_same_scan_fault_clears_arc_and_hot_wire_without_replay(self):
        b=Box();b.command(1,11);self.assertEqual(b.active(),11)
        b.w.set('g_byweldinmap02',4);b.scan();self.assertEqual(b.active(),2)
        self.assertEqual(b.w.get('g_byweldoutmap01')&1,0);self.assertEqual(b.w.get('g_byweldoutmap02')&1,0)
        b.w.set('g_byweldinmap02',0);b.scan(3);self.assertEqual(b.active(),2)
    def test_heartbeat_timeout_revokes_all_outputs_and_never_replays(self):
        b=Box();b.arc();b.scan(125,hb=False);self.assertEqual(b.active(),0)
        b.scan(3);self.assertEqual(b.active(),0)
    def test_local_permits_and_robot_interlocks_revoke_outputs(self):
        for group,name in [('g','xmappingconfirmed'),('g','xweldpermit'),('g','xpdooperational'),('b','xremotepermit'),('b','xhostalive'),('b','xrobotlinkok')]:
            b=Box();b.arc();getattr(b,group).set(name,False);b.scan();self.assertEqual(b.active(),0,name)
        for name in ('xrobotemergency','xrobotalarm','xstopinterlock'):
            b=Box();b.arc();b.b.set(name,True);b.scan();self.assertEqual(b.active(),0,name)
    def test_pause_mode_change_and_job_end_clear_energized_outputs(self):
        b=Box();b.arc();b.b.set('xrobotpaused',True);b.scan();self.assertEqual(b.active(),2)
        b=Box();b.arc();b.b.set('xrobotexternalauto',False);b.scan();self.assertEqual(b.active(),2)
        b=Box();b.b.set('xrobotrunning',True);b.arc();b.b.set('xrobotrunning',False);b.scan();self.assertEqual(b.active(),2)
    def test_checksum_owner_boot_and_replayed_requests_are_ignored(self):
        for kwargs in ({'bad':True},{'owner':123},{'boot':2},{'seq':0}):
            b=Box();b.command(1,3,**kwargs);self.assertEqual(b.active(),0)
        b=Box();b.command(1,2);b.command(1,3,seq=1);self.assertEqual(b.active(),2)
    def test_ready_gas_mode_and_parameter_guards(self):
        b=Box();b.command(1,1);self.assertEqual(b.status()[9],4)
        b.command(1,7);self.assertEqual(b.status()[9],5)
        b.b.set('xrobotexternalauto',False);b.command(1,3);self.assertEqual(b.status()[9],3)
        b.command(1,0x100);self.assertEqual(b.status()[9],2)
    def test_full_parameter_application_and_rejection_are_atomic(self):
        b=Box();p=b.status()[20:40].copy();p[2]=1254;p[5]=123;p[9]=123;p[10]=654;p[11]=2345;p[18]=7
        b.command(2,params=p);self.assertEqual(b.status()[8:10],[1,0]);self.assertEqual(b.status()[20:40],p)
        self.assertEqual(b.w.get('g_byweldoutmap05'),230);self.assertEqual(b.w.get('g_byweldoutmap06'),4)
        self.assertEqual(b.w.get('g_byweldoutmap14'),123);self.assertEqual(b.w.get('g_byweldoutmap20'),123)
        self.assertEqual(b.w.get('g_byweldoutmap21'),142);self.assertEqual(b.w.get('g_byweldoutmap22'),41);self.assertEqual(b.w.get('g_byweldoutmap23'),41)
        p[2]=4001;b.command(2,params=p);self.assertEqual(b.status()[9],7);self.assertEqual(b.status()[22],1254)
        b.command(1,2);p[2]=1000;b.command(2,params=p);self.assertEqual(b.status()[9],6);self.assertEqual(b.status()[22],1254)
    def test_voltage_current_and_alarm_scaling_matches_old_program(self):
        b=Box()
        for number,value in [(5,123),(6,0),(7,210),(8,4),(9,17),(10,0)]:b.w.set(f'g_byweldinmap{number:02d}',value)
        b.scan();self.assertEqual(b.status()[16:19],[123,1234,1700])
    def test_session_change_and_plc_restart_do_not_restore_old_frame(self):
        b=Box();b.arc();b.b.get('ahoststatus')[15:17]=[0xabcd,1];b.scan();self.assertEqual(b.active(),0)
        b=Box();b.arc();b.r.set('udibootcounter',124);b.p=b.world.instance('prg_welderbridge');b.p.values['prg_welderio']=b.io;b.scan();self.assertEqual(b.active(),0)
    def test_all_off_remains_valid_after_local_permission_revoked(self):
        b=Box();b.arc();b.g.set('xweldpermit',False);b.command(3);self.assertEqual(b.active(),0);self.assertEqual(b.status()[8],1)

if __name__=='__main__':unittest.main()
