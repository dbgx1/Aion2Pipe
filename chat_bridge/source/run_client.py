import os
import subprocess
import sys
import traceback
import account_switch  # Include local login IPC modules in frozen client builds.
from datetime import datetime
from pathlib import Path

from cryptography import x509
from cryptography.hazmat.primitives import hashes
from mitmproxy import certs
from mitmproxy.tools.main import mitmdump, mitmweb


CLIENT_BUILD = "2026-10-01.aion2pipe-integrated.2"
ROOT = Path(__file__).resolve().parent
FROZEN_ROOT = Path(getattr(sys, "_MEIPASS", ROOT))
APP_DIR = Path(sys.executable).resolve().parent if getattr(sys, "frozen", False) else ROOT.parent
ADDON = FROZEN_ROOT / "mitm_ws_message_monitor.py"
LOG_FILE = APP_DIR / "aion2-client.log"
MITMPROXY_DIR = Path(os.path.expanduser(os.getenv("AION2_MITM_CONFDIR", "~/.mitmproxy")))
MITMPROXY_CERT = MITMPROXY_DIR / "mitmproxy-ca-cert.cer"
def _env_enabled(name: str) -> bool:
    return os.getenv(name, "").strip().lower() in {"1", "true", "yes", "on"}


def _env_disabled(name: str) -> bool:
    return os.getenv(name, "").strip().lower() in {"0", "false", "no", "off"}


def _log(message: str) -> None:
    print(message, flush=True)
    try:
        with LOG_FILE.open("a", encoding="utf-8") as file:
            file.write(f"{datetime.now().isoformat(timespec='seconds')} {message}\n")
    except OSError:
        pass


def _is_admin() -> bool | None:
    if os.name != "nt":
        return None
    try:
        import ctypes

        return bool(ctypes.windll.shell32.IsUserAnAdmin())
    except Exception:
        return None


def _pause_on_error() -> None:
    if _env_disabled("AION2_PAUSE_ON_ERROR"):
        return
    if getattr(sys, "frozen", False) or _env_enabled("AION2_PAUSE_ON_ERROR"):
        try:
            input("Press Enter to close...")
        except EOFError:
            pass


def _certificate_thumbprint(cert_path: Path) -> str:
    certificate = x509.load_pem_x509_certificate(cert_path.read_bytes())
    return certificate.fingerprint(hashes.SHA1()).hex().upper()


def _certificate_store_args() -> tuple[list[str], str, str]:
    store = os.getenv("AION2_CERT_STORE", "user").strip().lower()
    if store == "machine":
        return [], "本机", "LocalMachine"
    if store != "user":
        _log(f"[AION2] warning: unknown AION2_CERT_STORE={store!r}; using user store.")
    return ["-user"], "当前用户", "CurrentUser"


def _certificate_is_installed(cert_path: Path, store_location: str) -> bool:
    thumbprint = _certificate_thumbprint(cert_path)
    script = (
        "$store = [System.Security.Cryptography.X509Certificates.X509Store]::new('Root', '"
        + store_location
        + "'); $store.Open('ReadOnly'); try { "
        + "$found = $store.Certificates.Find('FindByThumbprint', '"
        + thumbprint
        + "', $false).Count -gt 0; if ($found) { exit 0 } else { exit 1 } "
        + "} finally { $store.Close() }"
    )
    result = subprocess.run(
        ["powershell.exe", "-NoProfile", "-NonInteractive", "-Command", script],
        stdin=subprocess.DEVNULL,
        stdout=subprocess.DEVNULL,
        stderr=subprocess.DEVNULL,
        check=False,
    )
    return result.returncode == 0


def _confirm_certificate_install(store_label: str) -> bool:
    setting = os.getenv("AION2_AUTO_INSTALL_CERT", "").strip()
    if setting:
        return _env_enabled("AION2_AUTO_INSTALL_CERT")

    message = (
        "AION2 客户端需要信任本机刚生成的 mitmproxy CA 证书，才能读取 HTTPS/WSS 消息。\n\n"
        f"证书将安装到：{store_label} / 受信任的根证书颁发机构\n"
        f"证书文件：{MITMPROXY_CERT}\n\n"
        "此证书仅在当前电脑生成和使用。是否安装？"
    )
    try:
        import ctypes

        return ctypes.windll.user32.MessageBoxW(None, message, "AION2 客户端 - 安装证书", 0x134) == 6
    except Exception:
        try:
            return input(f"{message}\n输入 y 确认安装：").strip().lower() in {"y", "yes"}
        except EOFError:
            return False


def _ensure_mitmproxy_certificate() -> None:
    if os.name != "nt":
        return

    certs.CertStore.from_store(MITMPROXY_DIR, "mitmproxy", key_size=2048)
    if not MITMPROXY_CERT.exists():
        raise RuntimeError(f"mitmproxy certificate was not generated: {MITMPROXY_CERT}")

    store_args, store_label, store_location = _certificate_store_args()
    if _certificate_is_installed(MITMPROXY_CERT, store_location):
        _log(f"[AION2] mitmproxy certificate is trusted for {store_label}.")
        return

    if _env_disabled("AION2_AUTO_INSTALL_CERT"):
        _log("[AION2] automatic certificate installation is disabled.")
        return
    if not _confirm_certificate_install(store_label):
        _log("[AION2] certificate installation was declined.")
        return

    _log(f"[AION2] installing mitmproxy certificate for {store_label}...")
    result = subprocess.run(
        ["certutil.exe", *store_args, "-f", "-addstore", "Root", str(MITMPROXY_CERT)],
        check=False,
    )
    if result.returncode != 0:
        if not store_args and _is_admin() is False:
            raise RuntimeError("安装到本机证书库失败：请以管理员身份运行客户端。")
        raise RuntimeError(f"certutil failed with exit code {result.returncode}")
    if not _certificate_is_installed(MITMPROXY_CERT, store_location):
        raise RuntimeError("证书导入命令已完成，但未能在根证书库中验证该证书。")
    _log(f"[AION2] mitmproxy certificate installed for {store_label}.")


def _build_args(tool_name: str) -> list[str]:
    detected_mode = "virtual-adapter"
    proxy_process = "Aion2Pipe.exe"
    default_mode = f"regular@127.0.0.1:{os.getenv('AION2_RELAY_PROXY_PORT', '18080').strip() or '18080'}"
    proxy_mode = os.getenv("AION2_MITM_MODE", default_mode).strip() or default_mode

    args = [
        tool_name,
        "-s",
        str(ADDON),
        "--set",
        f"confdir={MITMPROXY_DIR}",
        "--mode",
        proxy_mode,
        "--set",
        "rawtcp=true",
    ]

    allow_hosts_setting = os.getenv("AION2_ALLOW_HOSTS")
    allow_hosts = (
        r"^(?:limep2-pub\.global|lime-p2-api\.global|lime-arizona-p4-(?:api|pub))\.plaync\.com:\d+$"
        if allow_hosts_setting is None
        else allow_hosts_setting.strip()
    )
    if allow_hosts:
        args.extend(["--allow-hosts", allow_hosts])

    ignore_hosts = os.getenv("AION2_IGNORE_HOSTS", "").strip()
    if ignore_hosts:
        args.extend(["--ignore-hosts", ignore_hosts])

    connection_strategy = os.getenv("AION2_CONNECTION_STRATEGY", "").strip()
    if connection_strategy:
        args.extend(["--set", f"connection_strategy={connection_strategy}"])

    if tool_name == "mitmweb":
        args.extend(
            [
                "--web-host",
                os.getenv("AION2_WEB_HOST", "127.0.0.1"),
                "--web-port",
                os.getenv("AION2_WEB_PORT", "8081"),
            ]
        )
        if _env_enabled("AION2_WEB_OPEN_BROWSER") or not _env_disabled(
            "AION2_WEB_OPEN_BROWSER"
        ):
            args.append("--web-open-browser")

    _log(f"[AION2] build={CLIENT_BUILD}")
    _log(f"[AION2] network_mode={detected_mode} proxy_process={proxy_process}")
    _log(f"[AION2] tool={tool_name} mode={proxy_mode} addon={ADDON}")
    if allow_hosts:
        _log(f"[AION2] allow-hosts={allow_hosts}")
    if ignore_hosts:
        _log(f"[AION2] ignore-hosts={ignore_hosts}")
    if connection_strategy:
        _log(f"[AION2] connection_strategy={connection_strategy}")
    if proxy_mode.startswith("local:"):
        admin = _is_admin()
        _log("[AION2] local mode may require running this client as Administrator.")
        if admin is False:
            _log("[AION2] warning: this process is not running as Administrator.")

    return args


def main() -> int | None:
    tool_name = os.getenv("AION2_MITM_TOOL", "mitmweb").strip().lower()
    if tool_name not in {"mitmweb", "mitmdump"}:
        raise SystemExit("AION2_MITM_TOOL must be mitmweb or mitmdump")

    _ensure_mitmproxy_certificate()
    sys.argv = _build_args(tool_name)
    if tool_name == "mitmweb":
        return mitmweb()
    else:
        return mitmdump()


def cli() -> int:
    try:
        result = main()
        return int(result or 0)
    except KeyboardInterrupt:
        _log("[AION2] interrupted")
        return 130
    except SystemExit as exc:
        code = exc.code
        if code in (None, 0):
            return 0
        _log(f"[AION2] exited: {code}")
        _pause_on_error()
        return code if isinstance(code, int) else 1
    except BaseException:
        details = traceback.format_exc()
        _log("[AION2] fatal error:")
        for line in details.rstrip().splitlines():
            _log(line)
        _pause_on_error()
        return 1


if __name__ == "__main__":
    raise SystemExit(cli())
