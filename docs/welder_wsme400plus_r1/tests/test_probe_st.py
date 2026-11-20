"""Execute the delivered ST with a mocked read-only CoE FB; not an IEC compiler.

Only the library enum, SDO responses, and scan time are simulated. The ST source
and its state machine execute unchanged in the repository's strict ST runtime.
"""
from pathlib import Path
import importlib.util
import re
import unittest

ROOT = Path(__file__).resolve().parents[3]
BASE = ROOT / 'docs/welder_wsme400plus_r1'
spec = importlib.util.spec_from_file_location(
    'wsme_probe_runtime', ROOT / 'docs/plc_fixed_job_v1_2/tests/st_runtime.py')
rt = importlib.util.module_from_spec(spec)
spec.loader.exec_module(rt)
rt.ROOT = BASE / 'plc'


class LibraryEnums:
    def get(self, name, raw=False):
        if name == 'etc_co_error.etc_co_no_error':
            return 0
        raise KeyError(name)


def mapping(index, sub, bits=8):
    return (index << 16) | (sub << 8) | bits


def device(rx=None, tx=None, rx_pdo=0x1601, tx_pdo=0x1A00):
    rx = [mapping(0x7010, n) for n in range(1, 17)] if rx is None else rx
    tx = [mapping(0x6000, n) for n in range(1, 17)] if tx is None else tx
    data = {
        (0x1018, 1): (8, 4), (0x1018, 2): (0x9252, 4),
        (0x1018, 3): (1, 4), (0x1018, 4): (1, 4),
        (0x1C12, 0): (1, 1), (0x1C13, 0): (1, 1),
        (0x1C12, 1): (rx_pdo, 2), (0x1C13, 1): (tx_pdo, 2),
        (rx_pdo, 0): (len(rx), 1), (tx_pdo, 0): (len(tx), 1),
    }
    data.update({(rx_pdo, n): (value, 4) for n, value in enumerate(rx, 1)})
    data.update({(tx_pdo, n): (value, 4) for n, value in enumerate(tx, 1)})
    return data


class SdoReadMock:
    """HC API from installed 1.1.1.0 embedded docs; native execution is mocked.

    Receives the caller's numeric array by reference, keeps it during a pending
    request, then fills bytes and returns szDataRead / udiSdoAbortCode.
    """
    def __init__(self, data, intercept=None):
        self.data, self.intercept = data, intercept
        self.values = {
            'xdone': False, 'xbusy': False, 'xerror': False, 'eerror': 0,
            'szdataread': 0, 'udisdoabortcode': 0}
        self.requests = []
        self.calls = []
        self.occurrences = {}
        self.old_execute = False
        self.pending = None
        self.initialized = False
        self.buffer = None

    def get(self, name, raw=False):
        return self.values[name]

    def call(self, args, dt):
        assert set(args) == {'xexecute', 'xabort', 'usicom', 'uidevice',
                             'byaddressmode', 'windex', 'bysubindex', 'uditimeout',
                             'readbuffer', 'szsize'}
        assert args['usicom'] == 1 and args['uidevice'] == 1001
        assert args['byaddressmode'] == 0  # documented default, not mode semantics
        assert args['szsize'] == 4
        assert isinstance(args['readbuffer'], list) and len(args['readbuffer']) == 4
        if self.buffer is not None:
            assert args['readbuffer'] is self.buffer, 'receive buffer must remain alive'
        self.buffer = args['readbuffer']
        self.calls.append(args.copy())
        execute = args['xexecute']
        edge = execute and not self.old_execute
        self.old_execute = execute
        if args['xabort'] or not execute:
            self.values.update(xdone=False, xbusy=False, xerror=False)
            self.pending = None
            if not args['xabort']:
                self.initialized = True
            return
        if edge:
            assert self.initialized, 'documented FALSE Execute initialization required'
            key = args['windex'], args['bysubindex']
            assert args['uditimeout'] == 2000
            self.requests.append(key)
            occurrence = self.occurrences.get(key, 0) + 1
            self.occurrences[key] = occurrence
            response = self.intercept(key, occurrence) if self.intercept else None
            if response is None:
                response = (*self.data[key], 0) if key in self.data else (0, 0, 5752)
            self.pending = response
            self.values['xbusy'] = True
            return
        if self.pending is None or self.pending == 'hang':
            return
        value, length, error, *abort = self.pending
        self.pending = None
        self.values.update(xbusy=False, xerror=bool(error), xdone=not error,
                           eerror=error, szdataread=length,
                           udisdoabortcode=abort[0] if abort else 0)
        # Deliberately dirty bytes beyond the returned length; they must be ignored.
        self.buffer[:] = [0xEE] * 4
        for n in range(min(length, 4)):
            self.buffer[n] = (value >> (8 * n)) & 255


class Probe:
    def __init__(self, data=None, intercept=None):
        self.world = rt.World()
        self.world.globals['iodrvethercatlib'] = LibraryEnums()
        self.program = self.world.instance('prg_wsmeprobe')
        self.reader = SdoReadMock(device() if data is None else data, intercept)
        self.program.values['fbread'] = self.reader
        self.set('uiSlaveAddress', 1001)

    def get(self, key):
        return self.program.get(key)

    def set(self, key, value):
        self.program.set(key.lower(), value)

    def scan(self, count=1):
        for _ in range(count):
            self.program.call({}, 10)

    def start(self):
        self.set('xStart', False)
        self.scan()
        self.set('xStart', True)
        self.scan()

    def finish(self):
        for _ in range(2500):
            if not self.get('xBusy'):
                return
            self.scan()
        raise AssertionError('probe did not finish')


class Tests(unittest.TestCase):
    def test_reads_all_entries_including_beyond_esi_nine(self):
        b = Probe(); b.start(); b.finish()
        self.assertTrue(b.get('xDone'))
        self.assertFalse(b.get('xError'))
        self.assertEqual([b.get(k) for k in ('udiVendor', 'udiProduct', 'udiRevision', 'udiSerial')],
                         [8, 37458, 1, 1])
        for prefix, index in [('Rx', 0x7010), ('Tx', 0x6000)]:
            self.assertEqual(b.get('ui' + prefix + 'Count'), 16)
            self.assertEqual(b.get('ui' + prefix + 'Bits'), 128)
            self.assertEqual(b.get('ui' + prefix + 'Bytes'), 16)
            self.assertTrue(b.get('x' + prefix + 'ByteAligned'))
            self.assertEqual(b.get('a' + prefix + 'Index')[:16], [index] * 16)
            self.assertEqual(b.get('a' + prefix + 'SubIndex')[:16], list(range(1, 17)))
            self.assertEqual(b.get('a' + prefix + 'BitOffset')[:16], list(range(0, 128, 8)))
        self.assertEqual(b.reader.occurrences[(0x1601, 16)], 2)
        self.assertEqual(b.reader.occurrences[(0x1A00, 16)], 2)

    def test_item_count_is_not_byte_count(self):
        rx = [mapping(0x7010, n, 16) for n in range(1, 17)]
        b = Probe(device(rx=rx)); b.start(); b.finish()
        self.assertTrue(b.get('xDone'))
        self.assertEqual(b.get('uiRxCount'), 16)
        self.assertEqual(b.get('uiRxBytes'), 32)

    def test_caller_owned_buffer_and_little_endian_decode(self):
        data = device([mapping(0x7ABC, 0xDE)], [mapping(0x6FED, 0xBC)])
        data[(0x1018, 1)] = (0x12345678, 4)
        b = Probe(data)
        b.start(); b.finish()
        self.assertIs(b.reader.buffer, b.get('abyReadData'))
        self.assertTrue(b.get('xDone'))
        self.assertEqual(b.get('udiVendor'), 0x12345678)
        self.assertEqual(b.get('aRxMap')[0], 0x7ABCDE08)
        self.assertEqual(b.get('aTxMap')[0], 0x6FEDBC08)
        self.assertEqual(b.get('aRxIndex')[0], 0x7ABC)
        self.assertEqual(b.get('aRxSubIndex')[0], 0xDE)

    def test_first_native_calls_use_valid_inputs_and_false_execute_without_abort(self):
        b = Probe(); b.scan(10)
        self.assertEqual(b.reader.calls, [])
        b.start(); b.scan(2)
        self.assertEqual(len(b.reader.calls), 3)
        for call in b.reader.calls[:2]:
            self.assertFalse(call['xexecute'])
            self.assertFalse(call['xabort'])
            self.assertEqual((call['usicom'], call['uidevice']), (1, 1001))
            self.assertEqual((call['windex'], call['bysubindex']), (0x1018, 1))
        self.assertTrue(b.reader.calls[2]['xexecute'])
        b.finish(); b.scan(3)
        self.assertFalse(b.reader.calls[-1]['xabort'])

    def test_native_status_observations_survive_timeout_and_clear_on_new_run(self):
        b = Probe(intercept=lambda key, occurrence: 'hang'); b.start(); b.finish()
        self.assertEqual(b.get('uiError'), 10)
        self.assertTrue(b.get('xReadBusySeen'))
        self.assertFalse(b.get('xReadDoneSeen'))
        self.assertFalse(b.get('xReadErrorSeen'))
        b.scan(3)
        self.assertTrue(b.get('xReadBusySeen'))
        b.reader.intercept = None
        b.start()
        self.assertFalse(b.get('xReadBusySeen'))
        b.finish()
        self.assertTrue(b.get('xReadDoneSeen'))
        self.assertFalse(b.get('xReadErrorSeen'))

    def test_full_capacity_and_dynamic_assignment_indices(self):
        rx = [mapping(0x7020, n) for n in range(1, 33)]
        tx = [mapping(0x6010, n) for n in range(1, 33)]
        b = Probe(device(rx, tx, 0x1602, 0x1A02)); b.start(); b.finish()
        self.assertTrue(b.get('xDone'))
        self.assertEqual(b.get('aRxMap'), rx)
        self.assertEqual(b.get('aTxMap'), tx)
        self.assertEqual(b.get('uiTxBytes'), 32)
        self.assertNotIn((0x1601, 0), b.reader.requests)

    def test_empty_maps_are_not_read_as_entry_one(self):
        b = Probe(device([], [])); b.start(); b.finish()
        self.assertTrue(b.get('xDone'))
        self.assertEqual(b.get('uiRxBytes'), 0)
        self.assertNotIn((0x1601, 1), b.reader.requests)
        self.assertNotIn((0x1A00, 1), b.reader.requests)

    def test_padding_and_non_byte_aligned_total(self):
        b = Probe(device([8, mapping(0x7010, 1, 1)], [mapping(0x6000, 1, 3)]))
        b.start(); b.finish()
        self.assertTrue(b.get('xDone'))
        self.assertEqual(b.get('aRxIndex')[:2], [0, 0x7010])
        self.assertEqual(b.get('aRxBitOffset')[:2], [0, 8])
        self.assertEqual([b.get('uiRxBits'), b.get('uiRxBytes'), b.get('uiTxBytes')], [9, 2, 1])
        self.assertFalse(b.get('xRxByteAligned'))
        self.assertFalse(b.get('xTxByteAligned'))

    def test_byte_rounding_and_alignment_at_maximum_supported_bit_width(self):
        for count in (1, 2, 7, 8, 9, 31, 32):
            entries = [mapping(0x7010, n, 255) for n in range(1, count + 1)]
            b = Probe(device(entries, entries)); b.start(); b.finish()
            bits = 255 * count
            self.assertTrue(b.get('xDone'))
            for direction in ('Rx', 'Tx'):
                self.assertEqual(b.get('ui' + direction + 'Bits'), bits)
                self.assertEqual(b.get('ui' + direction + 'Bytes'), (bits + 7) // 8)
                self.assertEqual(b.get('x' + direction + 'ByteAligned'), bits % 8 == 0)

    def test_rejects_over_capacity_on_either_side(self):
        for key in ((0x1601, 0), (0x1A00, 0)):
            data = device(); data[key] = (33, 1)
            b = Probe(data); b.start(); b.finish()
            self.assertEqual(b.get('uiError'), 4)
            self.assertFalse(b.get('xDone'))
            self.assertNotIn((key[0], 1), b.reader.requests)

    def test_requires_one_assigned_pdo_without_guessing(self):
        for key in ((0x1C12, 0), (0x1C13, 0)):
            for count in (0, 2):
                data = device(); data[key] = (count, 1)
                b = Probe(data); b.start(); b.finish()
                self.assertEqual(b.get('uiError'), 5)
                self.assertEqual(b.get('wErrorIndex'), key[0])

    def test_rejects_zero_pdo_index_and_zero_bit_length(self):
        for key, replacement in [((0x1C12, 1), (0, 2)), ((0x1C13, 1), (0, 2)),
                                 ((0x1601, 1), (mapping(0x7010, 1, 0), 4)),
                                 ((0x1A00, 1), (mapping(0x6000, 1, 0), 4))]:
            data = device(); data[key] = replacement
            b = Probe(data); b.start(); b.finish()
            self.assertEqual(b.get('uiError'), 6)
            self.assertEqual((b.get('wErrorIndex'), b.get('byErrorSubIndex')), key)

    def test_unconfigured_master_or_slave_sends_no_request(self):
        for field in ('usiMaster', 'uiSlaveAddress'):
            b = Probe(); b.set(field, 0); b.start()
            self.assertTrue(b.get('xError'))
            self.assertEqual(b.get('uiError'), 1)
            self.assertEqual(b.reader.requests, [])
            self.assertEqual(b.reader.calls, [])

    def test_native_error_is_exposed_and_not_retried(self):
        def error(key, occurrence):
            return (0, 0, 5751, 0x06020000) if key == (0x1601, 10) else None
        b = Probe(intercept=error); b.start(); b.finish()
        self.assertEqual(b.get('uiError'), 2)
        self.assertEqual(b.get('eLastReadError'), 5751)
        self.assertEqual(b.get('udiLastSdoAbortCode'), 0x06020000)
        self.assertTrue(b.get('xReadErrorSeen'))
        self.assertEqual((b.get('wErrorIndex'), b.get('byErrorSubIndex')), (0x1601, 10))
        count = len(b.reader.requests); b.scan(100)
        self.assertEqual(len(b.reader.requests), count)
        self.assertEqual(b.get('udiLastSdoAbortCode'), 0x06020000)

    def test_rejects_response_size_mismatch_for_all_sizes(self):
        for key, bad_length in [((0x1018, 1), 2), ((0x1C12, 0), 4), ((0x1C12, 1), 1)]:
            data = device(); data[key] = (data[key][0], bad_length)
            b = Probe(data); b.start(); b.finish()
            self.assertEqual(b.get('uiError'), 3)
            self.assertEqual(b.get('udiLastDataRead'), bad_length)
            self.assertFalse(b.get('xDone'))

    def test_fallback_timeout_stops_and_does_not_restart(self):
        b = Probe(intercept=lambda key, occurrence: 'hang'); b.start(); b.finish()
        self.assertEqual(b.get('uiError'), 10)
        b.scan(100)
        self.assertEqual(len(b.reader.requests), 1)
        self.assertFalse(b.reader.get('xbusy'))

    def test_abort_and_configuration_changes_stop_current_read(self):
        for field, value, code in [('xAbort', True, 8), ('usiMaster', 2, 7),
                                    ('uiSlaveAddress', 1002, 7)]:
            b = Probe(intercept=lambda key, occurrence: 'hang'); b.start(); b.scan(2)
            b.set(field, value); b.scan()
            self.assertEqual(b.get('uiError'), code)
            self.assertFalse(b.get('xBusy'))
            self.assertTrue(b.get('xError'))
            self.assertEqual(b.get('wErrorIndex'), 0x1018)
            self.assertFalse(b.reader.get('xbusy'))

    def test_detects_changed_metadata_or_mapping_during_read(self):
        for key, changed in [((0x1018, 1), 99), ((0x1C12, 0), 2),
                             ((0x1C13, 1), 0x1A02), ((0x1601, 0), 15),
                             ((0x1601, 12), mapping(0x7010, 12, 16)),
                             ((0x1A00, 16), mapping(0x6000, 16, 16))]:
            data = device()
            def change(read_key, occurrence):
                return (changed, data[key][1], 0) if read_key == key and occurrence == 2 else None
            b = Probe(data, change); b.start(); b.finish()
            self.assertEqual(b.get('uiError'), 9, key)
            self.assertFalse(b.get('xDone'))

    def test_held_start_and_pulses_while_busy_do_not_replay(self):
        b = Probe(); b.start(); b.scan(3)
        b.set('xStart', False); b.scan(); b.set('xStart', True); b.scan()
        b.finish(); count = len(b.reader.requests); b.scan(50)
        self.assertTrue(b.get('xDone'))
        self.assertEqual(len(b.reader.requests), count)
        self.assertEqual(b.reader.occurrences[(0x1018, 1)], 2)

    def test_new_trigger_clears_old_snapshot_and_error(self):
        b = Probe(); b.start(); b.finish()
        b.reader.intercept = lambda key, n: (0, 0, 5751)
        b.start()
        self.assertEqual(b.get('uiRxBytes'), 0)
        self.assertFalse(b.get('xDone'))
        self.assertEqual(b.get('aRxMap'), [0] * 32)
        b.finish(); self.assertEqual(b.get('uiError'), 2)
        b.reader.intercept = None
        b.start(); self.assertEqual(b.get('eLastReadError'), 0)
        self.assertEqual(b.get('udiLastSdoAbortCode'), 0)
        self.assertEqual(b.get('udiLastDataRead'), 0)
        b.finish(); self.assertTrue(b.get('xDone'))
        self.assertEqual(b.get('uiError'), 0)

    def test_only_read_operations_and_no_process_output_mapping(self):
        source = rt.base.clean((BASE / 'plc/PRG_WsmeProbe.st').read_text('utf-8'))
        self.assertEqual(re.findall(r':\s*(IoDrvEthercatLib\.\w+)\s*;', source),
                         ['IoDrvEthercatLib.ETC_CO_ERROR'])
        self.assertIn('fbRead : CmpHCCom.ETC_COE_SdoRead;', source)
        self.assertNotRegex(source, r'(?i)ETC_CO_SdoRead4|usiChannel|usiDataLength|fbRead\.abyData')
        self.assertNotRegex(source, r'(?i)SdoWrite|EEPROM|\bAT\s+%|GVL_|g_byWeldOut')
        self.assertNotRegex(source, r'(?i)\bDIV\b|\bMOD\b|\bIODrvEtherCAT\.')

    def test_paste_files_reconstruct_program_exactly(self):
        folder = BASE / 'plc/PRG_WsmeProbe'
        declaration = (folder / 'declaration.txt').read_text('utf-8')
        implementation = (folder / 'implementation.txt').read_text('utf-8')
        source = (BASE / 'plc/PRG_WsmeProbe.st').read_text('utf-8')
        self.assertEqual(source, declaration + '\n' + implementation + 'END_PROGRAM\n')


if __name__ == '__main__':
    unittest.main(verbosity=2)
