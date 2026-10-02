"""Read-only catalog export for the verified 2026-09-30 Steam build.

No process writes, code execution, injection or network traffic. This is an
analysis tool; the distributed application only reads its generated JSON.
"""
import argparse, datetime, json, struct, sys
from pathlib import Path
sys.path.insert(0, str(Path(__file__).resolve().parent))
from skin_memory_reader import Memory

def export(pid):
    m = Memory(pid)
    try:
        def fname(k):
            low = k & 0xffffffff
            p = m.q(0x14f110380 + 8 * ((low >> 16) + 2)) + 2 * (low & 65535)
            h = int.from_bytes(m.read(p, 2), 'little')
            n = h >> 6
            if not 0 < n <= 1023: raise ValueError('invalid FName')
            s = m.read(p + 2, n * (2 if h & 1 else 1)).decode('utf-16le' if h & 1 else 'utf-8')
            return s + ('_' + str((k >> 32) - 1) if k >> 32 else '')
        def wide(p, limit=512):
            # Read one UTF-16 unit at a time so a short string at a page boundary
            # never requires reading unrelated/inaccessible memory.
            b = bytearray()
            for i in range(limit):
                v = m.read(p + i * 2, 2)
                if v == b'\0\0': return b.decode('utf-16le')
                b.extend(v)
            raise ValueError('unterminated text key')
        weak = m.q(0x149031e73 + 0x62b0055)
        chunks = m.q(0x149031e94 + 0x5c36a8c)
        item = m.q(chunks + 8 * ((weak & 0xffffffff) >> 16)) + 24 * (weak & 65535)
        if m.u(item + 16) != weak >> 32: raise ValueError('stale global object')
        g = m.q(item)
        locale = wide(m.q(g + 712), 32)
        table = 0x14f2975e0
        count = m.u(table + 104)
        if not 1 <= count <= 50000: raise ValueError('unexpected skin table')
        rows = m.read(m.q(table + 96), count * 16)
        skins = []; wanted = set()
        for i in range(count):
            row = struct.unpack_from('<Q', rows, i * 16)[0]
            if not row: continue
            keys = {}
            for label, off in [('light', 20), ('dark', 36)]:
                if m.q(row + off + 8): raise ValueError('indirect text reference requires a new resolver')
                k = m.q(row + off)
                keys[label] = fname(k) if k else ''
                wanted.add(keys[label])
            skins.append({'id': m.u(row + 8), 'key': fname(m.q(row + 12)), 'text_keys': keys})
        registry = 0x14f3060a0
        reg_count = m.u(registry + 8)
        if not 1 <= reg_count <= 256: raise ValueError('unexpected string-table registry')
        text_table = None
        for i in range(reg_count):
            node = m.q(registry) + i * 32
            if fname(m.q(node)) == 'AION2': text_table = m.q(node + 8); break
        if not text_table: raise ValueError('AION2 string table missing')
        n = m.u(text_table + 56)
        if not 0 < n <= 500000: raise ValueError('unexpected text count')
        data = m.read(m.q(text_table + 48), n * 40)
        texts = {}
        for i in range(n):
            kp, _, entry = struct.unpack_from('<QQQ', data, i * 40)
            if not kp or not entry: continue
            # Cheap prefix check; load only skin localization keys.
            try:
                if m.read(kp, 8) != 'Skin'.encode('utf-16le'): continue
                key = wide(kp)
                if key not in wanted: continue
                length = m.u(entry + 24)
                if not 0 < length <= 4096: raise ValueError('unexpected display text length')
                texts[key] = m.read(m.q(entry + 16), length * 2).decode('utf-16le').rstrip('\0')
            except OSError:
                continue
        for skin in skins:
            skin['names'] = {side: texts.get(key, '') for side, key in skin['text_keys'].items()}
        return {'schema': 1, 'source': 'Steam 2026-09-30', 'locale': locale,
                'exported_at': datetime.datetime.now(datetime.timezone.utc).isoformat(),
                'skins': skins, 'unresolved_names': sum(not s['names'][k] for s in skins for k in ('light', 'dark'))}
    finally:
        m.close()

if __name__ == '__main__':
    ap = argparse.ArgumentParser(); ap.add_argument('pid', type=int); ap.add_argument('output', type=Path)
    args = ap.parse_args(); result = export(args.pid)
    if result['unresolved_names']:
        raise SystemExit('Incomplete live snapshot; catalog was not replaced. Retry after the game tables finish loading.')
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(result, ensure_ascii=False, indent=2), encoding='utf-8')
    print(json.dumps({k: v for k, v in result.items() if k != 'skins'}, ensure_ascii=False))
    print('skins:', len(result['skins'])); print(result['skins'][:2])
