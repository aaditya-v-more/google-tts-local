#!/usr/bin/python3
"""Convert the fetched arm64 ELF images into dyld-loadable arm64 dylibs.

The input addresses are preserved with one 16 KiB prefix. No ELF relocation or
instruction rewriting is performed by the runtime; dyld applies these fixups.
"""
import argparse
import collections
import hashlib
import os
import plistlib
import shutil
import struct
import subprocess
import sys

PAGE = 0x4000
PREFIX = PAGE
NAMES = ('libchrometts.so', 'runtime/libc++.so.1', 'runtime/libc++abi.so.1',
         'runtime/libgcc_s.so.1', 'runtime/libunwind.so.1')
OUTPUTS = ('libchrometts.dylib', 'libcxx.dylib', 'libcxxabi.dylib',
           'libgcc.dylib', 'libunwind.dylib')
SHIM = 'libgtts-shim.dylib'
MAX_IMPORTS = 2048
DATA_IMPORTS = {
    '__stack_chk_guard': 'gtts_slot_guard', 'stdin': 'gtts_slot_stdin',
    'stdout': 'gtts_slot_stdout', 'stderr': 'gtts_slot_stderr',
    'environ': 'gtts_slot_environ', '__environ': 'gtts_slot_environ',
    'tzname': 'gtts_slot_tzname', '__timezone': 'gtts_slot_timezone',
    'timezone': 'gtts_slot_timezone',
}
FAST_IMPORTS = {
    'memcpy': 'guest_fast_memcpy', 'memmove': 'guest_fast_memmove',
    'memset': 'guest_fast_memset', 'strlen': 'guest_fast_strlen',
    'strnlen': 'guest_fast_strnlen', 'strcmp': 'guest_fast_strcmp',
    'memcmp': 'guest_fast_memcmp', 'bcmp': 'guest_fast_memcmp',
    'memchr': 'guest_fast_memchr', 'malloc': 'guest_native_fast_malloc',
    'free': 'guest_native_fast_free',
}


def align(value, size):
    return (value + size - 1) & -size


def need(condition, message):
    if not condition:
        raise ValueError(message)


def uleb(value):
    need(value >= 0, 'negative ULEB')
    out = bytearray()
    while value >= 128:
        out.append((value & 127) | 128)
        value >>= 7
    out.append(value)
    return bytes(out)


def sleb(value):
    out = bytearray()
    while True:
        byte = value & 127
        value >>= 7
        done = (value == 0 and not byte & 64) or (value == -1 and byte & 64)
        out.append(byte if done else byte | 128)
        if done:
            return bytes(out)


def bounds(data, offset, size):
    need(0 <= offset <= len(data) and 0 <= size <= len(data) - offset,
         'file range outside input')


def unpack(data, fmt, offset):
    bounds(data, offset, struct.calcsize(fmt))
    return struct.unpack_from(fmt, data, offset)


def cstring(data, offset):
    bounds(data, offset, 1)
    end = data.find(b'\0', offset)
    need(end >= 0, 'unterminated string')
    return data[offset:end].decode('ascii')


class ELF:
    def __init__(self, path, index):
        self.path = path
        self.index = index
        with open(path, 'rb') as stream:
            self.data = stream.read()
        self.sha256 = hashlib.sha256(self.data).hexdigest()
        h = unpack(self.data, '<16sHHIQQQIHHHHHH', 0)
        need(h[0][:6] == b'\x7fELF\x02\x01' and h[1] == 3 and h[2] == 183,
             path + ': expected a little-endian AArch64 shared object')
        need(h[9] == 56 and h[11] == 64, path + ': unsupported ELF headers')
        self.ph = [unpack(self.data, '<IIQQQQQQ', h[5] + n * h[9]) for n in range(h[10])]
        self.sh = [unpack(self.data, '<IIQQQQIIQQ', h[6] + n * h[11]) for n in range(h[12])]
        self.loads = [p for p in self.ph if p[0] == 1]
        need(self.loads and all(p[5] <= p[6] and p[3] + p[6] <= 512 * 1024 * 1024
                                for p in self.loads), path + ': invalid segments')
        for p in self.loads:
            bounds(self.data, p[2], p[5])
        text = [p for p in self.loads if not p[1] & 2]
        writable = [p for p in self.loads if p[1] & 2]
        need(text and any(p[1] & 1 for p in text) and min(p[3] for p in text) == 0 and writable and
             all(not p[1] & 1 for p in writable), path + ': unsupported segment layout')
        self.text = text[0]
        self.text_files_end = max(p[3] + p[5] for p in text)
        self.text_mem_end = max(p[3] + p[6] for p in text)
        self.data_start = min(p[3] for p in writable) // PAGE * PAGE
        self.data_end = max(p[3] + p[6] for p in writable)
        need(align(self.text_mem_end, PAGE) <= self.data_start,
             path + ': executable and writable data share a 16 KB page')
        self.size = align(max(p[3] + p[6] for p in self.loads), 65536)
        self.phdr = self.vaddr_for_file(h[5], h[10] * h[9])
        self.phnum = h[10]
        symsections = [s for s in self.sh if s[1] == 11]
        need(len(symsections) == 1, path + ': missing dynamic symbols')
        sec = symsections[0]
        need(sec[9] == 24 and sec[5] % 24 == 0 and sec[6] < len(self.sh),
             path + ': invalid dynamic symbols')
        strings = self.sh[sec[6]]
        bounds(self.data, strings[4], strings[5])
        self.strings = self.data[strings[4]:strings[4] + strings[5]]
        self.syms = [unpack(self.data, '<IBBHQQ', sec[4] + n * 24)
                     for n in range(sec[5] // 24)]
        self.symbol_names = [cstring(self.strings, s[0]) for s in self.syms]
        dynamics = [p for p in self.ph if p[0] == 2]
        need(len(dynamics) == 1, path + ': missing dynamic section')
        dyn = dynamics[0]
        self.dynamic = dyn[3]
        self.tags = {}
        for off in range(dyn[2], dyn[2] + dyn[5], 16):
            tag, val = unpack(self.data, '<qQ', off)
            if tag == 0:
                break
            self.tags[tag] = val
        self.relas = []
        for ptrtag, sizetag in ((7, 8), (23, 2)):
            addr, size = self.tags.get(ptrtag, 0), self.tags.get(sizetag, 0)
            need(size % 24 == 0, path + ': invalid relocation table')
            if size:
                off = self.file_for_vaddr(addr, size)
                self.relas.extend(unpack(self.data, '<QQq', off + n)
                                  for n in range(0, size, 24))
        self.relr = []
        relrsz = self.tags.get(35, 0)
        need(relrsz % 8 == 0, path + ': invalid RELR size')
        if relrsz:
            off = self.file_for_vaddr(self.tags[36], relrsz)
            self.relr = [unpack(self.data, '<Q', off + n)[0] for n in range(0, relrsz, 8)]
        self.tls = next((p for p in self.ph if p[0] == 7), None)
        self.eh_frame = next((p[3] for p in self.ph if p[0] == 0x6474e550), 0)
        self.tls_offset = 0

    def vaddr_for_file(self, offset, size):
        for p in self.loads:
            if p[2] <= offset and offset + size <= p[2] + p[5]:
                return p[3] + offset - p[2]
        raise ValueError(self.path + ': file range is not mapped')

    def file_for_vaddr(self, addr, size):
        for p in self.loads:
            if p[3] <= addr and addr + size <= p[3] + p[5]:
                return p[2] + addr - p[3]
        raise ValueError(self.path + ': address range is not file backed')

    def writable(self, addr, size):
        return any(p[1] & 2 and p[3] <= addr and addr + size <= p[3] + p[6]
                   for p in self.loads)


def definitions(objects):
    result = {}
    for obj in objects:
        for sym, name in zip(obj.syms, obj.symbol_names):
            _, info, other, section, value, _ = sym
            binding, kind = info >> 4, info & 15
            if section and name and binding in (1, 2) and other & 3 != 2 and kind != 6:
                if name not in result or result[name][2] and binding == 1:
                    result[name] = (obj.index, value, binding == 2)
    return result


def resolve(obj, sym, name, defs):
    _, info, other, section, value, _ = sym
    binding, kind = info >> 4, info & 15
    if section and (binding == 0 or other & 3):
        return ('local', obj.index, value)
    if name in defs:
        provider, value, _ = defs[name]
        return ('local' if provider == obj.index else 'object', provider, value)
    if section:
        return ('local', obj.index, value)
    if name in DATA_IMPORTS:
        return ('data', DATA_IMPORTS[name], 0)
    if binding == 2:
        return ('weak', '', 0)
    need(kind != 1, obj.path + ': unsupported data import ' + name)
    return ('host', name, 0)


def collect_imports(objects, defs):
    names = set()
    for obj in objects:
        for _, info, _ in obj.relas:
            kind, index = info & 0xffffffff, info >> 32
            need(index < len(obj.syms), obj.path + ': invalid symbol index')
            if kind in (257, 1025, 1026):
                item = resolve(obj, obj.syms[index], obj.symbol_names[index], defs)
                if item[0] == 'host' and item[1] not in FAST_IMPORTS:
                    names.add(item[1])
    names = sorted(names)
    need(len(names) <= MAX_IMPORTS, 'too many host imports')
    return names


def trie(exports):
    root = {}
    for name, address in exports.items():
        node = root
        for char in name.encode('ascii'):
            node = node.setdefault(char, {})
        need(None not in node, 'duplicate export: ' + name)
        node[None] = address
    nodes = []
    def visit(node):
        edges = []
        for char in sorted(c for c in node if c is not None):
            edge = bytes([char])
            child = node[char]
            while None not in child and len(child) == 1:
                char = next(iter(child))
                edge += bytes([char])
                child = child[char]
            edges.append((edge, child))
        entry = {'node': node, 'edges': edges, 'off': 0, 'data': b''}
        nodes.append(entry)
        for _, child in edges:
            visit(child)
    visit(root)
    identities = {id(n['node']): n for n in nodes}
    for _ in range(20):
        before = [n['off'] for n in nodes]
        offset = 0
        for n in nodes:
            n['off'] = offset
            payload = uleb(0) + uleb(n['node'][None]) if None in n['node'] else b''
            n['data'] = (uleb(len(payload)) + payload + bytes([len(n['edges'])]) +
                         b''.join(edge + b'\0' + uleb(identities[id(child)]['off'])
                                  for edge, child in n['edges']))
            offset += len(n['data'])
        if before == [n['off'] for n in nodes]:
            return b''.join(n['data'] for n in nodes)
    raise ValueError('export trie offsets did not converge')


def segment(name, address, size, fileoff, filesize, maxprot, initprot, sections=()):
    name = name.encode().ljust(16, b'\0')
    secdata = b''.join(struct.pack('<16s16sQQIIIIIIII', sect.encode().ljust(16, b'\0'),
                                   name, addr, length, off, al, 0, 0, flags, 0, 0, 0)
                       for sect, addr, length, off, al, flags in sections)
    return (struct.pack('<II16sQQQQIIII', 0x19, 72 + len(secdata), name, address, size,
                        fileoff, filesize, maxprot, initprot, len(sections), 0) + secdata)


def dylib(command, name):
    data = name.encode() + b'\0'
    data += bytes((-len(data) - 24) % 8)
    return struct.pack('<IIIIII', command, 24 + len(data), 24, 1, 0, 0) + data


def branch(source, target):
    delta = target - source
    need(delta % 4 == 0 and -(1 << 27) <= delta < (1 << 27), 'branch is out of range')
    return 0x14000000 | ((delta >> 2) & 0x03ffffff)


def adrp(register, source, target):
    delta = (target >> 12) - (source >> 12)
    need(-(1 << 20) <= delta < (1 << 20), 'ADRP is out of range')
    imm = delta & 0x1fffff
    return 0x90000000 | ((imm & 3) << 29) | ((imm >> 2) << 5) | register


def patch_code(obj, image, key_address):
    stub = align(PREFIX + obj.text_mem_end, 4)
    first = stub
    count = svc = 0
    for sec in obj.sh:
        if sec[1] != 1 or not sec[2] & 4:
            continue
        addr, fileoff, length = sec[3], sec[4], sec[5]
        need(not (addr | length) & 3 and fileoff == obj.file_for_vaddr(addr, length) and
             any(p[1] & 1 and p[3] <= addr and addr + length <= p[3] + p[5] for p in obj.loads),
             obj.path + ': invalid code section')
        for pos in range(PREFIX + addr, PREFIX + addr + length, 4):
            insn = unpack(image, '<I', pos)[0]
            if insn & ~31 == 0xd53bd040:
                reg = insn & 31
                need(reg != 31, obj.path + ': invalid TLS register')
                scratch = 17 if reg == 16 else 16
                need(stub + 32 <= PREFIX + obj.data_start, obj.path + ': no room for TLS stubs')
                # The key's address is dyld-bound into the output's data segment.
                pageoff = key_address & 0xfff
                need(pageoff % 8 == 0, 'unaligned TLS key slot')
                words = [0xf81f0fe0 | scratch, 0xd53bd060 | reg,
                         adrp(scratch, stub + 8, key_address),
                         0xf9400000 | ((pageoff // 8) << 10) | (scratch << 5) | scratch,
                         0xb9400000 | (scratch << 5) | scratch,
                         0xf8607800 | (scratch << 16) | (reg << 5) | reg,
                         0xf84107e0 | scratch, branch(stub + 28, pos + 4)]
                struct.pack_into('<8I', image, stub, *words)
                struct.pack_into('<I', image, pos, branch(pos, stub))
                stub += 32
                count += 1
            elif insn & 0xffe0001f == 0xd4000001:
                need(obj.index == 0 and pos - PREFIX == 0x12a30b4 and insn == 0xd4000001 and
                     unpack(image, '<I', pos - 4)[0] == 0x528003c8,
                     obj.path + ': unexpected Linux syscall at %#x' % (pos - PREFIX))
                struct.pack_into('<I', image, pos, 0x928004a0)
                svc += 1
    if obj.index == 0 and obj.size > 0x12a30b8:
        need(svc == 1, obj.path + ': expected syscall is missing')
    return first, stub - first, count, svc


def rebase_stream(offsets, data_start):
    out = bytearray([0x11])
    ordered = sorted(offsets)
    need(len(set(ordered)) == len(ordered), 'duplicate rebase')
    i = 0
    while i < len(ordered):
        start = ordered[i]
        j = i + 1
        while j < len(ordered) and ordered[j] == ordered[j - 1] + 8:
            j += 1
        out += bytes([0x21]) + uleb(start - data_start) + bytes([0x60]) + uleb(j - i)
        i = j
    out.append(0)
    out += bytes((-len(out)) % 8)
    return bytes(out)


def bind_stream(binds, data_start):
    out = bytearray([0x51])
    for address, ordinal, name, addend in sorted(binds):
        need(1 <= ordinal <= 15, 'dylib ordinal out of range')
        out += (bytes([0x10 | ordinal, 0x40]) + name.encode('ascii') + b'\0' +
                bytes([0x60]) + sleb(addend) + bytes([0x71]) +
                uleb(address - data_start) + bytes([0x90]))
    out.append(0)
    out += bytes((-len(out)) % 8)
    return bytes(out)


def convert(obj, objects, defs, imports, directory):
    # File offsets equal preferred vmaddrs so the original ADRP/ADD distances survive.
    data_start = PREFIX + obj.data_start
    key_address = align(PREFIX + obj.data_end, 8)
    data_end = align(key_address + 8, PAGE)
    image = bytearray(data_end)
    for p in obj.loads:
        image[PREFIX + p[3]:PREFIX + p[3] + p[5]] = obj.data[p[2]:p[2] + p[5]]
    stub_start, stub_size, tls_patches, svc_patches = patch_code(obj, image, key_address)
    initializer = stub_start + stub_size
    need(initializer + 4 <= data_start, obj.path + ': no room for initializer entry')
    struct.pack_into('<I', image, initializer, 0xd61f0060)  # br x3
    stub_size += 4
    rebases = []
    binds = [(key_address, 1, '_gtts_tls_key', 0)]
    imports_by_name = {name: i + 1 for i, name in enumerate(imports)}
    dependencies = [SHIM] + [OUTPUTS[i] for i in range(len(objects)) if i != obj.index]
    ordinal = {name: i + 1 for i, name in enumerate(dependencies)}

    def local_rebase(address, value):
        need(obj.writable(address - PREFIX, 8), obj.path + ': relocation outside writable segment')
        struct.pack_into('<Q', image, address, value & 0xffffffffffffffff)
        rebases.append(address)

    for where, info, addend in obj.relas:
        kind, index = info & 0xffffffff, info >> 32
        need(index < len(obj.syms), obj.path + ': invalid relocation symbol')
        address = PREFIX + where
        need(obj.writable(where, 16 if kind == 1031 else 8),
             obj.path + ': relocation outside writable segment')
        if kind == 1027:
            local_rebase(address, PREFIX + addend)
        elif kind == 1031:
            need(index == 0, obj.path + ': external TLS is unsupported')
            binds.append((address, 1, '_native_tlsdesc', 0))
            struct.pack_into('<Q', image, address + 8, (obj.tls_offset + addend) & 0xffffffffffffffff)
        elif kind in (257, 1025, 1026):
            name = obj.symbol_names[index]
            how, provider, value = resolve(obj, obj.syms[index], name, defs)
            if how == 'local':
                local_rebase(address, PREFIX + value + addend)
            elif how == 'object':
                binds.append((address, ordinal[OUTPUTS[provider]], '_gtts_c_' + name, addend))
            elif how == 'data':
                binds.append((address, 1, '_' + provider, addend))
            elif how == 'host':
                symbol = FAST_IMPORTS.get(name, 'gtts_h%04d' % imports_by_name.get(name, 0))
                need(symbol != 'gtts_h0000', 'missing host import: ' + name)
                binds.append((address, 1, '_' + symbol, addend))
            else:
                struct.pack_into('<Q', image, address, addend & 0xffffffffffffffff)
        else:
            raise ValueError(obj.path + ': unsupported relocation %d' % kind)
    where = None
    for entry in obj.relr:
        if not entry & 1:
            where = entry
            old = unpack(image, '<Q', PREFIX + where)[0]
            local_rebase(PREFIX + where, old + PREFIX)
            where += 8
        else:
            need(where is not None, obj.path + ': malformed RELR')
            for bit in range(63):
                if entry & (1 << (bit + 1)):
                    address = PREFIX + where + 8 * bit
                    old = unpack(image, '<Q', address)[0]
                    local_rebase(address, old + PREFIX)
            where += 63 * 8

    exports = {'_gtts_image_base': PREFIX, '_gtts_explicit_init': initializer}
    for name, (provider, value, _) in defs.items():
        if provider == obj.index:
            address = PREFIX + value
            need(PREFIX <= address < data_end, obj.path + ': export outside image: ' + name)
            exports['_gtts_c_' + name] = address
    rebases_blob = rebase_stream(rebases, data_start)
    binds_blob = bind_stream(binds, data_start)
    exports_blob = trie(exports)
    linkoff = data_end
    link = bytearray(rebases_blob + binds_blob + exports_blob)
    link += bytes((-len(link)) % 8)
    symoff = linkoff + len(link)
    strings = bytearray(b'\0')
    symbols = bytearray()
    for name, address in exports.items():
        stroff = len(strings)
        strings += name.encode('ascii') + b'\0'
        section = 3 if address >= data_start else 2 if stub_start <= address < stub_start + stub_size else 1
        symbols += struct.pack('<IBBHQ', stroff, 0x0f, section, 0, address)
    link += symbols
    stroff = linkoff + len(link)
    link += strings
    text_sections = [('__gtts_payload', PREFIX, obj.text_files_end, PREFIX, 2, 0),
                     ('__gtts_stubs', stub_start, stub_size, stub_start, 2, 0x80000400)]
    commands = [segment('__TEXT', 0, data_start, 0, data_start, 5, 5, text_sections),
                segment('__DATA', data_start, data_end - data_start, data_start,
                        data_end - data_start, 3, 3,
                        [('__gtts_data', data_start, data_end - data_start, data_start, 3, 0)]),
                segment('__LINKEDIT', linkoff, align(len(link), PAGE), linkoff, len(link), 1, 1),
                dylib(0xd, '@loader_path/' + OUTPUTS[obj.index])]
    commands += [dylib(0xc, '@loader_path/' + name) for name in dependencies]
    commands += [struct.pack('<12I', 0x80000022, 48, linkoff, len(rebases_blob),
                             linkoff + len(rebases_blob), len(binds_blob), 0, 0, 0, 0,
                             linkoff + len(rebases_blob) + len(binds_blob), len(exports_blob)),
                 struct.pack('<6I', 2, 24, symoff, len(exports), stroff, len(strings)),
                 struct.pack('<20I', 0xb, 80, 0, 0, 0, len(exports), len(exports), 0, *([0] * 12)),
                 struct.pack('<6I', 0x32, 24, 1, 0x0d0000, 0x1b0000, 0),
                 struct.pack('<II16s', 0x1b, 24, hashlib.sha256(obj.data).digest()[:16])]
    header = struct.pack('<8I', 0xfeedfacf, 0x100000c, 0, 6, len(commands),
                         sum(map(len, commands)), 0x100085, 0) + b''.join(commands)
    need(len(header) <= PREFIX, 'Mach-O load commands exceed the reserved prefix')
    image[:len(header)] = header
    image += link
    path = os.path.join(directory, OUTPUTS[obj.index])
    with open(path, 'wb') as stream:
        stream.write(image)
    subprocess.run(['/usr/bin/codesign', '--force', '--sign', '-', path], check=True)
    subprocess.run(['/usr/bin/codesign', '--verify', '--strict', path], check=True)
    with open(path, 'rb') as stream:
        dylib_hash = hashlib.sha256(stream.read()).hexdigest()
    tls = obj.tls
    meta = {'name': OUTPUTS[obj.index], 'source': NAMES[obj.index], 'sha256': obj.sha256,
            'dylib_sha256': dylib_hash,
            'size': obj.size, 'phdr': obj.phdr, 'phnum': obj.phnum,
            'dynamic': obj.dynamic, 'eh_frame': obj.eh_frame,
            'init': obj.tags.get(12, 0), 'init_array': obj.tags.get(25, 0),
            'init_array_size': obj.tags.get(27, 0),
            'tls_image': tls[3] if tls else 0, 'tls_filesz': tls[5] if tls else 0,
            'tls_memsz': tls[6] if tls else 0, 'tls_align': (tls[7] or 1) if tls else 1,
            'tls_offset': obj.tls_offset}
    print('%s: %d rebases, %d binds, %d exports, %d TLS reads, %d syscalls' %
          (OUTPUTS[obj.index], len(rebases), len(binds), len(exports), tls_patches, svc_patches),
          file=sys.stderr)
    return meta


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--data', required=True)
    parser.add_argument('--output', required=True)
    parser.add_argument('--shim', required=True)
    parser.add_argument('--engine')
    args = parser.parse_args()
    os.makedirs(args.output, exist_ok=True)
    objects = [ELF(args.engine if i == 0 and args.engine else os.path.join(args.data, name), i)
               for i, name in enumerate(NAMES)]
    tls_used = 0x1000
    for obj in objects:
        if obj.tls:
            alignment = obj.tls[7] or 1
            need(alignment & (alignment - 1) == 0, 'invalid TLS alignment')
            tls_used = align(tls_used, alignment)
            obj.tls_offset = tls_used
            tls_used += obj.tls[6]
    need(tls_used <= 65536, 'TLS templates exceed guest TLS block')
    defs = definitions(objects)
    imports = collect_imports(objects, defs)
    shutil.copyfile(args.shim, os.path.join(args.output, SHIM))
    subprocess.run(['/usr/bin/codesign', '--verify', '--strict', os.path.join(args.output, SHIM)], check=True)
    with open(args.shim, 'rb') as stream:
        shim_hash = hashlib.sha256(stream.read()).hexdigest()
    with open(__file__, 'rb') as stream:
        converter_hash = hashlib.sha256(stream.read()).hexdigest()
    metadata = {'version': 1, 'imports': imports, 'shim_sha256': shim_hash,
                'converter_sha256': converter_hash,
                'objects': [convert(obj, objects, defs, imports, args.output) for obj in objects]}
    with open(os.path.join(args.output, 'metadata.plist'), 'wb') as stream:
        plistlib.dump(metadata, stream, sort_keys=True)


if __name__ == '__main__':
    try:
        main()
    except (ValueError, OSError, subprocess.CalledProcessError) as error:
        print('Mach-O conversion failed: %s' % error, file=sys.stderr)
        sys.exit(1)
