"""Small strict interpreter for the ST subset used by PRG_RobotBridge.st.

Executes the delivered source itself, not a separate copy of its control logic.
Virtual timers/inputs only. This is NOT an IEC compiler or an AC802 runtime.
Unsupported syntax or an undeclared identifier fails rather than being ignored.
"""
import operator
import re
from pathlib import Path

ROOT = Path(__file__).parents[1]
TOKEN = re.compile(r"\s*(T#\d+(?:ms|s|m)|(?:WORD|UINT|UDINT)#(?:16#[0-9A-Fa-f]+|\d+)"
                   r"|[A-Za-z_]\w*(?:\.[A-Za-z_]\w*)*|\d+|:=|<>|<=|>=|[()\[\],;:+\-*=<>])", re.I)


def clean(text):
    return re.sub(r"//[^\n]*|\(\*.*?\*\)", "", text, flags=re.S)


def literal(token):
    if token.upper() in ("TRUE", "FALSE"):
        return token.upper() == "TRUE"
    if token.upper().startswith("T#"):
        number, unit = re.fullmatch(r"T#(\d+)(ms|s|m)", token, re.I).groups()
        return int(number) * {"ms": 1, "s": 1000, "m": 60000}[unit.lower()]
    if "#" in token:
        value = token.split("#", 1)[1]
        return int(value[3:], 16) if value.upper().startswith("16#") else int(value)
    return int(token)


class Parser:
    precedence = {"OR": 1, "AND": 2, "=": 3, "<>": 3, "<": 3,
                  ">": 3, "<=": 3, ">=": 3, "+": 4, "-": 4, "*": 5}

    def __init__(self, text):
        self.tokens, position = [], 0
        while position < len(text):
            if not text[position:].strip():
                break
            match = TOKEN.match(text, position)
            if not match:
                raise SyntaxError(text[position:position + 100])
            self.tokens.append(match.group(1))
            position = match.end()
        self.index = 0

    def peek(self):
        return self.tokens[self.index] if self.index < len(self.tokens) else "EOF"

    def pop(self, expected=None):
        value = self.peek()
        if expected and value.upper() != expected:
            raise SyntaxError(f"expected {expected}, got {value}; tokens {self.tokens[self.index:self.index+15]}")
        self.index += 1
        return value

    def reference(self, name):
        index = None
        if self.peek() == "[":
            self.pop("["); index = self.expression(); self.pop("]")
        return ("var", name.lower(), index)

    def expression(self, minimum=0):
        token = self.pop()
        if token.upper() in ("NOT", "-"):
            left = ("unary", token.upper(), self.expression(6))
        elif token == "(":
            left = self.expression(); self.pop(")")
        elif token[0].isdigit() or "#" in token or token.upper() in ("TRUE", "FALSE"):
            left = ("value", literal(token))
        elif self.peek() == "(":
            self.pop("("); args = []
            if self.peek() != ")":
                args.append(self.expression())
                while self.peek() == ",":
                    self.pop(","); args.append(self.expression())
            self.pop(")"); left = ("function", token.upper(), args)
        else:
            left = self.reference(token)
        while self.peek().upper() in self.precedence and self.precedence[self.peek().upper()] >= minimum:
            op = self.pop().upper()
            left = ("binary", op, left, self.expression(self.precedence[op] + 1))
        return left

    def sequence(self, ends=(), case_labels=False):
        result = []
        while self.peek().upper() not in ends and self.peek() != "EOF":
            if case_labels and self.peek().isdigit() and self.tokens[self.index + 1] == ":":
                break
            kind = self.peek().upper()
            if kind == "IF":
                branches = []
                self.pop("IF"); condition = self.expression(); self.pop("THEN")
                branches.append((condition, self.sequence(("ELSIF", "ELSE", "END_IF"))))
                while self.peek().upper() == "ELSIF":
                    self.pop("ELSIF"); condition = self.expression(); self.pop("THEN")
                    branches.append((condition, self.sequence(("ELSIF", "ELSE", "END_IF"))))
                otherwise = []
                if self.peek().upper() == "ELSE":
                    self.pop("ELSE"); otherwise = self.sequence(("END_IF",))
                self.pop("END_IF"); self.pop(";")
                result.append(("if", branches, otherwise))
            elif kind == "FOR":
                self.pop("FOR"); ref = self.reference(self.pop()); self.pop(":=")
                start = self.expression(); self.pop("TO"); stop = self.expression(); self.pop("DO")
                body = self.sequence(("END_FOR",)); self.pop("END_FOR"); self.pop(";")
                result.append(("for", ref, start, stop, body))
            elif kind == "CASE":
                self.pop("CASE"); value = self.expression(); self.pop("OF")
                branches = {}
                while self.peek().upper() not in ("ELSE", "END_CASE"):
                    number = literal(self.pop()); self.pop(":")
                    branches[number] = self.sequence(("ELSE", "END_CASE"), case_labels=True)
                otherwise = []
                if self.peek().upper() == "ELSE":
                    self.pop("ELSE"); otherwise = self.sequence(("END_CASE",))
                self.pop("END_CASE"); self.pop(";")
                result.append(("case", value, branches, otherwise))
            else:
                name = self.pop()
                if self.peek() == "(":
                    self.pop("("); arguments = {}
                    while self.peek() != ")":
                        key = self.pop().upper(); self.pop(":=")
                        arguments[key] = self.expression()
                        if self.peek() != ",":
                            break
                        self.pop(",")
                    self.pop(")"); self.pop(";")
                    result.append(("timer", name.lower(), arguments))
                else:
                    ref = self.reference(name); self.pop(":=")
                    value = self.expression(); self.pop(";")
                    result.append(("assign", ref, value))
        return result


class Timer:
    def __init__(self):
        self.Q, self.ET, self.last = False, 0, False

    def call(self, enabled, preset, dt):
        self.ET = (self.ET + dt if self.last else 0) if enabled else 0
        self.last = bool(enabled)
        self.Q = bool(enabled and self.ET >= preset)


class ScanHarness:
    def __init__(self):
        self.values = {}
        self.kinds = {}
        self.time = 0
        for namespace, filename in [("gvl_robotbridge", "GVL_RobotBridge.st"),
                                     ("gvl_robotbridgeretain", "GVL_RobotBridgeRetain.st"),
                                     ("", "PRG_RobotBridge.st")]:
            source = clean((ROOT / filename).read_text(encoding="utf-8"))
            declaration = source if namespace else source.split("END_VAR", 1)[0]
            pattern = r"^\s*(\w+)(?:\s+AT\s+%\w+)?\s*:\s*(ARRAY\s*\[(\d+)\.\.(\d+)\]\s+OF\s+WORD|\w+)(?:\s*:=\s*([^;]+))?\s*;"
            for match in re.finditer(pattern, declaration, re.M | re.I):
                name, kind, lo, hi, init = match.groups()
                path = ((namespace + ".") if namespace else "") + name.lower()
                if lo is not None:
                    if int(lo) != 0:
                        raise ValueError("test harness arrays must start at zero")
                    value = [0] * (int(hi) + 1)
                elif kind.upper() == "TON":
                    value = Timer()
                else:
                    value = literal(init.strip()) if init else (False if kind.upper() == "BOOL" else 0)
                self.values[path], self.kinds[path] = value, kind.upper()
            if not namespace:
                body = source.split("END_VAR", 1)[1].rsplit("END_PROGRAM", 1)[0]
                parser = Parser(body)
                self.tree = parser.sequence()
                if parser.peek() != "EOF":
                    raise SyntaxError("unparsed ST source")

    def get(self, name):
        name = name.lower()
        if name in self.values:
            return self.values[name]
        root, attribute = name.rsplit(".", 1)
        return getattr(self.values[root], attribute.upper())

    def refget(self, ref):
        value = self.get(ref[1])
        return value if ref[2] is None else value[self.evaluate(ref[2])]

    def refset(self, ref, value):
        kind = self.kinds[ref[1]]
        if "WORD" in kind or kind == "UINT":
            if not 0 <= value <= 65535:
                raise OverflowError((ref, value))
        if kind == "UDINT" and not 0 <= value <= 0xFFFFFFFF:
            raise OverflowError((ref, value))
        if ref[2] is not None:
            self.values[ref[1]][self.evaluate(ref[2])] = value
        else:
            self.values[ref[1]] = value

    def evaluate(self, node):
        kind = node[0]
        if kind == "value": return node[1]
        if kind == "var": return self.refget(node)
        if kind == "unary":
            value = self.evaluate(node[2])
            return (not value) if node[1] == "NOT" else -value
        if kind == "binary":
            fn = {"OR": operator.or_, "AND": operator.and_, "=": operator.eq,
                  "<>": operator.ne, "<": operator.lt, ">": operator.gt,
                  "<=": operator.le, ">=": operator.ge, "+": operator.add,
                  "-": operator.sub, "*": operator.mul}[node[1]]
            return fn(self.evaluate(node[2]), self.evaluate(node[3]))
        if kind == "function":
            name, args = node[1], [self.evaluate(x) for x in node[2]]
            if name.endswith("_TO_WORD"): return args[0] & 0xFFFF
            if name in ("WORD_TO_UINT", "WORD_TO_UDINT"): return int(args[0])
            if name == "SHL": return args[0] << args[1]
            if name == "SHR": return args[0] >> args[1]
            raise ValueError("unsupported ST function " + name)
        raise ValueError(node)

    def execute(self, block, dt):
        for statement in block:
            kind = statement[0]
            if kind == "assign": self.refset(statement[1], self.evaluate(statement[2]))
            elif kind == "timer":
                args = {key: self.evaluate(value) for key, value in statement[2].items()}
                self.get(statement[1]).call(args["IN"], args["PT"], dt)
            elif kind == "if":
                for condition, body in statement[1]:
                    if self.evaluate(condition):
                        self.execute(body, dt); break
                else: self.execute(statement[2], dt)
            elif kind == "for":
                for value in range(self.evaluate(statement[2]), self.evaluate(statement[3]) + 1):
                    self.refset(statement[1], value); self.execute(statement[4], dt)
            elif kind == "case":
                self.execute(statement[2].get(self.evaluate(statement[1]), statement[3]), dt)
            else: raise ValueError(statement)

    def scan(self, *, robot_base=0x0090, robot_heartbeat=True, host_heartbeat=True, dt=10):
        rx = self.get("gvl_robotbridge.arobotrx")
        rx[0] = robot_base | (0x0200 if robot_heartbeat and (self.time // 1000) % 2 else 0)
        cmd = self.get("gvl_robotbridge.ahostcmd")
        if host_heartbeat and self.get("gvl_robotbridge.xhostregistered") and self.time % 250 == 0:
            cmd[15] = (cmd[15] + 1) & 0xFFFF
        self.execute(self.tree, dt)
        self.time += dt

    def scans(self, milliseconds, **kwargs):
        for _ in range(milliseconds // 10): self.scan(**kwargs)

    def frame(self, command, sequence, parameter=0, owner=0x12345678, boot=None):
        boot = self.get("gvl_robotbridgeretain.udibootcounter") if boot is None else boot
        cmd = self.get("gvl_robotbridge.ahostcmd")
        hb = cmd[15]
        cmd[:] = [2, command, sequence >> 16, sequence & 0xFFFF, parameter, 0,
                  owner >> 16, owner & 0xFFFF, boot >> 16, boot & 0xFFFF, 0, 0, 0, 0, 0, hb]

    def ready(self):
        self.values["gvl_robotbridge.xcommissioned"] = True
        self.values["gvl_robotbridge.xgdiprogrammappingconfirmed"] = True
        self.values["gvl_robotbridge.xremotepermit"] = True
        self.scans(2200)
        self.frame(1, 1); self.scans(60)
        assert self.get("gvl_robotbridge.xhostregistered")
