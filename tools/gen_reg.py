#!/usr/bin/env python3
# Writes a .reg file registering the shim for every metadata class Wine doesn't implement.
# usage: gen_reg.py winrt_meta.c current.reg 'C:\path\winrtshim.dll' out.reg
# current.reg is `wine reg export HKLM\Software\Microsoft\WindowsRuntime\ActivatableClassId`;
# a class counts as Wine's when its DllPath points into C:\windows.
import re
import sys

# Wine implements these, but apps need the shim's behaviour
FORCE = {"Windows.ApplicationModel.Package", "Windows.Storage.ApplicationData", "Windows.Foundation.Uri",
         "Windows.Security.Cryptography.CryptographicBuffer", "Windows.Networking.Connectivity.NetworkInformation"}

meta, existing, dll, out = sys.argv[1:5]
src = open(meta).read()
block = src[src.index("meta_classes[]"):src.index("meta_lists[]")]
classes = re.findall(r'^  \{"([^"]+)", -?\d+,', block, re.M)

raw = open(existing, "rb").read()
text = raw.decode("utf-16") if raw[:2] in (b"\xff\xfe", b"\xfe\xff") else raw.decode("utf-8", "replace")
wine, key = set(), None
for line in text.splitlines():
    m = re.match(r"\[.*\\ActivatableClassId\\([^\\\]]+)\]$", line.strip())
    if m:
        key = m.group(1)
    elif key and line.lower().startswith('"dllpath"="c:\\\\windows'):
        wine.add(key)

dll = dll.replace("\\", "\\\\")
with open(out, "w") as f:
    f.write("REGEDIT4\n\n")
    n = 0
    for c in classes:
        if c in wine and c not in FORCE:
            continue
        f.write(f"[HKEY_LOCAL_MACHINE\\Software\\Microsoft\\WindowsRuntime\\ActivatableClassId\\{c}]\n")
        f.write(f"\"DllPath\"=\"{dll}\"\n\"ActivationType\"=dword:00000000\n\"Threading\"=dword:00000000\n\n")
        n += 1
print(f"{n} classes registered, {len(wine)} left to Wine", file=sys.stderr)
