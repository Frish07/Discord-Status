"""Audit and launch a distributed ZIP using only Python's standard library.

Python runs the CI checks; it is NOT used by the application. The EXE is launched
from its own temporary directory with PATH restricted to Windows system folders.
"""
import argparse
import ctypes
from ctypes import wintypes
import hashlib
import json
import os
from pathlib import Path
import platform
import re
import struct
import subprocess
import tempfile
import time
import zipfile


# Deliberate allowlist: these DLLs ship with the supported Windows versions.
# An accidental /MD, Qt, MinGW DLL or WebView dependency must fail this check.
SYSTEM_IMPORTS = {
    'kernel32.dll', 'user32.dll', 'gdi32.dll', 'comdlg32.dll', 'comctl32.dll',
    'shell32.dll', 'ole32.dll', 'oleaut32.dll', 'advapi32.dll', 'd2d1.dll',
    'dwrite.dll', 'dwmapi.dll', 'ntdll.dll', 'shlwapi.dll', 'uxtheme.dll',
    'imm32.dll', 'version.dll', 'bcrypt.dll', 'crypt32.dll', 'rpcrt4.dll',
    'ws2_32.dll', 'msimg32.dll', 'userenv.dll', 'secur32.dll',
}
EXTERNAL_RUNTIME = re.compile(r'^(?:vcruntime\d|msvcp\d|msvcr\d|concrt\d|vcomp\d|'
                              r'libgcc|libstdc\+\+|libwinpthread|qt[56]|webview2loader)', re.I)


def pe_imports(data):
    """Read ordinary AND delay-loaded imports; no dumpbin/VS/third-party packages."""
    if data[:2] != b'MZ':
        raise ValueError('Not a Windows executable')
    pe = struct.unpack_from('<I', data, 0x3c)[0]
    if data[pe:pe + 4] != b'PE\0\0':
        raise ValueError('Invalid PE signature')
    machine, count = struct.unpack_from('<HH', data, pe + 4)
    if machine != 0x8664:
        raise ValueError('Expected a Windows x64 executable')
    opt_size = struct.unpack_from('<H', data, pe + 20)[0]
    opt = pe + 24
    if struct.unpack_from('<H', data, opt)[0] != 0x20b:
        raise ValueError('Expected PE32+')
    image_base = struct.unpack_from('<Q', data, opt + 24)[0]
    directory_count = struct.unpack_from('<I', data, opt + 108)[0]
    sections = []
    for i in range(count):
        section = opt + opt_size + i * 40
        vsize, rva, raw_size, raw = struct.unpack_from('<IIII', data, section + 8)
        sections.append((rva, max(vsize, raw_size), raw))

    def offset(rva):
        for start, size, raw in sections:
            if start <= rva < start + size:
                return raw + rva - start
        raise ValueError(f'Unmapped PE RVA: {rva:x}')

    def dll_name(rva):
        start = offset(rva)
        end = data.find(b'\0', start, start + 512)
        if end < 0:
            raise ValueError('Unterminated import name')
        return data[start:end].decode('ascii').lower()

    imports, delayed = set(), set()
    for index, stride, result in ((1, 20, imports), (13, 32, delayed)):
        if index >= directory_count:
            continue
        rva, size = struct.unpack_from('<II', data, opt + 112 + index * 8)
        if not rva:
            continue
        pos = offset(rva)
        # A corrupt import table must fail, rather than silently pass the audit.
        for step in range(max(size // stride, 1)):
            entry = struct.unpack_from('<' + 'I' * (stride // 4), data, pos + step * stride)
            if not any(entry):
                break
            name = entry[3] if index == 1 else entry[1]
            if index == 13 and not entry[0] & 1:
                name -= image_base
            result.add(dll_name(name))
        else:
            raise ValueError('Import table has no terminator')
    return sorted(imports), sorted(delayed)


def audit(data):
    ordinary, delayed = pe_imports(data)
    if not ordinary:
        raise ValueError('Import table is empty')
    unexpected = (set(ordinary) | set(delayed)) - SYSTEM_IMPORTS
    if unexpected:
        raise ValueError('Non-system DLL dependencies: ' + ', '.join(sorted(unexpected)))
    return ordinary, delayed


def launch_check(exe):
    user32 = ctypes.WinDLL('user32', use_last_error=True)
    kernel32 = ctypes.WinDLL('kernel32', use_last_error=True)
    psapi = ctypes.WinDLL('psapi', use_last_error=True)
    callback_type = ctypes.WINFUNCTYPE(wintypes.BOOL, wintypes.HWND, wintypes.LPARAM)
    user32.EnumWindows.argtypes = [callback_type, wintypes.LPARAM]
    user32.GetWindowThreadProcessId.argtypes = [wintypes.HWND, ctypes.POINTER(wintypes.DWORD)]
    user32.GetClassNameW.argtypes = [wintypes.HWND, wintypes.LPWSTR, ctypes.c_int]
    user32.IsWindowVisible.argtypes = [wintypes.HWND]
    user32.IsIconic.argtypes = [wintypes.HWND]
    user32.SendMessageTimeoutW.argtypes = [wintypes.HWND, wintypes.UINT, wintypes.WPARAM,
                                          wintypes.LPARAM, wintypes.UINT, wintypes.UINT,
                                          ctypes.POINTER(ctypes.c_size_t)]
    user32.SendMessageTimeoutW.restype = ctypes.c_size_t
    kernel32.OpenProcess.argtypes = [wintypes.DWORD, wintypes.BOOL, wintypes.DWORD]
    kernel32.OpenProcess.restype = wintypes.HANDLE
    kernel32.CloseHandle.argtypes = [wintypes.HANDLE]
    psapi.EnumProcessModules.argtypes = [wintypes.HANDLE, ctypes.POINTER(wintypes.HMODULE),
                                        wintypes.DWORD, ctypes.POINTER(wintypes.DWORD)]
    psapi.GetModuleFileNameExW.argtypes = [wintypes.HANDLE, wintypes.HMODULE, wintypes.LPWSTR, wintypes.DWORD]
    env = os.environ.copy()
    # os.environ is case-insensitive on Windows; its plain dict copy is not.
    system = Path(os.environ['SystemRoot']).resolve()
    env['PATH'] = str(system / 'System32') + os.pathsep + str(system)
    env.pop('QT_PLUGIN_PATH', None)
    startup = subprocess.STARTUPINFO()
    startup.dwFlags = subprocess.STARTF_USESHOWWINDOW
    startup.wShowWindow = 4  # SW_SHOWNOACTIVATE
    process = subprocess.Popen([str(exe)], cwd=exe.parent, env=env, startupinfo=startup)
    handle = None
    try:
        windows = []

        @callback_type
        def find_window(hwnd, unused):
            pid = wintypes.DWORD()
            user32.GetWindowThreadProcessId(hwnd, ctypes.byref(pid))
            name = ctypes.create_unicode_buffer(256)
            user32.GetClassNameW(hwnd, name, len(name))
            if pid.value == process.pid and name.value == 'DiscordPresenceManager':
                windows.append(hwnd)
            return True

        until = time.monotonic() + 15
        while time.monotonic() < until:
            if process.poll() is not None:
                raise RuntimeError(f'EXE exited at startup: {process.returncode:#x}')
            user32.EnumWindows(find_window, 0)
            if windows:
                break
            time.sleep(.1)
        if not windows:
            raise RuntimeError('EXE did not create its main window')
        hwnd = windows[0]

        def send(message, wp=0):
            result = ctypes.c_size_t()
            if not user32.SendMessageTimeoutW(hwnd, message, wp, 0, 2, 5000, ctypes.byref(result)):
                raise RuntimeError(f'Window did not respond to message {message:#x}')

        send(0)  # WM_NULL: main UI is responsive
        time.sleep(3)
        if process.poll() is not None:
            raise RuntimeError('EXE crashed after startup')
        handle = kernel32.OpenProcess(0x0400 | 0x0010, False, process.pid)
        if not handle:
            raise ctypes.WinError(ctypes.get_last_error())
        modules = (wintypes.HMODULE * 1024)()
        needed = wintypes.DWORD()
        if not psapi.EnumProcessModules(handle, modules, ctypes.sizeof(modules), ctypes.byref(needed)):
            raise ctypes.WinError(ctypes.get_last_error())
        if needed.value > ctypes.sizeof(modules):
            raise RuntimeError('Module list exceeds audit capacity')
        loaded = []
        for module in modules[:needed.value // ctypes.sizeof(wintypes.HMODULE)]:
            path_buffer = ctypes.create_unicode_buffer(32768)
            if not psapi.GetModuleFileNameExW(handle, module, path_buffer, len(path_buffer)):
                raise ctypes.WinError(ctypes.get_last_error())
            path = Path(path_buffer.value).resolve()
            if path == exe:
                continue
            if EXTERNAL_RUNTIME.match(path.name):
                raise RuntimeError(f'External runtime loaded: {path.name}')
            if system not in path.parents:
                raise RuntimeError(f'Non-Windows module loaded: {path}')
            loaded.append(path.name)
        send(0x0010)  # WM_CLOSE must keep the real distributed EXE alive
        time.sleep(.5)
        if process.poll() is not None:
            raise RuntimeError('Closing the window stopped the background process')
        if user32.IsWindowVisible(hwnd) and not user32.IsIconic(hwnd):
            raise RuntimeError('Closing the window did not hide/minimize it')
        send(0x8003)  # RestoreMessage: same instance returns from tray
        if not user32.IsWindowVisible(hwnd):
            raise RuntimeError('Window did not restore from tray')
        send(0)
        send(0x0111, 403)  # WM_COMMAND / explicit ExitCommand
        if process.wait(timeout=8) != 0:
            raise RuntimeError(f'Explicit exit returned {process.returncode}')
        return sorted(loaded, key=str.lower)
    finally:
        if handle:
            kernel32.CloseHandle(handle)
        if process.poll() is None:
            process.kill()
            process.wait(timeout=5)


def check(archive, sums, audit_only=False):
    archive_data = archive.read_bytes()
    digest = hashlib.sha256(archive_data).hexdigest()
    if digest != sums.read_text(encoding='utf-8-sig').split()[0].lower():
        raise ValueError('ZIP SHA-256 mismatch')
    with zipfile.ZipFile(archive) as package:
        exes = [entry for entry in package.namelist() if entry.lower().endswith('.exe')]
        if exes != ['DiscordPresenceManager.exe']:
            raise ValueError(f'Unexpected executables in package: {exes}')
        if any(entry.lower().endswith(('.dll', '.msi', '.ps1', '.bat')) for entry in package.namelist()):
            raise ValueError('Package must contain a standalone EXE and documents only')
        data = package.read(exes[0])
    ordinary, delayed = audit(data)
    # Regression guard: the auditor must reject a non-system import, even if a
    # developer machine happens to have that DLL installed.
    tampered = data.replace(b'COMDLG32.dll\0', b'Qt6Core.dll\0\0', 1)
    if tampered == data:
        raise ValueError('Missing expected COMDLG32 import for auditor regression check')
    try:
        audit(tampered)
    except ValueError as error:
        if 'Non-system DLL dependencies: qt6core.dll' not in str(error):
            raise AssertionError('Auditor rejected malformed data instead of detecting the external DLL') from error
    else:
        raise AssertionError('Dependency auditor accepted a third-party DLL')
    report = {'platform': platform.platform(), 'archive_sha256': digest,
              'exe_sha256': hashlib.sha256(data).hexdigest(),
              'imports': ordinary, 'delay_imports': delayed, 'audit': 'passed'}
    if not audit_only:
        with tempfile.TemporaryDirectory(prefix='Presence Studio portable ') as folder:
            # Only the EXE is deployed, not source files, test binaries or DLLs.
            directory = Path(folder) / 'Тест без пакетов'
            directory.mkdir()
            exe = (directory / 'DiscordPresenceManager.exe').resolve()
            exe.write_bytes(data)
            report['loaded_windows_modules'] = launch_check(exe)
            report['launch_with_system_path_only'] = 'passed'
            report['close_restore_exit'] = 'passed'
    return report


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('archive', type=Path)
    parser.add_argument('checksums', type=Path)
    parser.add_argument('--audit-only', action='store_true')
    parser.add_argument('--report', type=Path, required=True)
    args = parser.parse_args()
    try:
        result = check(args.archive, args.checksums, args.audit_only)
    except Exception as error:
        args.report.parent.mkdir(parents=True, exist_ok=True)
        args.report.write_text(json.dumps({'result': 'failed', 'error': str(error)}, indent=2), encoding='utf-8')
        raise
    args.report.parent.mkdir(parents=True, exist_ok=True)
    args.report.write_text(json.dumps(result, indent=2), encoding='utf-8')
    print(json.dumps(result, indent=2))
