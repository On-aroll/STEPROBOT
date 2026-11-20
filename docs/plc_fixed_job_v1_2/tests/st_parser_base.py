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
