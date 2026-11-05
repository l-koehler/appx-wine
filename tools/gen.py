#!/usr/bin/env python3
# Generates winrt_meta.c (interface/class tables with x86 ABI stack sizes) from .winmd files.
# Generic interfaces are instantiated for every closed instance the metadata mentions,
# with IIDs computed by the WinRT parameterized-interface algorithm.
# usage: gen.py OUT.c FILE.winmd...
import hashlib
import struct
import sys
import uuid

import dnfile

# ret kinds / param kinds (must match shim.h)
RK_VOID, RK_VALUE, RK_HSTRING, RK_CLASS, RK_IFACE, RK_OBJECT, RK_GENERIC, RK_ARRAY = range(8)
PK_VALUE, PK_OBJECT, PK_HSTRING = range(3)

PRIM = {0x02: ("Boolean", "b1", 1), 0x03: ("Char16", "c2", 2), 0x04: ("Int8", "i1", 1),
        0x05: ("UInt8", "u1", 1), 0x06: ("Int16", "i2", 2), 0x07: ("UInt16", "u2", 2),
        0x08: ("Int32", "i4", 4), 0x09: ("UInt32", "u4", 4), 0x0a: ("Int64", "i8", 8),
        0x0b: ("UInt64", "u8", 8), 0x0c: ("Single", "f4", 4), 0x0d: ("Double", "f8", 8),
        0x18: ("IntPtr", "i4", 4), 0x19: ("UIntPtr", "u4", 4)}
PINTERFACE_NS = uuid.UUID("11f47ad5-7b73-42c0-abae-878b1e16adee").bytes


class Reader:
    def __init__(self, data):
        self.d = data
        self.p = 0

    def u8(self):
        v = self.d[self.p]
        self.p += 1
        return v

    def peek(self):
        return self.d[self.p]

    def cu(self):
        b = self.u8()
        if b & 0x80 == 0:
            return b
        if b & 0xc0 == 0x80:
            return ((b & 0x3f) << 8) | self.u8()
        return ((b & 0x1f) << 24) | (self.u8() << 16) | (self.u8() << 8) | self.u8()


def rowname(r):
    return f"{r.TypeNamespace}.{r.TypeName}"


class Meta:
    def __init__(self, paths):
        self.types = {}  # full name -> (md, TypeDefRow, rowidx)
        self.mds = []
        for path in paths:
            md = dnfile.dnPE(path).net.mdtables
            self.mds.append(md)
            for i, row in enumerate(md.TypeDef.rows):
                if row.TypeName == "<Module>":
                    continue
                self.types.setdefault(rowname(row), (md, row, i + 1))
        self.ca_typedef = {}   # (id(md), rowidx) -> [(attrname, blob)]
        self.default_impl = set()  # (id(md), interfaceimpl rowidx)
        for md in self.mds:
            for ca in (md.CustomAttribute.rows if md.CustomAttribute else []):
                p, name = ca.Parent, self.ca_type_name(ca)
                if p.table.name == "TypeDef":
                    self.ca_typedef.setdefault((id(md), p.row_index), []).append((name, ca.Value.value))
                elif p.table.name == "InterfaceImpl" and name == "Windows.Foundation.Metadata.DefaultAttribute":
                    self.default_impl.add((id(md), p.row_index))
        self.impls = {}  # (id(md), typedef rowidx) -> [(type, is_default)]
        for md in self.mds:
            for i, ii in enumerate(md.InterfaceImpl.rows if md.InterfaceImpl else []):
                t = self.coded_type(md, ii.Interface)
                key = (id(md), ii.Class.row_index)
                self.impls.setdefault(key, []).append((t, (id(md), i + 1) in self.default_impl))

    @staticmethod
    def ca_type_name(ca):
        t = ca.Type
        if t.table.name == "MemberRef":
            return rowname(t.row.Class.row)
        return None

    # ---- type trees: ('prim', code) ('string',) ('object',) ('void',) ('named', name)
    #      ('ginst', name, args) ('var', n) ('byref', t) ('array', t)

    def coded_type(self, md, ci):
        tname = ci.table.name
        if tname in ("TypeDef", "TypeRef"):
            return ("named", rowname(ci.row))
        return self.parse_type(md, Reader(ci.row.Signature.value))

    def typedeforref(self, md, rd):
        coded = rd.cu()
        tag, idx = coded & 3, coded >> 2
        if tag == 0:
            return ("named", rowname(md.TypeDef.rows[idx - 1]))
        if tag == 1:
            return ("named", rowname(md.TypeRef.rows[idx - 1]))
        return self.parse_type(md, Reader(md.TypeSpec.rows[idx - 1].Signature.value))

    def parse_type(self, md, rd):
        t = rd.u8()
        while t in (0x1f, 0x20):
            rd.cu()
            t = rd.u8()
        if t == 0x01:
            return ("void",)
        if t in PRIM:
            return ("prim", t)
        if t == 0x0e:
            return ("string",)
        if t == 0x1c:
            return ("object",)
        if t in (0x11, 0x12):
            return self.typedeforref(md, rd)
        if t == 0x15:
            rd.u8()
            base = self.typedeforref(md, rd)[1]
            n = rd.cu()
            return ("ginst", base, tuple(self.parse_type(md, rd) for _ in range(n)))
        if t in (0x13, 0x1e):
            return ("var", rd.cu())
        if t == 0x10:
            return ("byref", self.parse_type(md, rd))
        if t == 0x1d:
            return ("array", self.parse_type(md, rd))
        raise ValueError(f"unknown elem type {t:#x}")

    def subst(self, t, targs):
        if t[0] == "var":
            return targs[t[1]]
        if t[0] == "ginst":
            return ("ginst", t[1], tuple(self.subst(a, targs) for a in t[2]))
        if t[0] in ("byref", "array"):
            return (t[0], self.subst(t[1], targs))
        return t

    def kind(self, name):
        if name == "System.Guid":
            return "struct"
        e = self.types.get(name)
        if not e:
            return None
        md, row, _ = e
        if row.Flags.tdInterface:
            return "iface"
        ext = self.coded_type(md, row.Extends)[1] if row.Extends and row.Extends.row else None
        return {"System.Enum": "enum", "System.ValueType": "struct",
                "System.MulticastDelegate": "delegate"}.get(ext, "class")

    def fields(self, name):
        md, row, _ = self.types[name]
        out = []
        for f in row.FieldList:
            fr = f.row
            if fr.Flags.fdStatic:
                continue
            rd = Reader(fr.Signature.value)
            assert rd.u8() == 0x06
            out.append(self.parse_type(md, rd))
        return out

    def size(self, t):
        """(size, align) of a value; arrays are the (length, pointer) parameter pair"""
        k = t[0]
        if k == "prim":
            s = PRIM[t[1]][2]
            return s, s
        if k in ("string", "object", "ginst", "byref", "var"):
            return 4, 4
        if k == "array":
            return 8, 4
        if k == "named":
            kind = self.kind(t[1])
            if t[1] == "System.Guid":
                return 16, 4
            if kind == "enum":
                return 4, 4
            if kind == "struct":
                size, align = 0, 1
                for ft in self.fields(t[1]):
                    s, a = self.size(ft)
                    size = (size + a - 1) // a * a + s
                    align = max(align, a)
                return (size + align - 1) // align * align, align
            return 4, 4
        raise ValueError(t)

    def guid(self, name):
        md, row, idx = self.types[name]
        for an, v in self.ca_typedef.get((id(md), idx), []):
            if an == "Windows.Foundation.Metadata.GuidAttribute":
                a, b, c = struct.unpack_from("<IHH", v, 2)
                return uuid.UUID(fields=(a, b, c, v[10], v[11], int.from_bytes(v[12:18], "big")))
        return None

    def default_iface(self, name):
        md, row, idx = self.types[name]
        impls = self.impls.get((id(md), idx), [])
        for t, d in impls:
            if d:
                return t
        return impls[0][0] if impls else None

    def sig(self, t):
        k = t[0]
        if k == "prim":
            return PRIM[t[1]][1]
        if k == "string":
            return "string"
        if k == "object":
            return "cinterface(IInspectable)"
        if k == "ginst":
            return f"pinterface({{{self.guid(t[1])}}};" + ";".join(self.sig(a) for a in t[2]) + ")"
        if k == "named":
            n = t[1]
            if n == "System.Guid":
                return "g16"
            kind = self.kind(n)
            if kind == "iface":
                return f"{{{self.guid(n)}}}"
            if kind == "delegate":
                return f"delegate({{{self.guid(n)}}})"
            if kind == "class":
                return f"rc({n};{self.sig(self.default_iface(n))})"
            if kind == "enum":
                under = self.fields(n)[0]
                return f"enum({n};{PRIM[under[1]][1]})"
            if kind == "struct":
                return f"struct({n};" + ";".join(self.sig(f) for f in self.fields(n)) + ")"
        raise ValueError(t)

    def iid(self, t):
        if t[0] == "named":
            return self.guid(t[1])
        h = bytearray(hashlib.sha1(PINTERFACE_NS + self.sig(t).encode()).digest()[:16])
        h[6] = (h[6] & 0x0f) | 0x50
        h[8] = (h[8] & 0x3f) | 0x80
        return uuid.UUID(bytes=bytes(h))

    def tname(self, t):
        k = t[0]
        if k == "prim":
            return PRIM[t[1]][0]
        if k == "string":
            return "String"
        if k == "object":
            return "Object"
        if k == "named":
            return t[1]
        if k == "ginst":
            return f"{t[1]}<{', '.join(self.tname(a) for a in t[2])}>"
        return str(t)

    def method(self, md, mrow, targs):
        """(argbytes, rk, rsize, rtype, pk, nested ginsts)"""
        rd = Reader(mrow.Signature.value)
        cc = rd.u8()
        if cc & 0x10:
            rd.cu()
        n = rd.cu()
        ret = self.subst(self.parse_type(md, rd), targs)
        params = [self.subst(self.parse_type(md, rd), targs) for _ in range(n)]
        nested = [p for p in params + [ret] if p[0] == "ginst"]
        nested += [p[1] for p in params if p[0] == "byref" and p[1][0] == "ginst"]
        argbytes = 4
        for p in params:
            s, _ = self.size(p)
            argbytes += (s + 3) // 4 * 4
        pk = PK_VALUE
        if params:
            p = params[0]
            if p[0] == "string":
                pk = PK_HSTRING
            elif p[0] in ("object", "ginst") or (p[0] == "named" and self.kind(p[1]) in ("class", "iface", "delegate")):
                pk = PK_OBJECT
        k = ret[0]
        rtype = None
        if k == "void":
            rk, rsize = RK_VOID, 0
        elif k == "string":
            rk, rsize = RK_HSTRING, 4
        elif k == "object":
            rk, rsize = RK_OBJECT, 4
        elif k == "array":
            rk, rsize = RK_ARRAY, 8
        elif k == "ginst":
            gk = self.kind(ret[1])
            rk, rsize, rtype = (RK_IFACE, 4, self.tname(ret)) if gk == "iface" else (RK_OBJECT, 4, None)
        elif k == "named" and self.kind(ret[1]) == "iface":
            rk, rsize, rtype = RK_IFACE, 4, ret[1]
        elif k == "named" and self.kind(ret[1]) == "class":
            rk, rsize, rtype = RK_CLASS, 4, ret[1]
        elif k == "named" and self.kind(ret[1]) == "delegate":
            rk, rsize = RK_OBJECT, 4
        else:
            rk, rsize = RK_VALUE, self.size(ret)[0]
        if rk != RK_VOID:
            argbytes += 8 if rk == RK_ARRAY else 4
        return argbytes, rk, rsize, rtype, pk, nested


def attr_type_arg(blob):
    """first fixed arg if it is a SerString type name"""
    rd = Reader(blob)
    rd.p = 2
    if rd.p >= len(blob) or blob[rd.p] == 0xff:
        return None
    try:
        n = rd.cu()
        s = blob[rd.p:rd.p + n].decode()
    except Exception:
        return None
    if "." in s and s.replace(".", "").replace("_", "").replace("`", "").isalnum():
        return s
    return None


def has_var(t):
    if t[0] == "var":
        return True
    if t[0] == "ginst":
        return any(has_var(a) for a in t[2])
    if t[0] in ("byref", "array"):
        return has_var(t[1])
    return False


def main():
    out = sys.argv[1]
    m = Meta(sys.argv[2:])
    ifaces = {}   # name -> dict(guid, meths, req, delegate)
    classes = {}
    pending = []

    def add_iface(t):
        """registers interface/delegate type t (named or closed ginst); returns its name"""
        name = m.tname(t)
        if name in ifaces or has_var(t):
            return name
        base = t[1]
        if base not in m.types:
            return name
        kind = m.kind(base)
        if kind not in ("iface", "delegate"):
            return name
        try:
            g = m.iid(t)
        except Exception as e:
            print(f"warn: iid of {name}: {e}", file=sys.stderr)
            return name
        if g is None:
            return name
        ifaces[name] = None  # placeholder against recursion
        pending.append((name, t, g, kind))
        return name

    def build(name, t, g, kind):
        md, row, idx = m.types[t[1]]
        targs = t[2] if t[0] == "ginst" else ()
        meths = []
        for mr in row.MethodList:
            mrow = mr.row
            if kind == "delegate" and mrow.Name != "Invoke":
                continue
            try:
                ab, rk, rs, rt, pk, nested = m.method(md, mrow, targs)
            except Exception as e:
                print(f"warn: {name}.{mrow.Name}: {e}", file=sys.stderr)
                ab, rk, rs, rt, pk, nested = 4, RK_VOID, 0, None, 0, []
            for nt in nested:
                add_iface(nt)
            meths.append((mrow.Name, ab, rk, rs, rt, pk))
        req = []
        for it, _ in m.impls.get((id(md), idx), []):
            req.append(add_iface(m.subst(it, targs)))
        ifaces[name] = dict(guid=g, meths=meths, req=req, delegate=kind == "delegate")

    for full, (md, row, idx) in m.types.items():
        if "`" in full:
            continue
        k = m.kind(full)
        if k in ("iface", "delegate"):
            if m.guid(full):
                add_iface(("named", full))
        elif k == "class":
            ext = m.coded_type(md, row.Extends)[1] if row.Extends and row.Extends.row else None
            if ext == "System.Object":
                ext = None
            statics, factories, composable, activatable = [], [], [], False
            for an, blob in m.ca_typedef.get((id(md), idx), []):
                a = attr_type_arg(blob)
                if an == "Windows.Foundation.Metadata.StaticAttribute" and a:
                    statics.append(a)
                elif an == "Windows.Foundation.Metadata.ActivatableAttribute":
                    if a:
                        factories.append(a)
                    else:
                        activatable = True
                elif an == "Windows.Foundation.Metadata.ComposableAttribute" and a:
                    composable.append(a)
            impls = [add_iface(t) for t, _ in m.impls.get((id(md), idx), [])]
            d = m.default_iface(full)
            classes[full] = dict(base=ext, ifaces=impls, statics=statics, factories=factories,
                                 composable=composable, activatable=activatable,
                                 default=m.tname(d) if d else None)
        while pending:
            build(*pending.pop())
    while pending:
        build(*pending.pop())

    ifaces = {k: v for k, v in ifaces.items() if v}
    iface_names = sorted(ifaces)
    iidx = {n: i for i, n in enumerate(iface_names)}
    class_names = sorted(classes)
    cidx = {n: i for i, n in enumerate(class_names)}
    lists = []

    def lst(names):
        start = len(lists)
        vals = [iidx[n] for n in names if n in iidx]
        lists.extend(vals)
        return start, len(vals)

    with open(out, "w") as f:
        f.write("/* generated by gen.py, do not edit */\n#include \"shim.h\"\n\n")
        f.write("const struct meth_info meta_meths[] = {\n")
        mi = 0
        rows = []
        for n in iface_names:
            d = ifaces[n]
            first = mi
            for (mn, ab, rk, rs, rt, pk) in d["meths"]:
                ri = -1
                if rk == RK_CLASS and rt in cidx:
                    ri = cidx[rt]
                elif rk == RK_IFACE and rt in iidx:
                    ri = iidx[rt]
                elif rk in (RK_CLASS, RK_IFACE):
                    rk = RK_OBJECT
                f.write(f"  {{\"{mn}\", {ab}, {rk}, {rs}, {ri}, {iidx[n]}, {pk}}},\n")
                mi += 1
            rows.append((n, d, first, len(d["meths"])))
        f.write("};\n\n")
        f.write("const struct iface_info meta_ifaces[] = {\n")
        for (n, d, first, cnt) in rows:
            a, b, c, d1, d2, d3 = d["guid"].fields
            dd = bytes([d1, d2]) + d3.to_bytes(6, "big")
            rs, rc = lst(d["req"])
            g = f"{{0x{a:08x}, 0x{b:04x}, 0x{c:04x}, {{{', '.join(f'0x{x:02x}' for x in dd)}}}}}"
            f.write(f"  {{\"{n}\", {g}, {first}, {cnt}, {rs}, {rc}, {int(d['delegate'])}}},\n")
        f.write("};\n\n")
        f.write("const struct class_info meta_classes[] = {\n")
        for n in class_names:
            d = classes[n]
            base = cidx.get(d["base"], -1)
            a = lst(d["ifaces"])
            st = lst(d["statics"])
            fa = lst(d["factories"])
            co = lst(d["composable"])
            dflt = iidx.get(d["default"], -1)
            f.write(f"  {{\"{n}\", {base}, {a[0]}, {a[1]}, {st[0]}, {st[1]}, {fa[0]}, {fa[1]}, "
                    f"{co[0]}, {co[1]}, {int(d['activatable'])}, {dflt}}},\n")
        f.write("};\n\n")
        f.write("const unsigned short meta_lists[] = {\n")
        for i in range(0, len(lists), 16):
            f.write("  " + ", ".join(str(x) for x in lists[i:i + 16]) + ",\n")
        f.write("};\n\n")
        f.write(f"const int meta_n_ifaces = {len(iface_names)};\n")
        f.write(f"const int meta_n_classes = {len(class_names)};\n")
    print(f"{len(iface_names)} interfaces, {mi} methods, {len(class_names)} classes", file=sys.stderr)


if __name__ == "__main__":
    main()
