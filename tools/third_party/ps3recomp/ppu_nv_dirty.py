#!/usr/bin/env python3
"""ppu_nv_dirty.py -- write a lifted function's register locals back to the
register file only where they have changed, and only where it matters.

Used by ppu_lifter.py's --nonvolatile-locals pass (PPULifter.
_nonvolatile_locals_pass) and its emitter, not run by itself.

With --nonvolatile-locals a lifted function keeps the guest registers it
names in C locals, loads them from the register file (`ctx->gpr[N]` ...) at
entry, and before every transfer of control -- a call, a return, a tail
jump through the trampoline -- writes ALL of them back, because the code on
the other side reads the file. A function that names thirty registers
stores thirty words before each of its calls, and Clang cannot remove one
of them: the callee is opaque and the file is memory. In call-heavy code
those stores, and the host stack traffic that feeds them, are most of what
the processor does.

1. What changed (rewrite)

Most of those stores write a value the file already holds. A local differs
from the file only if the function has assigned it since the two last
agreed, and they agree at entry (the local was loaded from the file), after
a call for every register the function reloads (the volatile ones, the
floating-point and vector ones, CR/XER/CTR), and after a call for r14-r31
too, which every lifted callee leaves in the file as it found them. So the
pass follows, along every path through the function, the set of locals
that MAY have been assigned since the last such point -- a forward
may-dirty dataflow over the function's own control flow -- and each
transfer writes back that set and nothing else.

What counts as an assignment is decided from the statement's text, which is
the lifter's own and comes in few shapes: a register followed by an
assignment operator or under ++/--, or with its address taken (except as
the source of a memcpy, which is how stfd reads one). Anything else that
mentions a register reads it. A vector register is always handled by
address; it is read where it is a later argument of one of the prelude's
vector helpers or the source of a memcpy, or cast to a pointer to const,
and assigned anywhere else.

A function is left to the plain write-everything form when its text is not
of a shape this pass knows: a transfer inside an expression or a loop
statement, a call followed by anything but its trampoline drain or a
return, a goto to a label it cannot find.

2. What matters (summaries, solve, expand; PPU_NV_LIVE)

Of the registers that did change, many are dead at the transfer: r0, r11
and r12 hold a prologue's or a call sequence's scratch, an argument
register holds a temporary no callee reads, CTR held a loop count. Writing
one back is needed only if someone may read it before writing it: the
callee, or -- the callee might leave it alone -- the caller's own code
after the call, or whoever the function returns to. That is liveness, and
across calls it is a property of the whole program, so it is computed, not
taken from the ABI:

  * each function is summarised (in the lifting worker): for the registers
    this applies to (CAND: r0, r3-r12, f0-f13, v0-v19, CTR and, with the
    flags apart, cr0, cr1, cr5-cr7 and XER's carry), which of them are read
    before being written from the entry and from the point after each call,
    and which transfers can be reached from there with the register not yet
    overwritten (a call is taken to pass every register through);
  * solve() then finds the least fixed point over all functions of
        LiveIn(F)   read on entry by F, or by what F transfers to
        LiveOut(F)  read after F returns, by any caller's code
    with the code outside the lift held to the ABI: an HLE import, a
    native hook and an indirect call's target may read any argument
    register (r3-r10, f1-f13, v2-v13), a syscall those and its number in
    r11; native callers read r3, r4, f1-f4 and v2 of what a function
    returns. An
    indirect JUMP (bctr: a switch's default arm) goes where the function
    registry sends it, which besides a function's start can be an entry
    the lifter made in the middle of the jumping function, so it reads
    what any of those reads;
  * expand() writes back, at each transfer, the changed registers that are
    live there. The rest of CAND is not stored: the file keeps an older
    value of a register nobody reads.

Registers outside CAND -- r1, r2, r13, the non-volatile ones, cr2-cr4 and
the rest of XER -- are written back whenever they changed.

3. And on the way (rewrite_function)

Three smaller rewrites are made on the same pass over a body, each with a
description where it is defined: split_flags gives every CR field the
function uses, and XER's carry, a local of its own (PPU_NV_FLAGS); with
`live` the volatile vector registers are locals as well (PPU_NV_VRALL);
frame_entry puts the frame accesses of the prologue under one page test
(PPU_FRAME_ENTRY) and mem_runs does the same for any run of loads and
stores off one register (PPU_VM_RUNS).

rewrite(..., verify=True) keeps every write-back the first part drops as a
comparison instead: at each transfer, a register taken as unchanged is
compared with the file and a difference reported (ppu_nv_differs in the
generated prelude); and every write-back the second part drops stores a
poison value (PPU_NV_POISON_*), which a wrong liveness would read. A lift
built that way and played shows whether either is ever wrong on the paths
the title takes.
"""
import os
import re

RE_TOK = re.compile(r"ctx->(?:gpr\[(\d+)\]|fpr\[(\d+)\]|vr\[(\d+)\]|(cr|xer|ctr|xca)\b|crf\[(\d)\])")
RE_LABEL = re.compile(r"(loc_[0-9A-Fa-f]{8}):")
RE_CASE = re.compile(r"(?:case\b[^:;{}]*|default\s*):")
RE_KW = re.compile(r"(if|else|switch|for|while|do|goto|return)\b")
RE_XFER = re.compile(r"\b\w*func_[0-9A-Fa-f]+\(ctx\)|ps3_indirect_call\(ctx\)|ps3_hle_call\(|"
                     r"lv2_syscall\(ctx\)|g_trampoline_fn\b|\breturn\b|\bgoto\b|DRAIN_TRAMPOLINE")
RE_S_GOTO = re.compile(r"goto (loc_[0-9A-Fa-f]{8});$")
RE_S_RET = re.compile(r"return;$")
RE_S_CALL = re.compile(r"(?:\w*func_([0-9A-Fa-f]+)\(ctx\)|(ps3_indirect_call)\(ctx\)|"
                       r"(ps3_hle_call)\(0x[0-9A-Fa-f]+u?, ctx\));$")
RE_S_SYSC = re.compile(r"lv2_syscall\(ctx\);$")
RE_S_DRAIN = re.compile(r"DRAIN_TRAMPOLINE\(ctx\);$")
RE_S_TRAMP = re.compile(r"g_trampoline_fn = \(void\(\*\)\(void\*\)\)\w*func_([0-9A-Fa-f]+);$")
RE_S_RESTORE = re.compile(r"ctx->gpr\[(\d+)\] = _cs_(\d+);$")
RE_ASSIGN_AFTER = re.compile(r"\s*(?:=(?!=)|\+=|-=|\*=|/=|%=|&=|\|=|\^=|<<=|>>=|\+\+|--)")
RE_PLAIN_ASSIGN = re.compile(r"\s*=(?!=)")
RE_MEMCPY_SRC = re.compile(r"memcpy\(\s*&?\w+(?:\[[^\]]*\])?\s*,\s*&$")
RE_MEMCPY_DST = re.compile(r"memcpy\(&ctx->fpr\[\d+\], &\w+, 8\);$")
# fctiw/fctiwz: the result goes in the low word of FRT, whose high word the
# architecture leaves undefined and the lifter leaves as it was. For
# liveness that is no read of FRT.
RE_FCTIW = re.compile(r"\{ uint32_t iv = ppu_f2i32\(ctx->fpr\[(\d+)\], \d\); uint64_t tmp; "
                      r"memcpy\(&tmp, &ctx->fpr\[(\d+)\], 8\); tmp = \(tmp & 0xFFFFFFFF00000000ULL\) \| iv; "
                      r"memcpy\(&ctx->fpr\[\2\], &tmp, 8\); \}")
RE_COMMENT = re.compile(r"/\*.*?\*/|//.*$")
RE_STRING = re.compile(r'"(?:[^"\\]|\\.)*"')
RE_MARK = re.compile(r"/\*@NV(\d+)@\*/ ?")
RE_MARKV = re.compile(r" ?/\*@NVV([\d,]+)@\*/")
RE_MARKG = re.compile(r" ?/\*@NVG(\d+)@\*/")

EMPTY = frozenset()


class Bail(Exception):
    """The function's text is not of a shape this pass follows."""


# The registers the second part applies to, as bits of a mask.
CAND = [("g", 0)] + [("g", n) for n in range(3, 13)] + [("f", n) for n in range(14)] + [("s", "ctr")] + \
       [("c", n) for n in (0, 1, 5, 6, 7)] + [("s", "xca")] + [("v", n) for n in range(20)]
CBIT = {k: 1 << i for i, k in enumerate(CAND)}
ALLC = (1 << len(CAND)) - 1
CTR = CBIT[("s", "ctr")]
XCA = CBIT[("s", "xca")]
CRF = 0                                     # the volatile CR fields
for _n in (0, 1, 5, 6, 7):
    CRF |= CBIT[("c", _n)]
FLAGS = CRF | XCA
ARGS = 0
for _n in range(3, 11):
    ARGS |= CBIT[("g", _n)]
for _n in range(1, 14):
    ARGS |= CBIT[("f", _n)]
for _n in range(2, 14):
    ARGS |= CBIT[("v", _n)]
RETS = CBIT[("g", 3)] | CBIT[("g", 4)] | CBIT[("f", 1)] | CBIT[("f", 2)] | CBIT[("f", 3)] | CBIT[("f", 4)] | \
       CBIT[("v", 2)]
SYSC = ARGS | CBIT[("g", 11)]

LIVE_ON = os.environ.get("PPU_NV_LIVE", "1") != "0"

# PPU_NV_CTX=1: count every register mention by how it was taken (R read,
# W assigned) and the text around it, numbers blanked, into stats["ctx"] --
# the list to read through when a template of the lifter changes.
CTX_ON = os.environ.get("PPU_NV_CTX") == "1"
RE_NUM = re.compile(r"0x[0-9A-Fa-f]+|\d+")


def _ctx_note(hist, kind, m, t, a, b):
    lo, hi = max(a, t.start() - 16), min(b, t.end() + 10)
    key = kind + " " + RE_NUM.sub("N", m[lo:t.start()]) + "<" + t.group(0)[5:8] + ">" + RE_NUM.sub("N", m[t.end():hi])
    hist[key] = hist.get(key, 0) + 1


def _mask(line):
    """The line with comments and string contents blanked, same length."""
    def blank(m):
        return " " * (m.end() - m.start())
    s = RE_STRING.sub(lambda m: '"' + " " * (m.end() - m.start() - 2) + '"', line)
    return RE_COMMENT.sub(blank, s)


def _ws(m, i, end):
    while i < end and m[i] in " \t":
        i += 1
    return i


def _close(m, i, end):
    """Index of the bracket that closes the one at i."""
    depth = 0
    for j in range(i, end):
        c = m[j]
        if c in "([{":
            depth += 1
        elif c in ")]}":
            depth -= 1
            if depth == 0:
                return j
    raise Bail("unbalanced bracket")


SPR_TYPE = {"cr": "uint32_t", "xer": "uint32_t", "ctr": "uint64_t", "xca": "uint32_t"}


def _cr_store(fields):
    """The statement that puts the locals of the CR fields `fields` in the file."""
    mask, terms = 0, []
    for n in sorted(fields):
        sh = 28 - 4 * n
        mask |= 0xF << sh
        terms.append("(cr%d << %d)" % (n, sh) if sh else "cr%d" % n)
    return "ctx->cr = (ctx->cr & ~0x%08Xu) | %s;" % (mask, " | ".join(terms))


class Regs:
    """The registers a function keeps in locals, and their spellings.

    With `split` the function's flags are apart (split_flags): CR is the
    fields it names, cr0..cr7, each a local holding its four bits, and XER's
    carry is `xca`, holding bit 29 or nothing; `xer` is then the rest of
    XER, its own bit 29 meaningless. The file keeps CR and XER whole, so a
    field is loaded by a shift and written back by a merge."""

    def __init__(self, gprs, fprs, vrs, sprs, crfs=(), split=False):
        self.gprs, self.fprs, self.vrs, self.sprs = set(gprs), set(fprs), set(vrs), set(sprs)
        self.crfs = set(crfs)
        self.split = split
        self.hist = None                    # PPU_NV_CTX: see _ctx_note
        order = [("g", n) for n in sorted(self.gprs)] + [("f", n) for n in sorted(self.fprs)] + \
                [("v", n) for n in sorted(self.vrs)] + [("s", n) for n in sorted(self.sprs)] + \
                [("c", n) for n in sorted(self.crfs)]
        self.keys = order
        self.order = {k: i for i, k in enumerate(order)}
        self.all = frozenset(order)
        self.cand = 0                       # the CAND registers among them
        for k in order:
            self.cand |= CBIT.get(k, 0)

    def key(self, m):
        """The register a RE_TOK match names, or None if it is not a local."""
        if m.group(1) is not None:
            n = int(m.group(1))
            return ("g", n) if n in self.gprs else None
        if m.group(2) is not None:
            n = int(m.group(2))
            return ("f", n) if n in self.fprs else None
        if m.group(3) is not None:
            n = int(m.group(3))
            return ("v", n) if n in self.vrs else None
        if m.group(4) is not None:
            return ("s", m.group(4)) if m.group(4) in self.sprs else None
        n = int(m.group(5))
        return ("c", n) if n in self.crfs else None

    @staticmethod
    def local(k):
        return {"g": "r%s", "f": "f%s", "v": "v%s", "s": "%s", "c": "cr%s"}[k[0]] % (k[1],)

    @staticmethod
    def file(k):
        return {"g": "ctx->gpr[%s]", "f": "ctx->fpr[%s]", "v": "ctx->vr[%s]", "s": "ctx->%s"}[k[0]] % (k[1],)

    def _load(self, k):
        if k[0] == "c":
            sh = 28 - 4 * k[1]
            return "(ctx->cr >> %d) & 0xFu" % sh if sh else "ctx->cr & 0xFu"
        if k == ("s", "xca"):
            return "ctx->xer & 0x20000000u"
        return self.file(k)

    def decl(self):
        out = []
        for k in self.keys:
            t = {"g": "uint64_t", "f": "double", "v": "ppu_vr", "c": "uint32_t"}.get(k[0]) or SPR_TYPE[k[1]]
            out.append("%s %s = %s;" % (t, self.local(k), self._load(k)))
        return " ".join(out)

    def reload(self, vr_mark=False):
        """What is re-read after a call: every local but r14-r31, which the
        callee leaves in the file as it found them. With vr_mark, v20-v31
        are left to expand(): they are re-read only in a program where some
        function restores them for its caller (see "vclob")."""
        out = ["%s = %s;" % (self.local(k), self._load(k))
               for k in self.keys
               if not (k[0] == "g" and k[1] >= 14) and not (vr_mark and k[0] == "v" and k[1] >= 20)]
        nv = sorted(n for n in self.vrs if n >= 20)
        if vr_mark and nv:
            out.append("/*@NVV%s@*/" % ",".join(str(n) for n in nv))
        return " ".join(out)

    def store(self, keys):
        out, crs = [], []
        for k in sorted(keys, key=self.order.get):
            if k[0] == "c":
                crs.append(k[1])
            elif k == ("s", "xca"):
                out.append("ctx->xer = (ctx->xer & ~0x20000000u) | xca;")
            elif k == ("s", "xer") and self.split:
                out.append("ctx->xer = (ctx->xer & 0x20000000u) | (xer & ~0x20000000u);")
            else:
                out.append(f"{self.file(k)} = {self.local(k)};")
        if crs:
            out.append(_cr_store(crs))
        return " ".join(out)

    def check(self, keys, addr):
        """One comparison of the locals `keys` with the file: an OR of their
        differences, so that a transfer costs one branch however many
        registers it leaves alone (a branch each tripled the build)."""
        terms = []
        for k in sorted(keys, key=self.order.get):
            if k[0] == "g":
                terms.append("(ctx->gpr[%s] ^ r%s)" % (k[1], k[1]))
            elif k[0] == "f":
                terms.append("PPU_NV_XF(%s)" % k[1])
            elif k[0] == "v":
                terms.append("PPU_NV_XV(%s)" % k[1])
            elif k[0] == "c":
                terms.append("(uint64_t)((%s) ^ cr%d)" % (self._load(k), k[1]))
            elif k == ("s", "xca"):
                terms.append("(uint64_t)((ctx->xer & 0x20000000u) ^ xca)")
            elif k == ("s", "xer") and self.split:
                terms.append("(uint64_t)((ctx->xer ^ xer) & ~0x20000000u)")
            else:
                terms.append("(uint64_t)(ctx->%s ^ %s)" % (k[1], k[1]))
        if not terms:
            return ""
        return "if (PPU_UNLIKELY((%s) != 0)) ppu_nv_differs(ctx, 0x%08X);" % (" | ".join(terms), addr)

    def rename(self, text):
        """ctx->gpr[N] and friends as the locals' names."""
        return RE_TOK.sub(lambda m: self.local(self.key(m)) if self.key(m) else m.group(0), text)


# The flags apart.
#
# The lifter keeps CR as one word and XER as another, and every compare is
# `cr = (cr & ~field) | value`, every addic/subfic/addc/srawi `xer = (xer &
# ~CA) | carry`: a read of the old word, so that to liveness CR and XER are
# always in use, and each of the half million carries a title computes and
# never looks at is merged into a word that is stored before the next call.
# split_flags rewrites those statements onto registers of their own --
# ctx->crf[K], the four bits of CR field K, and ctx->xca, XER's bit 29 --
# which a compare or an addic then simply assigns.
#
# The statements are the lifter's templates, matched as text; a function
# with CR or XER in any other form keeps both whole (and is taken to read
# every flag everywhere).
RE_F_FIELD_W = re.compile(r"ctx->cr\s*=\s*\(ctx->cr & ~\(0xFu\s*<<\s*(\d+)\)\)\s*\|\s*\((.+?)\s*<<\s*\1\);")
RE_F_BIT_W = re.compile(r"ctx->cr = \(ctx->cr & ~\(1u << \(31 - (\d+)\)\)\) \| \((.+?) << \(31 - \1\)\);")
RE_F_BIT_R = re.compile(r"\(ctx->cr >> \(31 - (\d+)\)\) & 1\b")
RE_F_FIELD_R = re.compile(r"\(ctx->cr >> (\d+)\) & (0xF|\d+)u?\b")
RE_F_MFCR = re.compile(r"(ctx->gpr\[\d+\] = )ctx->cr;")
RE_F_MFOCRF = re.compile(r"(ctx->gpr\[\d+\] = )ctx->cr & 0x([0-9A-Fa-f]{8})u;")
RE_F_MTCRF = re.compile(r"ctx->cr = \(ctx->cr & 0x([0-9A-Fa-f]{8})u\) \| "
                        r"\(\(uint32_t\)(ctx->gpr\[\d+\]) & 0x([0-9A-Fa-f]{8})u\);")
RE_F_MTCR = re.compile(r"ctx->cr = \(uint32_t\)(ctx->gpr\[\d+\]);")
RE_F_LEFT = re.compile(r"ctx->cr\b")
F_SC = ("ctx->cr = (ctx->cr & ~(0xFu << 28)) | (_sc ? (2u << 28) : 0u);", "ctx->crf[0] = (_sc ? 2u : 0u);")
F_REC = ("ctx->cr = (ctx->cr & 0x0FFFFFFFu) | (_c << 28);", "ctx->crf[0] = _c;")
# stwcx./stdcx. with XER[SO] in CR0, a vcmp record form's CR6, an FP record form's CR1
F_SC_SO = ("ctx->cr = (ctx->cr & ~(0xFu << 28)) | (_sc ? (2u << 28) : 0u) | ((ctx->xer >> 31) << 28);",
           "ctx->crf[0] = (_sc ? 2u : 0u) | ((ctx->xer >> 31) & 1u);")
F_VREC = ("ctx->cr = (ctx->cr & ~0xF0u) | (_c6 << 4);", "ctx->crf[6] = _c6;")
F_FREC = ("ctx->cr = (ctx->cr & ~0x0F000000u) | ((ctx->fpscr >> 4) & 0x0F000000u);",
          "ctx->crf[1] = (ctx->fpscr >> 28) & 0xFu;")
RE_X_CA_W = re.compile(r"ctx->xer = \(ctx->xer & ~\(1u << 29\)\) \| (.+?);")
RE_X_CA_R = re.compile(r"\(ctx->xer >> 29\) & 1\b")
RE_X_SRA = re.compile(r"(ppc_sra[wd]\()&ctx->xer\b")
RE_X_SO = re.compile(r"\(ctx->xer >> 31\) & 1u?\b")
RE_X_LEFT = re.compile(r"ctx->xer\b")
CR_PACK = "(" + " | ".join("(ctx->crf[%d] << %d)" % (n, 28 - 4 * n) if n < 7 else "ctx->crf[7]"
                           for n in range(8)) + ")"


def _field_w(m):
    sh = int(m.group(1))
    if sh % 4:
        return m.group(0)
    return "ctx->crf[%d] = (%s);" % (7 - sh // 4, m.group(2))


def _field_r(m):
    sh = int(m.group(1))
    if sh % 4:
        # one bit at any shift (the branch decode's `(ctx->cr >> 31-BI) & 1`):
        # bit sh % 4 of the field that holds it
        if m.group(2) == "1":
            return "((ctx->crf[%d] >> %d) & 1)" % (7 - sh // 4, sh % 4)
        return m.group(0)
    return "(ctx->crf[%d] & %s)" % (7 - sh // 4, m.group(2))


def _bit_w(m):
    b = int(m.group(1))
    k, j = b // 4, 3 - b % 4
    return "ctx->crf[%d] = (ctx->crf[%d] & ~(1u << %d)) | ((%s) << %d);" % (k, k, j, m.group(2), j)


def _bit_r(m):
    b = int(m.group(1))
    return "((ctx->crf[%d] >> %d) & 1)" % (b // 4, 3 - b % 4)


def _mtcrf(m):
    keep, src, mask = int(m.group(1), 16), m.group(2), int(m.group(3), 16)
    if keep != (~mask & 0xFFFFFFFF):
        return m.group(0)
    out = []
    for n in range(8):
        sh = 28 - 4 * n
        nib = (mask >> sh) & 0xF
        if nib == 0xF:
            out.append("ctx->crf[%d] = ((uint32_t)%s >> %d) & 0xFu;" % (n, src, sh))
        elif nib:
            return m.group(0)
    return "{ " + " ".join(out) + " }"


def _mfocrf(m):
    """mfocrf: the fields FXM names, the rest 0."""
    mask = int(m.group(2), 16)
    parts = []
    for n in range(8):
        sh = 28 - 4 * n
        nib = (mask >> sh) & 0xF
        if nib == 0xF:
            parts.append("(ctx->crf[%d] << %d)" % (n, sh) if sh else "ctx->crf[%d]" % n)
        elif nib:
            return m.group(0)
    return m.group(1) + ("(" + " | ".join(parts) + ")" if parts else "0") + ";"


def _mtcr(m):
    return "{ " + " ".join("ctx->crf[%d] = ((uint32_t)%s >> %d) & 0xFu;" % (n, m.group(1), 28 - 4 * n)
                           for n in range(8)) + " }"


def split_flags(lines):
    """The body with CR as its fields and XER's carry apart, or None when a
    statement has CR or XER in a form that is not one of the lifter's."""
    out = []
    for l in lines:
        if "ctx->cr" in l:
            l = l.replace(*F_SC_SO).replace(*F_SC).replace(*F_REC).replace(*F_VREC).replace(*F_FREC)
            l = RE_F_FIELD_W.sub(_field_w, l)
            l = RE_F_BIT_W.sub(_bit_w, l)
            l = RE_F_BIT_R.sub(_bit_r, l)
            l = RE_F_FIELD_R.sub(_field_r, l)
            l = RE_F_MFCR.sub(lambda m: m.group(1) + CR_PACK + ";", l)
            l = RE_F_MFOCRF.sub(_mfocrf, l)
            l = RE_F_MTCRF.sub(_mtcrf, l)
            l = RE_F_MTCR.sub(_mtcr, l)
            if RE_F_LEFT.search(_mask(l)):
                return None
        if "ctx->xer" in l:
            l = RE_X_CA_W.sub(lambda m: "ctx->xca = " + m.group(1) + ";", l)
            l = RE_X_CA_R.sub("(ctx->xca >> 29)", l)
            l = RE_X_SRA.sub(lambda m: m.group(1) + "&ctx->xca", l)
            if RE_X_LEFT.search(RE_X_SO.sub("", _mask(l))):
                return None
        out.append(l)
    return out


# The prelude's vector helpers: the first argument is the result, the others
# are read. ppu_vldu4/ppu_vldf4 copy a register out into an array.
VR_HELPERS = {"ppu_vmaddfp", "ppu_vnmsubfp", "ppu_vaddfp", "ppu_vsubfp", "ppu_vmulfp", "ppu_vperm",
              "ppu_vspltw", "PPU_VSLDOI", "ppu_vcmpfp", "ppu_vcmpequw", "ppu_vmrg", "memcpy",
              "ppu_vmaxfp", "ppu_vminfp", "ppu_vctsxs", "ppu_vctuxs", "ppu_lvlx", "ppu_lvrx", "ppu_lvsl",
              "ppu_vand", "ppu_vandc", "ppu_vor", "ppu_vxor", "ppu_vnor", "ppu_vsel"}
VR_LOADERS = {"ppu_vldu4", "ppu_vldf4"}
# the first argument: all sixteen bytes (the lifter's memset of a register is always the whole of it)
VR_WRITERS = VR_HELPERS | {"ppu_vstu4", "ppu_vstf4", "memset"}
# runtime/ppu/ppu_vmx.h's helpers (vmx_vaddfp, vmx_vpkswss, ...): the same shape --
# the result first, written whole from a temporary; the vector operands after it.
VMX_PREFIX = "vmx_v"


def _vr_helper(name):
    return name in VR_HELPERS or (name is not None and name.startswith(VMX_PREFIX))


def _vr_writer(name):
    return name in VR_WRITERS or (name is not None and name.startswith(VMX_PREFIX))
RE_CONST_CAST = re.compile(r"\(const [\w ]+\*\)\s*&$")
RE_MEMBER = re.compile(r"(?:\.\w+|\[[^\]]*\])+")


def _enclosing_call(m, a, pos):
    """(name, argument index) of the call whose argument list holds pos."""
    depth, commas = 0, 0
    i = pos - 1
    while i >= a:
        c = m[i]
        if c in ")]":
            depth += 1
        elif c in "([":
            if depth == 0:
                if c == "[":
                    return None, -1
                j = i
                while j > a and m[j - 1] == " ":
                    j -= 1
                e = j
                while j > a and (m[j - 1].isalnum() or m[j - 1] == "_"):
                    j -= 1
                return (m[j:e] or None), commas
            depth -= 1
        elif c == "," and depth == 0:
            commas += 1
        elif c in ";{}" and depth == 0:
            break
        i -= 1
    return None, -1


def _vr_is_def(m, a, b, t):
    """A vector register is handled by address. It is read where it is a
    later argument of a vector helper or the source of a memcpy, the array
    a loader copies out, or cast to a pointer to const; else assigned."""
    if RE_ASSIGN_AFTER.match(m, t.end(), b):
        return True
    mm = RE_MEMBER.match(m, t.end(), b)
    if mm and RE_ASSIGN_AFTER.match(m, mm.end(), b):
        return True
    before = m[a:t.start()].rstrip()
    if not before.endswith("&") or before.endswith("&&"):
        return False                        # by value: an element or the whole register read
    if RE_CONST_CAST.search(before):
        return False
    name, idx = _enclosing_call(m, a, t.start())
    if name in VR_LOADERS and idx == 0:
        return False
    if _vr_helper(name) and idx >= 1:
        return False
    return True


def _is_def(m, a, b, t, k):
    """Does the mention t of register k in m[a:b] (may) assign it?"""
    if k[0] == "v":
        return _vr_is_def(m, a, b, t)
    if RE_ASSIGN_AFTER.match(m, t.end(), b):
        return True
    before = m[a:t.start()].rstrip()
    if before.endswith("++") or before.endswith("--"):
        return True
    if before.endswith("&") and not before.endswith("&&"):
        lead = before[:-1].rstrip()
        if lead and (lead[-1].isalnum() or lead[-1] in "_)]"):
            return False                    # a bitwise and, the register its right operand
        if k[0] == "f" and RE_MEMCPY_SRC.search(before):
            return False                    # memcpy(&tmp, &fN, 8): stfd reads it
        return True                         # its address is taken: assume written
    return False


def _scan(m, a, b, regs, simple):
    """What the text m[a:b] does to the registers: (the locals it may assign,
    the CAND it certainly overwrites without reading, the CAND it may read).

    Certainly overwritten is kept narrow -- the statement is `rN = ...;` or
    lfd's `memcpy(&fN, &tmp, 8);`, and only where the text is one simple
    statement -- because liveness errs safely only by reading too much."""
    may, must, use = set(), 0, 0
    first = _ws(m, a, b)
    memcpy_dst = simple and RE_MEMCPY_DST.match(m, first, b) is not None
    for t in RE_TOK.finditer(m, a, b):
        k = regs.key(t)
        if k is None:
            continue
        d = _is_def(m, a, b, t, k)
        if d:
            may.add(k)
        if regs.hist is not None:
            _ctx_note(regs.hist, "W" if d else "R", m, t, a, b)
        bit = CBIT.get(k)
        if bit and k[0] == "v":
            # written whole, and not read, as the result of a vector helper
            # or the destination of a memcpy (lvx)
            name, idx = _enclosing_call(m, a, t.start()) if d else (None, -1)
            if simple and d and idx == 0 and _vr_writer(name) and m[a:t.start()].rstrip().endswith("&"):
                must |= bit
            else:
                use |= bit
        elif bit:
            if simple and d and ((t.start() == first and RE_PLAIN_ASSIGN.match(m, t.end(), b))
                                 or (memcpy_dst and t.start() == first + 8)
                                 or (bit == XCA and m.endswith(("ppc_sraw(&", "ppc_srad(&"), a, t.start()))):
                must |= bit                 # ppc_sraw/ppc_srad set the carry from nothing but their operands
            else:
                use |= bit
    return frozenset(may), must, use


# Statement nodes: tuples (kind, start, end, ...) of positions in their line.
#   plain (may, must, use)   if (may, use, then, else)   switch (may, use, body)
#   block (nodes)   label (name)   case   goto (name)   ret   drain   sysc
#   call (class)   tramp (class)   restore (reg, snapshot)
# A transfer's class is what the other side is: ("C", addr) a direct call,
# ("I",) an indirect one, ("H",) an HLE import, ("T", addr) a tail jump.
def _stmt(m, i, end, regs):
    c = m[i]
    if c == "{":
        fm = RE_FCTIW.match(m, i, end)
        if fm:
            src, dst = ("f", int(fm.group(1))), ("f", int(fm.group(2)))
            may = frozenset([dst]) if dst[1] in regs.fprs else EMPTY
            must = CBIT.get(dst, 0) if dst[1] in regs.fprs else 0
            use = CBIT.get(src, 0) if src[1] in regs.fprs else 0
            if regs.hist is not None:
                regs.hist["W fctiw <fpr>"] = regs.hist.get("W fctiw <fpr>", 0) + 1
            return ("plain", i, fm.end(), may, must, use), fm.end()
        j = _close(m, i, end)
        return ("block", i, j + 1, _stmts(m, i + 1, j, regs)), j + 1
    if c == ";":
        return ("plain", i, i + 1, EMPTY, 0, 0), i + 1
    lm = RE_LABEL.match(m, i, end)
    if lm:
        return ("label", i, lm.end(), lm.group(1)), lm.end()
    cm = RE_CASE.match(m, i, end)
    if cm:
        return ("case", i, cm.end()), cm.end()
    km = RE_KW.match(m, i, end)
    kw = km.group(1) if km else None
    if kw == "if":
        j = _ws(m, km.end(), end)
        if j >= end or m[j] != "(":
            raise Bail("if without a condition")
        k = _close(m, j, end)
        may, _, use = _scan(m, j, k + 1, regs, False)
        then, e = _stmt(m, _ws(m, k + 1, end), end, regs)
        els = None
        e2 = _ws(m, e, end)
        if m.startswith("else", e2) and not (e2 + 4 < end and (m[e2 + 4].isalnum() or m[e2 + 4] == "_")):
            els, e = _stmt(m, _ws(m, e2 + 4, end), end, regs)
        return ("if", i, e, may, use, then, els), e
    if kw == "switch":
        j = _ws(m, km.end(), end)
        if j >= end or m[j] != "(":
            raise Bail("switch without an expression")
        k = _close(m, j, end)
        b = _ws(m, k + 1, end)
        if b >= end or m[b] != "{":
            raise Bail("switch without a body")
        e = _close(m, b, end)
        may, _, use = _scan(m, j, k + 1, regs, False)
        return ("switch", i, e + 1, may, use, _stmts(m, b + 1, e, regs)), e + 1
    if kw in ("for", "while"):
        j = _ws(m, km.end(), end)
        if j >= end or m[j] != "(":
            raise Bail("loop without a head")
        k = _close(m, j, end)
        _, e = _stmt(m, _ws(m, k + 1, end), end, regs)
        return _opaque(m, i, e, regs), e
    if kw == "do":
        _, e = _stmt(m, _ws(m, km.end(), end), end, regs)
        e = _ws(m, e, end)
        if not m.startswith("while", e):
            raise Bail("do without while")
        j = _ws(m, e + 5, end)
        k = _close(m, j, end)
        e = _ws(m, k + 1, end)
        if e >= end or m[e] != ";":
            raise Bail("do-while without a semicolon")
        return _opaque(m, i, e + 1, regs), e + 1
    if kw == "else":
        raise Bail("else without if")
    # a simple statement: to the semicolon outside every bracket
    depth, j = 0, i
    while j < end:
        ch = m[j]
        if ch in "([{":
            depth += 1
        elif ch in ")]}":
            depth -= 1
            if depth < 0:
                raise Bail("unbalanced statement")
        elif ch == ";" and depth == 0:
            break
        j += 1
    if j >= end:
        raise Bail("statement without a semicolon")
    text = m[i:j + 1].strip()
    e = j + 1
    gm = RE_S_GOTO.match(text)
    if gm:
        return ("goto", i, e, gm.group(1)), e
    if RE_S_RET.match(text):
        return ("ret", i, e), e
    cm = RE_S_CALL.match(text)
    if cm:
        cls = ("C", int(cm.group(1), 16)) if cm.group(1) else ("I",) if cm.group(2) else ("H",)
        return ("call", i, e, cls), e
    if RE_S_SYSC.match(text):
        return ("sysc", i, e), e
    if RE_S_DRAIN.match(text):
        return ("drain", i, e), e
    tm = RE_S_TRAMP.match(text)
    if tm:
        return ("tramp", i, e, ("T", int(tm.group(1), 16))), e
    rm = RE_S_RESTORE.match(text)
    if rm and int(rm.group(1)) in regs.gprs:
        return ("restore", i, e, int(rm.group(1)), int(rm.group(2))), e
    if RE_XFER.search(text):
        raise Bail("a transfer inside a statement: " + text[:60])
    return ("plain", i, e) + _scan(m, i, e, regs, True), e


def _opaque(m, i, e, regs):
    if RE_XFER.search(m, i, e) or RE_LABEL.search(m, i, e):
        raise Bail("a transfer inside a loop statement")
    return ("plain", i, e) + _scan(m, i, e, regs, False)


def _stmts(m, i, end, regs):
    out = []
    while True:
        i = _ws(m, i, end)
        if i >= end:
            return out
        node, i = _stmt(m, i, end, regs)
        out.append(node)


# The control-flow graph: one atom per statement that matters, in program
# order. An atom is a list
#   [kind, line, start, end, successors, may, must, use, extra, bare]
# with kind one of plain branch restore label nop goto ret call sysc drain
# tramp end; `bare` marks a transfer that is the whole body of an if and
# has to be braced before anything is put beside it.
K, LI, A0, A1, SUCC, MAY, MUST, USE, EXTRA, BARE = range(10)


def _lower(flat):
    atoms, labels, gotos = [], {}, []

    def new(kind, li, nd, may=EMPTY, must=0, use=0, extra=None, bare=False):
        atoms.append([kind, li, nd[1] if nd else 0, nd[2] if nd else 0, [], may, must, use, extra, bare])
        return len(atoms) - 1

    def link(dangling, target):
        for d in dangling:
            atoms[d][SUCC].append(target)

    def seq(items, sw):
        first, dangling = None, []
        for li, nd in items:
            f, d = node(li, nd, sw, False)
            if first is None:
                first = f
            link(dangling, f)
            dangling = d
        return first, dangling

    def node(li, nd, sw, bare):
        k = nd[0]
        if k == "plain":
            i = new("plain", li, nd, nd[3], nd[4], nd[5])
            return i, [i]
        if k == "restore":
            i = new("restore", li, nd, extra=(nd[3], nd[4]))
            return i, [i]
        if k == "label":
            if nd[3] in labels:
                raise Bail("a label twice")
            i = new("label", li, nd, extra=nd[3])
            labels[nd[3]] = i
            return i, [i]
        if k == "case":
            if sw is None:
                raise Bail("case outside a switch")
            i = new("nop", li, nd)
            atoms[sw][SUCC].append(i)
            return i, [i]
        if k == "goto":
            i = new("goto", li, nd, extra=nd[3])
            gotos.append(i)
            return i, []
        if k == "ret":
            return new("ret", li, nd, bare=bare), []
        if k in ("call", "tramp"):
            i = new(k, li, nd, extra=nd[3], bare=bare)
            return i, [i]
        if k in ("sysc", "drain"):
            i = new(k, li, nd, bare=bare)
            return i, [i]
        if k == "block":
            if not nd[3]:
                i = new("nop", li, nd)
                return i, [i]
            for c in nd[3]:
                if c[0] == "label":
                    raise Bail("a label inside a block")
            return seq([(li, c) for c in nd[3]], sw)
        if k == "if":
            i = new("branch", li, nd, nd[3], 0, nd[4])
            tf, td = node(li, nd[5], sw, True)
            atoms[i][SUCC].append(tf)
            d = list(td)
            if nd[6] is not None:
                ef, ed = node(li, nd[6], sw, True)
                atoms[i][SUCC].append(ef)
                d += ed
            else:
                d.append(i)                 # the false edge falls through
            return i, d
        if k == "switch":
            i = new("branch", li, nd, nd[3], 0, nd[4])
            for c in nd[5]:
                if c[0] == "label":
                    raise Bail("a label inside a block")
            _, bd = seq([(li, c) for c in nd[5]], i) if nd[5] else (None, [])
            return i, bd + [i]              # no case taken: past the body
        raise Bail("an unknown node")

    _, dangling = seq(flat, None)
    if dangling or not atoms:
        # The body runs off its end into the fall-through trampoline the
        # lifter appends, which enters another function.
        e = new("end", flat[-1][0] if flat else 0, None)
        link(dangling, e)
    for g in gotos:
        name = atoms[g][EXTRA]
        if name not in labels:
            raise Bail("a goto to a label outside the function")
        atoms[g][SUCC].append(labels[name])
    # A call is followed by its drain or by a return, and nothing else.
    for at in atoms:
        if at[K] == "call":
            if len(at[SUCC]) != 1 or atoms[at[SUCC][0]][K] not in ("drain", "ret"):
                raise Bail("a call followed by neither a drain nor a return")
            # An indirect call that goes straight into a return is a jump
            # (bctr: a switch's default arm, a conditional bctr): it may land
            # in the middle of a function, on code that reads anything.
            nx = atoms[at[SUCC][0]]
            if at[EXTRA] == ("I",) and (nx[K] == "ret" or (
                    len(nx[SUCC]) == 1 and atoms[nx[SUCC][0]][K] == "ret")):
                at[EXTRA] = ("J",)
    return atoms


def _dirty(atoms):
    """The may-dirty set on entry to each atom (None: not reached)."""
    n = len(atoms)
    din = [None] * n
    din[0] = EMPTY
    changed = True
    rounds = 0
    while changed:
        changed = False
        rounds += 1
        if rounds > 400:
            raise Bail("no fixed point")
        for i in range(n):
            d = din[i]
            if d is None:
                continue
            at = atoms[i]
            k = at[K]
            if k == "plain" or k == "branch":
                if at[MAY]:
                    d = d | at[MAY]
            elif k == "restore":
                d = d - {("g", at[EXTRA][0])}
            elif k == "call" or k == "sysc" or k == "tramp":
                d = EMPTY                   # written back, and reloaded after a call
            for s in at[SUCC]:
                old = din[s]
                if old is None:
                    din[s] = d
                    changed = True
                elif not d <= old:
                    din[s] = old | d
                    changed = True
    return din


def _merge_t(a, b):
    if not b:
        return a
    if not a:
        return b
    out = dict(a)
    for t, m in b.items():
        out[t] = out.get(t, 0) | m
    return out


def _live(atoms):
    """Backward over the CAND registers: for each atom, the registers read
    before written from just before it (bl[i]), and for each kind of
    transfer reachable from there the registers not yet overwritten on some
    path to it (bt[i], class -> mask). Returns (bl, bt, after) with after[i]
    the same pair just after a call-like atom."""
    n = len(atoms)
    bl = [0] * n
    bt = [{} for _ in range(n)]
    after = {}
    changed = True
    rounds = 0
    while changed:
        changed = False
        rounds += 1
        if rounds > 400:
            raise Bail("no fixed point")
        for i in range(n - 1, -1, -1):
            at = atoms[i]
            k = at[K]
            ol, ot = 0, {}
            for s in at[SUCC]:
                ol |= bl[s]
                ot = _merge_t(ot, bt[s])
            if k == "plain" or k == "branch":
                must = at[MUST]
                l = (ol & ~must) | at[USE]
                if must and ot:
                    t = {}
                    for c, m in ot.items():
                        m &= ~must
                        if m:
                            t[c] = m
                else:
                    t = ot
            elif k == "call" or k == "tramp":
                # Every register passes through a call (the callee may leave
                # it alone); the callee's own reads are the transfer's.
                after[i] = (ol, ot)
                l = ol
                t = dict(ot)
                t[at[EXTRA]] = ALLC
            elif k == "sysc":
                after[i] = (ol, ot)
                l = ol
                t = dict(ot)
                t[("S",)] = ALLC
            elif k == "ret":
                l, t = 0, {("R",): ALLC}
            elif k == "end":
                l, t = 0, {("E",): ALLC}
            else:
                l, t = ol, ot
            if l != bl[i] or t != bt[i]:
                bl[i], bt[i] = l, t
                changed = True
    return bl, bt, after


RE_VR_STORE = re.compile(r"memcpy\(vm_base \+ \(uint32_t\)ea, (?:\(const uint8_t\*\))?&ctx->vr\[(\d+)\]")
RE_CR_SAVE = re.compile(r"(?:vm_write(?:32|64)\(|PPU_FW(?:32|64)\(-?\d+, |PPU_RW(?:32|64)\(1, \d+, )"
                        r"ctx->gpr\[1\] \+ (0x[0-9A-Fa-f]+), "
                        r"(?:\(uint32_t\))?ctx->gpr\[(\d+)\]\);$")
RE_CR_PICK = re.compile(r"ctx->gpr\[(\d+)\] = \(uint64_t\)ppc_rlwinm\(\(uint32_t\)ctx->gpr\[(\d+)\], (\d+), (\d+), (\d+)\);$")
RE_SP_ADJ = re.compile(r"ctx->gpr\[1\] \+= (-0x[0-9A-Fa-f]+|-\d+);$")
RE_SP_SET = re.compile(r"ctx->gpr\[1\]\s*(?:=(?!=)|[-+*/%&|^]=|<<=|>>=)")


def _rlwinm_fields(sh, mb, me):
    """The CR fields (as CAND bits) of x that ppc_rlwinm(x, sh, mb, me) reads."""
    if mb <= me:
        mask = (0xFFFFFFFF >> mb) & (0xFFFFFFFF << (31 - me))
    else:
        mask = (0xFFFFFFFF >> mb) | (0xFFFFFFFF << (31 - me))
    mask &= 0xFFFFFFFF
    sh &= 31
    want = ((mask >> sh) | (mask << (32 - sh))) & 0xFFFFFFFF if sh else mask
    out = 0
    for n in range(8):
        if want & (0xF << (28 - 4 * n)):
            out |= CBIT.get(("c", n), 0)
    return out


RE_CR_TAKE = re.compile(r"ctx->crf\[(\d)\] = \(\(uint32_t\)ctx->gpr\[(\d+)\] >> (\d+)\) & 0xFu;$")


def _cr_word_fields(atoms, masked, mi, x, bl):
    """The volatile CR fields that the word an mfcr left in rX is asked for,
    or None where that cannot be told: every statement the word can reach
    in rX is looked at, and each read of it has to be one of

      stw rX, 8(r1)             the prologue's save of CR into the caller's
                                frame (8 + the frame's size once the frame
                                is allocated; only in the straight run of
                                statements after the mfcr): what the
                                epilogue takes back out is cr2-cr4
      rlwinm rD, rX, s, b, e    a compare's outcome as a number: the fields
                                under the mask
      mtcrf m, rX               a compare kept across calls: the fields of m
    """
    tok = "ctx->gpr[%d]" % x
    head = tok + " = "
    bit = CBIT.get(("g", x), 0)
    fields = 0
    # the straight run after the mfcr, where r1's moves can be followed
    slot = 8
    j = mi + 1
    n = len(atoms)
    start = [mi]
    while j < n and atoms[j][K] == "plain" and atoms[j - 1][SUCC] == [j]:
        t = masked[atoms[j][LI]][atoms[j][A0]:atoms[j][A1]].strip()
        if tok in t:
            sm = RE_CR_SAVE.match(t)
            if not (sm and int(sm.group(2)) == x and int(sm.group(1), 16) == slot):
                break
        am = RE_SP_ADJ.match(t)
        if am:
            slot -= int(am.group(1), 0)
        elif RE_SP_SET.search(t):
            break
        start = [j]
        j += 1
    work = []
    for sx in start:
        work.extend(atoms[sx][SUCC])
    seen = set()
    while work:
        i = work.pop()
        if i in seen:
            continue
        seen.add(i)
        at = atoms[i]
        k = at[K]
        if k == "plain" or k == "branch":
            t = masked[at[LI]][at[A0]:at[A1]].strip()
            if tok in t:
                if k == "branch":
                    return None
                pm = RE_CR_PICK.match(t)
                tm = RE_CR_TAKE.match(t)
                if pm and int(pm.group(2)) == x:
                    fields |= _rlwinm_fields(int(pm.group(3)), int(pm.group(4)), int(pm.group(5)))
                    if int(pm.group(1)) == x:
                        continue            # the pick replaces the word
                elif tm and int(tm.group(2)) == x and int(tm.group(3)) == 28 - 4 * int(tm.group(1)):
                    fields |= CBIT.get(("c", int(tm.group(1))), 0)
                elif t.startswith(head) and tok not in t[len(head):]:
                    continue                # rX gets something else
                else:
                    return None             # read some other way
        elif k == "restore":
            if at[EXTRA][0] == x:
                continue
        elif k in ("call", "sysc"):
            if x < 14:
                # a volatile register comes back from the file: not the
                # word any more, unless the code reads it all the same
                later = 0
                for sx in at[SUCC]:
                    later |= bl[sx]
                if later & bit:
                    return None
                continue
        elif k in ("tramp", "end"):
            return None                     # the function goes on elsewhere, the word with it
        elif k == "ret":
            continue
        work.extend(at[SUCC])
    return fields


def _cr_words(atoms, masked, bl):
    """[(mfcr atom, the volatile fields its word is asked for)] for every
    mfcr whose word can be followed."""
    out = []
    head = " = " + CR_PACK + ";"
    for i, at in enumerate(atoms):
        if at[K] != "plain":
            continue
        text = masked[at[LI]][at[A0]:at[A1]].strip()
        if not text.endswith(head) or not text.startswith("ctx->gpr["):
            continue
        r = text[len("ctx->gpr["):text.index("]")]
        if not r.isdigit():
            continue
        f = _cr_word_fields(atoms, masked, i, int(r), bl)
        if f is not None:
            out.append((i, f))
    return out


def summarize_plain(body_lines):
    """The summary of a function this pass could not follow: it reads every
    register, everywhere, and reaches every transfer its text names."""
    toks = {}
    for l in body_lines:
        m = _mask(l)
        for c in re.finditer(r"\w*func_([0-9A-Fa-f]{8})\(ctx\)", m):
            toks[("C", int(c.group(1), 16))] = ALLC
        for c in re.finditer(r"g_trampoline_fn = \(void\(\*\)\(void\*\)\)\w*func_([0-9A-Fa-f]{8})", m):
            toks[("T", int(c.group(1), 16))] = ALLC
        if "ps3_indirect_call(ctx)" in m:
            toks[("J",)] = ALLC
        if "ps3_hle_call(" in m:
            toks[("H",)] = ALLC
    toks[("R",)] = ALLC
    toks[("E",)] = ALLC
    return {"in": (ALLC, tuple(toks.items())), "sites": [], "locals": 0, "plain": True}


# The frame accesses of a function's entry under one page test.
#
# From the top of the body down to its first branch, label or transfer, the
# loads and stores at a constant offset from r1 -- the prologue's saves, in
# the main -- are rewritten onto PPU_FR*/PPU_FW* (ppu_vm_fast.h), which take
# the offset from the stack pointer the function was entered with: r1's own
# moves on the way (`stdu r1,-N(r1)`) are followed. PPU_FRAME_INIT at the top
# then tests the pages of the whole stretch once. Four accesses pay for the
# test; a stretch of 64 KiB or more is left alone, as is everything from the
# first statement that sets r1 some other way.
RE_FRAME_ACC = re.compile(r"vm_(read|write)(8|16|32|64)\((?=ctx->gpr\[1\] \+ (-?0x[0-9A-Fa-f]+)[,)])")
RE_SP_ADJ_ANY = re.compile(r"ctx->gpr\[1\] \+= (-0x[0-9A-Fa-f]+|-\d+);")
RE_ENTRY_STOP = re.compile(r"\w*func_[0-9A-Fa-f]+\(ctx\)|ps3_indirect_call|ps3_hle_call|lv2_syscall|"
                           r"g_trampoline_fn|\breturn\b|\bgoto\b|\bif\s*\(|\bswitch\b|\bfor\s*\(|"
                           r"\bwhile\s*\(|\bdo\b|^\s*loc_[0-9A-Fa-f]{8}:")
FRAME_ON = os.environ.get("PPU_FRAME_ENTRY", "1") != "0"


def frame_entry(lines):
    """(lines, init): the body with its entry's frame accesses on PPU_FR*/
    PPU_FW*, and the PPU_FRAME_INIT statement for the top of the function;
    or (lines, None) untouched."""
    first = 0
    while first < len(lines) and lines[first].strip().startswith("uint64_t _cs_"):
        first += 1
    disp = 0
    found = []                              # (line, match, offset from the entry's r1)
    for li in range(first, len(lines)):
        m = _mask(lines[li])
        if RE_ENTRY_STOP.search(m):
            break
        adj = list(RE_SP_ADJ_ANY.finditer(m))
        if len(RE_SP_SET.findall(m)) != len(adj):
            break                           # r1 set some other way
        events = [(x.start(), 0, x) for x in adj] + [(x.start(), 1, x) for x in RE_FRAME_ACC.finditer(m)]
        for _pos, kind, x in sorted(events, key=lambda e: e[0]):
            if kind == 0:
                disp += int(x.group(1), 0)
            else:
                found.append((li, x, disp + int(x.group(3), 0)))
    if len(found) < 4:
        return lines, None
    lo = min(e for _li, _x, e in found)
    hi = max(e + int(x.group(2)) // 8 for _li, x, e in found)
    if hi - lo >= 0x10000:
        return lines, None
    out = list(lines)
    for li, x, e in reversed(found):        # right to left: the positions of a line stay good
        name = "PPU_F%s%s(%d, " % ("R" if x.group(1) == "read" else "W", x.group(2), e)
        out[li] = out[li][:x.start()] + name + out[li][x.end():]
    return out, "    PPU_FRAME_INIT(r1, %d, %d)" % (lo, hi)


# Runs of accesses off one register under one page test.
#
# The same for any register and anywhere in the body: where four or more
# loads and stores at constant offsets from one register follow one another
# with no label, call or return between them and the register unchanged --
# a structure filled in, a vector or a matrix copied to the stack, an
# object's fields read -- PPU_RUN(reg, ...) before the first tests the pages
# of the whole stretch and keeps its host address, and the accesses become
# PPU_RR*/PPU_RW* (ppu_vm_fast.h), which go straight to memory when that
# test passed and take their ordinary path, test and all, when it did not.
# A conditional branch out of the run does not end it, nor does a
# conditional statement in it: the test is made before the first access and
# nothing can enter below it. Only loads: the test asks for the load bit;
# only stores: for the store bit; else both. Whatever is handed the context
# itself (a call, a helper) may set any register, and ends every run.
RE_RUN_ACC = re.compile(r"vm_(read|write)(8|16|32|64)\("
                        r"(?=ctx->gpr\[(\d+)\](?: \+ (-?0x[0-9A-Fa-f]+|-?\d+))?[,)])")
RE_RUN_DEF = re.compile(r"ctx->gpr\[(\d+)\]\s*(?:=(?!=)|[-+*/%&|^]=|<<=|>>=|\+\+|--)|"
                        r"(?:\+\+|--|&)ctx->gpr\[(\d+)\]")
RE_RUN_BREAK = re.compile(r"\bctx\b(?!\s*->)|ps3_indirect_call|ps3_hle_call|lv2_syscall|g_trampoline_fn|"
                          r"DRAIN_TRAMPOLINE|\breturn\b|\bswitch\b|\bwhile\s*\(|\bdo\b|"
                          r"^\s*\w+:(?!:)|^\s*goto\b|\bcase\b|\bdefault\b")
RE_RUN_ODD = re.compile(r"ctx->gpr\[(?!\d+\])")
RUNS_ON = os.environ.get("PPU_VM_RUNS", "1") != "0"
RUN_MIN = int(os.environ.get("PPU_VM_RUN_MIN", "4"))


def mem_runs(lines, stats=None):
    """(lines, decl): the body with its runs of accesses on PPU_RR*/PPU_RW*
    and a PPU_RUN line before each, and the PPU_RUN_VARS declarations for
    the top of the function; or (lines, None) untouched."""
    first = 0
    while first < len(lines) and lines[first].strip().startswith("uint64_t _cs_"):
        first += 1
    open_runs = {}                          # register -> [(line, match, offset)]
    closed = []

    def close(reg):
        run = open_runs.pop(reg, None)
        if not run or len(run) < RUN_MIN:
            return
        lo = min(e for _li, _x, e in run)
        hi = max(e + int(x.group(2)) // 8 for _li, x, e in run)
        if hi - lo < 0x8000:
            closed.append((reg, lo, hi, run))

    for li in range(first, len(lines)):
        m = _mask(lines[li])
        if RE_RUN_BREAK.search(m) or RE_RUN_ODD.search(m):
            for reg in list(open_runs):
                close(reg)
            continue
        # an assignment takes effect where its statement ends: the load in
        # `r3 = vm_read32(r3 + 8);` is still off the old r3
        events = [(x.start(), 0, x) for x in RE_RUN_ACC.finditer(m)]
        for x in RE_RUN_DEF.finditer(m):
            end = m.find(";", x.end()) if x.group(1) else -1
            events.append((end if end >= 0 else x.start(), 1, x))
        if not events:
            continue
        set_here = set()                    # set earlier on this line: what follows is not at the run's base
        for _pos, kind, x in sorted(events, key=lambda e: e[0]):
            if kind == 0:
                reg = int(x.group(3))
                if reg in set_here:
                    continue
                open_runs.setdefault(reg, []).append((li, x, int(x.group(4) or "0", 0)))
            else:
                reg = int(x.group(1) or x.group(2))
                close(reg)
                set_here.add(reg)
    for reg in list(open_runs):
        close(reg)
    if not closed:
        return lines, None
    out = list(lines)
    edits = []                              # (line, match, text)
    inits = {}                              # line -> [PPU_RUN statements]
    for reg, lo, hi, run in closed:
        kinds = set(x.group(1) for _li, x, _e in run)
        bits = 1 if kinds == {"read"} else 8 if kinds == {"write"} else 9
        inits.setdefault(run[0][0], []).append(
            "PPU_RUN(%d, ctx->gpr[%d], %d, %d, %d);" % (reg, reg, lo, hi - lo, bits))
        for li, x, e in run:
            edits.append((li, x, "PPU_R%s%s(%d, %d, " % ("R" if x.group(1) == "read" else "W", x.group(2), reg, e - lo)))
        if stats is not None:
            stats["run_acc"] = stats.get("run_acc", 0) + len(run)
    for li, x, text in sorted(edits, key=lambda e: (e[0], e[1].start()), reverse=True):
        out[li] = out[li][:x.start()] + text + out[li][x.end():]
    for li in sorted(inits, reverse=True):
        out.insert(li, "    " + " ".join(inits[li]))
    if stats is not None:
        stats["runs"] = stats.get("runs", 0) + len(closed)
    regs = sorted(set(reg for reg, _lo, _hi, _run in closed))
    return out, "    " + " ".join("PPU_RUN_VARS(%d)" % r for r in regs)


RE_ANY_G = re.compile(r"ctx->gpr\[(\d+)\]")
RE_ANY_F = re.compile(r"ctx->fpr\[(\d+)\]")
RE_ANY_V = re.compile(r"ctx->vr\[(\d+)\]")
RE_ANY_S = re.compile(r"ctx->(cr|xer|ctr|xca)\b")
RE_ANY_C = re.compile(r"ctx->crf\[(\d)\]")


def rewrite_function(body_lines, addr, verify=False, stats=None, live=None, split=None):
    """rewrite() for a body as the lifter wrote it: every GPR and FPR it
    names, v20-v31 and CR/XER/CTR become locals, with the flags apart
    (split_flags) where `split` (default: with `live`) and the body allows.
    With `live` the volatile vector registers, v0-v19, are locals as well
    (PPU_NV_VRALL=0: not): written back at every transfer they would cost
    more than they save, written back where they are live they let the
    compiler keep a vector in a host register from one AltiVec operation
    to the next, and cancel the byte swaps between them.
    Returns (lines, summary) as rewrite() does."""
    if live is None:
        live = LIVE_ON
    vrall = live and os.environ.get("PPU_NV_VRALL", "1") != "0"
    if split is None:
        split = live and os.environ.get("PPU_NV_FLAGS", "1") != "0"
    lines, did = body_lines, False
    if split:
        s2 = split_flags(body_lines)
        if s2 is not None:
            lines, did = s2, True
        elif stats is not None:
            stats["whole_flags"] = stats.get("whole_flags", 0) + 1
    frame_init = None
    if FRAME_ON:
        lines, frame_init = frame_entry(lines)
        if frame_init and stats is not None:
            stats["frame_entry"] = stats.get("frame_entry", 0) + 1
    run_decl = None
    if RUNS_ON:
        lines, run_decl = mem_runs(lines, stats)
    gprs, fprs, vrs, sprs, crfs = set(), set(), set(), set(), set()
    for l in lines:
        if "ctx->" not in l:
            continue
        for m in RE_ANY_G.finditer(l):
            gprs.add(int(m.group(1)))
        for m in RE_ANY_F.finditer(l):
            fprs.add(int(m.group(1)))
        for m in RE_ANY_V.finditer(l):
            if vrall or int(m.group(1)) >= 20:
                vrs.add(int(m.group(1)))
        for m in RE_ANY_S.finditer(l):
            sprs.add(m.group(1))
        for m in RE_ANY_C.finditer(l):
            crfs.add(int(m.group(1)))
    regs = Regs(gprs, fprs, vrs, sprs, crfs, did)
    if CTX_ON and stats is not None:
        regs.hist = stats.setdefault("ctx", {})
    decl = ("    " + regs.decl()) if regs.all else None
    if decl and frame_init:
        decl += "\n" + frame_init
    if decl and run_decl:
        decl += "\n" + run_decl
    try:
        return _rewrite(lines, addr, regs, decl,
                        regs.reload(vr_mark=live), verify, stats, live)
    except Bail as e:
        if stats is not None:
            stats["bail"] = stats.get("bail", 0) + 1
            why = str(e).split(":")[0]
            stats.setdefault("why", {})[why] = stats.setdefault("why", {}).get(why, 0) + 1
        return None, (summarize_plain(body_lines) if live else None)


def rewrite(body_lines, addr, gprs, fprs, vrs, sprs, decl_line, reload, verify=False, stats=None,
            live=None):
    """The body with locals for its registers and write-backs of what changed.

    body_lines  the lifter's statements, still spelt with ctx->...
    addr        the function's guest address, for the verifying form
    decl_line   the locals' declarations (loaded from the file), or None for
                a body that names no register (it is only summarised)
    reload      the statements that re-read the file after a call
    live        leave the CAND write-backs to expand() (default: PPU_NV_LIVE)
    Returns (lines, summary): the new body lines, or None if the function
    has to keep the write-everything form; and its summary for solve().
    """
    if live is None:
        live = LIVE_ON
    regs = Regs(gprs, fprs, vrs, sprs)
    if CTX_ON and stats is not None:
        regs.hist = stats.setdefault("ctx", {})
    try:
        return _rewrite(body_lines, addr, regs, decl_line, reload, verify, stats, live)
    except Bail as e:
        if stats is not None:
            stats["bail"] = stats.get("bail", 0) + 1
            why = str(e).split(":")[0]
            stats.setdefault("why", {})[why] = stats.setdefault("why", {}).get(why, 0) + 1
        return None, (summarize_plain(body_lines) if live else None)


def _rewrite(body_lines, addr, regs, decl_line, reload, verify, stats, live):
    # Parse. The snapshot lines at the top (`uint64_t _cs_N = ...`) come
    # before the locals and stay as they are.
    first = 0
    while first < len(body_lines) and body_lines[first].strip().startswith("uint64_t _cs_"):
        first += 1
    lines = body_lines[first:]
    masked = [_mask(l) for l in lines]
    flat = []                               # (line index, node), top level, in order
    restore_lines = set()
    for li, m in enumerate(masked):
        nodes = _stmts(m, 0, len(m), regs)
        for nd in nodes:
            flat.append((li, nd))
            if nd[0] == "restore":
                if len(nodes) != 1:
                    raise Bail("a register restore sharing its line")
                restore_lines.add(li)
    atoms = _lower(flat)
    din = _dirty(atoms)
    n = len(atoms)
    # A return straight after a call or a tail jump: nothing was reloaded in
    # between, so the locals are not compared with the file there.
    stale = [False] * n
    for at in atoms:
        if at[K] in ("call", "tramp"):
            for sx in at[SUCC]:
                if atoms[sx][K] == "ret":
                    stale[sx] = True

    sites = [i for i in range(n) if atoms[i][K] in ("call", "sysc", "tramp", "ret", "end")]
    summary = None
    if live:
        bl, bt, after = _live(atoms)
        if regs.split and regs.crfs:
            # mfcr copies all of CR, but what is done with the copy says
            # which fields were wanted (_cr_word_fields): a prologue saves
            # it for the sake of cr2-cr4, a compare-to-number picks one
            # field out of it, a compare kept across calls is put back by
            # an mtcrf of its field. The other volatile fields ride along
            # and nothing takes them out again: that mfcr is no read of
            # them. (The copy then holds
            # what the locals hold, which for a field no caller wrote back
            # is an older value: a difference in bits of the save slot that
            # only a reader of someone else's frame could see.)
            redo = False
            for mi, fields in _cr_words(atoms, masked, bl):
                if atoms[mi][USE] & CRF & ~fields:
                    atoms[mi][USE] &= ~CRF | fields
                    redo = True
            if redo:
                bl, bt, after = _live(atoms)
        ssum = []
        for i in sites:
            at = atoms[i]
            k = at[K]
            d = din[i]
            if d is None:
                # Not reached from the entry: what follows a return inside a
                # function's range is as often a literal pool as code, and
                # its "transfers" and what "reads" them mean nothing.
                ssum.append((("X",), 0, 0, ()))
                continue
            dm = 0
            if d:
                for key in d:
                    dm |= CBIT.get(key, 0)
            if k == "call":
                al, at_ = after[i]
                ssum.append((at[EXTRA], dm, al, tuple(at_.items())))
            elif k == "sysc":
                al, at_ = after[i]
                ssum.append((("S",), dm, al, tuple(at_.items())))
            elif k == "tramp":
                ssum.append((at[EXTRA], dm, 0, ()))
            elif k == "ret":
                ssum.append((("R",), dm, 0, ()))
            else:
                ssum.append((("E",), dm, 0, ()))
        # A body that keeps CR or XER whole reads and rewrites the flags in
        # them with every compare and carry: all of them are in use, always.
        whole = (CRF if "cr" in regs.sprs else 0) | (XCA if ("xer" in regs.sprs and not regs.split) else 0)
        if whole:
            ssum = [(c, dm, (al | whole) if c[0] in ("C", "I", "J", "S") else al, at_)
                    for (c, dm, al, at_) in ssum]
        summary = {"in": (bl[0] | whole, tuple(bt[0].items())), "sites": ssum, "locals": regs.cand}
        # v20-v31: a function that assigns one saves it first (stvx: the
        # register is the source of a memcpy into guest memory) and restores
        # it. One that assigns one it never stores is a restore routine, or
        # breaks the ABI; with any such in the program the callers re-read.
        # r14-r31: what the function may hand back changed -- assigned and
        # not put back from its entry snapshot before a return or a tail
        # jump. A compiled function never does; an out-of-line restore
        # routine (_restgpr: `ld r30,-16(r12); ld r31,-8(r12); blr`) does
        # nothing else, and its caller's locals have to follow.
        nvout = 0
        for i in sites:
            if atoms[i][K] in ("ret", "tramp", "end") and din[i]:
                for key in din[i]:
                    if key[0] == "g" and key[1] >= 14:
                        nvout |= 1 << key[1]
        if nvout:
            summary["nvout"] = nvout
        nvg = 0
        for n_ in regs.gprs:
            if n_ >= 14:
                nvg |= 1 << n_
        summary["nvg"] = nvg
        if regs.vrs:
            stored = set()
            for mline in masked:
                for sm in RE_VR_STORE.finditer(mline):
                    stored.add(int(sm.group(1)))
            for at in atoms:
                if at[MAY] and any(key[0] == "v" and key[1] >= 20 and key[1] not in stored for key in at[MAY]):
                    summary["vclob"] = True
                    break

    if decl_line is None:
        return list(body_lines), summary

    # Emit.
    all_regs = regs.all
    n_flush = n_points = 0
    ins = {}                                # line -> [(pos, text)]
    tail = None
    for idx, i in enumerate(sites):
        at = atoms[i]
        k = at[K]
        d = din[i] if din[i] is not None else EMPTY
        if live:
            keep = frozenset(key for key in d if key not in CBIT)
            text = regs.store(keep)
            if any(key in CBIT for key in d):
                text = (text + " /*@NV%d@*/" % idx).strip()
        else:
            text = regs.store(d)
        if verify and not stale[i]:
            text = (regs.check(all_regs - d, addr) + " " + text).strip()
        n_points += 1
        n_flush += len(d)
        if k == "end":
            tail = text if din[i] is not None else None
            continue
        pre = text + " " if text else ""
        post = " " + reload if (k == "sysc" and reload) else ""
        if at[BARE] and (pre or post):
            pre, post = "{ " + pre, post + " }"
        if pre:
            ins.setdefault(at[LI], []).append((at[A0], pre))
        if post:
            ins.setdefault(at[LI], []).append((at[A1], post))
    if reload or live:
        # After a direct call, r14-r31 are re-read too if the callee hands
        # some back changed: expand() knows.
        after_call = {}
        if live:
            for idx, i in enumerate(sites):
                at = atoms[i]
                if at[K] == "call" and at[EXTRA][0] == "C" and any(n_ >= 14 for n_ in regs.gprs):
                    after_call[at[SUCC][0]] = idx
        for i, at in enumerate(atoms):
            if at[K] == "drain":
                text = reload
                if i in after_call:
                    text = (text + " /*@NVG%d@*/" % after_call[i]).strip()
                if not text:
                    continue
                if at[BARE]:
                    raise Bail("a drain as an if's whole body")
                ins.setdefault(at[LI], []).append((at[A1], " " + text))

    out_lines = list(body_lines[:first])
    out_lines.append(decl_line)
    for li, line in enumerate(lines):
        if li in restore_lines:
            nd = next(nd for l2, nd in flat if l2 == li)
            out_lines.append(f"    r{nd[3]} = _cs_{nd[4]}; ctx->gpr[{nd[3]}] = _cs_{nd[4]};")
            continue
        li_ins = ins.get(li)
        if not li_ins:
            out_lines.append(regs.rename(line))
            continue
        li_ins.sort(key=lambda t: t[0])
        parts, at0 = [], 0
        for pos, text in li_ins:
            parts.append(regs.rename(line[at0:pos]))
            parts.append(text)
            at0 = pos
        parts.append(regs.rename(line[at0:]))
        out_lines.append("".join(parts))
    if tail:
        out_lines.append("    " + tail)
    if stats is not None:
        stats["done"] = stats.get("done", 0) + 1
        stats["points"] = stats.get("points", 0) + n_points
        stats["stores"] = stats.get("stores", 0) + n_flush
        stats["stores_all"] = stats.get("stores_all", 0) + n_points * len(all_regs)
    return out_lines, summary


# --------------------------------------------------------------------------
# The whole program
# --------------------------------------------------------------------------

def solve(summaries, callable_addrs, hooked, fallthrough, ends=None, log=None):
    """need[addr] = the CAND mask to write back at each of the function's
    transfers (in the order of its summary's sites).

    summaries     addr -> summary (rewrite's), every lifted function
    callable_addrs  the functions a guest function pointer can name
    hooked        the functions whose func_<addr> is native code calling
                  the lifted body
    fallthrough   addr -> the function a body runs off its end into (absent:
                  the lifter makes that a return)
    ends          addr -> the address the function's body ends at
    """
    # The functions that start inside another's body: where its indirect
    # jumps can land.
    inner = {}
    if ends:
        order = sorted(summaries)
        import bisect
        for f in summaries:
            if any(c[0][0] == "J" for c in summaries[f]["sites"]) or summaries[f].get("plain"):
                lo = bisect.bisect_left(order, f)
                hi = bisect.bisect_left(order, ends.get(f, f))
                inner[f] = order[lo:hi]
    why_any, why_out = {}, {}
    live_in = {a: 0 for a in summaries}
    live_out = {a: RETS for a in summaries}   # native callers read the ABI's results
    callable_addrs = set(callable_addrs) & set(summaries)
    # An indirect call is held to the ABI: its target reads the argument
    # registers, and what follows it reads the result registers. Followed
    # through the callees instead, both sets fill up with every register
    # within a few rounds, from reads that never happen: a call that does
    # not return (an assertion's handler) runs, as far as the text shows,
    # into whatever block comes next, which reads registers set on quite
    # another path. A scratch register read like that across an indirect
    # call would be read uninitialised. What a callable function reads
    # ITSELF before writing it, and what a function reads itself straight
    # after an indirect call, is added all the same, and reported.
    # (The flags are among them: cr1 holds the bit a variadic function reads
    # to know whether it was passed floating-point arguments.)
    st = {"any_in": ARGS, "ind_out": 0}
    for f in callable_addrs:
        extra = summaries[f]["in"][0] & ~st["any_in"]
        if extra:
            for k in CAND:
                if extra & CBIT[k]:
                    why_any.setdefault(Regs.local(k), []).append(f)
    for f in callable_addrs:
        st["any_in"] |= summaries[f]["in"][0]
    for f, s in summaries.items():
        for cls, _dm, al, _at in s["sites"]:
            if cls[0] == "I" and al & ~RETS:
                for k in CAND:
                    if al & ~RETS & CBIT[k]:
                        why_out.setdefault(Regs.local(k), []).append(f)
                st["ind_out"] |= al

    def val_in(f, cls):
        """What the other side of a transfer reads while F's caller waits."""
        c = cls[0]
        if c == "C" or c == "T":
            g = cls[1]
            if g not in summaries:
                return ALLC                   # not lifted: unknown code
            return live_in[g] | (ARGS if g in hooked else 0)
        if c == "I":
            return st["any_in"] | CTR
        if c == "H":
            return ARGS
        if c == "R":
            return 0
        if c == "E":
            g = fallthrough.get(f)
            if g is None:
                return 0                      # a return
            return live_in[g] if g in summaries else ALLC
        if c == "S":
            return SYSC
        if c == "J" and ends:
            v = st["any_in"] | CTR
            for g in inner.get(f, ()):
                v |= live_in[g]
            return v
        return ALLC

    def val_full(f, cls):
        """The same, and what is read once F's caller runs again."""
        c = cls[0]
        if c == "R":
            return live_out[f]
        if c == "T" or c == "E" or c == "J":
            return val_in(f, cls) | live_out[f]   # the target returns to F's caller
        return val_in(f, cls)

    def ev(f, pair, val):
        l, toks = pair
        for cls, m in toks:
            l |= m & val(f, cls)
        return l

    def raise_out(g, m):
        if g in summaries and m | live_out[g] != live_out[g]:
            live_out[g] |= m
            return True
        return False

    rounds = 0
    changed = True
    while changed:
        changed = False
        rounds += 1
        for f, s in summaries.items():
            li = ev(f, s["in"], val_in)
            if li | live_in[f] != live_in[f]:
                live_in[f] |= li
                changed = True
            if f in callable_addrs and st["ind_out"] | live_out[f] != live_out[f]:
                live_out[f] |= st["ind_out"]
                changed = True
            lo = live_out[f]
            if s.get("plain"):
                # It reloads and may read everything after each of its calls.
                for cls, _m in s["in"][1]:
                    c = cls[0]
                    if c == "C":
                        changed |= raise_out(cls[1], ALLC)
                    elif c == "J" and st["ind_out"] != ALLC:
                        st["ind_out"] = ALLC
                        changed = True
                    elif c == "T":
                        changed |= raise_out(cls[1], lo)
                    elif c == "E" and fallthrough.get(f) is not None:
                        changed |= raise_out(fallthrough[f], lo)
                continue
            for cls, _dm, al, at in s["sites"]:
                c = cls[0]
                if c == "C":
                    changed |= raise_out(cls[1], ev(f, (al, at), val_full))
                elif c == "T":
                    changed |= raise_out(cls[1], lo)
                elif c == "E" and fallthrough.get(f) is not None:
                    changed |= raise_out(fallthrough[f], lo)
    need = {}
    for f, s in summaries.items():
        out = []
        for cls, _dm, al, at in s["sites"]:
            c = cls[0]
            if c in ("C", "I", "S"):
                out.append(ev(f, (al, at), val_full) | val_in(f, cls))
            else:
                out.append(val_full(f, cls))  # J, T, E, R: nothing of F runs after it
        need[f] = out
    if log is not None:
        log.update(rounds=rounds, any_in=st["any_in"], ind_out=st["ind_out"],
                   live_in=live_in, live_out=live_out, why_any=why_any, why_out=why_out)
    return need


def names(mask):
    return " ".join(Regs.local(k) for k in CAND if mask & CBIT[k])


def expand(line, summary, need, verify=False, counts=None, vr_reload=True, nvout=None):
    """A body line with its /*@NVn@*/ marks replaced by the write-backs of
    the CAND registers that changed and are live at that transfer, its
    /*@NVVa,b@*/ marks by the re-read of those vector registers, or nothing,
    and its /*@NVGn@*/ marks by the re-read of the r14-r31 the callee at
    transfer n hands back changed (nvout: addr -> mask)."""
    if "/*@NVG" in line:
        def regs_back(m):
            callee = summary["sites"][int(m.group(1))][0]
            back = (nvout or {}).get(callee[1], 0) & summary.get("nvg", 0) if callee[0] == "C" else 0
            return "".join(" r%d = ctx->gpr[%d];" % (n, n) for n in range(14, 32) if back >> n & 1)
        line = RE_MARKG.sub(regs_back, line)
    if "/*@NVV" in line:
        line = RE_MARKV.sub((lambda m: " " + " ".join("v%s = ctx->vr[%s];" % (n, n) for n in m.group(1).split(",")))
                            if vr_reload else "", line)
    def one(m):
        idx = int(m.group(1))
        dm = summary["sites"][idx][1] & summary["locals"]
        keep = dm & need[idx]
        out, crs = [], []
        for k in CAND:
            if not keep & CBIT[k]:
                continue
            if k[0] == "c":
                crs.append(k[1])
            elif k == ("s", "xca"):
                out.append("ctx->xer = (ctx->xer & ~0x20000000u) | xca;")
            else:
                out.append(f"{Regs.file(k)} = {Regs.local(k)};")
        if crs:
            out.append(_cr_store(crs))
        if counts is not None:
            counts[0] += bin(dm).count("1")
            counts[1] += bin(keep).count("1")
        if verify:
            drop, pm = dm & ~keep, 0
            for k in CAND:
                if not drop & CBIT[k]:
                    continue
                if k[0] == "c":
                    pm |= 0xF << (28 - 4 * k[1])
                elif k == ("s", "xca"):
                    out.append("ctx->xer |= 0x20000000u;")
                else:
                    out.append("PPU_NV_POISON_%s(%s);" % (k[0].upper(), k[1]))
            if pm:
                out.append("ctx->cr |= 0x%08Xu;" % pm)
        return " ".join(out) + " " if out else ""
    return RE_MARK.sub(one, line)
