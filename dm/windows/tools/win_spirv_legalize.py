"""Legalize the stock NVN translator's binary SPIR-V for Windows Vulkan.

GMEM helpers intentionally have a narrow input contract: recognize the stock
translator's raw lookup, address calculation and miss behavior, or reject the
module before it is marked checked. The host verifies 16-byte-aligned guest
intervals and device-address deltas, so a naturally aligned access that starts
inside an interval is wholly inside it (also for a subword's containing word).
Read-only compute can also convert a blocked pool delta into a GPU fault
without dereferencing the staged image's backing store.
"""
from collections import defaultdict
from functools import cache
from pathlib import Path
import runpy
import struct

# SPIR-V core opcodes and operands (SPIRV-Headers unified1 grammar).
NAME, ENTRY_POINT, CAPABILITY = 5, 15, 17
TYPE_VOID, TYPE_BOOL, TYPE_INT, TYPE_VECTOR, TYPE_IMAGE = 19, 20, 21, 23, 25
TYPE_RUNTIME_ARRAY, TYPE_STRUCT, TYPE_POINTER = 29, 30, 32
CONSTANT_FALSE, CONSTANT, CONSTANT_NULL = 42, 43, 46
FUNCTION, PARAMETER, FUNCTION_END, CALL = 54, 55, 56, 57
VARIABLE, LOAD, STORE, ACCESS, INBOUNDS_ACCESS = 59, 61, 62, 65, 66
DECORATE, MEMBER_DECORATE = 71, 72
COMPOSITE, EXTRACT, COPY = 80, 81, 83
IMAGE_FETCH, IMAGE_READ, IMAGE_WRITE = 95, 98, 99
CONVERT_TO_PTR, BITCAST, ADD, MUL = 120, 124, 128, 132
LOGICAL_NOT, SELECT, EQUAL, NOT_EQUAL, LESS_U = 168, 169, 170, 171, 176
SHR, SAR, SHL, AND, INSERT, EXTRACT_S, EXTRACT_U = 194, 195, 196, 199, 201, 202, 203
ATOMIC_LOAD, ATOMIC_STORE, ATOMIC_OR, ATOMIC_XOR = 227, 228, 241, 242
PHI, LOOP_MERGE, SELECTION_MERGE, LABEL, BRANCH, BRANCH_COND = 245, 246, 247, 248, 249, 250
RETURN, RETURN_VALUE, UNREACHABLE, MODULE_PROCESSED = 253, 254, 255, 330
UNIFORM, PRIVATE, FUNCTION_STORAGE, STORAGE_BUFFER, PHYSICAL_STORAGE = 2, 6, 7, 12, 5349
PHYSICAL_CAP, FORMATLESS_READ, FORMATLESS_WRITE = 5347, 55, 56
IMAGE_OFFSET = 0x10
COMPUTE, ALIGNED, DEVICE_SCOPE, RELAXED = 5, 2, 1, 0
BLOCK, BUFFER_BLOCK, ARRAY_STRIDE, NON_WRITABLE, OFFSET = 2, 3, 6, 24, 35
_READ_ALIAS = b'nxnative.gmem.read-alias.v1'
_READ_ALIAS_WORDS = struct.unpack(f'<{len(_READ_ALIAS)//4+1}I', _READ_ALIAS + bytes(4 - len(_READ_ALIAS) % 4))


@cache
def gmem_layout():
    # Use the native ABI's parser without depending on the caller's sys.path.
    abi = runpy.run_path(str(Path(__file__).resolve().parents[2] / 'gfx/tools/dmsh_abi.py'))['VALUES']
    stride = abi['GMEM_MAP_ENTRY'] // 4
    return stride, abi['GMEM_MAP_ENTRIES'] * stride


def require(condition, message):
    if not condition:
        raise ValueError('Vulkan SPIR-V: ' + message)


class Module:
    def __init__(self, raw):
        require(len(raw) >= 20 and len(raw) % 4 == 0, 'truncated module')
        words = struct.unpack(f'<{len(raw) // 4}I', raw)
        self.header = list(words[:5])
        require(words[0] == 0x07230203, 'bad module magic')
        self.ops, self.types, self.defs, self.added_globals = [], {}, {}, []
        self.functions, self.caps = {}, set()
        self.constants = {}
        start = None
        i = 5
        while i < len(words):
            op, size = words[i] & 0xffff, words[i] >> 16
            require(size and i + size <= len(words), 'invalid instruction extent')
            a = words[i + 1:i + size]
            at = len(self.ops)
            self.ops.append((op, a))
            if 19 <= op <= 38:
                self.types[a[0]] = (op, a[1:])
            elif op == CAPABILITY:
                self.caps.add(a[0])
            elif op == FUNCTION:
                start = at
            elif op == FUNCTION_END:
                require(start is not None, 'unmatched function end')
                self.functions[self.ops[start][1][1]] = (start, at)
                start = None
            # A function-body instruction with a type ID as its first operand
            # has a typed result. Exclude literal-only instructions by only
            # indexing actual result IDs (unique in the module).
            if (start is not None or op in (1, 41, 42, 43, 44, 46, VARIABLE)) and len(a) >= 2 and a[0] in self.types:
                require(a[1] not in self.defs, 'duplicate result ID')
                self.defs[a[1]] = (op, a)
                if op == CONSTANT and len(a) == 3:
                    self.constants.setdefault((a[0], a[2]), a[1])
            i += size
        require(start is None, 'unterminated function')

    def allocate(self):
        n = self.header[3]
        self.header[3] += 1
        return n

    def constant(self, type_id, value):
        at = self.constants.get((type_id, value))
        if at is None:
            at = self.allocate()
            self.constants[type_id, value] = at
            self.added_globals.append((CONSTANT, (type_id, at, value)))
        return at

    def typed(self, id_):
        require(id_ in self.defs, f'unknown type of %{id_}')
        return self.defs[id_][1][0]

    def value(self, id_):
        op, a = self.defs.get(id_, (None, ()))
        if op == CONSTANT and len(a) == 3:
            return a[2]
        if op in (CONSTANT_NULL, CONSTANT_FALSE):
            return 0
        return None

    def pointer(self, id_, storage):
        op, args = self.types.get(id_, (None, ()))
        return args[1] if op == TYPE_POINTER and args[0] == storage else None

    def emission(self, insert, replace, caps):
        first_func = min(start for start, _ in self.functions.values())
        first_cap = next(i for i, (op, _) in enumerate(self.ops) if op == CAPABILITY)
        insert[first_func][:0] = self.added_globals
        insert[first_cap][:0] = [(CAPABILITY, (cap,)) for cap in sorted(caps - self.caps)]
        words = self.header
        for i, item in enumerate(self.ops):
            for op, args in (*insert.get(i, ()), replace.get(i, item)):
                words.append(((1 + len(args)) << 16) | op)
                words.extend(args)
        return struct.pack(f'<{len(words)}I', *words)


def legalize_images(module, insert, replace):
    caps = set()
    for i, (op, a) in enumerate(module.ops):
        if op not in (IMAGE_READ, IMAGE_WRITE, IMAGE_FETCH):
            continue
        if op == IMAGE_FETCH and (len(a) < 5 or not a[4] & IMAGE_OFFSET):
            continue
        image = module.typed(a[0] if op == IMAGE_WRITE else a[2])
        code, dim = module.types[image]
        require(code == TYPE_IMAGE and len(dim) >= 7, 'image operand is not an image type')
        if op in (IMAGE_READ, IMAGE_WRITE):
            if dim[6] == 0 and dim[1] != 6:  # Unknown, excluding input attachments (SubpassData)
                caps.add(FORMATLESS_READ if op == IMAGE_READ else FORMATLESS_WRITE)
            continue

        # Image operands appear in mask-bit order. Bias, Lod and ConstOffset
        # occupy one word each, Grad two. Keep Lod/Sample and other operands.
        mask = a[4]
        at = 5 + bool(mask & 1) + bool(mask & 2) + 2 * bool(mask & 4) + bool(mask & 8)
        require(at < len(a) and not mask & 8, 'bad dynamic fetch offset operands')
        offset, coord = a[at], a[3]
        dims, array = {0: 1, 1: 2, 2: 3, 4: 2, 5: 1}.get(dim[1]), dim[3]
        require(dims is not None and array in (0, 1), 'unsupported fetch coordinate dimension')
        coord_type, offset_type = module.typed(coord), module.typed(offset)
        code, args = module.types[offset_type]
        scalar = args[0] if code == TYPE_VECTOR else offset_type
        scalar_op, scalar_args = module.types[scalar]
        require(scalar_op == TYPE_INT and scalar_args[0] == 32 and
                (dims == 1 and code == TYPE_INT or code == TYPE_VECTOR and args == (scalar, dims)),
                'fetch offset is not an integer spatial coordinate')
        if array:
            require(module.types[coord_type] == (TYPE_VECTOR, (scalar, dims + 1)), 'bad array fetch coordinates')
            parts = [offset]
            if dims > 1:
                parts = []
                for axis in range(dims):
                    part = module.allocate()
                    insert[i].append((EXTRACT, (scalar, part, offset, axis)))
                    parts.append(part)
            padded = module.allocate()
            insert[i].append((COMPOSITE, (coord_type, padded, *parts, module.constant(scalar, 0))))
            offset = padded
        else:
            require(coord_type == offset_type, 'fetch offset and coordinate type mismatch')
        result = module.allocate()
        insert[i].append((ADD, (coord_type, result, coord, offset)))
        flags = (mask ^ IMAGE_OFFSET,) if mask != IMAGE_OFFSET else ()
        replace[i] = (op, (*a[:3], result, *flags, *a[5:at], *a[at+1:]))
    return caps


class RawHelper:
    def __init__(self, module, start, end, physical, lookup, map_id):
        self.m, self.start = module, start
        self.body = module.ops[start:end]
        self.stores = defaultdict(list)
        for op, a in self.body:
            if op == STORE:
                self.stores[a[0]].append(a[1])
        params = [a for op, a in self.body if op == PARAMETER]
        require(len(params) >= 2, 'raw helper has no guest address parameters')
        self.low, self.high = params[0][1], params[1][1]
        self.i32 = module.pointer(params[0][0], FUNCTION_STORAGE)
        require(module.types.get(self.i32) in ((TYPE_INT, (32, 0)), (TYPE_INT, (32, 1))) and
                module.pointer(params[1][0], FUNCTION_STORAGE) == self.i32, 'raw address has an unexpected parameter type')
        calls = [a for op, a in self.body if op == CALL and a[2] == lookup]
        require(len(calls) == 1 and len(calls[0]) == 5, 'physical pointers are not in a raw lookup helper')
        for arg, register in zip(calls[0][3:], (self.low, self.high)):
            require(self.is_load(self.single_store(arg), register), 'raw lookup does not take its guest address parameters')

        origins, lane_counts = set(), set()
        pointers = {}
        for a in physical:
            pointee = module.pointer(a[0], PHYSICAL_STORAGE)
            require(pointee is not None, 'not a physical storage buffer pointer')
            op, args = module.types[pointee]
            scalar, lanes = args if op == TYPE_VECTOR else (pointee, 1)
            require(module.types[scalar] == (TYPE_INT, (32, 0)) and lanes in (1, 2, 4), 'unsupported physical access width')
            lane_counts.add(lanes)
            pointers[a[1]] = pointee

            op, cast = module.defs[self.peel(a[2])]
            # peel does not retain the bitcast to uint64: its source is the
            # two address words constructed by GetStoragePointer.
            require(op == COMPOSITE and len(cast) == 4, 'unexpected physical pointer address encoding')
            op, add = module.defs[self.peel(cast[2])]
            require(op == ADD and len(add) == 4, 'physical address is not guest low plus pool delta')
            candidates = []
            for word, delta in ((add[2], add[3]), (add[3], add[2])):
                origin = self.word_origin(word)
                if origin and self.map_load(delta, map_id):
                    candidates.append(origin)
            require(len(candidates) == 1, 'cannot prove physical word uses the guest address and pool delta')
            origins.add(candidates[0])
        self.check_pointer_uses(pointers)
        require(len(lane_counts) == 1 and len(origins) == 1, 'mixed raw helper access widths')
        self.lanes = lane_counts.pop()
        self.alignment = self.lanes * 4
        if origins.pop() == 'subword':
            require(self.lanes == 1, 'subword address used by a vector access')
            widths = set()
            for op, a in self.body:
                if op in (INSERT, EXTRACT_S, EXTRACT_U) and self.from_word(a[2], pointers):
                    widths.add(module.value(a[-1]))
                elif op == AND:
                    for value, mask in ((a[2], a[3]), (a[3], a[2])):
                        mask = module.value(mask)
                        if mask in (0xff, 0xffff) and self.from_word(value, pointers):
                            widths.add(8 if mask == 0xff else 16)
            require(widths in ({8}, {16}), 'cannot prove raw subword width')
            self.alignment = widths.pop() // 8

    def check_pointer_uses(self, pointers):
        used = set()
        for op, a in self.body:
            if op == CONVERT_TO_PTR:
                continue
            if len(a) >= 2 and a[0] in self.m.types:
                require(self.m.pointer(a[0], PHYSICAL_STORAGE) is None,
                        'physical pointer escapes its direct raw access')
            if op == LOAD or ATOMIC_LOAD <= op <= ATOMIC_XOR and op != ATOMIC_STORE:
                type_id, _, ptr = a[:3]
            elif op in (STORE, ATOMIC_STORE):
                ptr = a[0]
                if ptr not in pointers:
                    if op == STORE:
                        require(a[1] not in pointers, 'raw pointer stored as a value')
                    continue
                type_id = self.m.typed(a[1] if op == STORE else a[3])
            else:
                if op == CALL:
                    require(not any(arg in pointers for arg in a[3:]), 'raw pointer passed to another function')
                continue
            if ptr in pointers:
                require(type_id == pointers[ptr], 'physical access width disagrees with its pointer')
                used.add(ptr)
        require(used == pointers.keys(), 'physical pointer has no direct raw access')

    def single_store(self, ref):
        op, a = self.m.defs.get(ref, (None, ()))
        values = self.stores[ref]
        return values[0] if op == VARIABLE and a[2] == FUNCTION_STORAGE and len(values) == 1 else None

    def peel(self, value):
        seen = set()
        while value in self.m.defs and value not in seen:
            seen.add(value)
            op, a = self.m.defs[value]
            if op in (BITCAST, COPY):
                value = a[2]
            elif op == LOAD and (stored := self.single_store(a[2])) is not None:
                value = stored
            else:
                break
        return value

    def is_load(self, value, ptr):
        op, a = self.m.defs.get(self.peel(value), (None, ()))
        return op == LOAD and a[2] == ptr

    def map_load(self, value, map_id):
        op, load = self.m.defs.get(self.peel(value), (None, ()))
        if op != LOAD:
            return False
        op, access = self.m.defs.get(load[2], (None, ()))
        return op in (ACCESS, INBOUNDS_ACCESS) and access[2] == map_id

    def word_origin(self, value):
        if self.is_load(value, self.low):
            return 'whole'
        op, a = self.m.defs.get(self.peel(value), (None, ()))
        if op == AND and ((self.is_load(a[2], self.low) and self.m.value(a[3]) == 0xfffffffc) or
                          (self.is_load(a[3], self.low) and self.m.value(a[2]) == 0xfffffffc)):
            return 'subword'
        return None

    def from_word(self, value, ptr_ids):
        op, a = self.m.defs.get(self.peel(value), (None, ()))
        if op == LOAD:
            return a[2] in ptr_ids
        if op in (SHR, SAR):
            return self.from_word(a[2], ptr_ids)
        return False

    def operands(self, value, opcode):
        op, a = self.m.defs.get(self.peel(value), (None, ()))
        return a[2:] if op == opcode else ()

    def binary_pairs(self, value, opcode):
        a = self.operands(value, opcode)
        return (a, a[::-1]) if len(a) == 2 else ()

    def without_constant(self, value, opcode, constant):
        a = self.operands(value, opcode)
        if len(a) == 2:
            if self.m.value(a[1]) == constant:
                return a[0]
            if self.m.value(a[0]) == constant:
                return a[1]
        return None

    def delta_row(self, value, map_id, map_word_ptr, member):
        load = self.operands(value, LOAD)
        if len(load) != 1:
            return None
        op, a = self.m.defs.get(load[0], (None, ()))
        if (op not in (ACCESS, INBOUNDS_ACCESS) or len(a) != 5 or
                a[0] != map_word_ptr or a[2] != map_id or self.m.value(a[3]) != 0):
            return None
        row = self.without_constant(a[4], ADD, member)
        return self.peel(row) if self.without_constant(row, MUL, gmem_layout()[0]) is not None else None

    def alias_address_low(self, convert, map_id, map_word_ptr, uint):
        _, _, address = convert
        op, a = self.m.defs.get(address, (None, ()))
        require(op == BITCAST and self.m.types[a[0]] == (TYPE_INT, (64, 0)), 'unexpected raw address bitcast')
        op, a = self.m.defs.get(a[2], (None, ()))
        require(op == COMPOSITE and self.m.types[a[0]] == (TYPE_VECTOR, (uint, 2)),
                'raw address is not an ordered pair of 32-bit words')
        low, high = a[2:]
        # The conversion bitcasts uvec2(low, high) to u64. Read the very same
        # low word (not the guest subword address or an unrelated table load).
        candidates = [(origin, row) for word, delta in self.binary_pairs(low, ADD)
                      if (origin := self.word_origin(word)) and
                      (row := self.delta_row(delta, map_id, map_word_ptr, 4)) is not None]
        require(len(candidates) == 1, 'raw address does not use the table delta low word')
        origin, row = candidates[0]
        for sum_, carry in self.binary_pairs(high, ADD):
            args = self.operands(carry, SELECT)
            if len(args) != 3:
                continue
            cond, one, zero = args
            cmp = self.operands(cond, LESS_U)
            if (len(cmp) != 2 or self.peel(cmp[0]) != self.peel(low) or self.word_origin(cmp[1]) != origin or
                    self.m.value(one) != 1 or self.m.value(zero) != 0):
                continue
            if any(self.is_load(guest, self.high) and self.delta_row(delta, map_id, map_word_ptr, 5) == row
                   for guest, delta in self.binary_pairs(sum_, ADD)):
                return low, origin
        require(False, 'raw address does not use the matching delta high word and carry')

    def check_subword_read(self, ret, loaded):
        width = self.alignment * 8
        shifted = self.without_constant(ret, AND, (1 << width) - 1)
        shift_op = SHR
        if shifted is None:
            args = self.operands(ret, EXTRACT_S)
            require(len(args) == 3 and list(map(self.m.value, args[1:])) == [0, width],
                    'unexpected signed raw subword extraction')
            shifted, shift_op = args[0], SAR
        args = self.operands(shifted, shift_op)
        require(len(args) == 2 and self.peel(args[0]) == loaded, 'raw subword is not from the physical load')
        shl = self.operands(args[1], SHL)
        require(len(shl) == 2 and self.m.value(shl[1]) == 3 and
                self.is_load(self.without_constant(shl[0], AND, 3), self.low),
                'raw subword shift does not select the guest byte lane')

    def aligned_edge(self, cond):
        op, a = self.m.defs.get(self.peel(cond), (None, ()))
        inverted = op == LOGICAL_NOT
        if inverted:
            op, a = self.m.defs.get(self.peel(a[2]), (None, ()))
        if op not in (EQUAL, NOT_EQUAL) or len(a) != 4:
            return None
        for bits, zero in (a[2:], a[2:][::-1]):
            if self.m.value(zero) == 0 and self.is_load(self.without_constant(bits, AND, self.alignment - 1), self.low):
                return 1 if (op == EQUAL) != inverted else 2  # BranchConditional operand for the aligned edge
        return None

    def has_alignment_guard(self, blocks, hit):
        if self.alignment == 1:
            return True
        # Follow every path on which the address *could* be misaligned. This
        # recognizes both the older hit-block and the current entry guards.
        todo = [next(iter(blocks))]
        seen = set()
        while todo:
            label = todo.pop()
            if label in seen:
                continue
            if label == hit:
                return False
            seen.add(label)
            _, end = blocks[label]
            op, a = self.body[end - 1]
            if op == BRANCH:
                todo.append(a[0])
            elif op == BRANCH_COND:
                aligned = self.aligned_edge(a[0])
                todo.extend(a[i] for i in (1, 2) if i != aligned)
            else:
                require(op in (RETURN, RETURN_VALUE, UNREACHABLE), 'unexpected raw helper control flow')
        return True

    def guard_read_alias(self, insert, map_id, map_word_ptr, uint):
        sites = [i for i, (op, _) in enumerate(self.body) if op == CONVERT_TO_PTR]
        require(len(sites) == 1, 'read-alias helper must have exactly one physical pointer')
        at = sites[0]
        convert = self.body[at][1]
        ptr = convert[1]
        op, load = self.body[at + 1]
        require(op == LOAD and load[2:] == (ptr, ALIGNED, 4), 'read-alias helper is not a direct physical load')
        require(all(ptr not in a for i, (_, a) in enumerate(self.body) if i not in (at, at + 1)),
                'read-alias physical pointer has another use')
        params = {a[1] for op, a in self.body if op == PARAMETER}
        require(len(params) == 2 and all(not params.intersection(a) for op, a in self.body if op not in (PARAMETER, LOAD)),
                'read-alias helper does not have two immutable address parameters')
        low, origin = self.alias_address_low(convert, map_id, map_word_ptr, uint)

        starts = [i for i, (op, _) in enumerate(self.body) if op == LABEL]
        blocks = {self.body[i][1][0]: (i, end) for i, end in zip(starts, starts[1:] + [len(self.body)])}
        hit, end = next((label, end) for label, (i, end) in blocks.items() if i < at < end)
        ret, a = self.body[end - 1]
        # A returning hit block has no successor PHIs to rewrite when split.
        require(ret == RETURN_VALUE and not any(op in (SELECTION_MERGE, LOOP_MERGE)
                                               for op, _ in self.body[at:end]), 'unexpected raw load hit block')
        if origin == 'subword':
            self.check_subword_read(a[0], load[1])
        if not self.has_alignment_guard(blocks, hit):
            self.guard(insert)

        boolean = next(k for k, (op, _) in self.m.types.items() if op == TYPE_BOOL)
        bits, valid, fault, proceed, dst, old = (self.m.allocate() for _ in range(6))
        zero, one = (self.m.constant(uint, n) for n in (0, 1))
        scope, semantics = (self.m.constant(uint, n) for n in (DEVICE_SCOPE, RELAXED))
        fault_word = self.m.constant(uint, gmem_layout()[1])
        insert[self.start + at].extend((
            (AND, (uint, bits, low, one)), (EQUAL, (boolean, valid, bits, zero)),
            (SELECTION_MERGE, (proceed, 0)), (BRANCH_COND, (valid, proceed, fault)), (LABEL, (fault,)),
            (ACCESS, (map_word_ptr, dst, map_id, zero, fault_word)),
            (ATOMIC_OR, (uint, old, dst, scope, semantics, one)),
            *self.miss(), (LABEL, (proceed,))))

    def miss(self):
        # The last block is the translator's complete miss path. Only its
        # private vector reset and return are observable outside this helper.
        final = max(i for i, (op, _) in enumerate(self.body) if op == LABEL)
        tail = self.body[final+1:]
        ret, args = tail[-1]
        result = self.body[0][1][0]
        require((ret == RETURN and self.m.types[result][0] == TYPE_VOID) or
                (ret == RETURN_VALUE and self.m.typed(args[0]) == result and self.m.value(args[0]) == 0),
                'raw helper has no canonical miss return')
        resets = {}
        chains = {}
        for op, a in tail[:-1]:
            if op in (ACCESS, INBOUNDS_ACCESS) and len(a) == 4 and self.m.pointer(a[0], PRIVATE):
                base, index = a[2:]
                def_op, var = self.m.defs.get(base, (None, ()))
                require(def_op == VARIABLE and var[2] == PRIVATE and self.m.value(index) is not None,
                        'unexpected raw vector miss access')
                chains[a[1]] = (op, a)
            elif op == STORE:
                ptr, value = a[:2]
                if ptr in chains:
                    require(self.m.value(value) == 0 and ptr not in resets, 'unexpected raw vector miss value')
                    resets[ptr] = value
                else:
                    def_op, var = self.m.defs.get(ptr, (None, ()))
                    require(def_op == VARIABLE and var[2] == FUNCTION_STORAGE, 'unexpected write on raw pointer miss')
            else:
                require(False, 'unexpected operation on raw pointer miss')
        require(chains.keys() == resets.keys() and len(chains) == self.lanes - 1,
                'incomplete raw vector miss reset')
        require({self.m.value(a[3]) for _, a in chains.values()} == set(range(self.lanes - 1)) and
                len({a[2] for _, a in chains.values()}) <= 1, 'unexpected raw vector miss lanes')
        new = []
        for ptr, value in resets.items():
            op, a = chains[ptr]
            dst = self.m.allocate()
            new.extend(((op, (a[0], dst, *a[2:])), (STORE, (dst, value))))
        new.append((ret, args))
        return new

    def guard(self, insert):
        miss = self.miss()
        if self.alignment == 1:
            return  # any U8 address fits its containing word
        first = next(i for i, (op, _) in enumerate(self.body) if op == LABEL)
        old_label = self.body[first][1][0]
        require(not any(op == PHI and old_label in a[3::2] for op, a in self.body), 'raw helper entry has a phi successor')
        at = first + 1
        while self.body[at][0] == VARIABLE:
            at += 1
        boolean = next(k for k, (op, _) in self.m.types.items() if op == TYPE_BOOL)
        low, bits, valid, invalid, proceed = [self.m.allocate() for _ in range(5)]
        mask = self.m.constant(self.i32, self.alignment - 1)
        zero = self.m.constant(self.i32, 0)
        insert[self.start+at].extend((
            (LOAD, (self.i32, low, self.low)), (AND, (self.i32, bits, low, mask)), (EQUAL, (boolean, valid, bits, zero)),
            (SELECTION_MERGE, (proceed, 0)), (BRANCH_COND, (valid, proceed, invalid)), (LABEL, (invalid,)),
            *miss, (LABEL, (proceed,))))


def raw_helpers(module):
    lookups = [a[0] for op, a in module.ops if op == NAME and
               struct.pack(f'<{len(a)-1}I', *a[1:]).split(b'\0', 1)[0] == b'dmsh_raw_find']
    require(len(lookups) == 1 and lookups[0] in module.functions, 'no unique raw pool lookup function')
    lookup = lookups[0]
    start, end = module.functions[lookup]
    maps = {a[2] for op, a in module.ops[start:end] if op in (ACCESS, INBOUNDS_ACCESS) and
            module.defs.get(a[2], (0, ()))[0] == VARIABLE and
            module.defs[a[2]][1][2] in (UNIFORM, STORAGE_BUFFER)}
    require(len(maps) == 1, 'cannot identify the raw pool table')
    map_id = maps.pop()
    helpers = []
    for start, end in module.functions.values():
        physical = [a for op, a in module.ops[start:end] if op == CONVERT_TO_PTR]
        if physical:
            helpers.append(RawHelper(module, start, end, physical, lookup, map_id))
    require(helpers, 'physical addressing is declared but no raw helpers were recognized')
    return map_id, helpers


def read_alias_map_pointer(module, map_id):
    _, var = module.defs[map_id]
    storage = var[2]
    block = module.pointer(var[0], storage)
    op, a = module.types.get(block, (None, ()))
    require(op == TYPE_STRUCT and len(a) == 1, 'raw map is not a single-member buffer block')
    array = a[0]
    op, a = module.types.get(array, (None, ()))
    require(op == TYPE_RUNTIME_ARRAY and module.types.get(a[0]) == (TYPE_INT, (32, 0)),
            'raw map is not a runtime array of uint32 words')
    uint = a[0]
    decorators = {a for op, a in module.ops if op == DECORATE and a[0] in (block, array, map_id)}
    members = {a for op, a in module.ops if op == MEMBER_DECORATE and a[0] == block}
    require((block, BUFFER_BLOCK if storage == UNIFORM else BLOCK) in decorators and
            (array, ARRAY_STRIDE, 4) in decorators and (block, 0, OFFSET, 0) in members and
            all(a[1] != NON_WRITABLE for a in decorators) and all(a[2] != NON_WRITABLE for a in members),
            'raw map does not have the writable packed-word buffer layout')
    ptr = next((k for k in module.types if module.pointer(k, storage) == uint), None)
    require(ptr is not None, 'raw map word pointer type is missing')
    return ptr, uint


def upgrade_read_alias(raw: bytes) -> bytes:
    """Guard a Windows read-only raw GMEM compute module; reject unknown raw accesses.

    The caller must bind the reserved map descriptor with the extra fault word
    and publish the new binary hash in runtime reflection.
    """
    module = Module(raw)
    if ([a[0] for op, a in module.ops if op == ENTRY_POINT] != [COMPUTE] or
            PHYSICAL_CAP not in module.caps or (MODULE_PROCESSED, _READ_ALIAS_WORDS) in module.ops):
        return raw

    require(module.header[1] >= 0x00010100, 'read-alias marker requires SPIR-V 1.1 or later')
    require(all(op == CONVERT_TO_PTR for op, a in module.defs.values() if module.pointer(a[0], PHYSICAL_STORAGE)),
            'unexpected physical pointer producer')
    map_id, helpers = raw_helpers(module)
    ptr, uint = read_alias_map_pointer(module, map_id)
    insert = defaultdict(list)
    for helper in helpers:
        helper.guard_read_alias(insert, map_id, ptr, uint)

    # OpModuleProcessed is last in the logical debug section, before annotations.
    at = next(i for i, (op, _) in enumerate(module.ops) if 71 <= op <= 75 or op in (332, 5632, 5633))
    insert[at].append((MODULE_PROCESSED, _READ_ALIAS_WORDS))
    return module.emission(insert, {}, set())


def legalize(raw, *, guard_gmem=False):
    """Return Windows-valid SPIR-V; if selected, check every physical helper."""
    module = Module(raw)
    insert, replace = defaultdict(list), {}
    caps = legalize_images(module, insert, replace)
    if guard_gmem and PHYSICAL_CAP in module.caps:
        _, helpers = raw_helpers(module)
        for helper in helpers:
            helper.guard(insert)
    if not insert and not replace and not (caps - module.caps):
        return raw
    return module.emission(insert, replace, caps)
