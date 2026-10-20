#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 tiper
"""Build pkg/ into an .ipk (bare ar member names, as appinstalld expects)."""
import io, json, tarfile, gzip, time
from pathlib import Path

root = Path(__file__).resolve().parent
pkg = root / "pkg"
app = json.loads((pkg / "appinfo.json").read_text())
aid, ver = app["id"], app["version"]

def tgz(entries):
    buf = io.BytesIO()
    with gzip.GzipFile(fileobj=buf, mode="wb", mtime=0) as gz:
        with tarfile.open(fileobj=gz, mode="w", format=tarfile.GNU_FORMAT) as t:
            for name, data, mode in entries:
                if data is None:
                    ti = tarfile.TarInfo(name); ti.type = tarfile.DIRTYPE; ti.mode = 0o755
                    t.addfile(ti); continue
                ti = tarfile.TarInfo(name); ti.size = len(data); ti.mode = mode
                t.addfile(ti, io.BytesIO(data))
    return buf.getvalue()

base = f"./usr/palm/applications/{aid}"
entries = [("./usr", None, 0), ("./usr/palm", None, 0), ("./usr/palm/applications", None, 0), (base, None, 0)]
size = 0
for p in sorted(pkg.rglob("*")):
    rel = p.relative_to(pkg)
    if p.is_dir():
        entries.append((f"{base}/{rel}", None, 0)); continue
    d = p.read_bytes(); size += len(d)
    entries.append((f"{base}/{rel}", d, 0o755 if p.name == app["main"] else 0o644))
pi = json.dumps({"app": aid, "id": aid, "loc_name": app["title"], "package_format_version": 2,
                 "vendor": app["vendor"], "version": ver}, indent=2).encode()
entries += [("./usr/palm/packages", None, 0), (f"./usr/palm/packages/{aid}", None, 0),
            (f"./usr/palm/packages/{aid}/packageinfo.json", pi, 0o644)]
data = tgz(entries)
control = (f"Package: {aid}\nVersion: {ver}\nSection: misc\nPriority: optional\nArchitecture: arm\n"
           f"Installed-Size: {size // 1024 + 1}\nMaintainer: tiper\nDescription: {app['title']}\n"
           "webOS-Package-Format-Version: 2\nwebOS-Packager-Version: x.y.x\n").encode()
ctl = tgz([("./control", control, 0o644)])

def member(name, d):
    h = f"{name:<16}{0:<12}{0:<6}{0:<6}{'100644':<8}{len(d):<10}`\n".encode()
    return h + d + (b"\n" if len(d) % 2 else b"")
out = root / f"{aid}_{ver}_arm.ipk"
out.write_bytes(b"!<arch>\n" + member("debian-binary", b"2.0\n") + member("control.tar.gz", ctl) + member("data.tar.gz", data))
print(out.name, out.stat().st_size)
