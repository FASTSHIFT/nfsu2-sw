"""Static x87 stack depth per function.

The lifter models the x87 stack as a ring g_fp_stack[8] indexed by a moving
g_fp_top. In most compiled code the depth at every instruction is fixed --
the same on every path -- so each st(i) names a fixed position relative to
the function's entry top, and the ring could live in C locals the compiler
keeps in registers.

For each function this computes, relative to its entry top:
  depth[insn]  values pushed (negative: caller values consumed) before insn
  delta        depth at every return (a float return leaves +1)
  reach        how many of the caller's positions (st(0).. at entry) the
               function reads, writes or pops: __ftol reads and pops st(0),
               so reach 1, delta -1
A function is static if depth agrees at every join and at every return, and
everything it calls is static too. Anything else -- indirect calls and jumps,
fsave/frstor and friends, depth over 8 -- makes it dynamic.
"""
import re

# Net pushes (+) and pops (-) by mnemonic.
_PUSH1 = frozenset({"fld", "fild", "fldz", "fld1", "fldpi", "fldl2e", "fldl2t",
                    "fldlg2", "fldln2", "fbld"})
_POP1 = frozenset({"fstp", "fistp", "fisttp", "fcomp", "fucomp", "faddp",
                   "fsubp", "fmulp", "fdivp", "fsubrp", "fdivrp", "fcomip",
                   "fucomip", "fcompi", "fucompi", "fpatan", "fyl2x",
                   "fyl2xp1", "fbstp"})
_POP2 = frozenset({"fcompp", "fucompp"})
_ZERO = frozenset({"fst", "fist", "fadd", "fsub", "fmul", "fdiv", "fsubr",
                   "fdivr", "fiadd", "fisub", "fisubr", "fimul", "fidiv",
                   "fidivr", "fchs", "fabs", "fsqrt", "fsin", "fcos", "f2xm1",
                   "frndint", "fscale", "fprem", "fprem1", "fxch", "fcom",
                   "fucom", "fcomi", "fucomi", "ftst", "fxam", "fnstsw",
                   "fstsw", "fnstcw", "fstcw", "fldcw", "fwait", "wait",
                   "fnclex", "fclex", "fnop"})
_PUSH_KEEP = frozenset({"fptan", "fsincos", "fxtract"})   # st0 rewritten, +1
# Forms that implicitly use st(1).
_USES_ST1 = frozenset({"fpatan", "fyl2x", "fyl2xp1", "fprem", "fprem1",
                       "fscale", "fcompp", "fucompp"})
_BINARY = frozenset({"fadd", "faddp", "fsub", "fsubp", "fsubr", "fsubrp",
                     "fmul", "fmulp", "fdiv", "fdivp", "fdivr", "fdivrp",
                     "fcom", "fcomp", "fucom", "fucomp", "fxch"})


def x87_effect(insn):
    """(net, max st index touched) for an x87 instruction, (None, None) for
    one this model cannot follow, or (0, -1) for a non-x87 instruction."""
    m = insn.mnemonic
    if not m.startswith("f") and m not in ("wait",):
        return 0, -1
    idx = -1
    for op in insn.operands:
        if op.type == "reg" and op.reg and op.reg.startswith("st"):
            mm = re.search(r"(\d+)", op.reg)
            idx = max(idx, int(mm.group(1)) if mm else 0)
    if m in _PUSH1:
        net = 1
    elif m in _POP1:
        net = -1
    elif m in _POP2:
        net = -2
    elif m in _ZERO:
        net = 0
    elif m in _PUSH_KEEP:
        net = 1
    else:
        return None, None
    if m in _USES_ST1:
        idx = max(idx, 1)
    elif m in _BINARY and not any(o.type == "mem" for o in insn.operands):
        idx = max(idx, 1)
    elif m in _PUSH1:
        pass   # reads only an explicit st(i) (fld st(i)), before the push
    elif m not in ("fnstsw", "fstsw", "fnstcw", "fstcw", "fldcw",
                   "fwait", "wait", "fnclex", "fclex", "fnop"):
        idx = max(idx, 0)
    return net, idx


class X87Summary:
    def __init__(self):
        self.info = {}    # addr -> (delta, reach)
        self.depths = {}  # addr -> {insn address: depth before}
        self.icalls = {}  # call-through-pointer address -> float results
        self.reasons = {}
        self.overridden = set()
        self.stats = {}

    def static(self, addr):
        return addr in self.info


def _is_kernel_import(insn):
    """`call dword ptr [thunk]` through the XBE's kernel thunk table."""
    from . import config
    ops = insn.operands
    if len(ops) != 1 or ops[0].type != "mem":
        return False
    op = ops[0]
    if op.mem_base or op.mem_index:
        return False
    base = getattr(config, "KERNEL_THUNK_ADDR", None)
    return base is not None and base <= (op.mem_disp & 0xFFFFFFFF) < base + 0x1000


def _result_used(f_blocks, addr, summaries, limit=64):
    """After a call through a pointer at whose return address `addr` the code
    continues: 1 if it shows the call left a float -- some path reads or pops
    a position below what it pushed itself since the call -- 0 if every path
    reaches an ordinary call first with nothing of its own pushed (the stack
    has to be empty there, so there was no float or it was already popped),
    None if that cannot be told or the paths disagree. A path that returns
    first casts no vote: it may be handing the float on to our caller, and
    the returns-agree check holds it to the other paths' answer."""
    seen = set()
    answer = set()
    work = [(addr, 0)]
    while work:
        s, k = work.pop()
        if (s, k) in seen:
            continue
        seen.add((s, k))
        if len(seen) > limit or s not in f_blocks:
            return None
        b = f_blocks[s]
        decided = False
        for insn in b.instructions:
            if insn.is_ret:
                decided = True
                break
            if insn.is_call:
                sm = summaries.get(insn.call_target)
                if insn.call_target is not None and sm is None:
                    return None
                if sm is not None and sm[1] > k:
                    answer.add(1)      # __ftol and the like take st(0)
                elif k == 0:
                    answer.add(0)
                else:
                    return None
                decided = True
                break
            net, idx = x87_effect(insn)
            if net is None:
                return None
            if idx >= k or k + net < 0:
                answer.add(1)
                decided = True
                break
            k += net
        if decided:
            continue
        last = b.last_insn
        if last is not None and last.is_jump and not b.successors:
            return None
        if not b.successors:
            return None
        work.extend((succ, k) for succ in b.successors)
    if len(answer) != 1:
        return None
    return answer.pop()


def _analyse(f_blocks, start, end, summaries, call_target_known, forced=None,
             tries=0):
    """(delta, reach, depths, icalls) or a reason string when f is dynamic.
    icalls: {call address: float results} for its calls through pointers.

    A call through a pointer whose result nothing after it shows (the paths
    all return first) is tried both ways; if exactly one makes the depths
    agree -- usually because another return path fixes the function's own
    delta -- that is the answer. Two consistent readings stay dynamic."""
    r = _analyse1(f_blocks, start, end, summaries, forced)
    if not (isinstance(r, tuple) and r and r[0] == "ambiguous"):
        return r
    site = r[1]
    if tries >= 3:
        return "indirect call"
    good = {}
    for choice in (0, 1):
        f2 = dict(forced or {})
        f2[site] = choice
        rr = _analyse(f_blocks, start, end, summaries, call_target_known, f2,
                      tries + 1)
        if not isinstance(rr, str):
            good[choice] = rr
    if len(good) == 1:
        return next(iter(good.values()))
    if not good:
        return "indirect call"
    # Both readings hold: the float, if any, goes straight back to our
    # callers, so ask them (call_target_known is the vote callback here).
    v = call_target_known() if callable(call_target_known) else None
    if v in good:
        return good[v]
    return "indirect call ambiguous"


def _analyse1(f_blocks, start, end, summaries, forced):
    icalls = {}
    depth_in = {start: 0}
    work = [start]
    depths = {}
    deltas = set()
    reach = 0
    while work:
        s = work.pop()
        b = f_blocks[s]
        d = depth_in[s]
        for insn in b.instructions:
            depths[insn.address] = d
            if insn.is_call:
                t = insn.call_target
                if t is None:
                    # Through a pointer: the standard convention, under which
                    # the x87 stack is empty at a call and holds at most the
                    # float result after it. Which of the two is told by what
                    # the code does next (_result_used). A kernel import
                    # never returns a float.
                    if d != 0:
                        return "indirect call at depth"
                    if _is_kernel_import(insn):
                        icalls[insn.address] = 0
                        continue
                    if forced and insn.address in forced:
                        r = forced[insn.address]
                    else:
                        r = _result_used(f_blocks, insn.end_address, summaries)
                    if r is None:
                        return ("ambiguous", insn.address)
                    icalls[insn.address] = r
                    d += r
                    continue
                sm = summaries.get(t)
                if sm is None:
                    return "call %08X" % t
                g_delta, g_reach = sm
                reach = max(reach, g_reach - d)
                d += g_delta
                continue
            net, idx = x87_effect(insn)
            if net is None:
                return "op " + insn.mnemonic
            if idx >= 0:
                reach = max(reach, idx + 1 - d)
            if net < 0:
                reach = max(reach, -(d + net))
            d += net
            if d > 8 or d < -8:
                return "depth"
        last = b.last_insn
        exits = []
        if last is not None:
            if last.is_ret:
                exits.append(("ret", None))
            elif last.is_jump:
                t = last.jump_target
                if t is not None and not (start <= t < end):
                    exits.append(("tail", t))
                elif t is None and not b.successors:
                    if not _is_kernel_import(last):
                        return "indirect jump"
                    exits.append(("ret", None))   # the kernel leaves x87 alone
            elif last.is_cond_jump:
                t = last.jump_target
                if t is not None and not (start <= t < end):
                    exits.append(("tail", t))
                if last.end_address >= end:
                    exits.append(("tail", last.end_address))
            elif not b.successors and last.mnemonic != "int3":
                if last.mnemonic in ("ud2", "hlt", "iret", "iretd"):
                    pass
                else:
                    exits.append(("tail", last.end_address))
        for kind, t in exits:
            if kind == "ret":
                deltas.add(d)
            else:
                sm = summaries.get(t)
                if sm is None:
                    return "tail %08X" % t
                reach = max(reach, sm[1] - d)
                deltas.add(d + sm[0])
        for succ in b.successors:
            if succ not in f_blocks:
                continue
            if succ not in depth_in:
                depth_in[succ] = d
                work.append(succ)
            elif depth_in[succ] != d:
                return "join"
    if len(deltas) > 1:
        return "returns disagree"
    delta = deltas.pop() if deltas else 0
    return delta, max(reach, 0), depths, icalls


def _sccs(nodes, edges):
    """Tarjan's strongly connected components, iteratively; returned in
    reverse topological order (a component after everything it reaches)."""
    index = {}
    low = {}
    on = set()
    stack = []
    out = []
    counter = 0
    for root in nodes:
        if root in index:
            continue
        work = [(root, iter(sorted(edges.get(root, ()))))]
        index[root] = low[root] = counter
        counter += 1
        stack.append(root)
        on.add(root)
        while work:
            v, it = work[-1]
            advanced = False
            for w in it:
                if w not in index:
                    index[w] = low[w] = counter
                    counter += 1
                    stack.append(w)
                    on.add(w)
                    work.append((w, iter(sorted(edges.get(w, ())))))
                    advanced = True
                    break
                elif w in on:
                    low[v] = min(low[v], index[w])
            if advanced:
                continue
            work.pop()
            if work:
                low[work[-1][0]] = min(low[work[-1][0]], low[v])
            if low[v] == index[v]:
                comp = []
                while True:
                    w = stack.pop()
                    on.discard(w)
                    comp.append(w)
                    if w == v:
                        break
                out.append(comp)
    return out


def compute(translator, func_list, verbose=False):
    import sys
    funcs = {}
    for n, (addr, info) in enumerate(func_list):
        if verbose and n % 4000 == 0:
            print(f"  [x87sum] decode {n}/{len(func_list)}", file=sys.stderr)
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
        bl = {b.start: b for b in blocks}
        if addr not in bl:
            continue
        funcs[addr] = (bl, translator.lifter.func_end)

    import os
    if os.environ.get("RECOMP_X87SUM_PICKLE"):
        import pickle
        with open(os.environ["RECOMP_X87SUM_PICKLE"], "wb") as fh:
            pickle.dump(funcs, fh)
    return compute_from(funcs, verbose)


def compute_from(funcs, verbose=False):
    import sys
    # Optimistic fixpoint, so recursion can be static: every function starts
    # as (delta 0, reach 0); a function the model cannot follow drops out,
    # and so does, in turn, everything that calls it. Iterate until nothing
    # changes; whatever still moves after the cap is made dynamic.
    # Callees before callers (strongly connected components of the direct
    # call graph, in reverse topological order), so a function is analysed
    # once with its callees final; only recursion needs the optimistic
    # iteration.
    edges = {}
    for a, (bl, end) in funcs.items():
        out = set()
        for b in bl.values():
            for insn in b.instructions:
                t = insn.call_target if insn.is_call else (
                    insn.jump_target if (insn.is_branch and insn.jump_target
                                         and not (a <= insn.jump_target < end))
                    else None)
                if t is not None and t in funcs:
                    out.add(t)
            last = b.last_insn
            if (last is not None and not b.successors
                    and not last.is_ret and not last.is_jump
                    and last.end_address in funcs):
                out.add(last.end_address)
        edges[a] = out
    sccs = _sccs(funcs, edges)
    callers = {}
    for a, (bl, end) in funcs.items():
        for b in bl.values():
            for insn in b.instructions:
                if insn.is_call and insn.call_target in funcs:
                    callers.setdefault(insn.call_target, []).append(
                        (a, insn.end_address))
    defaulted = []

    def vote(a):
        """Whether a's callers read st(0) after calling it: 1, 0, or None if
        they disagree. No caller says either way: 0, counted."""
        votes = set()
        for c, ret in callers.get(a, ()):
            v = _result_used(funcs[c][0], ret, summaries)
            if v is not None:
                votes.add(v)
        if len(votes) == 1:
            return votes.pop()
        if not votes:
            defaulted.append(a)
            return 0
        return None

    summaries = {}
    depths = {}
    icall_results = {}
    reasons = {}
    rounds = 0
    # Hand-given summaries (RECOMP_X87_OVERRIDE="addr:delta:reach,..."), for
    # CRT routines the model cannot follow -- _CIpow saves the FPU state on
    # its error path -- but whose x87 interface is fixed: _CIpow takes x and
    # y in st(1)/st(0) and leaves x**y, so delta -1, reach 2. The routine
    # itself stays dynamic; its callers can be static around it.
    import os
    for item in os.environ.get("RECOMP_X87_OVERRIDE", "").split(","):
        if item.strip():
            a, dl, rc = item.split(":")
            summaries[int(a, 16)] = (int(dl), int(rc))
    overridden = set(summaries)

    for comp in sccs:
        comp = [a for a in comp if a not in overridden]
        if not comp:
            continue
        recursive = len(comp) > 1 or comp[0] in edges[comp[0]]
        if not recursive:
            a = comp[0]
            bl, end = funcs[a]
            r = _analyse(bl, a, end, summaries, (lambda a=a: vote(a)))
            rounds += 1
            if isinstance(r, str):
                reasons[a] = r
            else:
                summaries[a] = (r[0], r[1])
                depths[a] = r[2]
                icall_results[a] = r[3]
            continue
        if len(comp) == 1:
            # Plain self-recursion: guess the float-returning shapes too
            # (a recursive call whose result the caller then adds to).
            a = comp[0]
            bl, end = funcs[a]
            for guess in ((0, 0), (1, 0), (-1, 1), (0, 1)):
                summaries[a] = guess
                r = None
                for _ in range(6):
                    rounds += 1
                    r = _analyse(bl, a, end, summaries, (lambda a=a: vote(a)))
                    if isinstance(r, str) or (r[0], r[1]) == summaries[a]:
                        break
                    summaries[a] = (r[0], r[1])
                if not isinstance(r, str) and (r[0], r[1]) == summaries[a]:
                    depths[a] = r[2]
                    icall_results[a] = r[3]
                    reasons.pop(a, None)
                    break
                reasons[a] = r if isinstance(r, str) else "no fixpoint"
                summaries.pop(a, None)
            continue
        alive = set(comp)
        for a in alive:
            summaries[a] = (0, 0)
        for _ in range(30):
            rounds += 1
            changed = False
            for a in sorted(alive):
                bl, end = funcs[a]
                r = _analyse(bl, a, end, summaries, (lambda a=a: vote(a)))
                if isinstance(r, str):
                    reasons[a] = r
                    alive.discard(a)
                    summaries.pop(a, None)
                    changed = True
                    continue
                depths[a] = r[2]
                icall_results[a] = r[3]
                if (r[0], r[1]) != summaries[a]:
                    summaries[a] = (r[0], r[1])
                    changed = True
            if not changed:
                break
        else:
            for a in alive:
                reasons[a] = "no fixpoint"
                summaries.pop(a, None)
            alive = set()
    for a in reasons:
        depths.pop(a, None)
    res = X87Summary()
    res.info = summaries
    res.overridden = overridden
    res.depths = depths
    res.icalls = {}
    for a in summaries:
        res.icalls.update(icall_results.get(a, {}))
    res.reasons = reasons
    import collections
    kinds = collections.Counter(v.split()[0] for v in reasons.values())
    res.stats = {"functions": len(funcs), "static": len(summaries), "defaulted": len(set(defaulted) & set(summaries)),
                 "rounds": rounds, "dynamic": dict(kinds)}
    if verbose:
        print(f"  [x87sum] {res.stats}", file=sys.stderr)
    return res
