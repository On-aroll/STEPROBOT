"""Execute the delivered read-only joint ST against byte queues, never a device."""
import struct
import unittest
import test_auto_st as auto

rt = auto.rt


class Cabinet:
    def __init__(self, fault=None, fragmented=False, connect_delay=1):
        self.connections = set()
        self.request = bytearray()
        self.reply = bytearray()
        self.requests = []
        self.axes = (-12.5, 23.25, -34.75, 45.5, -56.125, 67.875)
        self.words = [v for axis in self.axes for v in struct.unpack('<2H', struct.pack('<f', axis))]
        self.fault, self.fragmented, self.connect_delay = fault, fragmented, connect_delay
        self.connect_scans = 0
        self.closed = []

    def native(self, fb, args):
        v, cid = fb.values, args['diconnectid']
        assert cid == 244, 'joint reader must never borrow FTP/control socket IDs'
        if not args['xexecute']:
            v.update(xdone=False, xerror=False, werrorid=0)
        elif fb.kind == 'SktTCPClient':
            if v['xdone']:
                return
            self.connect_scans += 1
            if self.connect_scans >= self.connect_delay:
                self.connections.add(cid)
                v['xdone'] = True
        elif fb.kind == 'SktClose':
            if v['xdone']:
                return
            assert cid in self.connections, 'never close an unowned/pending connection'
            if self.fault == 'close':
                v.update(xerror=True, werrorid=99)
            else:
                self.connections.remove(cid)
                self.closed.append(cid)
                self.connect_scans = 0
                self.request.clear()
                self.reply.clear()
                v['xdone'] = True
        else:
            raise AssertionError(fb.kind)

    def response(self, request):
        self.requests.append(request)
        assert request[2:6] == b'\0\0\0\x06'
        assert request[6:] == b'\x01\x04\x00\x40\x00\x0c', 'ONLY read FC04 offset64 count12'
        if self.fault == 'stall':
            return
        reply = bytearray(request[:2] + b'\0\0\0\x1b\x01\x04\x18' + struct.pack('>12H', *self.words))
        if self.fault == 'transaction':
            reply[1] ^= 1
        elif self.fault == 'protocol':
            reply[3] = 1
        elif self.fault == 'unit':
            reply[6] = 2
        elif self.fault == 'function':
            reply[7] = 3
        elif self.fault == 'count':
            reply[8] = 22
        elif self.fault == 'exception':
            reply = bytearray(request[:2] + b'\0\0\0\x03\x01\x84\x02')
        elif self.fault == 'short':
            reply.pop()
        elif self.fault == 'extra':
            reply.append(99)
        self.reply.extend(reply)


class JointWorld(rt.World):
    def raw(self, name, args):
        if name == 'cmphctcpip.getsocketinfo':
            return super().raw(name, args)
        cabinet = self.native_factory.__self__
        cid, pointer, count, flags, result = args
        assert cid == 244 and flags == 64
        result.env.set(result.name, 0)
        if name == 'syssocket.syssocksend':
            count = min(count, 2) if cabinet.fragmented else count
            cabinet.request.extend(pointer[i] for i in range(count))
            if len(cabinet.request) == 12:
                cabinet.response(bytes(cabinet.request))
                cabinet.request.clear()
            return count
        if not cabinet.reply:
            result.env.set(result.name, 0x0206)
            return -1
        count = min(count, len(cabinet.reply), 3 if cabinet.fragmented else 8192)
        for i in range(count):
            pointer[i] = cabinet.reply[i]
        del cabinet.reply[:count]
        return count


class JointFeedbackTests(unittest.TestCase):
    def create(self, cabinet=None):
        cabinet = cabinet or Cabinet()
        world = JointWorld(native_factory=cabinet.native)
        fb = world.instance('fb_robotjointread')
        args = dict(xenable=True, saddress='192.168.3.188', uiport=502, byunit=1)
        return cabinet, world, fb, args

    def until(self, fb, args, condition, limit=400):
        for _ in range(limit):
            fb.call(args, 10)
            if condition():
                return
        self.fail(('condition timed out', fb.get('uistep'), fb.get('werrorid')))

    def test_actual_source_reads_all_axes_and_continues_while_unchanged(self):
        cabinet, _, fb, args = self.create()
        self.until(fb, args, lambda: fb.get('udicounter') >= 3)
        self.assertTrue(fb.get('xvalid'))
        self.assertEqual(fb.get('awords'), cabinet.words)
        self.assertEqual(len(cabinet.requests), 3)
        self.assertEqual(cabinet.connections, {244})

    def test_partial_send_and_fragmented_receive_are_not_replayed(self):
        cabinet, _, fb, args = self.create(Cabinet(fragmented=True))
        self.until(fb, args, lambda: fb.get('udicounter') == 1)
        self.assertTrue(fb.get('xvalid'))
        self.assertEqual(fb.get('awords'), cabinet.words)
        self.assertEqual(len(cabinet.requests), 1)

    def test_bad_replies_never_publish_axes(self):
        for fault in ('transaction', 'protocol', 'unit', 'function', 'count', 'exception', 'short', 'extra', 'stall'):
            with self.subTest(fault=fault):
                cabinet, _, fb, args = self.create(Cabinet(fault=fault))
                self.until(fb, args, lambda: fb.get('xerror') and not cabinet.connections)
                self.assertFalse(fb.get('xvalid'))
                self.assertEqual(fb.get('udicounter'), 0)

    def test_stale_feedback_invalidates_previous_good_axes(self):
        cabinet, _, fb, args = self.create()
        self.until(fb, args, lambda: fb.get('xvalid'))
        cabinet.fault = 'stall'
        self.until(fb, args, lambda: fb.get('xerror'))
        self.assertFalse(fb.get('xvalid'))
        self.assertEqual(fb.get('udicounter'), 1)

    def test_disable_waits_for_pending_connect_then_closes(self):
        cabinet, _, fb, args = self.create(Cabinet(connect_delay=12))
        fb.call(args, 10)
        args['xenable'] = False
        self.until(fb, args, lambda: bool(cabinet.closed))
        self.assertEqual(cabinet.requests, [])
        self.assertFalse(fb.get('xvalid'))

    def test_close_failure_latches_without_reusing_socket(self):
        cabinet, _, fb, args = self.create(Cabinet(fault='close'))
        self.until(fb, args, lambda: fb.get('xvalid'))
        args['xenable'] = False
        self.until(fb, args, lambda: fb.get('xcleanupfault'))
        for _ in range(150):
            fb.call(args, 10)
        self.assertEqual(fb.get('uistep'), 200)
        self.assertFalse(fb.get('xvalid'))
        self.assertEqual(len(cabinet.requests), 1)

    def test_counter_wrap_and_config_change_do_not_reuse_old_response(self):
        cabinet, _, fb, args = self.create()
        fb.set('udicounter', 0xffffffff)
        self.until(fb, args, lambda: fb.get('xvalid'))
        self.assertEqual(fb.get('udicounter'), 1)
        args['saddress'] = '192.168.3.189'
        fb.call(args, 10)
        self.assertFalse(fb.get('xvalid'))
        self.until(fb, args, lambda: bool(cabinet.closed))

    def test_publisher_works_without_manual_entry_or_host_registration(self):
        cabinet = Cabinet()
        world = JointWorld(native_factory=cabinet.native)
        robot = world.globals['gvl_robotbridge']
        robot.set('xrobotlinkok', True)
        robot.set('xrobotrunning', True)
        world.globals['gvl_robotbridgeretain'].set('udibootcounter', 7)
        gvl = world.globals['gvl_robotjoints']
        gvl.values['astatus'][:] = [65535] * 40
        prg = world.instance('prg_robotjoints')
        for _ in range(40):
            prg.call({}, 10)
        words = gvl.get('astatus')
        self.assertEqual(words[0:3], [0x4a52, 1, 9])
        self.assertEqual(words[5], 7)
        self.assertEqual(words[10:22], cabinet.words)
        self.assertEqual(words[6:8], words[32:34])
        self.assertFalse(robot.get('xhostregistered'))
        self.assertFalse(world.globals['gvl_manualbridge'].get('xactive'))
        self.assertEqual(robot.get('arobottx'), [0, 0])
        robot.set('xrobotlinkok', False)
        prg.call({}, 10)
        self.assertFalse(gvl.get('astatus')[2] & 9)


if __name__ == '__main__':
    unittest.main()
