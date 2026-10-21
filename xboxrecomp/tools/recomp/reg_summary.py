"""Whole-program register summaries for call-site sync elision.

_localize_registers keeps a function's guest registers in C locals and, at
every call, stores all of them back to the thread-local globals before and
reloads all of them after: the callee loads them on entry and stores them on
return. That is 2 x 7 TLS accesses per call site, and most of them move a
value nobody looks at -- a callee that never reads esi does not need the
caller's esi stored, and one that never changes it does not need it reloaded.

This pass computes, for each translated function f and each register R of
eax/ecx/edx/ebx/esi/edi (esp is always synchronised):

  IN(f)   R's value at entry may be observed: f reads R before writing it, or
          calls something that does, or may return with R holding its entry
          value while also being able to change it (the caller reloads R
          after the call then, so the entry value must have been real).
  OUT(f)  R may differ at return: f writes R, or calls something that does,
          and does not restore it from a prologue save.

A caller then stores only IN(callee) and reloads only OUT(callee) around a
direct call to a translated function. If f neither reads nor changes R, the
stale value f's own entry load and return store move around is never seen:
the caller keeps R in its local and does not reload it.

Everything not understood is "all registers": indirect calls and jumps,
manual or wrapped functions, the SEH and setjmp helpers, untranslated
targets, pushad/popad, interrupts and anything capstone cannot describe.

Three fixpoints, all monotone from the bottom:
  MUSTDEF(f)  registers fully written on every path to every return (least
              fixpoint of a must-analysis, so an under-approximation);
  IN/OUT      may-analyses over the call graph with MUSTDEF fixed.
"""
from capstone import Cs, CS_ARCH_X86, CS_MODE_32

REGS = ("eax", "ecx", "edx", "ebx", "esi", "edi")
ALL = frozenset(REGS)
NONE = frozenset()
CALLEE_SAVED = ("ebx", "esi", "edi")

_ALIAS = {}
for _r, _subs in (("eax", ("al", "ah", "ax")), ("ecx", ("cl", "ch", "cx")),
                  ("edx", ("dl", "dh", "dx")), ("ebx", ("bl", "bh", "bx")),
                  ("esi", ("si",)), ("edi", ("di",))):
    _ALIAS[_r] = (_r, True)
    for _s in _subs:
        _ALIAS[_s] = (_r, False)

# Instructions whose effect on the registers is not described by their
# operands, or that hand control to something else: treated as reading and
# writing every register.
_DANGER = frozenset({
    "int", "int1", "int3", "into", "ud2", "hlt", "iret", "iretd", "sysenter",
    "syscall", "pushal", "popal", "pushad", "popad", "in", "out", "insb",
    "insw", "insd", "outsb", "outsw", "outsd", "cpuid", "rdmsr", "wrmsr",
    "rdtsc", "rdpmc", "xgetbv", "lcall", "ljmp", "retf", "enter",
})

# Writes that may not happen: the destination keeps its old value on some
# executions, so the write is also a read of it.
_COND_WRITE_PREFIX = ("cmov", "cmpxchg")


class _Effects:
    __slots__ = ("reads", "defs", "writes", "danger")

    def __init__(self, reads, defs, writes, danger=False):
        self.reads = reads
        self.defs = defs
        self.writes = writes
        self.danger = danger


class RegSummary:
    """IN/OUT per function address, plus the statistics of the analysis."""

    def __init__(self):
        self.ins = {}
        self.outs = {}
        self.mustdef = {}
        self.saved = {}
        self.stats = {}

    def known(self, addr):
        return addr in self.ins

    def call_sets(self, addr):
        """(spill, reload) register sets for a direct call to addr, or None
        when the call must keep the full synchronisation."""
        if addr not in self.ins:
            return None
        return self.ins[addr], self.outs[addr]


def _effects(cs, insn, cache):
    eff = cache.get(insn.address)
    if eff is not None:
        return eff
    m = insn.mnemonic
    base = m.split()[-1] if m else m
    if base in _DANGER or m.startswith("lock int"):
        eff = _Effects(ALL, NONE, ALL, danger=True)
        cache[insn.address] = eff
        return eff
    try:
        ci = next(cs.disasm(bytes.fromhex(insn.bytes_hex), insn.address, 1))
        rd, wr = ci.regs_access()
        rd = [ci.reg_name(r) for r in rd]
        wr = [ci.reg_name(r) for r in wr]
    except Exception:
        eff = _Effects(ALL, NONE, ALL, danger=True)
        cache[insn.address] = eff
        return eff
    reads, defs, writes = set(), set(), set()
    for op in insn.operands:
        if op.type == "mem":
            for r in (op.mem_base, op.mem_index):
                if r in _ALIAS:
                    reads.add(_ALIAS[r][0])
    for name in rd:
        if name in _ALIAS:
            reads.add(_ALIAS[name][0])
    cond = base.startswith(_COND_WRITE_PREFIX)
    for name in wr:
        if name not in _ALIAS:
            continue
        r, full = _ALIAS[name]
        writes.add(r)
        if full and not cond:
            defs.add(r)
        else:
            reads.add(r)
    # xor r,r / sub r,r / sbb r,r: the result does not depend on r.
    ops = insn.operands
    if (base in ("xor", "sub", "sbb", "pxor") and len(ops) == 2
            and ops[0].type == "reg" and ops[1].type == "reg"
            and ops[0].reg == ops[1].reg and ops[0].reg in _ALIAS
            and _ALIAS[ops[0].reg][1]):
        reads.discard(ops[0].reg)
    eff = _Effects(frozenset(reads), frozenset(defs), frozenset(writes))
    cache[insn.address] = eff
    return eff


class _Func:
    """Per-function facts, independent of the other functions' summaries."""
    __slots__ = ("addr", "blocks", "order", "preds", "saved", "writes",
                 "danger", "events", "local_in", "out_all", "callees",
                 "exit_u", "calls")


def _exits_of(block, func, start, end, known_tail):
    """Exit edges of a block: list of (kind, target) where kind is "ret",
    "tail" (target is the function continued into) or "unknown"."""
    last = block.last_insn
    out = []
    if last is None:
        return out
    if last.is_ret:
        out.append(("ret", None))
    elif last.is_jump:
        t = last.jump_target
        if t is not None:
            if not (start <= t < end):
                out.append(("tail", t))
        elif not block.successors:
            out.append(("unknown", None))
    elif last.is_cond_jump:
        t = last.jump_target
        if t is not None and not (start <= t < end):
            out.append(("tail", t))
        if last.end_address >= end:
            out.append(("tail", last.end_address))
    elif not block.successors:
        if last.mnemonic in ("ud2", "hlt", "iret", "iretd", "int3"):
            pass  # handled as a dangerous instruction
        else:
            out.append(("tail", last.end_address))
    return out


def _prologue_saves(func, start, end):
    """Callee-saved registers f pushes in its entry block (before any call or
    write of them) and pops again before every exit, with nothing writing them
    between that pop and the exit."""
    entry = func.blocks.get(start)
    if entry is None:
        return NONE
    cand = []
    written = set()
    for insn in entry.instructions:
        if insn.is_call:
            break
        ops = insn.operands
        if (insn.mnemonic == "push" and len(ops) == 1 and ops[0].type == "reg"
                and ops[0].reg in CALLEE_SAVED and ops[0].reg not in written
                and ops[0].reg not in cand):
            cand.append(ops[0].reg)
            continue
        for r in insn.regs_written:
            if r in _ALIAS:
                written.add(_ALIAS[r][0])
    if not cand:
        return NONE
    saved = set(cand)
    any_exit = False
    for b in func.blocks.values():
        if not _exits_of(b, func, start, end, None) and not any(
                i.mnemonic in _DANGER for i in b.instructions):
            continue
        any_exit = True
        for r in list(saved):
            ok = False
            for insn in b.instructions:
                ops = insn.operands
                if (insn.mnemonic == "pop" and len(ops) == 1
                        and ops[0].type == "reg" and ops[0].reg == r):
                    ok = True
                elif ok and any(_ALIAS.get(w, (None,))[0] == r
                                for w in insn.regs_written):
                    ok = False
            if not ok:
                saved.discard(r)
    if not any_exit:
        return NONE
    return frozenset(saved)


def compute(translator, func_list, unknown=(), verbose=False):
    """Summaries for the functions in func_list ((addr, func_info) pairs).

    unknown: addresses whose callers must keep the full synchronisation
    (manual and wrapped functions, the SEH and setjmp helpers)."""
    import sys
    cs = Cs(CS_ARCH_X86, CS_MODE_32)
    cs.detail = True
    unknown = set(unknown)
    lifter = translator.lifter
    for a in (getattr(lifter, "SEH_HELPERS", ()) or ()):
        unknown.add(a)
    for a in (getattr(lifter, "SETJMP_FN", None),
              getattr(lifter, "LONGJMP_FN", None)):
        if a is not None:
            unknown.add(a)

    funcs = {}
    eff_cache = {}
    for n, (addr, info) in enumerate(func_list):
        if addr in unknown:
            continue
        if verbose and n % 2000 == 0:
            print(f"  [regsum] decode {n}/{len(func_list)}", file=sys.stderr)
        recovered = getattr(translator, "_recovered_cfg", {}).get(addr)
        end = recovered["end"] if recovered else info.get("end")
        if not end:
            end = addr + info.get("size", 0)
        if end <= addr:
            continue
        try:
            _, blocks = translator.decode_function(addr, end)
        except Exception:
            continue
        if not blocks:
            continue
        end = translator.lifter.func_end
        f = _Func()
        f.addr = addr
        f.blocks = {b.start: b for b in blocks}
        if addr not in f.blocks:
            continue
        f.saved = _prologue_saves(f, addr, end)
        f.events = {}
        for b in blocks:
            f.events[b.start] = _exits_of(b, f, addr, end, None)
        f.preds = {b.start: [] for b in blocks}
        for b in blocks:
            for s in b.successors:
                if s in f.preds:
                    f.preds[s].append(b.start)
        funcs[addr] = (f, end)

    def target_known(t):
        return t is not None and t in funcs

    # ---- Phase 1: MUSTDEF (least fixpoint of a must-analysis). ----
    mustdef = {a: NONE for a in funcs}

    def run_defs(f):
        """Forward must-def over f's CFG with the current MUSTDEF. Returns
        (D_in per block, D at each exit with its kind/target)."""
        din = {f.addr: NONE}
        work = [f.addr]
        seen_top = set()
        while work:
            s = work.pop()
            b = f.blocks[s]
            d = set(din[s])
            for insn in b.instructions:
                if insn.is_call:
                    t = insn.call_target
                    if target_known(t):
                        d |= mustdef[t]
                    continue
                e = _effects(cs, insn, eff_cache)
                if e.danger:
                    continue
                ops = insn.operands
                if (insn.mnemonic == "pop" and len(ops) == 1
                        and ops[0].type == "reg"
                        and ops[0].reg in f.saved):
                    d.discard(ops[0].reg)
                    continue
                d |= e.defs
            fd = frozenset(d)
            for succ in b.successors:
                if succ not in f.blocks:
                    continue
                if succ not in din:
                    din[succ] = fd
                    work.append(succ)
                else:
                    nd = din[succ] & fd
                    if nd != din[succ]:
                        din[succ] = nd
                        work.append(succ)
        return din

    changed = True
    rounds = 0
    dins = {}
    while changed:
        changed = False
        rounds += 1
        for a, (f, end) in funcs.items():
            din = run_defs(f)
            dins[a] = din
            md = None
            for s, b in f.blocks.items():
                if s not in din:
                    continue
                exits = f.events[s]
                if not exits and not any(
                        _effects(cs, i, eff_cache).danger
                        for i in b.instructions if not i.is_call):
                    continue
                # D at the end of the block.
                d = set(din[s])
                for insn in b.instructions:
                    if insn.is_call:
                        t = insn.call_target
                        if target_known(t):
                            d |= mustdef[t]
                        continue
                    e = _effects(cs, insn, eff_cache)
                    if e.danger:
                        continue
                    ops = insn.operands
                    if (insn.mnemonic == "pop" and len(ops) == 1
                            and ops[0].type == "reg"
                            and ops[0].reg in f.saved):
                        d.discard(ops[0].reg)
                        continue
                    d |= e.defs
                for kind, t in exits or (("unknown", None),):
                    if kind == "ret":
                        x = frozenset(d)
                    elif kind == "tail" and target_known(t):
                        x = frozenset(d) | mustdef[t]
                    else:
                        x = NONE
                    md = x if md is None else (md & x)
            md = (md or NONE) - f.saved
            if md != mustdef[a]:
                mustdef[a] = md
                changed = True
        if rounds > 50:
            break

    # ---- Per-function events with MUSTDEF fixed. ----
    for a, (f, end) in funcs.items():
        din = run_defs(f)
        local_in = set()
        writes = set()
        out_all = False
        calls = []     # (target, U) : U = registers undefined at the call
        exit_u = []    # U at each return-like exit
        for s, b in f.blocks.items():
            if s not in din:
                continue
            d = set(din[s])
            entry_block = (s == f.addr)
            pre_call = True
            exempted = set()
            for insn in b.instructions:
                if insn.is_call:
                    pre_call = False
                    t = insn.call_target
                    u = ALL - d
                    if target_known(t):
                        calls.append((t, frozenset(u)))
                        d |= mustdef[t]
                    else:
                        # Indirect, untranslated, manual or helper.
                        local_in |= u
                        out_all = True
                        # The call's own operand registers.
                        e = _effects(cs, insn, eff_cache)
                        if not e.danger:
                            local_in |= (e.reads & u)
                    continue
                e = _effects(cs, insn, eff_cache)
                if e.danger:
                    local_in |= (ALL - d)
                    out_all = True
                    continue
                ops = insn.operands
                is_save_push = (
                    entry_block and pre_call and insn.mnemonic == "push"
                    and len(ops) == 1 and ops[0].type == "reg"
                    and ops[0].reg in f.saved and ops[0].reg not in d
                    and ops[0].reg not in exempted)
                if is_save_push:
                    # Only the first push is the save; a later one is an
                    # argument that happens to live in the register.
                    exempted.add(ops[0].reg)
                else:
                    local_in |= (e.reads - d)
                if (insn.mnemonic == "pop" and len(ops) == 1
                        and ops[0].type == "reg" and ops[0].reg in f.saved):
                    d.discard(ops[0].reg)
                    continue
                writes |= e.writes
                d |= e.defs
            for kind, t in f.events[s]:
                u = ALL - d
                if kind == "ret":
                    exit_u.append(frozenset(u))
                elif kind == "tail" and target_known(t):
                    calls.append((t, frozenset(u)))
                    exit_u.append(frozenset(u - mustdef[t]))
                else:
                    local_in |= u
                    out_all = True
        f.local_in = frozenset(local_in)
        f.writes = frozenset(writes)
        f.out_all = out_all
        f.calls = calls
        f.exit_u = exit_u

    # ---- Phase 2: IN/OUT (least fixpoint of may-analyses). ----
    ins = {a: NONE for a in funcs}
    outs = {a: NONE for a in funcs}
    changed = True
    rounds2 = 0
    while changed:
        changed = False
        rounds2 += 1
        for a, (f, end) in funcs.items():
            o = set(f.writes)
            if f.out_all:
                o = set(ALL)
            for t, u in f.calls:
                o |= outs[t]
            o = frozenset(o) - f.saved
            i = set(f.local_in)
            for t, u in f.calls:
                i |= (ins[t] & u)
            for u in f.exit_u:
                i |= (o & u)
            i = frozenset(i)
            if i != ins[a] or o != outs[a]:
                ins[a] = i
                outs[a] = o
                changed = True

    res = RegSummary()
    res.ins = ins
    res.outs = outs
    res.mustdef = mustdef
    res.saved = {a: f.saved for a, (f, _) in funcs.items()}
    n = len(funcs) or 1
    res.stats = {
        "functions": len(funcs),
        "mustdef_rounds": rounds,
        "inout_rounds": rounds2,
        "avg_in": sum(len(v) for v in ins.values()) / n,
        "avg_out": sum(len(v) for v in outs.values()) / n,
        "in_all": sum(1 for v in ins.values() if v == ALL),
        "out_all": sum(1 for v in outs.values() if v == ALL),
        "per_reg_in": {r: sum(1 for v in ins.values() if r in v) for r in REGS},
        "per_reg_out": {r: sum(1 for v in outs.values() if r in v) for r in REGS},
    }
    if verbose:
        print(f"  [regsum] {res.stats}", file=sys.stderr)
    import os
    dump = os.environ.get("RECOMP_REGSUM_DUMP")
    if dump:
        import json
        srt = lambda s: [r for r in REGS if r in s]
        with open(dump, "w") as fh:
            json.dump({f"{a:08X}": {
                "in": srt(ins[a]), "out": srt(outs[a]),
                "saved": srt(f.saved), "mustdef": srt(mustdef[a]),
                "local_in": srt(f.local_in), "writes": srt(f.writes),
                "out_all": f.out_all,
                "calls": [f"{t:08X}" for t, _ in f.calls],
            } for a, (f, _) in funcs.items()}, fh)
    return res
