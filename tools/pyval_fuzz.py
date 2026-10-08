#!/usr/bin/env python3
"""Generate fixtures for cpp/tests/test_pyval.cpp from the running CPython.

Each output line is: <hex of input bytes> TAB <expected>, where expected is
what the engine would observe for that bot reply line:
  E <repr(exception)>       decode or json.loads failed
  V <repr(value)>           parsed value
  S <json.dumps(value)>     extra line for str values (ensure_ascii dump)
"""
import json, random, sys

ATOMS = ['0', '1', '-1', '1.0', '-0', '-0.0', '1e5', '1E+2', '1e-7', '12345678901234567890',
         '123456789012345678901234567890', '-9223372036854775808', '9223372036854775807',
         '1.5', '0.1', '1e16', '1e22', '123456789.123456789', '2.5e-5', '0.0001', '1e400',
         'NaN', 'Infinity', '-Infinity', 'true', 'false', 'null', '""', '"a"', '"pass"',
         '"move"', '"\\u00e9"', '"\\ud83d\\ude00"', '"\\ud800"', '"\\udc00x"', '"it\'s"',
         '"say \\"hi\\""', '"both \' \\""', '"tab\\t nl\\n"', '"\\u0000\\u001f\\u007f"',
         '"\\u0085\\u00a0\\u2028\\u200b\\ue000"', '"日本"', '"🙂"', '"\\/"', '"\\b\\f\\r"']
BROKEN = ['{', '}', '[', ']', ',', ':', '"', '\\', '-', '+', '.', 'e', 'E', '0', '1', 'x',
          ' ', '\t', '\n', '\r', 'nul', 'tru', 'fals', 'Na', 'Infinit', '-Inf', '"\\u12',
          '"\\x"', '"\\u12g4"', '"a\nb"', '\x01', '01', '1.', '.5', '1e', '1e+', '-', '--1',
          '﻿', '\xff', '\xc3', '\xe2\x82', '\xf0\x9f\x99', '\xed\xa0\x80', '\xe0\x80\x80',
          '\xf4\x90\x80\x80', '\xc0\xaf', '\x80']

def rnd_value(r, depth=0):
    k = r.random()
    if depth > 3 or k < 0.4:
        return r.choice(ATOMS)
    if k < 0.7:
        items = [rnd_value(r, depth + 1) for _ in range(r.randint(0, 4))]
        return '[' + r.choice([',', ', ', ' ,']).join(items) + ']'
    keys = ['"command"', '"x"', '"y"', '"energy"', '"destination"', '"name"', '"a"', '"x"']
    items = [r.choice(keys) + r.choice([':', ': ', ' : ']) + rnd_value(r, depth + 1)
             for _ in range(r.randint(0, 4))]
    return '{' + r.choice([',', ', ']).join(items) + '}'

def mutate(r, s):
    s = list(s)
    for _ in range(r.randint(1, 3)):
        op = r.random()
        if op < 0.4 and s:
            del s[r.randrange(len(s))]
        elif op < 0.8:
            s.insert(r.randint(0, len(s)), r.choice(BROKEN))
        elif s:
            s[r.randrange(len(s))] = r.choice(BROKEN)
    return ''.join(s)

def to_bytes(s):
    # BROKEN contains raw bytes written as latin-1 code points (\xff...);
    # everything else is encoded as UTF-8.
    out = bytearray()
    for ch in s:
        if '\x80' <= ch <= '\xff':
            out.append(ord(ch))
        else:
            out += ch.encode('utf-8', 'surrogatepass')
    return bytes(out)

def expected(b):
    try:
        line = b.decode()
    except Exception as e:
        return ['E ' + repr(e)]
    try:
        v = json.loads(line)
    except Exception as e:
        return ['E ' + repr(e)]
    out = ['V ' + repr(v)]
    if isinstance(v, str):
        out.append('S ' + json.dumps(v))
    return out

def main():
    r = random.Random(int(sys.argv[1]) if len(sys.argv) > 1 else 1)
    n = int(sys.argv[2]) if len(sys.argv) > 2 else 20000
    seen = set()
    cases = [a for a in ATOMS] + [b for b in BROKEN] + ['', '\n', ' {} ', '{"a":1,"a":2,"b":3}',
             '[' * 50 + ']' * 50, '1' * 4301, '"' + 'x' * 10 + '\\']
    while len(cases) < n:
        v = rnd_value(r)
        cases.append(v if r.random() < 0.3 else mutate(r, v))
    for c in cases:
        b = to_bytes(c + ('\n' if r.random() < 0.8 else ''))
        if b in seen:
            continue
        seen.add(b)
        for e in expected(b):
            sys.stdout.write(b.hex() + '\t' + e.replace('\\', '\\\\').replace('\n', '\\n') + '\n')

if __name__ == '__main__':
    main()
