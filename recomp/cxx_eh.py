"""MSVC x86 C++ exception-handling tables, recovered for the generator.

Opt-in generation switch GUEST_CXX_EH=1 (default 0: the corpus is byte-for-
byte what it was without this module).  The runtime half lives in
runtime/guest_cxx_eh.c and is compiled only with GUEST_CXX_EH=1.

What MSVC emits for a function with an EH frame (/EHsc, x86):

    push ebp / mov ebp, esp / push -1 / push __ehhandler$F / push fs:[0] ...
    mov fs:[0], <ebp-0Ch>               ; registration node RN = EBP-0Ch
    mov [ebp-10h], esp                  ; only with try blocks: catch ESP
    ...
    __ehhandler$F:  (cookie checks) ; mov eax, offset FuncInfo
                                    ; jmp __CxxFrameHandler3

FuncInfo (magic 0x1993052x) names every unwind action (destructor funclet)
and every try block with its catch funclets.  A catch funclet runs with EBP
set to the owner's frame and returns the address at which the owner resumes
(`mov eax, offset $continuation ; ret`).

Nothing in the ordinary call graph reaches these funclets, and nothing
reaches a continuation except that RET.  The generator therefore adds:

  * every catch and unwind funclet as a supplemental translated root, so the
    runtime can call it through guest_call;
  * every continuation as a supplemental root too: the rest of the owner
    from that address, ending in the owner's own epilogue RET;
  * at the owner's `push __ehhandler$F` (in every body that decodes it), a
    native landing site (guest_eh_site + setjmp).  The runtime restores
    EBP/ESP/fs:[0] and longjmps there; the second setjmp return tail-calls
    the continuation root and returns.  Its RET pops the owner's return
    word, so the owner activation has nothing left to do but return.

The owner bodies keep their exact decode (no new entries), so every
pinned seam proof over them still holds.  A continuation root is a second
copy of the owner's tail; the two (pinned) whose tail holds a Vita seam
window are faulting stubs instead (SEAM_BLOCKED_CONTINUATIONS).

Everything is derived from the PE: relocation-backed `push imm32` of a
handler thunk whose last two instructions are `mov eax, FuncInfo` and
`jmp <__CxxFrameHandler3 import thunk>`.  The census is pinned so a
different PE fails closed instead of silently changing the EH model.
"""
from __future__ import annotations

import collections
import hashlib
import os
import struct

from capstone import x86 as cx

import trans as T

ENV = "GUEST_CXX_EH"

FRAME_HANDLER_IMPORT = "VCRUNTIME140.dll!__CxxFrameHandler3"
THROW_IMPORT = "VCRUNTIME140.dll!_CxxThrowException"
FUNCINFO_MAGICS = (0x19930520, 0x19930521, 0x19930522)

# Frozen census of the supported PE (isaac-ng.exe 1.7.9b J835).
EXPECTED_FUNCINFOS = 1117
EXPECTED_TRY_FUNCINFOS = 161
EXPECTED_TRY_BLOCKS = 165
EXPECTED_CATCHES = 165
EXPECTED_ANCHORS = 161
EXPECTED_CONTINUATIONS = 119
EXPECTED_FUNCLETS = 4381          # distinct catch + unwind funclet entries
# Base-independent digest of every landing site, catch row and unwind row.
EXPECTED_DIGEST = (
    "0ed95a58b991e4362d22f7227435af72ae31a9a0f0e6d7caf012b16e85fdb7cd")

# Continuation roots whose rest-of-owner decode runs into an instruction
# window that a Vita seam pins to its owner's exact decode (lazy-SFX
# LoadConfig in SoundEffect config parsing, FXLayers::Init NULL rollback).
# A second copy of such a window would bypass those proofs, so these two
# continuations are emitted as loud faulting stubs instead: a throw caught
# by their owner stops with "untranslated C++ catch continuation" after the
# catch block ran.  The generator requires exactly this set to refuse.
SEAM_BLOCKED_CONTINUATIONS = frozenset((
    0x00488515,
    0x004E870B,
))

# Module global for the emitter (set in the parent and in every spawn worker,
# like leaf_inline.TABLE).  None: the feature is off.
TABLE = None


def enabled() -> bool:
    value = os.environ.get(ENV, "0")
    if value not in ("0", "1"):
        raise ValueError("%s must be 0 or 1, not %r" % (ENV, value))
    return value == "1"


def _u32(img, rva):
    return struct.unpack_from("<I", img.mem, rva)[0]


def _import_thunks(img, slot_rva):
    """`jmp dword ptr [slot]` linker thunks of one IAT slot."""
    pattern = b"\xff\x25" + struct.pack("<I", img.va(slot_rva))
    found = []
    start = img.text_rva
    data = img.mem
    while True:
        at = data.find(pattern, start, img.text_end)
        if at < 0:
            return tuple(found)
        found.append(at)
        start = at + 1


def _disasm_one(img, rva):
    got = list(T.CS.disasm(img.code_at(rva, 16), rva, count=1))
    if not got:
        raise RuntimeError("C++ EH: undecodable bytes at %08x" % rva)
    return got[0]


def _handler_thunk_funcinfo(img, start, frame_thunks):
    """Linear decode of a candidate `__ehhandler$` thunk.  Returns the
    FuncInfo RVA when it ends in `mov eax, imm32 ; jmp <frame handler>`."""
    rva = start
    previous = None
    for _ in range(24):
        ins = _disasm_one(img, rva)
        m = ins.mnemonic
        if m == "jmp":
            op = ins.operands[0]
            if (op.type == cx.X86_OP_IMM and
                    (op.imm & 0xFFFFFFFF) in frame_thunks and
                    previous is not None and previous.mnemonic == "mov" and
                    previous.operands[0].type == cx.X86_OP_REG and
                    previous.operands[0].reg == cx.X86_REG_EAX and
                    previous.operands[1].type == cx.X86_OP_IMM and
                    previous.address + 1 in img.reloc_rvas):
                return (previous.operands[1].imm & 0xFFFFFFFF) - img.base
            return None
        if m in ("ret", "int3") or m.startswith("j") or m == "call" and (
                ins.operands[0].type != cx.X86_OP_IMM):
            return None
        previous = ins
        rva += ins.size
    return None


def _parse_funcinfo(img, fi):
    magic, max_state, p_unwind, n_try, p_try = struct.unpack_from(
        "<IiIII", img.mem, fi)
    if magic not in FUNCINFO_MAGICS or not 0 <= max_state < 0x10000:
        raise RuntimeError("C++ EH: bad FuncInfo at %08x" % fi)
    unwind = []
    for state in range(max_state):
        to_state, action = struct.unpack_from(
            "<iI", img.mem, p_unwind - img.base + 8 * state)
        unwind.append((to_state, action - img.base if action else 0))
    tries = []
    for index in range(n_try):
        low, high, catch_high, n_catch, p_handlers = struct.unpack_from(
            "<iiiiI", img.mem, p_try - img.base + 20 * index)
        catches = []
        for k in range(n_catch):
            adjectives, p_type, disp, handler = struct.unpack_from(
                "<IIiI", img.mem, p_handlers - img.base + 16 * k)
            catches.append((adjectives, p_type - img.base if p_type else 0,
                            disp, handler - img.base))
        tries.append((low, high, catch_high, tuple(catches)))
    return {"max_state": max_state, "unwind": tuple(unwind),
            "tries": tuple(tries)}


def _catch_continuations(img, funclet, throw_thunks):
    """Recursive descent of one catch funclet, local to the funclet.

    Every path must end in `mov eax, offset $cont ; ret` (a relocated
    immediate: the resume address) or in a call of the throw thunk (a
    rethrow / new throw; MSVC never returns from it) or at the int3 MSVC
    places after any other noreturn call (`catch (...) { terminate(); }`)."""
    continuations = set()
    work = [funclet]
    seen = set()
    while work:
        rva = work.pop()
        previous = None
        while True:
            if rva in seen:
                break
            seen.add(rva)
            if not img.is_code(rva):
                raise RuntimeError(
                    "C++ EH: catch funclet %08x leaves .text" % funclet)
            ins = _disasm_one(img, rva)
            m = ins.mnemonic
            if m == "ret":
                if (previous is None or previous.mnemonic != "mov" or
                        previous.operands[0].type != cx.X86_OP_REG or
                        previous.operands[0].reg != cx.X86_REG_EAX or
                        previous.operands[1].type != cx.X86_OP_IMM or
                        previous.address + 1 not in img.reloc_rvas or
                        previous.address + previous.size != rva):
                    raise RuntimeError(
                        "C++ EH: catch funclet %08x returns without a "
                        "continuation at %08x" % (funclet, rva))
                continuations.add(
                    (previous.operands[1].imm & 0xFFFFFFFF) - img.base)
                break
            if m == "call" and ins.operands[0].type == cx.X86_OP_IMM and (
                    ins.operands[0].imm & 0xFFFFFFFF) in throw_thunks:
                break
            if m == "int3":
                # After a noreturn call (`terminate()`, a throw helper).
                break
            if m == "jmp":
                op = ins.operands[0]
                if op.type != cx.X86_OP_IMM:
                    raise RuntimeError(
                        "C++ EH: indirect jump in catch funclet %08x" %
                        funclet)
                previous = None
                rva = op.imm & 0xFFFFFFFF
                continue
            if m.startswith("j"):
                op = ins.operands[0]
                work.append(op.imm & 0xFFFFFFFF)
            previous = ins
            rva += ins.size
    return continuations


def _rows_sha256(rows):
    digest = hashlib.sha256()
    for row in rows:
        for value in row:
            digest.update((value & 0xFFFFFFFF).to_bytes(4, "little"))
    return digest.hexdigest()


def discover(img, imports):
    """Return the EH table for `img` (any base; outputs are RVAs)."""
    by_name = {name: slot for slot, name in imports}
    # Capstone decodes at RVAs, so direct branch targets are RVAs too.
    frame_thunks = frozenset(
        _import_thunks(img, by_name[FRAME_HANDLER_IMPORT]))
    throw_thunks = frozenset(_import_thunks(img, by_name[THROW_IMPORT]))
    if len(frame_thunks) != 1 or len(throw_thunks) != 1:
        raise RuntimeError("C++ EH: import thunk census changed")

    # Relocation-backed `push imm32` of a .text address, by pushed target.
    pushes = collections.defaultdict(list)
    for reloc in sorted(img.reloc_rvas):
        if not (img.is_code(reloc - 1) and img.is_code(reloc + 3)):
            continue
        if img.mem[reloc - 1] != 0x68:
            continue
        target = _u32(img, reloc) - img.base
        if img.is_code(target):
            pushes[target].append(reloc - 1)

    handler_funcinfo = {}
    for target in sorted(pushes):
        fi = _handler_thunk_funcinfo(img, target, frame_thunks)
        if fi is not None:
            handler_funcinfo[target] = fi

    funcinfos = {}
    for fi in sorted(set(handler_funcinfo.values())):
        funcinfos[fi] = _parse_funcinfo(img, fi)

    anchors = {}
    funclets = set()
    catch_rows = []
    unwind_rows = []
    for handler, fi in sorted(handler_funcinfo.items()):
        info = funcinfos[fi]
        for state, (to_state, action) in enumerate(info["unwind"]):
            unwind_rows.append((fi, state, to_state, action))
            if action:
                funclets.add(action)
        if not info["tries"]:
            continue
        continuations = set()
        for low, high, catch_high, catches in info["tries"]:
            for adjectives, p_type, disp, funclet in catches:
                catch_rows.append((fi, low, high, catch_high, adjectives,
                                   p_type, disp & 0xFFFFFFFF, funclet))
                funclets.add(funclet)
                continuations |= _catch_continuations(
                    img, funclet, throw_thunks)
        for site in pushes[handler]:
            # The owner's own `push -1` (trylevel) precedes the handler push.
            if img.code_at(site - 2, 2) != b"\x6a\xff":
                raise RuntimeError(
                    "C++ EH: handler push %08x is not an MSVC EH prologue"
                    % site)
            anchors[site] = {
                "handler": handler,
                "funcinfo": fi,
                "continuations": tuple(sorted(continuations)),
            }

    table = {
        "anchors": anchors,
        "funclets": tuple(sorted(funclets)),
        "funcinfos": len(funcinfos),
        "try_funcinfos": sum(1 for info in funcinfos.values()
                             if info["tries"]),
        "try_blocks": sum(len(info["tries"]) for info in funcinfos.values()),
        "catches": len(catch_rows),
        "continuations": len({c for spec in anchors.values()
                              for c in spec["continuations"]}),
    }
    table["digest"] = _rows_sha256(
        [(site, spec["handler"], spec["funcinfo"]) + spec["continuations"]
         for site, spec in sorted(anchors.items())] +
        sorted(catch_rows) + sorted(unwind_rows))
    return table


def validate(table):
    census = {
        "funcinfos": EXPECTED_FUNCINFOS,
        "try_funcinfos": EXPECTED_TRY_FUNCINFOS,
        "try_blocks": EXPECTED_TRY_BLOCKS,
        "catches": EXPECTED_CATCHES,
    }
    actual = {key: table[key] for key in census}
    if actual != census or len(table["anchors"]) != EXPECTED_ANCHORS:
        raise RuntimeError("C++ EH census changed: %r anchors=%d"
                           % (actual, len(table["anchors"])))
    for key, expected in (("continuations", EXPECTED_CONTINUATIONS),
                          ("digest", EXPECTED_DIGEST)):
        if expected is not None and table[key] != expected:
            raise RuntimeError("C++ EH %s changed: %r != %r"
                               % (key, table[key], expected))
    if (EXPECTED_FUNCLETS is not None and
            len(table["funclets"]) != EXPECTED_FUNCLETS):
        raise RuntimeError("C++ EH funclet census changed: %d != %d"
                           % (len(table["funclets"]), EXPECTED_FUNCLETS))
    return table


def anchors_in(insns):
    """Anchor sites decoded into one body, in address order."""
    if TABLE is None:
        return ()
    return tuple(site for site in sorted(TABLE["anchors"]) if site in insns)


def supplemental_roots(table):
    """Funclets and continuations, the roots the EH model calls into."""
    out = set(table["funclets"])
    for spec in table["anchors"].values():
        out.update(spec["continuations"])
    return tuple(sorted(out))


def site_var(site):
    return "_guest_eh_site_%08x" % site


def render_declarations(sites):
    """Body-scope landing records; the cleanup unlinks one on every C return
    (a nonlocal exit is pruned by the runtime instead)."""
    lines = []
    for site in sites:
        lines.append("    guest_eh_site %s GUEST_EH_SITE_CLEANUP;"
                     % site_var(site))
        lines.append("    %s.active = 0U;" % site_var(site))
    return lines


def render_anchor(site, emit_base, known):
    """Lines after the owner's `push __ehhandler$F`.

    `guest_eh_frame_enter` records RN = ESP-4 (the next push stores the old
    fs:[0] there).  The native setjmp returns again only when the runtime
    resumes a catch continuation of exactly this registration: it has then
    restored EBP/ESP/fs:[0] and stored the guest continuation address.  The
    CPU struct is the register file on that path (the seam bracket of
    gpr_locals encloses this block), so the tail call needs no flush."""
    spec = TABLE["anchors"][site]
    var = site_var(site)
    lines = [
        "    /* C++ EH landing site: FuncInfo %08x, %d continuation(s) */"
        % (spec["funcinfo"], len(spec["continuations"])),
        "    guest_eh_frame_enter(c, &%s, 0x%08xU);"
        % (var, (emit_base + spec["handler"]) & 0xFFFFFFFF),
        "    if (setjmp(%s.native_env) != 0) {" % var,
    ]
    for continuation in spec["continuations"]:
        if continuation not in known:
            raise RuntimeError(
                "C++ EH continuation %08x of %08x is not a translated root"
                % (continuation, site))
        lines.append(
            "        if (%s.continuation == 0x%08xU) { sub_%08x(c); return; }"
            % (var, (emit_base + continuation) & 0xFFFFFFFF,
               continuation))
    lines.append(
        "        guest_fault(c, %s.continuation, "
        "\"C++ catch continuation is not in its owner\");" % var)
    lines.append("        return;")
    lines.append("    }")
    return lines
