"""List assignments that sit outside any Method in iasl disassembly (.dsl) files.

  python3 -I find_module_level_stores.py *.dsl

macOS's ACPICA 20160930 skips such Stores ("Unsupported module-level executable opcode 0x70") or never runs them
in Device bodies, so the names keep their initial value under macOS. Reports file:line, the enclosing scope chain,
and the statement. Comments, strings and Method bodies are ignored.
"""
import re
import sys

OPEN = re.compile(r"^\s*(DefinitionBlock|Scope|Device|Processor|PowerResource|ThermalZone|Method|If|Else|ElseIf|While|"
                  r"Field|IndexField|BankField|Name|Package|Buffer|Switch|Case|Default|ResourceTemplate)\b(.*)$")
ASSIGN = re.compile(r"^\s*([\\^A-Z_][\\^A-Z0-9_.]*)\s*(=|\+\+|--|\+=|-=|\|=|&=)\s*(.*)$")


def scan(path):
    stack = []           # (kind, label)
    pending = None
    for n, raw in enumerate(open(path, encoding="utf-8", errors="replace"), 1):
        line = re.sub(r"//.*|/\*.*?\*/", "", raw)
        line = re.sub(r'"[^"]*"', '""', line)
        s = line.strip()
        if not s:
            continue
        m = OPEN.match(line)
        if m:
            pending = (m.group(1), (m.group(1) + m.group(2)).strip()[:60])
        in_method = any(k == "Method" for k, _ in stack)
        if not in_method and not m:
            a = ASSIGN.match(line)
            if a and s != "{" and s != "}":
                chain = " > ".join(l for k, l in stack if k in ("Scope", "Device", "Processor", "If", "Else"))
                print(f"{path}:{n}: [{chain}] {s}")
        for ch in line:
            if ch == "{":
                stack.append(pending if pending else ("?", ""))
                pending = None
            elif ch == "}":
                if stack:
                    stack.pop()


if __name__ == "__main__":
    for p in sys.argv[1:]:
        scan(p)
