"""Resolve module-relative addresses to symbol+offset (and file:line) using dbghelp and a PDB.

Usage: python symbolize.py <module.exe|dll> <pdb_dir> <rva> [rva ...]
"""
import ctypes
import ctypes.wintypes as wt
import os
import sys

dbghelp = ctypes.WinDLL("dbghelp.dll")

SYMOPT_UNDNAME = 0x2
SYMOPT_LOAD_LINES = 0x10
MAX_SYM_NAME = 2000


class SYMBOL_INFO(ctypes.Structure):
    _fields_ = [
        ("SizeOfStruct", wt.ULONG), ("TypeIndex", wt.ULONG), ("Reserved", ctypes.c_ulonglong * 2),
        ("Index", wt.ULONG), ("Size", wt.ULONG), ("ModBase", ctypes.c_ulonglong), ("Flags", wt.ULONG),
        ("Value", ctypes.c_ulonglong), ("Address", ctypes.c_ulonglong), ("Register", wt.ULONG),
        ("Scope", wt.ULONG), ("Tag", wt.ULONG), ("NameLen", wt.ULONG), ("MaxNameLen", wt.ULONG),
        ("Name", ctypes.c_char * MAX_SYM_NAME),
    ]


class IMAGEHLP_LINE64(ctypes.Structure):
    _fields_ = [("SizeOfStruct", wt.DWORD), ("Key", ctypes.c_void_p), ("LineNumber", wt.DWORD),
                ("FileName", ctypes.c_char_p), ("Address", ctypes.c_ulonglong)]


def main():
    module, pdb_dir = sys.argv[1], sys.argv[2]
    proc = wt.HANDLE(0x1234)
    dbghelp.SymSetOptions(SYMOPT_UNDNAME | SYMOPT_LOAD_LINES)
    if not dbghelp.SymInitialize(proc, pdb_dir.encode(), False):
        raise OSError("SymInitialize failed")
    base = 0x10000000
    dbghelp.SymLoadModuleEx.restype = ctypes.c_ulonglong
    dbghelp.SymLoadModuleEx.argtypes = [wt.HANDLE, wt.HANDLE, ctypes.c_char_p, ctypes.c_char_p,
                                        ctypes.c_ulonglong, wt.DWORD, ctypes.c_void_p, wt.DWORD]
    loaded = dbghelp.SymLoadModuleEx(proc, None, module.encode(), None, base, os.path.getsize(module), None, 0)
    if not loaded:
        raise OSError("SymLoadModuleEx failed")
    for text in sys.argv[3:]:
        rva = int(text, 16)
        sym = SYMBOL_INFO()
        sym.SizeOfStruct = ctypes.sizeof(SYMBOL_INFO) - MAX_SYM_NAME
        sym.MaxNameLen = MAX_SYM_NAME
        disp = ctypes.c_ulonglong(0)
        name = "?"
        if dbghelp.SymFromAddr(proc, ctypes.c_ulonglong(base + rva), ctypes.byref(disp), ctypes.byref(sym)):
            name = f"{sym.Name.decode(errors='replace')}+0x{disp.value:X}"
        line = IMAGEHLP_LINE64()
        line.SizeOfStruct = ctypes.sizeof(IMAGEHLP_LINE64)
        ldisp = wt.DWORD(0)
        where = ""
        if dbghelp.SymGetLineFromAddr64(proc, ctypes.c_ulonglong(base + rva), ctypes.byref(ldisp), ctypes.byref(line)):
            where = f"  {line.FileName.decode(errors='replace')}:{line.LineNumber}"
        print(f"+0x{rva:X}  {name}{where}")
    dbghelp.SymCleanup(proc)


if __name__ == "__main__":
    main()
