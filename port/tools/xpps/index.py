"""SQLite index of the .xpps packages (stdlib only), incremental by file fingerprint.

    python -m xpps.index build <folder with .xpps> <out.sqlite> [--jobs N] [--full] [--verify-footers]
    python -m xpps.index stats <index.sqlite>
    python -m xpps.index sha   <index.sqlite>

`build` prints `PROGRESS index <n> <total>` lines on stdout (one per package, reused or scanned)
for the launcher; everything else goes to stderr. A package is scanned again only when its
fingerprint changes: size + mtime + SHA-256 of the first and last 64 KiB. The index is written
to <out>.tmp and renamed at the end, so a crash keeps the previous index.

All u64 values (hashes, class ids) are stored as signed int64 (SQLite integers); 16-byte hashes
and raw structures are BLOBs. Offsets are relative to the package data (KCAP doff). Nothing in
the index leaves the machine: it is built from the player's own files.
"""
from __future__ import annotations

import argparse
import hashlib
import json
import os
import shutil
import sqlite3
import sys
import time
from collections import Counter
from multiprocessing import Pool
from typing import Optional, Sequence

from . import kcap, scan

# 2: same tables, but the link tables split across items are read (knli.read_links): indexes of
# schema 1 are rebuilt instead of reusing their scans
SCHEMA_VERSION = 2
FP_CHUNK = 64 << 10
HASH_BITS = 1 << 64
INT64_LIMIT = 1 << 63

SCHEMA = """
CREATE TABLE schema_version(version INTEGER NOT NULL);
CREATE TABLE meta(key TEXT PRIMARY KEY, value TEXT);
CREATE TABLE files(id INTEGER PRIMARY KEY, path TEXT NOT NULL UNIQUE, size INTEGER, mtime_ns INTEGER,
                   fp TEXT, kcap TEXT, stats TEXT);
CREATE TABLE shader_occ(file INTEGER, hdr_off INTEGER, code_off INTEGER, size INTEGER, stage TEXT,
                        hash INTEGER, footer_off INTEGER, PRIMARY KEY(file, hdr_off));
CREATE TABLE shader_hdr(file INTEGER, hdr_off INTEGER, hdr_size INTEGER, common INTEGER, regs BLOB,
                        slots BLOB, semantics BLOB, PRIMARY KEY(file, hdr_off));
CREATE TABLE passes(file INTEGER, off INTEGER, class_hash INTEGER, cid_class INTEGER, pass_id INTEGER,
                    vs INTEGER, ps INTEGER, vs_hdr INTEGER, ps_hdr INTEGER, fetch_off INTEGER,
                    fetch_size INTEGER, iro_vs BLOB, iro_ps BLOB, list58 BLOB, list68 BLOB,
                    list78 BLOB, smp_pairs BLOB, tex_pairs BLOB, ids_a8 BLOB, PRIMARY KEY(file, off));
CREATE TABLE effects(file INTEGER, off INTEGER, cid_class INTEGER, passes_off INTEGER, n_passes INTEGER,
                     nmat_tex INTEGER, slot_map BLOB, list90 BLOB, lista0 BLOB, listb0 BLOB,
                     smp_desc BLOB, tex_desc BLOB, matmap BLOB, matsmp BLOB, PRIMARY KEY(file, off));
CREATE TABLE matinst(file INTEGER, off INTEGER, kind TEXT, cid_class INTEGER, name_hash INTEGER,
                     effect_file INTEGER, effect_off INTEGER, effect_import BLOB, block_off INTEGER,
                     cb_vsharp BLOB, n_tex INTEGER, tex_hashes BLOB, tex_list BLOB,
                     PRIMARY KEY(file, off));
CREATE TABLE textures(file INTEGER, off INTEGER, cid_class INTEGER, hash INTEGER, tsharp BLOB,
                      fmtword INTEGER, PRIMARY KEY(file, off));
CREATE TABLE meshes(file INTEGER, off INTEGER, matinst_off INTEGER, matinst_import BLOB,
                    n_vsharps INTEGER, vsharps BLOB, PRIMARY KEY(file, off));
CREATE TABLE imports(file INTEGER, local INTEGER, hash BLOB, PRIMARY KEY(file, local));
CREATE TABLE exports(hash BLOB, file INTEGER, off INTEGER);
CREATE TABLE footer_check(file INTEGER PRIMARY KEY, footers INTEGER, start_one INTEGER,
                          start_none INTEGER, start_many INTEGER, footer_cut INTEGER);
"""
SHADERS = """
DROP TABLE IF EXISTS shaders;
CREATE TABLE shaders(hash INTEGER PRIMARY KEY, stage TEXT, size INTEGER, n_occ INTEGER,
                     n_variants INTEGER, file INTEGER, code_off INTEGER, hdr_off INTEGER,
                     hdr_size INTEGER, common INTEGER, regs BLOB, slots BLOB, semantics BLOB,
                     n_hdr_variants INTEGER);
"""
INDEXES = """
CREATE INDEX IF NOT EXISTS occ_hash ON shader_occ(hash);
CREATE INDEX IF NOT EXISTS passes_pair ON passes(vs, ps);
CREATE INDEX IF NOT EXISTS tex_hash ON textures(hash);
CREATE INDEX IF NOT EXISTS exports_hash ON exports(hash);
"""
PER_FILE_TABLES = ("shader_occ", "shader_hdr", "passes", "effects", "matinst", "textures", "meshes",
                   "imports", "exports", "footer_check")
# Content hash: every table in this order, rows sorted by these keys ("path" = the file's path);
# file ids are replaced by paths so that an incremental build and a fresh one hash the same.
CONTENT = (
    ("files", "path"),
    ("shader_occ", "path, hdr_off"),
    ("shader_hdr", "path, hdr_off"),
    ("shaders", "hash"),
    ("passes", "path, off"),
    ("effects", "path, off"),
    ("matinst", "path, off"),
    ("textures", "path, off"),
    ("meshes", "path, off"),
    ("imports", "path, local"),
    ("exports", "hash, path, off"),
)
FILE_ID_COLUMNS = {"file", "effect_file", "id"}


class IndexError_(Exception):
    """User-facing problem (missing folder, unreadable index)."""


def s64(v: Optional[int]) -> Optional[int]:
    if v is None:
        return None
    return v - HASH_BITS if v >= INT64_LIMIT else v


def u64(v: Optional[int]) -> Optional[int]:
    if v is None:
        return None
    return v + HASH_BITS if v < 0 else v


def fingerprint(path: str) -> str:
    """SHA-256 (hex, 32 chars) of the size and the first and last 64 KiB."""
    h = hashlib.sha256()
    with open(path, "rb") as f:
        size = os.fstat(f.fileno()).st_size
        h.update(size.to_bytes(8, "little"))
        h.update(f.read(FP_CHUNK))
        if size > FP_CHUNK:
            f.seek(max(FP_CHUNK, size - FP_CHUNK))
            h.update(f.read(FP_CHUNK))
    return h.hexdigest()[:32]


def progress(n: int, total: int) -> None:
    print(f"PROGRESS index {n} {total}", flush=True)


def log(msg: str) -> None:
    print(msg, file=sys.stderr, flush=True)


# --- worker -------------------------------------------------------------------------------

_ROOT = None
_VERIFY = False


def _init_worker(root: str, verify: bool, nice: bool) -> None:
    global _ROOT, _VERIFY
    _ROOT, _VERIFY = root, verify
    if nice:
        try:
            os.nice(10)
        except (AttributeError, OSError):
            pass


def _scan_one(rel: str):
    path = os.path.join(_ROOT, rel)
    res = scan.scan_pack(path, rel)
    footers = None
    if _VERIFY and not res.stats.get("not_kcap"):
        footers = scan.scan_footers(path)
    return rel, res, footers


# --- writing ------------------------------------------------------------------------------

def _insert_scan(db: sqlite3.Connection, fid: int, r: scan.PackScan, footers) -> None:
    db.executemany("INSERT INTO shader_occ VALUES(?,?,?,?,?,?,?)",
                   [(fid, h, code, size, stage, s64(hs), fo)
                    for h, code, size, stage, hs, fo, *_ in r.shaders])
    db.executemany("INSERT INTO shader_hdr VALUES(?,?,?,?,?,?,?)",
                   [(fid, h, hsz, common, regs, slots, sem)
                    for h, _c, _s, _st, _hs, _fo, hsz, common, regs, slots, sem in r.shaders])
    db.executemany("INSERT INTO passes VALUES(?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?)",
                   [(fid, b, s64(cls), s64(cid), s64(pid), s64(vs), s64(ps), vh, ph, fetch, fsize,
                     iv, ip, *lists)
                    for b, cls, cid, pid, vs, ps, vh, ph, fetch, fsize, iv, ip, *lists in r.passes])
    db.executemany("INSERT INTO effects VALUES(?,?,?,?,?,?,?,?,?,?,?,?,?,?)",
                   [(fid, e, s64(cid), first, n, nmat, smap, *lists)
                    for e, cid, first, n, nmat, smap, *lists in r.effects])
    db.executemany("INSERT INTO matinst VALUES(?,?,?,?,?,?,?,?,?,?,?,?,?)",
                   [(fid, m, kind, s64(cid), s64(name), fid if eff is not None else None, eff, imp,
                     block, cb, n, hashes, raw)
                    for m, kind, cid, name, eff, imp, block, cb, n, hashes, raw in r.matinst])
    db.executemany("INSERT INTO textures VALUES(?,?,?,?,?,?)",
                   [(fid, x, s64(cid), s64(h), t, fmt) for x, cid, h, t, fmt in r.textures])
    db.executemany("INSERT INTO meshes VALUES(?,?,?,?,?,?)",
                   [(fid, m, mi, imp, n, vs) for m, mi, imp, n, vs in r.meshes])
    db.executemany("INSERT INTO imports VALUES(?,?,?)", [(fid, l, h) for l, h in r.imports])
    db.executemany("INSERT INTO exports VALUES(?,?,?)", [(h, fid, o) for h, o in r.exports])
    if footers is not None:
        db.execute("INSERT INTO footer_check VALUES(?,?,?,?,?,?)",
                   (fid, footers["footers"], footers["start_one"], footers["start_none"],
                    footers["start_many"], footers["footer_cut"]))


def _delete_file(db: sqlite3.Connection, fid: int) -> None:
    for t in PER_FILE_TABLES:
        db.execute(f"DELETE FROM {t} WHERE file = ?", (fid,))
    db.execute("DELETE FROM files WHERE id = ?", (fid,))


def _resolve_imports(db: sqlite3.Connection) -> Counter:
    """Fill matinst.effect_file/effect_off for imported effects (first export in path order
    that is an effect)."""
    c = Counter()
    db.execute("UPDATE matinst SET effect_file = NULL, effect_off = NULL WHERE kind = 'import'")
    rows = db.execute(
        "SELECT m.file, m.off, x.file, x.off FROM matinst m "
        "JOIN exports x ON x.hash = m.effect_import "
        "JOIN files f ON f.id = x.file "
        "JOIN effects e ON e.file = x.file AND e.off = x.off "
        "WHERE m.kind = 'import' ORDER BY m.file, m.off, f.path, x.off").fetchall()
    done = {}
    for mf, mo, xf, xo in rows:
        done.setdefault((mf, mo), (xf, xo))
    db.executemany("UPDATE matinst SET effect_file = ?, effect_off = ? WHERE file = ? AND off = ?",
                   [(xf, xo, mf, mo) for (mf, mo), (xf, xo) in done.items()])
    c["import_resolved_to_effect"] = len(done)
    c["import_count_mismatch"] = db.execute(
        "SELECT COUNT(*) FROM matinst m JOIN effects e ON e.file = m.effect_file AND e.off = m.effect_off "
        "WHERE m.kind = 'import' AND m.n_tex != e.nmat_tex").fetchone()[0]
    c["import_total"] = db.execute("SELECT COUNT(*) FROM matinst WHERE kind = 'import'").fetchone()[0]
    c["import_hash_exported"] = db.execute(
        "SELECT COUNT(*) FROM matinst m WHERE kind = 'import' AND EXISTS "
        "(SELECT 1 FROM exports x WHERE x.hash = m.effect_import)").fetchone()[0]
    return c


def _build_shaders(db: sqlite3.Connection) -> None:
    """One row per hash: the first occurrence in (path, header offset) order, plus counts."""
    db.executescript(SHADERS)
    rows = db.execute(
        "SELECT o.hash, o.stage, o.size, o.file, o.code_off, o.hdr_off, h.hdr_size, h.common, "
        "h.regs, h.slots, h.semantics FROM shader_occ o JOIN files f ON f.id = o.file "
        "JOIN shader_hdr h ON h.file = o.file AND h.hdr_off = o.hdr_off "
        "ORDER BY o.hash, f.path, o.hdr_off")
    out = []
    cur = None
    for r in rows:
        if cur is None or r[0] != cur[0][0]:
            if cur is not None:
                out.append(cur)
            cur = [r, 0, set(), set()]
        cur[1] += 1
        cur[2].add(r[2])
        cur[3].add((r[8], r[9], r[10]))
    if cur is not None:
        out.append(cur)
    db.executemany("INSERT INTO shaders VALUES(?,?,?,?,?,?,?,?,?,?,?,?,?,?)",
                   [(r[0], r[1], r[2], n, len(sizes), r[3], r[4], r[5], r[6], r[7], r[8], r[9], r[10],
                     len(hdrs)) for r, n, sizes, hdrs in out])


def content_sha(db: sqlite3.Connection) -> str:
    """SHA-256 of the index content (independent of file ids, build time and SQLite layout)."""
    paths = dict(db.execute("SELECT id, path FROM files"))
    h = hashlib.sha256()
    for table, order in CONTENT:
        names = [r[1] for r in db.execute(f"PRAGMA table_info({table})")]
        h.update(f"#{table}:{','.join(names)}\n".encode())
        join = "" if table == "files" else " LEFT JOIN files ON files.id = t.file"
        path = "t.path" if table == "files" else "files.path"
        sql = (f"SELECT {', '.join('t.' + n for n in names)} FROM {table} t{join} "
               f"ORDER BY {order.replace('path', path)}")
        for row in db.execute(sql):
            for n, v in zip(names, row):
                if n in FILE_ID_COLUMNS and v is not None:
                    v = paths.get(v, v)
                if v is None:
                    h.update(b"N")
                elif isinstance(v, bytes):
                    h.update(b"B%d:" % len(v) + v)
                elif isinstance(v, int):
                    h.update(b"I%d;" % v)
                else:
                    t = str(v).encode()
                    h.update(b"S%d:" % len(t) + t)
            h.update(b"\n")
    return h.hexdigest()


def _open_previous(out: str):
    """Previous index (same schema) as {path: (id, size, mtime_ns, fp, verified)} or None."""
    if not os.path.isfile(out):
        return None
    try:
        db = sqlite3.connect(out)
        try:
            (ver,) = db.execute("SELECT version FROM schema_version").fetchone()
            if ver != SCHEMA_VERSION:
                return None
            rows = db.execute("SELECT f.path, f.id, f.size, f.mtime_ns, f.fp, c.file IS NOT NULL "
                              "FROM files f LEFT JOIN footer_check c ON c.file = f.id").fetchall()
        finally:
            db.close()
    except sqlite3.Error:
        return None
    return {p: (i, s, m, fp, bool(v)) for p, i, s, m, fp, v in rows}


def build(root: str, out: str, jobs: int = 4, full: bool = False, verify_footers: bool = False,
          nice: bool = False) -> dict:
    if not os.path.isdir(root):
        raise IndexError_(f"folder not found: {root}")
    t0 = time.time()
    rels = list(kcap.iter_packages(root))
    if not rels:
        raise IndexError_(f"no .xpps files under {root}")
    total = len(rels)
    progress(0, total)
    prev = None if full else _open_previous(out)
    tmp = out + ".tmp"
    if os.path.exists(tmp):
        os.unlink(tmp)
    os.makedirs(os.path.dirname(os.path.abspath(out)), exist_ok=True)
    if prev is not None:
        shutil.copyfile(out, tmp)
    db = sqlite3.connect(tmp)
    done = 0
    counts = Counter()
    try:
        db.execute("PRAGMA journal_mode=OFF")
        db.execute("PRAGMA synchronous=OFF")
        if prev is None:
            db.executescript(SCHEMA)
            db.execute("INSERT INTO schema_version VALUES(?)", (SCHEMA_VERSION,))
            prev = {}
        current = set(rels)
        for path, (fid, *_rest) in prev.items():
            if path not in current:
                _delete_file(db, fid)
                counts["removed"] += 1
        todo = []
        ids = {}
        next_id = max([v[0] for v in prev.values()], default=0) + 1
        for rel in rels:
            full_path = os.path.join(root, rel)
            st = os.stat(full_path)
            old = prev.get(rel)
            if old is not None:
                fid, size, mtime, fp, verified = old
                if size == st.st_size and mtime == st.st_mtime_ns and (verified or not verify_footers) \
                        and fp == fingerprint(full_path):
                    ids[rel] = fid
                    done += 1
                    counts["reused"] += 1
                    progress(done, total)
                    continue
                _delete_file(db, fid)
            fid = old[0] if old is not None else next_id
            if old is None:
                next_id += 1
            ids[rel] = fid
            todo.append(rel)
        scanned_stats = Counter()
        with Pool(max(1, jobs), initializer=_init_worker, initargs=(root, verify_footers, nice)) as pool:
            for rel, res, footers in pool.imap(_scan_one, todo, chunksize=1):
                full_path = os.path.join(root, rel)
                st = os.stat(full_path)
                fid = ids[rel]
                db.execute("INSERT INTO files VALUES(?,?,?,?,?,?,?)",
                           (fid, rel, st.st_size, st.st_mtime_ns, fingerprint(full_path),
                            json.dumps(res.kcap, sort_keys=True),
                            json.dumps(dict(sorted(res.stats.items())), sort_keys=True)))
                _insert_scan(db, fid, res, footers)
                scanned_stats.update(res.stats)
                done += 1
                counts["scanned"] += 1
                progress(done, total)
        db.executescript(INDEXES)
        resolve = _resolve_imports(db)
        _build_shaders(db)
        sha = content_sha(db)
        meta = {
            "root": os.path.abspath(root),
            "files": total,
            "scanned": counts["scanned"],
            "reused": counts["reused"],
            "removed": counts["removed"],
            "build_seconds": round(time.time() - t0, 1),
            "build_time": time.strftime("%Y-%m-%d %H:%M:%S"),
            "content_sha": sha,
            "schema_version": SCHEMA_VERSION,
        }
        meta.update({"resolve_" + k: v for k, v in resolve.items()})
        db.execute("DELETE FROM meta")
        db.executemany("INSERT INTO meta VALUES(?,?)", [(k, str(v)) for k, v in sorted(meta.items())])
        db.commit()
    finally:
        db.close()
    os.replace(tmp, out)
    log(f"index: {total} packages ({counts['scanned']} scanned, {counts['reused']} reused) in "
        f"{time.time() - t0:.1f}s -> {out}")
    return meta


# --- reading ------------------------------------------------------------------------------

def open_index(path: str) -> sqlite3.Connection:
    if not os.path.isfile(path):
        raise IndexError_(f"index not found: {path}")
    db = sqlite3.connect(path)
    try:
        (ver,) = db.execute("SELECT version FROM schema_version").fetchone()
    except (sqlite3.Error, TypeError) as exc:
        db.close()
        raise IndexError_(f"not an xpps index: {path}") from exc
    if ver != SCHEMA_VERSION:
        db.close()
        raise IndexError_(f"index schema {ver}, this tool reads {SCHEMA_VERSION}: rebuild it")
    return db


def summary(db: sqlite3.Connection) -> dict:
    q = lambda sql: db.execute(sql).fetchone()[0]
    s = {
        "files": q("SELECT COUNT(*) FROM files"),
        "shader_occurrences": q("SELECT COUNT(*) FROM shader_occ"),
        "code_starts": q("SELECT COUNT(*) FROM (SELECT DISTINCT file, code_off FROM shader_occ)"),
        "shaders_unique": q("SELECT COUNT(*) FROM shaders"),
        "shaders_hdr_variants": q("SELECT COUNT(*) FROM shaders WHERE n_hdr_variants > 1"),
        "records": q("SELECT COUNT(*) FROM passes"),
        "records_with_fetch_size": q("SELECT COUNT(*) FROM passes WHERE fetch_size IS NOT NULL"),
        "pairs_vs_ps": q("SELECT COUNT(*) FROM (SELECT DISTINCT vs, ps FROM passes)"),
        "effects": q("SELECT COUNT(*) FROM effects"),
        "matinst": q("SELECT COUNT(*) FROM matinst"),
        "textures": q("SELECT COUNT(*) FROM textures"),
        "texture_hashes": q("SELECT COUNT(DISTINCT hash) FROM textures"),
        "meshes": q("SELECT COUNT(*) FROM meshes"),
        "meshes_with_effect": q(
            "SELECT COUNT(*) FROM meshes s JOIN matinst m ON m.file = s.file AND m.off = s.matinst_off "
            "WHERE m.effect_off IS NOT NULL"),
        "imports": q("SELECT COUNT(*) FROM imports"),
        "exports": q("SELECT COUNT(*) FROM exports"),
    }
    for stage, n in db.execute("SELECT stage, COUNT(*) FROM shaders GROUP BY stage ORDER BY stage"):
        s["shaders_" + stage] = n
    for kind, n in db.execute("SELECT kind, COUNT(*) FROM matinst GROUP BY kind ORDER BY kind"):
        s["matinst_" + kind] = n
    s["matinst_import_resolved"] = q("SELECT COUNT(*) FROM matinst WHERE kind = 'import' AND effect_off IS NOT NULL")
    for kind, n in db.execute(
            "SELECT m.kind, COUNT(*) FROM meshes s JOIN matinst m ON m.file = s.file AND m.off = s.matinst_off "
            "GROUP BY m.kind ORDER BY m.kind"):
        s["meshes_matinst_" + kind] = n
    fc = db.execute("SELECT COUNT(*), SUM(footers), SUM(start_one), SUM(start_none), SUM(start_many) "
                    "FROM footer_check").fetchone()
    if fc[0]:
        s.update(footer_check_files=fc[0], footers=fc[1], footer_start_one=fc[2],
                 footer_start_none=fc[3], footer_start_many=fc[4])
    return s


def cmd_build(a) -> int:
    build(a.root, a.out, a.jobs, a.full, a.verify_footers, a.nice)
    db = open_index(a.out)
    for k, v in summary(db).items():
        log(f"  {k:<28} {v}")
    return 0


def cmd_stats(a) -> int:
    db = open_index(a.db)
    for k, v in summary(db).items():
        print(f"{k:<28} {v}")
    for k, v in db.execute("SELECT key, value FROM meta ORDER BY key"):
        print(f"meta.{k:<23} {v}")
    return 0


def cmd_sha(a) -> int:
    print(content_sha(open_index(a.db)))
    return 0


def make_parser() -> argparse.ArgumentParser:
    p = argparse.ArgumentParser(prog="python -m xpps.index", description="Index of the .xpps packages.")
    sub = p.add_subparsers(dest="cmd", required=True)
    b = sub.add_parser("build", help="scan the packages and write the index")
    b.add_argument("root", help="folder with the .xpps files (searched recursively)")
    b.add_argument("out", help="index file (.sqlite)")
    b.add_argument("--jobs", type=int, default=min(4, os.cpu_count() or 1),
                   help="processes, one package at a time each (default: up to 4)")
    b.add_argument("--full", action="store_true", help="ignore the previous index")
    b.add_argument("--verify-footers", action="store_true",
                   help="also search every OrbShdr by signature (reads the whole packages)")
    b.add_argument("--nice", action="store_true", help="lower the CPU priority of the workers")
    b.set_defaults(run=cmd_build)
    s = sub.add_parser("stats", help="counts of an index")
    s.add_argument("db")
    s.set_defaults(run=cmd_stats)
    h = sub.add_parser("sha", help="content hash of an index (determinism check)")
    h.add_argument("db")
    h.set_defaults(run=cmd_sha)
    return p


def main(argv: Optional[Sequence[str]] = None) -> int:
    a = make_parser().parse_args(argv)
    try:
        return a.run(a)
    except (IndexError_, kcap.KcapError) as exc:
        log(f"error: {exc}")
        return 1


if __name__ == "__main__":
    sys.exit(main())
