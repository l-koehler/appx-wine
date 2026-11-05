#!/usr/bin/env python3
# Prints a field of an AppxManifest.xml: name | executable | arch | dependencies
# usage: manifest.py AppxManifest.xml FIELD
import sys
import xml.etree.ElementTree as ET

root = ET.parse(sys.argv[1]).getroot()
ns = {"m": root.tag[1:root.tag.index("}")]} if root.tag.startswith("{") else {"m": ""}
q = (lambda t: f"m:{t}") if ns["m"] else (lambda t: t)
ident = root.find(q("Identity"), ns)
field = sys.argv[2]
if field == "name":
    print(ident.get("Name"))
elif field == "arch":
    print(ident.get("ProcessorArchitecture", "neutral"))
elif field == "executable":
    print(root.find(f"{q('Applications')}/{q('Application')}", ns).get("Executable"))
elif field == "dependencies":
    for d in root.iter(root.tag.replace("Package", "PackageDependency")):
        print(d.get("Name"), d.get("MinVersion"))
else:
    sys.exit(f"unknown field {field}")
