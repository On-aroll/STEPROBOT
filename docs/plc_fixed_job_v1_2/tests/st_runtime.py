"""Strict offline executor for THIS delivered ST subset, not an IEC compiler.

Reuses the existing V2 parser with explicit support for typed CASE labels,
strings, pointers, arrays and function blocks. Socket I/O is simulated.
Unknown syntax/identifiers/functions fail. No connection to real PLC/robot.
"""
import importlib.util
from pathlib import Path
import operator
import re

ROOT = Path(__file__).parents[1]
OLD = Path(__file__).parent / 'st_parser_base.py'
spec = importlib.util.spec_from_file_location('old_st_runtime', OLD)
base = importlib.util.module_from_spec(spec)
spec.loader.exec_module(base)
source = OLD.read_text(encoding='utf-8')
start, end = source.index('class Parser:'), source.index('\n\nclass Timer:')
parser_source = source[start:end]
parser_source = parser_source.replace('"OR": 1, "AND": 2', '"OR": 1, "XOR": 1, "AND": 2')
parser_source = parser_source.replace('"*": 5}', '"*": 5, "DIV": 5, "MOD": 5}')
parser_source = parser_source.replace('token[0].isdigit() or "#" in token', 'token[0].isdigit() or token.startswith("\'") or "#" in token')
parser_source = parser_source.replace('self.peek().isdigit() and self.tokens[self.index + 1] == ":"',
                                    '(self.peek()[0].isdigit() or "#" in self.peek()) and self.tokens[self.index + 1] in (":", ",")')
parser_source = parser_source.replace('number = literal(self.pop()); self.pop(":")\n                    branches[number] = self.sequence(("ELSE", "END_CASE"), case_labels=True)',
    '''numbers = [literal(self.pop())]
                    while self.peek() == ",":
                        self.pop(","); numbers.append(literal(self.pop()))
                    self.pop(":")
                    block = self.sequence(("ELSE", "END_CASE"), case_labels=True)
                    for number in numbers: branches[number] = block''')
TOKEN = re.compile(r"\s*(T#\d+(?:ms|s|m)|(?:BYTE|WORD|UINT|UDINT|DINT)#(?:16#[0-9A-Fa-f]+|\d+)"
                   r"|'(?:[^']|'')*'|[A-Za-z_]\w*(?:\.[A-Za-z_]\w*)*|\d+|:=|<>|<=|>=|[()\[\],;:+\-*=<>])", re.I)


def literal(token):
    if token.startswith("'"):
        return token[1:-1].replace("''", "'").replace('$R', '\r').replace('$N', '\n').replace('$$', '$')
    return base.literal(token)


exec(parser_source, globals())


class Pointer:
    def __init__(self, env, name):
        self.env, self.name = env, name

    def __getitem__(self, index):
        value = self.env.get(self.name, raw=True)
        if isinstance(value, str):
            return ord(value[index]) if index < len(value) else 0
        if not 0 <= index < len(value):
            raise IndexError((self.name, index, len(value)))
        return value[index]

    def __setitem__(self, index, value):
        target = self.env.get(self.name, raw=True)
        if isinstance(target, str):
            target = target.ljust(index + 1, '\0')
            self.env.set(self.name, target[:index] + chr(value) + target[index + 1:])
        else:
            if not 0 <= index < len(target):
                raise IndexError((self.name, index, len(target)))
            target[index] = value


class Native:
    def __init__(self, kind, factory):
        self.kind, self.factory = kind, factory
        self.values = dict(xdone=False, xerror=False, werrorid=0, uicount=0, xbusy=False)

    def get(self, name, raw=False):
        return self.values[name]

    def call(self, args, dt):
        self.factory(self, args)


class Timer:
    def __init__(self):
        self.timer = base.Timer()

    def get(self, name, raw=False):
        return getattr(self.timer, name.upper())

    def call(self, args, dt):
        self.timer.call(args['in'], args['pt'], dt)


class World:
    def __init__(self, native_factory=None, ftp_factory=None):
        self.globals, self.sources, self.functions = {}, {}, {}
        self.native_factory, self.ftp_factory = native_factory, ftp_factory
        for path in ROOT.glob('*.st'):
            if path.name.startswith('GVL_'):
                self.globals[path.stem.lower()] = Instance(self, path, namespace=True)
            else:
                text = path.read_text(encoding='utf-8-sig')
                self.sources[path.stem.lower()] = path
                if text.startswith('FUNCTION '):
                    self.functions[path.stem.lower()] = path

    def instance(self, name):
        if name == 'fb_fileftp' and self.ftp_factory:
            return self.ftp_factory()
        return Instance(self, self.sources[name])


class Instance:
    def __init__(self, world, path, namespace=False):
        self.world, self.values, self.kinds, self.inputs = world, {}, {}, []
        text = path.read_text(encoding='utf-8-sig')
        text = re.sub(r"\{attribute[^}]*\}", '', text)
        text = base.clean(text)
        header = re.search(r'^(FUNCTION_BLOCK|FUNCTION|PROGRAM)\s+(\w+)(?:\s*:\s*(\w+))?', text.strip(), re.I)
        self.name = header[2].lower() if header else path.stem.lower()
        self.result_type = header[3] if header else None
        sections = list(re.finditer(r'\b(VAR_GLOBAL(?:\s+PERSISTENT\s+RETAIN)?|VAR_INPUT|VAR_OUTPUT|VAR\s+CONSTANT|VAR)\s*\n(.*?)\bEND_VAR\b', text, re.S | re.I))
        for section in sections:
            for line in section[2].splitlines():
                line = line.strip()
                if not line:
                    continue
                match = re.fullmatch(r'(\w+)(?:\s+AT\s+%\w+)?\s*:\s*(.*?)(?:\s*:=\s*(.*?))?;', line)
                if not match:
                    raise SyntaxError(('declaration', path.name, line))
                name, kind, init = match.groups()
                name = name.lower()
                array = re.fullmatch(r'ARRAY\[0\.\.(\d+)\] OF (.*)', kind, re.I)
                if array:
                    value = ['' if array[2].startswith('STRING') else 0 for _ in range(int(array[1]) + 1)]
                elif kind == 'TON':
                    value = Timer()
                elif kind.startswith('CmpHCTCPIP.'):
                    if kind.endswith('ADDRESS'):
                        value = Address()
                        if init:
                            value.values = {'stripaddr': re.search(r"strIPAddr := '([^']+)'", init)[1],
                                            'uiport': literal(re.search(r'uiPort := (UINT#\d+)', init)[1])}
                    else:
                        value = Native(kind.split('.')[-1], world.native_factory)
                elif kind.lower() in world.sources or kind == 'FB_FileFtp':
                    value = world.instance(kind.lower())
                else:
                    value = literal(init) if init else ('' if kind.startswith('STRING') else False if kind == 'BOOL' else 0)
                self.values[name], self.kinds[name] = value, kind
                if section[1] == 'VAR_INPUT':
                    self.inputs.append(name)
        if self.result_type:
            self.values[self.name] = 0
        body = '' if namespace else text[sections[-1].end():]
        body = re.sub(r'END_(?:PROGRAM|FUNCTION_BLOCK|FUNCTION)\s*$', '', body.strip())
        parser = Parser(body)
        self.tree = parser.sequence()
        if parser.peek() != 'EOF':
            raise SyntaxError(('unparsed', path.name, parser.peek()))

    def get(self, name, raw=False):
        name = name.lower()
        if '.' in name:
            root, tail = name.split('.', 1)
            obj = self.values.get(root, self.world.globals.get(root))
            if obj is None:
                raise KeyError(name)
            return obj.get(tail, raw=raw)
        value = self.values[name]
        return value if raw or not isinstance(value, str) else value.split('\0', 1)[0]

    def set(self, name, value):
        if '.' in name:
            root, tail = name.split('.', 1)
            obj = self.values.get(root, self.world.globals.get(root))
            obj.set(tail, value)
            return
        if name not in self.values:
            raise KeyError(name)
        kind = self.kinds.get(name, self.result_type)
        if kind in ('BYTE', 'WORD', 'UINT', 'UDINT'):
            maximum = {'BYTE': 255, 'WORD': 65535, 'UINT': 65535, 'UDINT': 0xFFFFFFFF}[kind]
            if not 0 <= value <= maximum:
                raise OverflowError((name, kind, value))
        self.values[name] = value

    def refget(self, node):
        value = self.get(node[1])
        if node[2] is None:
            return value
        index = self.evaluate(node[2])
        if isinstance(value, list) and not 0 <= index < len(value):
            raise IndexError((node[1], index))
        result = value[index]
        return result.split('\0', 1)[0] if isinstance(result, str) else result

    def refset(self, node, value):
        if node[2] is None:
            return self.set(node[1], value)
        array, index = self.get(node[1]), self.evaluate(node[2])
        if isinstance(array, list) and not 0 <= index < len(array):
            raise IndexError((node[1], index))
        array[index] = value

    def evaluate(self, node):
        kind = node[0]
        if kind == 'value': return node[1]
        if kind == 'var': return self.refget(node)
        if kind == 'unary':
            value = self.evaluate(node[2])
            return (not value if isinstance(value, bool) else ~value) if node[1] == 'NOT' else -value
        if kind == 'binary':
            fn = {'OR': operator.or_, 'XOR': operator.xor, 'AND': operator.and_, '=': operator.eq,
                  '<>': operator.ne, '<': operator.lt, '>': operator.gt, '<=': operator.le,
                  '>=': operator.ge, '+': operator.add, '-': operator.sub, '*': operator.mul,
                  'DIV': operator.floordiv, 'MOD': operator.mod}[node[1]]
            return fn(self.evaluate(node[2]), self.evaluate(node[3]))
        if kind == 'function':
            name = node[1].lower()
            if name == 'adr':
                assert node[2][0][0] == 'var' and node[2][0][2] is None
                return Pointer(self, node[2][0][1])
            args = [self.evaluate(arg) for arg in node[2]]
            if name.endswith('_to_word') or name.endswith('_to_uint'): return int(args[0]) & 0xFFFF
            if name.endswith('_to_byte'): return int(args[0]) & 0xFF
            if name.endswith('_to_udint'): return int(args[0]) & 0xFFFFFFFF
            if name == 'shl': return (args[0] << args[1]) & 0xFFFFFFFF
            if name == 'shr': return args[0] >> args[1]
            if name == 'len': return len(args[0])
            if name == 'left': return args[0][:args[1]]
            if name == 'right': return args[0][-args[1]:] if args[1] else ''
            if name == 'concat': return args[0] + args[1]
            if name in self.world.functions:
                inst = self.world.instance(name)
                inst.call(dict(zip(inst.inputs, args)), 0)
                return inst.get(name)
            raise ValueError(('unsupported ST function', name))
        raise ValueError(node)

    def execute(self, block, dt):
        for statement in block:
            kind = statement[0]
            if kind == 'assign': self.refset(statement[1], self.evaluate(statement[2]))
            elif kind == 'timer':
                args = {key.lower(): self.evaluate(value) for key, value in statement[2].items()}
                self.get(statement[1]).call(args, dt)
            elif kind == 'if':
                for condition, body in statement[1]:
                    if self.evaluate(condition):
                        self.execute(body, dt); break
                else: self.execute(statement[2], dt)
            elif kind == 'for':
                for value in range(self.evaluate(statement[2]), self.evaluate(statement[3]) + 1):
                    self.refset(statement[1], value); self.execute(statement[4], dt)
            elif kind == 'case':
                self.execute(statement[2].get(self.evaluate(statement[1]), statement[3]), dt)
            else: raise ValueError(statement)

    def call(self, args, dt=10):
        for name, value in args.items(): self.set(name, value)
        self.execute(self.tree, dt)


class Address:
    def __init__(self): self.values = {'stripaddr': '', 'uiport': 0}
    def get(self, name, raw=False): return self.values[name]
    def set(self, name, value): self.values[name] = value
