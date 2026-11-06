"""aot's artifacts run here: a module mapped into this process, its kernel
side played by ctypes callbacks (bin/aot/src/abi.rs).

A trap ends the process, so Instance.run makes a call in a child and tells a
trap from a result.
"""
import ctypes
import mmap
import os
import struct
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent.parent / "dev"))

import wasm_info  # noqa: E402

PAGE = 65536
CTYPES = {"i32": ctypes.c_int32, "i64": ctypes.c_int64, "f32": ctypes.c_float, "f64": ctypes.c_double}

# abi.rs: context words, helpers, trap codes.
MEMORY_BASE, MEMORY_SIZE, STACK_LIMIT, TABLE_BASE, TABLE_SIZE, GLOBALS = range(6)
HELPERS = ["trap", "stack_check", "memory_grow", "memory_copy", "memory_fill", "memory_init", "data_drop"]
UNREACHABLE, OUT_OF_BOUNDS, DIVISION_BY_ZERO, OVERFLOW, CONVERSION, TABLE, SIGNATURE = range(7)
TRAPPED = 64


class Trap:
    def __init__(self, code):
        self.code = code

    def __repr__(self):
        return f"trap {self.code}"


class Instance:
    def __init__(self, module, artifact, kernel):
        self.layout = wasm_info.layout(module)
        magic, version, arch, count, imports, relocations, size, self.max_frame = struct.unpack_from("<8I", artifact)
        assert (magic, version, arch) == (int.from_bytes(b"PZAO", "little"), 1, 1)
        assert count == len(self.layout.functions) and imports == len(self.layout.imports)
        self.records = [struct.unpack_from("<4I", artifact, 32 + 16 * i) for i in range(count)]
        at = 32 + 16 * count
        relocs = [struct.unpack_from("<4I", artifact, at + 16 * i) for i in range(relocations)]
        at += 16 * relocations
        assert len(artifact) == at + size

        self.code = mmap.mmap(-1, size, prot=mmap.PROT_READ | mmap.PROT_WRITE | mmap.PROT_EXEC)
        self.code[:] = artifact[at:]
        self.code_base = ctypes.addressof(ctypes.c_char.from_buffer(self.code))
        # Address space only: pages are touched as the module grows into them.
        maximum = self.layout.memory[1] or 65536
        self.memory = mmap.mmap(-1, maximum * PAGE)
        self.memory_base = ctypes.addressof(ctypes.c_char.from_buffer(self.memory))
        self.max_pages = maximum
        self.vmctx = (ctypes.c_uint64 * (GLOBALS + len(self.layout.globals)))()
        self.vmctx_address = ctypes.addressof(self.vmctx)
        self.vmctx[MEMORY_BASE] = self.memory_base
        self.vmctx[MEMORY_SIZE] = self.layout.memory[0] * PAGE
        self.stack_checks = 0
        self.callbacks = []

        for i, (_, _, bits) in enumerate(self.layout.globals):
            self.vmctx[GLOBALS + i] = bits

        for offset, data in self.layout.data:
            self.memory[offset:offset + len(data)] = data

        helpers = [self.helper(name) for name in HELPERS]
        imported = [self.import_(index, kernel) for index in range(imports)]

        for offset, kind, target, index in relocs:
            assert kind == 1 and target in (1, 2)
            address = imported[index] if target == 1 else helpers[index]
            struct.pack_into("<Q", self.code, offset, address)

        entries = max([offset + len(functions) for offset, functions in self.layout.elements], default=0)
        self.table = (ctypes.c_uint64 * (2 * entries))(*([0, 2**64 - 1] * entries))

        for offset, functions in self.layout.elements:
            for i, function in enumerate(functions):
                self.table[2 * (offset + i)] = imported[function] if function < imports else self.address(function)
                self.table[2 * (offset + i) + 1] = self.records[function][3]

        self.vmctx[TABLE_BASE] = ctypes.addressof(self.table)
        self.vmctx[TABLE_SIZE] = entries

    def keep(self, prototype, function):
        callback = prototype(function)
        self.callbacks.append(callback)

        return ctypes.cast(callback, ctypes.c_void_p).value

    def prototype(self, function):
        params, results = self.layout.types[self.layout.functions[function]]

        return ctypes.CFUNCTYPE(CTYPES[results[0]] if results else None, ctypes.c_uint64, *(CTYPES[p] for p in params))

    def import_(self, index, kernel):
        module, name = self.layout.imports[index]

        return self.keep(self.prototype(index), lambda vmctx, *args: kernel[f"{module}.{name}"](*args))

    def helper(self, name):
        u64, i32, u32 = ctypes.c_uint64, ctypes.c_int32, ctypes.c_uint32

        def trap(vmctx, code):
            os._exit(TRAPPED + code)

        def stack_check(vmctx):
            self.stack_checks += 1
            self.vmctx[STACK_LIMIT] = 0

        def memory_grow(vmctx, pages):
            old = self.vmctx[MEMORY_SIZE] // PAGE

            if pages < 0 or old + pages > self.max_pages:
                return -1

            self.vmctx[MEMORY_SIZE] += pages * PAGE

            return old

        def bounds(*ranges):
            if any(start + length > self.vmctx[MEMORY_SIZE] for start, length in ranges):
                trap(0, OUT_OF_BOUNDS)

        def memory_copy(vmctx, to, start, length):
            bounds((to, length), (start, length))
            ctypes.memmove(self.memory_base + to, self.memory_base + start, length)

        def memory_fill(vmctx, to, byte, length):
            bounds((to, length))
            ctypes.memset(self.memory_base + to, byte, length)

        def unused(*args):
            raise SystemExit("ops.wasm has no passive segments")

        prototypes = {
            "trap": (ctypes.CFUNCTYPE(None, u64, i32), trap),
            "stack_check": (ctypes.CFUNCTYPE(None, u64), stack_check),
            "memory_grow": (ctypes.CFUNCTYPE(i32, u64, i32), memory_grow),
            "memory_copy": (ctypes.CFUNCTYPE(None, u64, u32, u32, u32), memory_copy),
            "memory_fill": (ctypes.CFUNCTYPE(None, u64, u32, i32, u32), memory_fill),
            "memory_init": (ctypes.CFUNCTYPE(None, u64, u32, u32, u32, u32), unused),
            "data_drop": (ctypes.CFUNCTYPE(None, u64, u32), unused),
        }
        prototype, function = prototypes[name]

        return self.keep(prototype, function)

    def address(self, function):
        offset, size, frame, _ = self.records[function]
        assert frame <= self.max_frame

        return self.code_base + offset

    def call(self, name, *args):
        function = self.layout.exports[name]

        return self.prototype(function)(self.address(function))(self.vmctx_address, *args)

    def run(self, name, *args):
        """The result, or Trap when the call traps; a child makes the call."""
        reader, writer = os.pipe()
        pid = os.fork()

        if pid == 0:
            os.close(reader)
            os.write(writer, repr(self.call(name, *args)).encode())
            os._exit(0)

        os.close(writer)
        output = os.read(reader, 4096).decode()
        os.close(reader)
        _, status = os.waitpid(pid, 0)
        code = os.waitstatus_to_exitcode(status)

        if code >= TRAPPED:
            return Trap(code - TRAPPED)

        assert code == 0, f"{name}{args} exited {code}"

        return output
