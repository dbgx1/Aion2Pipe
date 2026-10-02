"""Build v13.2 from the clean v13.1 archive, without copying runtime logs/state."""
import hashlib
import json
from pathlib import Path
import zipfile

root = Path(__file__).resolve().parents[1]
name = 'Aion2Pipe-2026.10.01-v13.2-win64-portable'
base = root / 'release/Aion2Pipe-2026.10.01-v13.1-win64-portable.zip'
output = root / 'release' / name
output.mkdir(exist_ok=True)
with zipfile.ZipFile(base) as source:
    for entry in source.infolist():
        parts = Path(entry.filename.replace('\\', '/')).parts
        if len(parts) < 2 or entry.is_dir():
            continue
        target = output.joinpath(*parts[1:]).resolve()
        if not target.is_relative_to(output.resolve()):
            raise ValueError('Archive path escaped package')
        target.parent.mkdir(parents=True, exist_ok=True)
        target.write_bytes(source.read(entry))

for source, destination in [
    ('build/Release/Aion2Pipe.exe', 'Aion2Pipe.exe'),
    ('chat_bridge/source/mitm_ws_message_monitor.py', 'chat_bridge/_internal/mitm_ws_message_monitor.py'),
    ('docs/QUERY_SERVICE.md', 'docs/QUERY_SERVICE.md'),
]:
    (output / destination).write_bytes((root / source).read_bytes())

instructions = output / '使用说明.txt'
instructions.write_text(instructions.read_text(encoding='utf-8-sig') + '''

v13.2 多电脑修复：
- 不同电脑的查询服务可同时注册，按各自区服接收任务。
- 客户端识别到游戏区服后，自动同步到控制台客户端列表。
- 区服同步无需先在游戏聊天中发言。

请在需要修复的电脑解压整个目录，不要只替换主程序。
退出旧版及游戏，运行新版，再进入游戏建立新连接；控制台刷新并等待下一次心跳。
''', encoding='utf-8-sig')
files = sorted(p for p in output.rglob('*') if p.is_file() and p.name not in ('manifest.json', 'SHA256SUMS.txt'))
assert not any(p.suffix in ('.log', '.jsonl', '.sqlite3') for p in files), 'Runtime data must not be packaged'
(output / 'manifest.json').write_text(json.dumps([{'path':p.relative_to(output).as_posix(),'size':p.stat().st_size} for p in files],ensure_ascii=False,indent=2),encoding='utf-8')
(output / 'SHA256SUMS.txt').write_text('\n'.join(hashlib.sha256(p.read_bytes()).hexdigest()+'  '+p.relative_to(output).as_posix() for p in files)+'\n',encoding='utf-8')
archive = output.with_suffix('.zip')
# with_suffix would remove the dotted version suffix, so retain the full name.
archive = output.parent / (name + '.zip')
with zipfile.ZipFile(archive, 'w', compression=zipfile.ZIP_DEFLATED, compresslevel=6) as target:
    for path in output.rglob('*'):
        if path.is_file():
            target.write(path, name + '/' + path.relative_to(output).as_posix())
with zipfile.ZipFile(archive) as check:
    assert check.testzip() is None
    assert check.read(name+'/Aion2Pipe.exe') == (root/'build/Release/Aion2Pipe.exe').read_bytes()
    assert check.read(name+'/chat_bridge/_internal/mitm_ws_message_monitor.py') == (root/'chat_bridge/source/mitm_ws_message_monitor.py').read_bytes()
print(json.dumps({'archive':str(archive),'bytes':archive.stat().st_size,'sha256':hashlib.sha256(archive.read_bytes()).hexdigest()}))
