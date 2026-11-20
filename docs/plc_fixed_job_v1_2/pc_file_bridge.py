"""PC -> Modbus TCP -> AC802 RAM -> PLC FTP -> STEP cabinet.

No dependencies. No robot motion or program-load command. Default is local plan.
Wire format is specified in REGISTER_MAP.md; addresses are ZERO-BASED offsets.
"""
from __future__ import annotations
import argparse
import datetime as dt
import hashlib
import json
from pathlib import Path, PurePosixPath
import re
import secrets
import socket
import struct
import sys
import time
import zipfile
import zlib

COMMAND, HEARTBEAT, STATUS = 6000, 6120, 6200
MAX_FILE, MAX_TOTAL, MAX_FILES, CHUNK = 1048576, 5242880, 32, 192
NAME = re.compile(r'[a-z0-9_]{1,44}\.(?:srp|srd|txt)\Z')
PROJECT = re.compile(r'pcjob_[a-z0-9_]{3,39}\.sr\Z')


def u32(words, index):
    return (words[index] << 16) | words[index + 1]


def put32(words, index, value):
    if not 0 <= value <= 0xFFFFFFFF:
        raise ValueError('32-bit integer out of range')
    words[index:index + 2] = [value >> 16, value & 0xFFFF]


def frame(op, seq, session, boot, *, size=0, offset=0, payload=b'',
          crc=0, count=0, ordinal=0, mode=0):
    if not 0 < seq <= 0xFFFFFFFF or not session or len(payload) > CHUNK:
        raise ValueError('invalid frame sequence/session/payload')
    words = [0] * 120
    words[:3] = [0xF7B1, 1, op]
    for index, value in [(3, seq), (5, session), (7, boot), (9, size),
                         (11, offset), (14, crc)]:
        put32(words, index, value)
    words[13], words[16], words[17], words[18] = len(payload), count, ordinal, mode
    padded = payload.ljust(CHUNK, b'\0')
    words[20:116] = struct.unpack('>96H', padded)
    put32(words, 118, zlib.crc32(struct.pack('>118H', *words[:118])))
    return words


class Modbus:
    def __init__(self, host, port=502, unit=1, timeout=3):
        self.sock = socket.create_connection((host, port), timeout)
        self.sock.settimeout(timeout)
        self.sock.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
        self.unit, self.tid = unit, 0

    def close(self):
        self.sock.close()

    def exact(self, size):
        data = bytearray()
        while len(data) < size:
            part = self.sock.recv(size - len(data))
            if not part:
                raise ConnectionError('PLC closed Modbus connection; no automatic resume')
            data.extend(part)
        return bytes(data)

    def request(self, pdu):
        self.tid = (self.tid + 1) & 0xFFFF
        self.sock.sendall(struct.pack('>HHHB', self.tid, 0, len(pdu) + 1, self.unit) + pdu)
        tid, protocol, length, unit = struct.unpack('>HHHB', self.exact(7))
        if (tid, protocol, unit) != (self.tid, 0, self.unit) or not 2 <= length <= 254:
            raise ConnectionError('invalid Modbus response header')
        result = self.exact(length - 1)
        if result[0] == (pdu[0] | 0x80):
            raise RuntimeError(f'Modbus exception FC{pdu[0]:02X}: {result[1:].hex()}')
        if result[0] != pdu[0]:
            raise ConnectionError('unexpected Modbus function')
        return result

    def read(self, address, count):
        if not 1 <= count <= 125:
            raise ValueError('FC03 register count')
        result = self.request(struct.pack('>BHH', 3, address, count))
        if len(result) != 2 + count * 2 or result[1] != count * 2:
            raise ConnectionError('invalid FC03 response length')
        return list(struct.unpack(f'>{count}H', result[2:]))

    def write(self, address, words):
        if not 1 <= len(words) <= 123:
            raise ValueError('FC16 register count')
        pdu = struct.pack('>BHHB', 16, address, len(words), 2 * len(words))
        result = self.request(pdu + struct.pack(f'>{len(words)}H', *words))
        if result != struct.pack('>BHH', 16, address, len(words)):
            raise ConnectionError('invalid FC16 response echo')


def read_status(client):
    for _ in range(10):
        w = client.read(STATUS, 64)
        if w[:2] != [0xF7B2, 1]:
            raise RuntimeError('HR6200/6201 signature mismatch: file bridge is not running here')
        if w[38] == w[63] and not w[38] & 1:
            return {
                'state': w[2], 'error': w[3], 'ack': u32(w, 4), 'ack_op': w[6],
                'flags': w[7], 'session': u32(w, 8), 'boot': u32(w, 10),
                'received': u32(w, 12), 'size': u32(w, 14), 'crc32': u32(w, 16),
                'expected_crc32': u32(w, 18), 'verified': w[20], 'expected': w[21],
                'ordinal': w[22], 'ftp_step': w[23], 'ftp_reply': w[24],
                'ftp_error': w[25], 'ftp_error_stage': w[26], 'sent': u32(w, 27),
                'readback': u32(w, 29), 'reject': w[31], 'reject_seq': u32(w, 32),
                'reject_session': u32(w, 34), 'max_files': w[37], 'max_file': u32(w, 39),
                'chunk': w[41], 'data_close_error': w[42], 'control_close_error': w[43],
                'mode': w[44], 'upload_completion': w[45], 'readback_completion': w[46],
                'capabilities': w[47], 'revision': w[48],
            }
    raise RuntimeError('status snapshot changed repeatedly; check PLC task cycle and mapping')


def validate_files(files, native):
    if not 1 <= len(files) <= MAX_FILES:
        raise ValueError(f'require 1..{MAX_FILES} files')
    total = 0
    for name, data in files.items():
        if not NAME.fullmatch(name) or len(name) > 48:
            raise ValueError(f'only flat lowercase ASCII file names are accepted: {name!r}')
        if native and not name.endswith(('.srp', '.srd')):
            raise ValueError(f'unexpected native project file: {name}')
        if not 0 <= len(data) <= MAX_FILE:
            raise ValueError(f'{name}: file must be 0..{MAX_FILE} bytes')
        total += len(data)
    if total > MAX_TOTAL:
        raise ValueError('bundle exceeds 5 MiB')
    if native:
        programs = [n for n in files if n.endswith('.srp')]
        if '_project.srd' not in files or not programs:
            raise ValueError('native bundle requires _project.srd and at least one .srp')
        for name in programs:
            if name[:-1] + 'd' not in files:
                raise ValueError(f'missing paired .srd: {name}')
    return dict(sorted(files.items()))


def load_files(args):
    files = {}
    native = args.file is None
    if args.file:
        path = Path(args.file)
        if path.is_symlink() or not path.is_file() or not 0 <= path.stat().st_size <= MAX_FILE:
            raise ValueError('test file must be a regular file of 0 bytes..1 MiB')
        if path.suffix != '.txt':
            raise ValueError('--file is for .txt transport tests; use --project-dir/--zip for native programs')
        files[path.name] = path.read_bytes()
    elif args.project_dir:
        root = Path(args.project_dir)
        if not root.is_dir() or root.is_symlink():
            raise ValueError('project directory is not a regular directory')
        for path in root.iterdir():
            if len(files) >= MAX_FILES:
                raise ValueError('too many files')
            if path.is_symlink() or not path.is_file() or not 0 <= path.stat().st_size <= MAX_FILE:
                raise ValueError(f'nested, linked or oversized file: {path.name}')
            files[path.name] = path.read_bytes()
    elif args.zip:
        with zipfile.ZipFile(args.zip) as archive:
            entries = archive.infolist()
            if len(entries) > MAX_FILES + 1:
                raise ValueError('too many ZIP entries')
            roots = set()
            for entry in entries:
                name = entry.filename
                if '\\' in name or name.startswith('/'):
                    raise ValueError(f'unsafe ZIP name: {name!r}')
                path = PurePosixPath(name)
                if any(p in ('.', '..') for p in name.split('/') if p) or ':' in name:
                    raise ValueError(f'unsafe ZIP name: {name!r}')
                if entry.is_dir():
                    if len(path.parts) != 1 or not path.name.endswith('.sr'):
                        raise ValueError('unexpected ZIP directory')
                    roots.add(path.name)
                    continue
                if len(path.parts) != 2 or not path.parts[0].endswith('.sr'):
                    raise ValueError('ZIP must contain one .sr root with flat files')
                if (entry.external_attr >> 16) & 0o170000 == 0o120000:
                    raise ValueError('ZIP symlinks are forbidden')
                if entry.flag_bits & 1 or not 0 <= entry.file_size <= MAX_FILE:
                    raise ValueError('encrypted or oversized ZIP entry')
                roots.add(path.parts[0])
                if path.name in files:
                    raise ValueError('duplicate ZIP filename')
                files[path.name] = archive.read(entry)
            if len(roots) != 1:
                raise ValueError('ZIP must contain exactly one .sr root')
    else:
        raise ValueError('select --file, --project-dir, or --zip')
    return validate_files(files, native), native


class Uploader:
    def __init__(self, client, project, *, wait_seconds=43200, report=print):
        self.client, self.project, self.report = client, project, report
        self.first = read_status(client)
        if self.first['state'] not in (0, 400) or self.first['flags'] & 3 != 3:
            raise RuntimeError('PLC must be idle/complete with local permit TRUE and sockets closed')
        if (self.first['max_file'], self.first['chunk'], self.first['max_files']) != (MAX_FILE, CHUNK, MAX_FILES):
            raise RuntimeError('PLC capacity/protocol does not match this uploader')
        self.boot = self.first['boot']
        self.session = secrets.randbelow(0xFFFFFFFF) + 1
        self.seq, self.hb, self.last_hb = 0, 0, -1e30
        self.wait_seconds = wait_seconds

    def heartbeat(self):
        now = time.monotonic()
        if now - self.last_hb >= .5:
            words = [0] * 5
            put32(words, 0, self.session)
            put32(words, 2, self.boot)
            self.hb = (self.hb + 1) & 0xFFFF
            words[4] = self.hb
            self.client.write(HEARTBEAT, words)
            self.last_hb = now

    def status(self):
        self.heartbeat()
        status = read_status(self.client)
        if status['boot'] != self.boot:
            raise RuntimeError('PLC restarted; transfer invalidated, no automatic retry')
        if status['session'] != self.session:
            raise RuntimeError('PLC transfer ownership changed')
        return status

    def command(self, op, **kwargs):
        self.seq += 1
        words = frame(op, self.seq, self.session, self.boot, **kwargs)
        self.heartbeat()
        self.client.write(COMMAND, words)
        deadline = time.monotonic() + 8
        while time.monotonic() < deadline:
            self.heartbeat()
            status = read_status(self.client)
            if status['boot'] != self.boot:
                raise RuntimeError('PLC restarted before ACK; no automatic retry')
            if (status['reject_session'], status['reject_seq']) == (self.session, self.seq) and status['reject']:
                raise RuntimeError(f'PLC rejected command: 0x{status["reject"]:04X}; {status}')
            if (status['session'], status['ack'], status['ack_op']) == (self.session, self.seq, op):
                if status['state'] in (900, 910):
                    raise RuntimeError(f'PLC transfer failed: {status}')
                return status
            time.sleep(.05)
        raise TimeoutError('PLC command ACK timed out; no automatic retry/resume')

    def upload(self, files, native):
        files = validate_files(files, native)
        if any(len(data) == 0 for data in files.values()) and not self.first['capabilities'] & 1:
            raise RuntimeError('PLC does not advertise zero-length file support (HR6247 bit0). '
                               'Update PRG_FileBridge and FB_FileFtp to V1.1 first. '
                               'No transfer command or heartbeat was written.')
        receipts = []
        for ordinal, (name, data) in enumerate(files.items(), 1):
            crc = zlib.crc32(data)
            self.report(f'[{ordinal}/{len(files)}] {name}: {len(data)} bytes -> PLC')
            status = self.command(1, size=len(data), payload=(self.project + '\0' + name + '\0').encode('ascii'),
                         crc=crc, count=len(files), ordinal=ordinal, mode=int(native))
            for offset in range(0, len(data), CHUNK):
                part = data[offset:offset + CHUNK]
                status = self.command(2, offset=offset, payload=part)
                if status['received'] != offset + len(part):
                    raise RuntimeError('PLC acknowledged unexpected file offset')
            if status['crc32'] != crc or status['size'] != len(data):
                raise RuntimeError('PLC RAM size/CRC mismatch; FTP will not start')
            self.command(3)
            deadline, last_report = time.monotonic() + self.wait_seconds, -1e30
            while True:
                status = self.status()
                if status['state'] == 200 and status['verified'] == ordinal:
                    if status['flags'] & 6 != 6 or status['sent'] != len(data) or status['readback'] != len(data):
                        raise RuntimeError('PLC verification/cleanup report incomplete')
                    break
                if status['state'] in (900, 910):
                    raise RuntimeError(f'PLC FTP failure: {status}')
                if time.monotonic() >= deadline:
                    raise TimeoutError('FTP wait expired; bridge lease will abort after heartbeat stops')
                if time.monotonic() - last_report >= 5:
                    self.report(f'  FTP step={status["ftp_step"]}, sent={status["sent"]}/{len(data)}, '
                                f'readback={status["readback"]}/{len(data)}, reply={status["ftp_reply"]}')
                    last_report = time.monotonic()
                time.sleep(.15)
            self.report('  Uploaded, byte-exact readback verified, sockets closed.')
            receipts.append({'name': name, 'bytes': len(data), 'crc32': f'{crc:08x}',
                             'local_sha256': hashlib.sha256(data).hexdigest(), 'plc_status': status})
        final = self.command(5)
        if final['state'] != 400 or not final['flags'] & 8:
            raise RuntimeError('PLC did not confirm complete bundle')
        return {'protocol': 1, 'completed_at': dt.datetime.now().astimezone().isoformat(),
                'implementation': '1.1', 'project': self.project, 'remote_directory': '/UserPrograms/' + self.project,
                'session': self.session, 'plc_boot': self.boot, 'native_bundle': native,
                'files': receipts, 'final_status': final,
                'robot_loaded': False, 'robot_started': False}


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--host', default='192.168.1.188')
    parser.add_argument('--port', type=int, default=502)
    parser.add_argument('--unit', type=int, choices=range(1, 248), default=1)
    source = parser.add_mutually_exclusive_group()
    source.add_argument('--file')
    source.add_argument('--project-dir')
    source.add_argument('--zip')
    parser.add_argument('--probe', action='store_true', help='read-only status check')
    parser.add_argument('--upload', action='store_true', help='actually write to PLC and cabinet')
    parser.add_argument('--project', help='new unique pcjob_*.sr destination')
    parser.add_argument('--ftp-wait-seconds', type=float, default=43200)
    parser.add_argument('--receipt', help='new JSON file path; never contains FTP credentials')
    args = parser.parse_args(argv)
    client = None
    try:
        if args.probe:
            if args.upload:
                raise ValueError('--probe cannot be combined with --upload')
            client = Modbus(args.host, args.port, args.unit)
            print(json.dumps(read_status(client), ensure_ascii=False, indent=2))
            return 0
        files, native = load_files(args)
        project = args.project or ('pcjob_' + dt.datetime.now().strftime('%Y%m%d_%H%M%S_') + secrets.token_hex(3) + '.sr')
        if not PROJECT.fullmatch(project) or not 12 <= len(project) <= 48:
            raise ValueError('destination must be a new lowercase pcjob_*.sr name, 12..48 ASCII characters')
        print(json.dumps({'destination': '/UserPrograms/' + project, 'native_bundle': native,
                          'files': [{'name': n, 'bytes': len(d)} for n, d in files.items()],
                          'mode': 'UPLOAD' if args.upload else 'LOCAL PLAN ONLY'}, ensure_ascii=False, indent=2))
        if not args.upload:
            return 0
        receipt_path = Path(args.receipt or f'bridge_receipt_{project[:-3]}.json').resolve()
        # Reserve a writable receipt before touching PLC. Existing files are never replaced.
        with receipt_path.open('x', encoding='utf-8') as output:
            pending = {'status': 'PENDING', 'project': project, 'host': args.host}
            json.dump(pending, output, ensure_ascii=False, indent=2)
            output.flush()
            try:
                client = Modbus(args.host, args.port, args.unit)
                receipt = Uploader(client, project, wait_seconds=args.ftp_wait_seconds).upload(files, native)
                receipt['status'] = 'COMPLETE'
            except (Exception, KeyboardInterrupt) as error:
                output.seek(0); output.truncate()
                json.dump({**pending, 'status': 'INCOMPLETE', 'error': str(error)}, output, ensure_ascii=False, indent=2)
                raise
            output.seek(0); output.truncate()
            json.dump(receipt, output, ensure_ascii=False, indent=2)
        print(f'COMPLETE: {receipt_path}\nFiles verified only. Load/run on cabinet remains manual.')
        return 0
    except (Exception, KeyboardInterrupt) as error:
        print(f'STOPPED: {error}\nNo automatic retry. Stop heartbeat, inspect PLC status/partial directory.', file=sys.stderr)
        return 1
    finally:
        if client is not None:
            client.close()


if __name__ == '__main__':
    raise SystemExit(main())
