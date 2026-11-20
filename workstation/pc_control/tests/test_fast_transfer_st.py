"""Batch transfer and direct-upload verification regressions. Offline sockets only."""
import json,sys,unittest,zlib
from pathlib import Path
sys.path.insert(0,str(Path(__file__).resolve().parent))
import test_auto_st as auto
rt,legacy=auto.rt,auto.legacy

class FastFtpTests(unittest.TestCase):
    def run_ftp(self,data,server=None,verify=False,expected_crc=None,abort_step=None,dt=10):
        server=server or legacy.Cabinet(existing=True)
        world=rt.World(native_factory=server.native)
        env=world.globals['gvl_filebridge'];env.set('xfixedmappingconfirmed',True);env.set('xfixedidle',True)
        world.globals['gvl_robotbridge'].set('uifixedjobguardversion',140)
        env.values['abuffer'][:len(data)]=data
        if verify:server.upload=bytearray(data)
        fb=world.instance('fb_fileftp')
        args=dict(xstart=True,xabort=False,pbuffer=rt.Pointer(env,'abuffer'),udisize=len(data),
                  sproject=auto.PROJECT,sfile='auto_main.srp',xcreateproject=True,xreplacefixed=True,
                  xverifyonly=verify,udiverifycrc=zlib.crc32(data) if expected_crc is None else expected_crc,
                  suser='test',spassword='local_test')
        for scans in range(20000):
            if abort_step is not None and fb.get('uistep')==abort_step:args['xabort']=True
            old_calls=world.calls.copy()
            fb.call(args,dt)
            for key,count in world.calls.items():
                self.assertLessEqual(count-old_calls.get(key,0),1,'at most one native send/recv per instance and scan')
            if fb.get('xdone'):return fb,server,scans+1,world
        self.fail(('did not terminate',fb.get('uistep')))
    def test_packet_boundaries_and_uint_boundary_are_byte_exact(self):
        for size in (0,1,8191,8192,8193,40001,65535,65536,65539):
            with self.subTest(size=size):
                data=bytes(i%256 for i in range(size))
                fb,server,scans,_=self.run_ftp(data)
                self.assertTrue(fb.get('xtestpassed'),(fb.get('werrorid'),fb.get('uierrorstage')))
                self.assertEqual(server.upload,data);self.assertEqual(fb.get('udireadbackbytes'),size)
                self.assertFalse(server.connections)
                self.assertLess(scans,100)
    def test_partial_send_wouldblock_and_fragmented_tail(self):
        server=legacy.Cabinet(existing=True)
        server.send_sizes=(1,31,4093,8192,7);server.recv_sizes=(1,17,8192,29,4000);server.wouldblock_every=4
        data=bytes(i%251 for i in range(40001))
        fb,server,_,_=self.run_ftp(data,server)
        self.assertTrue(fb.get('xtestpassed'));self.assertEqual(server.upload,data)
    def test_full_readback_rejects_corrupt_short_extra_and_bad_completion(self):
        for verify in (False,True):
            for options in ({'corrupt':True},{'short':True},{'extra':True},{'completion':426}):
                fb,server,_,_=self.run_ftp(b'x'*8193,legacy.Cabinet(existing=True,**options),verify)
                self.assertTrue(fb.get('xerror'));self.assertFalse(fb.get('xtestpassed'));self.assertFalse(server.connections)
    def test_direct_verifies_empty_and_multiblock_crc_without_stor(self):
        for size in (0,1,8192,8193,40001,65539):
            data=bytes(i%251 for i in range(size))
            fb,server,_,_=self.run_ftp(data,verify=True)
            self.assertTrue(fb.get('xtestpassed'),fb.get('werrorid'))
            self.assertFalse(any(s.startswith(('STOR','MKD')) for s in server.commands))
            self.assertEqual(fb.get('udiuploadedbytes'),0)
            self.assertEqual(fb.get('udireadbackbytes'),size)
    def test_wrong_direct_expected_crc_never_passes(self):
        fb,_,_,_=self.run_ftp(b'abc',verify=True,expected_crc=123)
        self.assertFalse(fb.get('xtestpassed'));self.assertEqual(fb.get('werrorid'),0xF205)
    def test_codesys_graceful_closed_result_finishes_full_readback(self):
        for verify in (False,True):
            for size in (0,1,8192,8193,22858):
                with self.subTest(verify=verify,size=size):
                    server=legacy.Cabinet(existing=True);server.recv_end_result=0x0211
                    data=bytes(i%251 for i in range(size))
                    fb,server,_,_=self.run_ftp(data,server,verify)
                    self.assertTrue(fb.get('xtestpassed'),(fb.get('werrorid'),fb.get('uierrorstage')))
                    self.assertEqual(fb.get('udireadbackbytes'),size)
                    self.assertEqual(fb.get('uidownloadcompletecode'),226)
                    self.assertFalse(server.connections)
    def test_graceful_close_preserves_length_content_and_ftp_completion_checks(self):
        for verify in (False,True):
            for options in ({'short':True},{'extra':True},{'corrupt':True},{'completion':426}):
                with self.subTest(verify=verify,options=options):
                    server=legacy.Cabinet(existing=True,**options);server.recv_end_result=0x0211
                    fb,server,_,_=self.run_ftp(b'x'*8193,server,verify)
                    self.assertFalse(fb.get('xtestpassed'));self.assertTrue(fb.get('xerror'))
                    self.assertFalse(server.connections)
    def test_other_native_errors_after_full_readback_are_never_eof(self):
        for result in (1,0x020A,0x020F,123):
            for count in (0,-1):
                with self.subTest(result=result,count=count):
                    server=legacy.Cabinet(existing=True)
                    server.recv_end_result=result;server.recv_end_count=count
                    fb,_,_,_=self.run_ftp(b'x'*22858,server)
                    self.assertEqual(fb.get('udireadbackbytes'),22858)
                    self.assertFalse(fb.get('xtestpassed'))
                    self.assertEqual(fb.get('werrorid'),0xF223)
                    self.assertEqual(fb.get('uierrorstage'),220)
    def test_inconsistent_closed_result_is_not_a_successful_read(self):
        for count in (-1,1):
            server=legacy.Cabinet(existing=True);server.recv_end_result=0x0211;server.recv_end_count=count
            fb,_,_,_=self.run_ftp(b'abc',server)
            self.assertFalse(fb.get('xtestpassed'));self.assertEqual(fb.get('werrorid'),0xF223)
    def test_native_receive_error_remains_available_after_cleanup(self):
        server=legacy.Cabinet(existing=True);server.recv_end_result=0x020F
        fb,_,_,_=self.run_ftp(b'abc',server)
        self.assertEqual(fb.get('fbdataio').get('udinativeresult'),0x020F)
        self.assertEqual(fb.get('fbdataio').get('xicount'),0)
    def test_cancel_at_upload_or_readback_cleans_owned_connections(self):
        for step in (60,70):
            fb,server,_,_=self.run_ftp(b'x'*40001,abort_step=step)
            self.assertFalse(fb.get('xtestpassed'));self.assertEqual(fb.get('werrorid'),0xF20D)
            self.assertFalse(server.connections)
    def test_stalled_send_expires_and_native_error_does_not_become_eof(self):
        for option in ('wouldblock_every','raw_error'):
            server=legacy.Cabinet(existing=True);setattr(server,option,1)
            fb,_,_,_=self.run_ftp(b'abc',server,dt=1000)
            self.assertTrue(fb.get('xerror'));self.assertFalse(fb.get('xtestpassed'))
    def test_coalesced_completion_and_fragmented_multiline_replies(self):
        class Server(legacy.Cabinet):
            def reply(self,text):
                if text.startswith('220'):text='220-Welcome\r\narbitrary text\r\n220 Ready'
                super().reply(text)
        server=Server(existing=True);server.recv_sizes=(2,5,8192)
        fb,_,_,_=self.run_ftp(b'abc',server)
        self.assertTrue(fb.get('xtestpassed'))
    def test_40kb_scan_budget_includes_upload_and_full_readback(self):
        fb,_,scans,world=self.run_ftp(b'x'*40000)
        self.assertTrue(fb.get('xtestpassed'));self.assertLess(scans,100)
        self.assertEqual(world.calls[('syssocket.syssocksend',246)],5)
        self.assertEqual(world.calls[('syssocket.syssockrecv',246)],6)
        result={'bytes':40000,'batch_bytes':8192,'new_total_scans':scans,
                'data_send_calls':5,'data_recv_calls_including_eof':6,
                'old_minimum_data_scans':120000,'physical_device_connected':False,
                'note':'Scan-count simulation only; not measured PLC execution time or field throughput.'}
        out=auto.ROOT/'docs/auto_job_r6_4/validation/fast_transfer_scans.json'
        out.write_text(json.dumps(result,indent=2),'utf-8')

class DirectMailboxTests(unittest.TestCase):
    def box(self):
        box=auto.Box()
        original=box.send
        def send(words):
            if words[2]==1 and words[18]==3:
                words[19],words[116],words[117]=box.tag
                legacy.put32(words,118,zlib.crc32(auto.struct.pack('>118H',*words[:118])))
            return original(words)
        box.send=send
        return box
    def test_direct_reserves_interlock_and_rejects_file_chunks(self):
        box=self.box();box.start(b'abc',name='auto_main.srp',count=4,mode=3,project=auto.PROJECT)
        self.assertEqual(box.prg.get('uistate'),100);self.assertTrue(box.gvl.get('xfixedupdatebusy'))
        self.assertFalse(box.gvl.get('xfixedready'))
        box.command(2,payload=b'abc');self.assertEqual(box.prg.get('wreject'),0xE00C)
    def test_direct_deploy_needs_no_ram_payload_and_cannot_finish_partial_set(self):
        box=self.box();box.start(b'abc',name='auto_main.srp',count=4,mode=3,project=auto.PROJECT)
        box.command(3);box.scan(4)
        self.assertEqual(box.prg.get('uistate'),200)
        self.assertEqual(box.prg.get('udireceived'),0)
        self.assertEqual(box.prg.get('udirunningcrc')^0xFFFFFFFF,zlib.crc32(b'abc'))
        box.command(5);self.assertFalse(box.gvl.get('xfixedready'))
    def test_direct_protected_files_and_idle_guard_are_identical(self):
        for name in ('_project.srd','manual_service.srp'):
            box=self.box();box.start(b'abc',name=name,count=4,mode=3,project=auto.PROJECT)
            self.assertEqual(box.prg.get('wreject'),0xE015)
    def test_expired_closed_session_releases_without_local_reset(self):
        box=self.box();box.start(b'abc',name='auto_main.srp',count=4,mode=3,project=auto.PROJECT)
        box.scan(12,heartbeat=False,dt=1000)
        self.assertEqual(box.prg.get('uistate'),900)
        self.assertEqual(box.prg.get('werror'),0xE011)
        self.assertEqual(box.gvl.get('astatus')[7]&3,3)
        box.command(6)
        self.assertEqual(box.prg.get('uistate'),0)
        self.assertFalse(box.gvl.get('xreset'))
        self.assertFalse(box.gvl.get('xfixedready'))
        box.session+=1;box.seq=0;box.tag=(321,654,987)
        box.start(b'new',name='auto_main.srp',count=4,mode=3,project=auto.PROJECT)
        self.assertEqual(box.prg.get('uistate'),100)
        self.assertTrue(box.gvl.get('xfixedupdatebusy'))
        self.assertEqual(box.prg.get('wreject'),0)
    def test_real_mailbox_and_real_ftp_verify_all_files_before_ready(self):
        server=legacy.Cabinet(existing=True)
        world=rt.World(native_factory=server.native)
        g=world.globals['gvl_filebridge'];g.set('xfiletransferpermit',True);g.set('xfixedmappingconfirmed',True)
        g.set('sftpuser','test');g.set('sftppassword','local_test')
        r=world.globals['gvl_robotbridge'];r.set('uifixedjobguardversion',140);r.set('xrobotlinkok',True);r.set('xrobotstopped',True)
        program=world.instance('prg_filebridge');program.call({})
        boot=world.globals['gvl_filebridgeretain'].get('udibootcounter');seq=0;hb=0;owner=1234567;tag=auto.TAG
        def scan():
            nonlocal hb
            hb+=1;g.values['aheartbeat'][:]=[owner>>16,owner&65535,boot>>16,boot&65535,hb]
            program.call({})
        def command(op,**kwargs):
            nonlocal seq
            seq+=1;words=legacy.frame(op,seq,owner,boot,**kwargs)
            if op==1:
                words[19],words[116],words[117]=tag
                legacy.put32(words,118,zlib.crc32(auto.struct.pack('>118H',*words[:118])))
            g.values['acommand'][:]=words;scan();scan()
            self.assertEqual(program.get('wreject'),0)
        files={'auto_main.srd':b'','auto_main.srp':b'x'*40001,'auto_01.srd':b'vars','auto_01.srp':b'code\x00\xff'}
        for attempt in range(3):
            seq=0;owner+=1;tag=tuple(value+attempt for value in auto.TAG)
            files['auto_01.srp']=b'code\x00\xff'+bytes([attempt])
            for ordinal,(name,data) in enumerate(files.items(),1):
                command(1,size=len(data),crc=zlib.crc32(data),count=4,ordinal=ordinal,mode=3,payload=(auto.PROJECT+'\0'+name+'\0').encode())
                self.assertFalse(g.get('xfixedready'));self.assertTrue(g.get('xfixedupdatebusy'))
                server.upload=bytearray(data) # PC completed STOR; only RETR remains for PLC.
                command(3)
                for _ in range(150):
                    scan()
                    if program.get('uistate')==200:break
                self.assertEqual(program.get('uistate'),200,(program.get('werror'),program.get('fbftp').get('werrorid')))
                self.assertEqual(program.get('udireceived'),0)
                self.assertEqual(program.get('udirunningcrc')^0xFFFFFFFF,zlib.crc32(data))
                self.assertFalse(g.get('xfixedready'))
            command(5)
            self.assertTrue(g.get('xfixedready'));self.assertFalse(g.get('xfixedupdatebusy'))
            self.assertEqual(g.get('aautotag'),list(tag));self.assertFalse(server.connections)
            self.assertFalse(g.get('xreset'))
        self.assertFalse(any(s.startswith('STOR ') for s in server.commands))

if __name__=='__main__':unittest.main(verbosity=2)
