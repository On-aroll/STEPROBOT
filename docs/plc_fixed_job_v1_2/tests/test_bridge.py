import argparse
import copy
import json
from pathlib import Path
import sys
import tempfile
import unittest
import zipfile
import zlib

ROOT = Path(__file__).parents[1]
sys.path.insert(0, str(ROOT))
from pc_file_bridge import (frame, u32, put32, read_status, validate_files, load_files,
                            Uploader, MAX_FILE, COMMAND, HEARTBEAT, STATUS)
from st_runtime import World, Pointer


class FtpStub:
    """Mailbox tests use a delayed FTP mock; backend itself is tested below."""
    def __init__(self):
        self.starts = 0
        self.old = False
        self.values = dict(xdone=False, xtestpassed=False, xcontrolowned=False, xdataowned=False,
            uistep=0, uireplycode=0, werrorid=0, uierrorstage=0, udiuploadedbytes=0,
            udireadbackbytes=0, wdatacloseerrorid=0, wcontrolcloseerrorid=0,
            uiuploadcompletecode=0, uidownloadcompletecode=0)

    def get(self, name, raw=False): return self.values[name]

    def call(self, args, dt):
        v = self.values
        if args['xstart'] and not self.old and v['uistep'] == 0:
            self.starts += 1
            self.size = args['udisize']
            v.update(xdone=False, xtestpassed=False, uistep=60)
        elif args['xstart'] and v['uistep'] == 60:
            v.update(xdone=True, xtestpassed=not args['xabort'], uistep=100,
                     udiuploadedbytes=self.size, udireadbackbytes=self.size,
                     uiuploadcompletecode=226, uidownloadcompletecode=226)
        elif not args['xstart'] and v['uistep'] == 100:
            v['uistep'] = 0  # xDone intentionally remains TRUE, like field probe.
        self.old = args['xstart']


class Mailbox:
    def __init__(self):
        self.world = World(ftp_factory=FtpStub)
        self.gvl = self.world.globals['gvl_filebridge']
        self.gvl.set('xfiletransferpermit', True)
        self.prg = self.world.instance('prg_filebridge')
        self.seq, self.session, self.hb = 0, 0x87654321, 0
        self.scan(1)
        self.boot = self.world.globals['gvl_filebridgeretain'].get('udibootcounter')

    def scan(self, n=1, heartbeat=True, dt=10):
        for _ in range(n):
            if heartbeat and hasattr(self, 'boot'):
                self.hb = (self.hb + 1) & 0xFFFF
                self.gvl.values['aheartbeat'][:] = [self.session >> 16, self.session & 65535,
                                                    self.boot >> 16, self.boot & 65535, self.hb]
            self.prg.call({}, dt)

    def send(self, words):
        self.gvl.values['acommand'][:] = words
        self.scan(2)
        return self.gvl.get('astatus')[:]

    def command(self, op, **kwargs):
        self.seq += 1
        return self.send(frame(op, self.seq, self.session, self.boot, **kwargs))

    def start(self, data=b'abc', name='probe.txt', count=1, ordinal=1, mode=0, project='pcjob_test_abc.sr'):
        return self.command(1, size=len(data), crc=zlib.crc32(data), count=count,
            ordinal=ordinal, mode=mode, payload=(project + '\0' + name + '\0').encode())

    def file(self, data=b'abc', name='probe.txt', count=1, ordinal=1, mode=0):
        self.start(data, name, count, ordinal, mode)
        for offset in range(0, len(data), 192):
            self.command(2, offset=offset, payload=data[offset:offset + 192])
        self.command(3)
        self.scan(4)
        assert self.prg.get('uistate') == 200


class WireTests(unittest.TestCase):
    def test_frame_crc_and_binary_high_word(self):
        import struct
        data = bytes([0, 255, 13, 10, 128])
        words = frame(2, 0xFEDCBA98, 0x87654321, 0x10203040, offset=70001, payload=data)
        self.assertEqual(u32(words, 11), 70001)
        self.assertEqual(struct.pack('>96H', *words[20:116])[:5], data)
        self.assertEqual(u32(words, 118), zlib.crc32(struct.pack('>118H', *words[:118])))
        self.assertEqual(len(words), 120)

    def test_names_and_complete_native_bundle(self):
        good = {'_project.srd': b'globals', 'main.srp': b'code', 'main.srd': b'vars'}
        self.assertEqual(len(validate_files(good, True)), 3)
        for files in ({'main.srp': b'x'}, {'../main.txt': b'x'}, {'UP.txt': b'x'},
                      {'p.txt': b''}, {'p.txt': b'x' * (MAX_FILE + 1)}):
            with self.assertRaises(ValueError): validate_files(files, True)

    def test_zip_traversal_duplicate_and_missing_pair(self):
        for names in (['x.sr/../probe.txt'], ['x.sr/main.srp', 'x.sr/main.srp'],
                      ['x.sr/_project.srd', 'x.sr/main.srp']):
            with tempfile.TemporaryDirectory() as folder:
                path = Path(folder) / 'bad.zip'
                import warnings
                with warnings.catch_warnings():
                    warnings.simplefilter('ignore')
                    with zipfile.ZipFile(path, 'w') as archive:
                        for name in names: archive.writestr(name, b'abc')
                args = argparse.Namespace(file=None, project_dir=None, zip=str(path))
                with self.assertRaises(ValueError): load_files(args)


class SourceMailboxTests(unittest.TestCase):
    def test_crc_function_known_vector_and_fragmentation(self):
        box = Mailbox()
        function = box.world.instance('f_filecrc32')
        box.prg.values['testbytes'] = list(b'123456789')
        function.call(dict(pdata=Pointer(box.prg, 'testbytes'), uicount=9, udiseed=0xFFFFFFFF))
        self.assertEqual(function.get('f_filecrc32') ^ 0xFFFFFFFF, 0xCBF43926)

    def test_odd_binary_transfer_exact_replay_and_second_file_stale_done(self):
        box = Mailbox()
        data = bytes(range(256)) + b'\x00\xff\x0d\x0aX'
        box.file(data, 'probe.txt', 2, 1)
        self.assertEqual(box.gvl.get('abuffer')[:len(data)], list(data))
        self.assertEqual(box.prg.get('uiverified'), 1)
        accepted = box.prg.get('aaccepted')[:]
        box.send(frame(3, box.seq, box.session, box.boot))  # exact replay
        self.assertEqual(box.prg.get('fbftp').starts, 1)
        box.start(b'new', 'other.txt', 2, 2)
        box.command(2, payload=b'new')
        box.command(3)
        # A stale TRUE xDone from file 1 must not verify file 2 before new launch.
        self.assertEqual(box.prg.get('uiverified'), 1)
        self.assertEqual(box.prg.get('fbftp').starts, 2)
        box.scan(4)
        box.command(5)
        self.assertEqual(box.prg.get('uistate'), 400)
        self.assertEqual(box.prg.get('uiverified'), 2)

    def test_reused_sequence_with_changed_payload_rejected(self):
        box = Mailbox(); box.start()
        box.command(2, payload=b'a')
        box.send(frame(2, box.seq, box.session, box.boot, payload=b'b'))
        self.assertEqual(box.prg.get('wreject'), 0xE004)
        self.assertEqual(box.prg.get('udireceived'), 1)

    def test_wrong_boot_foreign_owner_out_of_order_crc(self):
        box = Mailbox(); box.start()
        cases = [(frame(2, 2, box.session, box.boot + 1, payload=b'a'), 0xE003),
                 (frame(2, 2, 123, box.boot, payload=b'a'), 0xE006),
                 (frame(2, 3, box.session, box.boot, payload=b'a'), 0xE005),
                 (frame(2, 2, box.session, box.boot, offset=1, payload=b'a'), 0xE00C)]
        corrupt = frame(2, 2, box.session, box.boot, payload=b'a'); corrupt[20] ^= 1
        cases.append((corrupt, 0xE002))
        for words, expected in cases:
            box.send(words)
            self.assertEqual(box.prg.get('wreject'), expected)
            self.assertEqual(box.prg.get('udireceived'), 0)
        self.assertEqual(box.prg.get('fbftp').starts, 0)

    def test_incomplete_or_bad_crc_never_invokes_ftp(self):
        box = Mailbox(); box.start(); box.command(3)
        self.assertEqual(box.prg.get('wreject'), 0xE00D)
        self.assertEqual(box.prg.get('fbftp').starts, 0)
        box = Mailbox(); box.start(); box.command(2, payload=b'bad'); box.command(3)
        self.assertEqual(box.prg.get('werror'), 0xE00E)
        self.assertEqual(box.prg.get('fbftp').starts, 0)

    def test_offsets_and_counters_cross_65535(self):
        box = Mailbox(); data = b'A' * 65535 + b'B\x00C'
        box.start(data)
        # Seed already received prefix; execute the actual boundary CHUNK path.
        box.prg.set('udireceived', 65535)
        box.prg.set('udirunningcrc', zlib.crc32(data[:65535]) ^ 0xFFFFFFFF)
        box.command(2, offset=65535, payload=data[65535:])
        self.assertEqual(box.prg.get('udireceived'), 65538)
        self.assertEqual(box.gvl.get('abuffer')[65535:65538], [66, 0, 67])
        self.assertEqual(box.prg.get('udirunningcrc') ^ 0xFFFFFFFF, zlib.crc32(data))

    def test_zero_length_file_crc_and_mailbox_deploy(self):
        box = Mailbox()
        box.start(b'')
        self.assertEqual(box.prg.get('uistate'), 100)
        self.assertEqual(box.prg.get('udireceived'), 0)
        self.assertEqual(box.prg.get('udirunningcrc') ^ 0xFFFFFFFF, 0)
        box.command(3)  # No CHUNK: empty file still needs actual FTP verification.
        self.assertEqual(box.prg.get('fbftp').starts, 1)
        box.scan(4)
        self.assertEqual(box.prg.get('uiverified'), 1)
        box.command(5)
        status = box.gvl.get('astatus')
        self.assertEqual(status[2], 400)
        self.assertEqual(status[47], 3)
        self.assertEqual(status[48], 0x0102)

    def test_empty_bundle_refuses_old_plc_before_any_write(self):
        box = Mailbox()
        writes = []
        class OldClient:
            def read(self, address, count):
                box.scan()
                words = box.gvl.get('astatus')[:]
                words[47] = words[48] = 0
                return words
            def write(self, address, words): writes.append((address, words))
        sender = Uploader(OldClient(), 'pcjob_empty_old_plc.sr', report=lambda _: None)
        with self.assertRaisesRegex(RuntimeError, 'zero-length file support'):
            sender.upload({'_project.srd': b'global', 'main.srp': b'code', 'main.srd': b''}, True)
        self.assertEqual(writes, [])

    def test_missing_heartbeat_and_permit_withdrawal(self):
        box = Mailbox(); box.start()
        box.scan(4, heartbeat=False, dt=6000)
        self.assertEqual(box.prg.get('werror'), 0xE011)
        self.assertEqual(box.prg.get('uistate'), 900)
        box = Mailbox(); box.start(); box.gvl.set('xfiletransferpermit', False); box.scan()
        self.assertEqual(box.prg.get('werror'), 0xE007)

    def test_duplicate_file_and_incomplete_bundle(self):
        box = Mailbox(); box.file(b'a', 'one.txt', 2, 1)
        box.command(5)
        self.assertEqual(box.prg.get('wreject'), 0xE00F)
        box.seq -= 1  # rejected request did not consume sequence
        box.start(b'b', 'one.txt', 2, 2)
        self.assertEqual(box.prg.get('wreject'), 0xE00B)

    def test_actual_uploader_over_st_mailbox_three_native_files(self):
        box = Mailbox()
        class Client:
            def read(self, address, count):
                assert (address, count) == (STATUS, 64)
                box.scan(2, heartbeat=False)
                return box.gvl.get('astatus')[:]
            def write(self, address, words):
                if address == COMMAND: box.gvl.values['acommand'][:] = words
                elif address == HEARTBEAT: box.gvl.values['aheartbeat'][:] = words
                else: raise AssertionError(address)
                box.scan(2, heartbeat=False)
        uploader = Uploader(Client(), 'pcjob_python_test.sr', wait_seconds=3, report=lambda _: None)
        result = uploader.upload({'_project.srd': b'global\x00', 'main.srd': b'vars', 'main.srp': b'code'}, True)
        self.assertEqual(result['final_status']['state'], 400)
        self.assertEqual(len(result['files']), 3)
        self.assertFalse(result['robot_started'])
        self.assertEqual(box.prg.get('fbftp').starts, 3)


class Cabinet:
    """Local byte queues implement FTP peer; native socket calls never use network."""
    def __init__(self, *, existing=False, corrupt=False, short=False, extra=False,
                 completion=226, close_failure=False, connect_delay=1):
        self.existing, self.corrupt, self.short, self.extra = existing, corrupt, short, extra
        self.completion, self.close_failure, self.connect_delay = completion, close_failure, connect_delay
        self.control, self.download, self.command = bytearray(), bytearray(), bytearray()
        self.upload = bytearray()
        self.directory, self.mode = False, None
        self.connections, self.counts, self.closed = set(), {}, []
        self.commands = []
        self.files = {}
        self.current_file = ''

    def reply(self, text): self.control.extend((text + '\r\n').encode())

    def line(self, text):
        self.commands.append(text)
        if text.startswith('USER '): self.reply('331 Password')
        elif text.startswith('PASS '): self.reply('230 Logged in')
        elif text == 'CWD /UserPrograms': self.reply('250 Base')
        elif text.startswith('CWD '): self.reply('250 Folder' if self.directory or self.existing else '550 Missing')
        elif text.startswith('MKD '): self.directory = True; self.reply('257 Created')
        elif text == 'TYPE I': self.reply('200 Binary')
        elif text == 'PASV': self.reply('227 Passive (192,168,3,188,128,109)')
        elif text.startswith('STOR '):
            self.current_file = text[5:]; self.mode = 'stor'; self.upload.clear(); self.reply('150 Upload')
        elif text.startswith('RETR '):
            self.mode = 'retr'; self.download = bytearray(self.upload)
            if self.corrupt: self.download[0] ^= 1
            if self.short: self.download.pop()
            if self.extra: self.download.append(99)
            self.reply('150 Download'); self.reply(f'{self.completion} Complete')
        elif text == 'QUIT': self.reply('221 Bye')
        else: raise AssertionError(text)

    def native(self, fb, args):
        v, kind, cid = fb.values, fb.kind, args['diconnectid']
        enabled = args.get('xexecute', args.get('xenable', False))
        if not enabled:
            v.update(xdone=False, xerror=False, werrorid=0, uicount=0)
            return
        if kind == 'SktTCPClient':
            if v['xdone']: return
            self.counts[cid] = self.counts.get(cid, 0) + 1
            if self.counts[cid] < self.connect_delay: return
            self.connections.add(cid); v['xdone'] = True
            if cid == 245: self.reply('220 Test FTP')
        elif kind == 'SktTCPSend':
            if v['xdone']: return
            assert cid in self.connections and args['uidatasize'] == 1
            data = args['abydata'][0]
            if cid == 245:
                self.command.append(data)
                if self.command.endswith(b'\r\n'):
                    self.line(self.command[:-2].decode()); self.command.clear()
            else: self.upload.append(data)
            v['xdone'] = True
        elif kind == 'SktTCPRecv':
            queue = self.control if cid == 245 else self.download
            v.update(uicount=0, xerror=False, werrorid=0)
            if queue:
                args['abydata'][0] = queue.pop(0); v['uicount'] = 1
            elif cid == 246 and self.mode == 'retr':
                v.update(xerror=True, werrorid=17)
        elif kind == 'SktClose':
            if v['xdone']: return
            assert cid in self.connections, 'closing an unowned/pending socket'
            if self.close_failure:
                v.update(xerror=True, werrorid=99); return
            self.connections.remove(cid); self.closed.append(cid); self.counts[cid] = 0
            if cid == 246 and self.mode == 'stor':
                self.files[self.current_file] = bytes(self.upload)
                self.reply('226 Stored')
            v['xdone'] = True
        else: raise AssertionError(kind)


class SourceFtpTests(unittest.TestCase):
    def run_ftp(self, cabinet, data=b'\x00A\xff\r\nB\x80', abort_at=None, initial_offset=None):
        world = World(native_factory=cabinet.native)
        env = world.globals['gvl_filebridge']
        env.values['abuffer'][:len(data)] = data
        fb = world.instance('fb_fileftp')
        args = dict(xstart=True, xabort=False, pbuffer=Pointer(env, 'abuffer'),
                    udisize=len(data), sproject='pcjob_source_test.sr', sfile='probe.txt',
                    xcreateproject=True, suser='test', spassword='local_test')
        for i in range(4000):
            args['xabort'] = abort_at is not None and i >= abort_at
            fb.call(args)
            if fb.get('xdone'): return fb
        self.fail(('FTP mock did not terminate', fb.get('uistep')))

    def test_actual_ftp_source_success_byte_exact_binary(self):
        server = Cabinet(); fb = self.run_ftp(server)
        self.assertTrue(fb.get('xtestpassed'))
        self.assertEqual(fb.get('udiuploadedbytes'), 7)
        self.assertEqual(fb.get('udireadbackbytes'), 7)
        self.assertEqual(server.upload, b'\x00A\xff\r\nB\x80')
        self.assertFalse(fb.get('xcontrolowned') or fb.get('xdataowned'))

    def test_empty_ftp_file_is_created_and_verified_without_dummy_byte(self):
        server = Cabinet()
        fb = self.run_ftp(server, data=b'')
        self.assertTrue(fb.get('xtestpassed'))
        self.assertEqual(fb.get('udiuploadedbytes'), 0)
        self.assertEqual(fb.get('udireadbackbytes'), 0)
        self.assertEqual(server.files, {'probe.txt': b''})
        self.assertIn('STOR probe.txt', server.commands)
        self.assertIn('RETR probe.txt', server.commands)
        self.assertFalse(server.connections)

    def test_empty_ftp_still_requires_positive_final_reply(self):
        fb = self.run_ftp(Cabinet(completion=426), data=b'')
        self.assertFalse(fb.get('xtestpassed'))
        self.assertTrue(fb.get('xerror'))

    def test_no_overwrite_existing_directory(self):
        server = Cabinet(existing=True); fb = self.run_ftp(server)
        self.assertFalse(fb.get('xtestpassed'))
        self.assertEqual(fb.get('werrorid'), 0xF208)
        self.assertFalse(any(c.startswith('STOR') for c in server.commands))

    def test_bad_readback_or_final_ftp_reply_never_passes(self):
        for kwargs in ({'corrupt': True}, {'short': True}, {'extra': True}, {'completion': 426}):
            with self.subTest(kwargs=kwargs):
                fb = self.run_ftp(Cabinet(**kwargs))
                self.assertFalse(fb.get('xtestpassed'))
                self.assertTrue(fb.get('xerror'))

    def test_abort_waits_for_pending_connect_then_closes(self):
        server = Cabinet(connect_delay=4)
        fb = self.run_ftp(server, abort_at=2)
        self.assertFalse(fb.get('xtestpassed'))
        self.assertEqual(fb.get('werrorid'), 0xF20D)
        self.assertEqual(server.closed, [245])
        self.assertFalse(server.upload)

    def test_cleanup_failure_retains_ownership_and_blocks_rearm(self):
        server = Cabinet(close_failure=True)
        fb = self.run_ftp(server)
        self.assertFalse(fb.get('xtestpassed'))
        self.assertTrue(fb.get('xdataowned') or fb.get('xcontrolowned'))
        fb.call(dict(xstart=False))
        self.assertEqual(fb.get('uistep'), 100)

    def test_ftp_data_indices_cross_65535_in_delivered_source(self):
        server = Cabinet()
        world = World(native_factory=server.native)
        env = world.globals['gvl_filebridge']
        env.values['abuffer'][65535:65538] = [65, 0, 255]
        fb = world.instance('fb_fileftp')
        fb.set('pbufferRun'.lower(), Pointer(env, 'abuffer'))
        fb.set('udiexpectedbytes', 65538)
        fb.set('udiuploadedbytes', 65535)
        fb.set('uistep', 60)
        server.connections.add(246)
        server.mode = 'stor'
        for _ in range(6): fb.call({})
        self.assertEqual(fb.get('udiuploadedbytes'), 65538)
        self.assertEqual(server.upload, b'A\x00\xff')
        # Independently exercise readback pointer/counter boundary.
        fb.set('uistep', 70)
        fb.set('udireadbackbytes', 65535)
        server.mode = 'retr'; server.download = bytearray(b'A\x00\xff')
        for _ in range(3): fb.call({})
        self.assertEqual(fb.get('udireadbackbytes'), 65538)
        self.assertFalse(fb.get('xerror'))

    def test_pc_uploader_receiver_and_real_ftp_st_together(self):
        server = Cabinet()
        world = World(native_factory=server.native)
        gvl = world.globals['gvl_filebridge']
        gvl.set('xfiletransferpermit', True)
        gvl.set('sftpuser', 'test')
        gvl.set('sftppassword', 'local_test')
        program = world.instance('prg_filebridge')
        program.call({})
        class Client:
            def read(self, address, count):
                self_test.assertEqual((address, count), (STATUS, 64))
                for _ in range(50): program.call({}, 1)
                return gvl.get('astatus')[:]
            def write(self, address, words):
                target = 'acommand' if address == COMMAND else 'aheartbeat'
                gvl.values[target][:] = words
                program.call({}, 1)
        self_test = self
        files = {'_project.srd': b'globals\x00', 'main.srd': b'variables', 'main.srp': b'code\r\n'}
        result = Uploader(Client(), 'pcjob_integrated_test.sr', wait_seconds=10,
                          report=lambda _: None).upload(files, True)
        self.assertEqual(result['final_status']['state'], 400)
        self.assertEqual(server.files, files)
        self.assertEqual(len([x for x in server.commands if x.startswith('MKD ')]), 1)
        self.assertFalse(server.connections)

    def test_empty_srd_pc_to_receiver_to_ftp_source_integration(self):
        server = Cabinet()
        world = World(native_factory=server.native)
        gvl = world.globals['gvl_filebridge']
        gvl.set('xfiletransferpermit', True)
        gvl.set('sftpuser', 'test'); gvl.set('sftppassword', 'local_test')
        program = world.instance('prg_filebridge')
        program.call({})
        class Client:
            def read(self, address, count):
                for _ in range(50): program.call({}, 1)
                return gvl.get('astatus')[:]
            def write(self, address, words):
                gvl.values['acommand' if address == COMMAND else 'aheartbeat'][:] = words
                program.call({}, 1)
        files = {'_project.srd': b'globals', 'capture_home.srd': b'', 'capture_home.srp': b'code\r\n'}
        result = Uploader(Client(), 'pcjob_empty_integrated.sr', wait_seconds=10,
                          report=lambda _: None).upload(files, True)
        self.assertEqual(result['final_status']['state'], 400)
        self.assertEqual(result['final_status']['verified'], 3)
        self.assertEqual(server.files, files)
        empty = next(file for file in result['files'] if file['name'] == 'capture_home.srd')
        self.assertEqual(empty['bytes'], 0)
        self.assertEqual(empty['crc32'], '00000000')
        self.assertEqual(empty['plc_status']['sent'], 0)
        self.assertEqual(empty['plc_status']['readback'], 0)
        self.assertEqual(empty['plc_status']['upload_completion'], 226)
        self.assertEqual(empty['plc_status']['readback_completion'], 226)


if __name__ == '__main__':
    unittest.main(verbosity=2)
