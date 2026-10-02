# -*- mode: python ; coding: utf-8 -*-
from pathlib import Path
from PyInstaller.utils.hooks import collect_all, collect_submodules

root = Path(SPECPATH)
source = root / "source"
datas = [(str(source / "mitm_ws_message_monitor.py"), ".")]
binaries = []
hiddenimports = ["sqlite3", "_sqlite3", "concurrent.futures.thread"]
for package in ("mitmproxy", "aioquic", "paho"):
    hiddenimports += collect_submodules(package)
for package in ("mitmproxy", "mitmproxy_rs", "aioquic", "paho"):
    package_datas, package_binaries, package_hidden = collect_all(package)
    datas += package_datas
    binaries += package_binaries
    hiddenimports += package_hidden

a = Analysis(
    [str(source / "run_client.py")],
    pathex=[str(source)],
    binaries=binaries,
    datas=datas,
    hiddenimports=hiddenimports,
    hookspath=[],
    hooksconfig={},
    runtime_hooks=[],
    excludes=[],
    noarchive=False,
    optimize=0,
)
pyz = PYZ(a.pure)
exe = EXE(
    pyz,
    a.scripts,
    [],
    exclude_binaries=True,
    name="Aion2ChatBridge",
    debug=False,
    bootloader_ignore_signals=False,
    strip=False,
    upx=True,
    console=True,
    disable_windowed_traceback=False,
)
coll = COLLECT(
    exe,
    a.binaries,
    a.datas,
    strip=False,
    upx=True,
    name="Aion2ChatBridge",
)
