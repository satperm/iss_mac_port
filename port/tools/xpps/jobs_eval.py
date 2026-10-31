"""Development tools for the job builder, measured against a capture (TLG_PRECOMPILE_CAPTURE).

    python -m xpps.jobs_eval infer <capture> <index.sqlite> <tables.json>
    python -m xpps.jobs_eval rules <capture> <index.sqlite>
    python -m xpps.jobs_eval eval  <capture> <index.sqlite> [--tables <tables.json> [--cv K]] [--cost]
                                   [--max-state N] [--max-material N] [--max-compute N]

infer  writes the INFERRED tables (enums only) from the captured draws and dispatches.
rules  says, part by part, how much of the capture each rule explains (headers, semantics,
       material textures, samplers, constant buffers, meshes, inferred tables).
eval   builds the jobs for the captured pairs and compute shaders and reports the coverage
       (captured signatures found among the generated ones), per kind and per missing part,
       and the cost (generated / needed). With --cv K the inferred tables of each fold come
       from the other folds only (pairs and compute programs never seen by the tables), which
       is what a player with no capture gets for the parts the tables decide. --cost also
       builds the whole game to count the records.
The capture is a development aid: the product never reads one.
"""
from __future__ import annotations

import argparse
import struct
import json
import random
import sys
from collections import Counter, defaultdict

from . import engine_tables as ET
from . import jobfile
from . import jobs as J
from . import signature as S


def _slot_witnesses(hdr: S.Header, flat) -> dict:
    out = {}
    for key, usage, _api, start, n in J.sharp_slots(hdr):
        if start + n <= len(flat):
            out[key] = J.witness_of(usage, flat[start:start + n])
    return out


def pass_ids(db):
    pid = defaultdict(set)
    for vs, ps, p in db.execute("SELECT vs, ps, pass_id FROM passes"):
        pid[(vs & J.U64, ps & J.U64)].add(p)
    return pid


def infer(records, ix: J.Index) -> dict:
    """INFERRED tables from captured records."""
    pid = pass_ids(ix.db)
    engine = defaultdict(list)
    per_key = defaultdict(Counter)
    fetch = Counter()
    fetch_sem = defaultdict(Counter)
    compute = []
    for r in records:
        sig = S.signature(r, ix.headers)
        if sig is None:
            continue
        st = {s["sw"]: s for s in r["stages"]}
        if r["kind"] == 1:
            cs = st[5]
            h = ix.headers[cs["hash"]]
            w = _slot_witnesses(h, cs["flat"])
            for k, v in w.items():
                per_key[k][json.dumps(v)] += 1
            w["_nt"] = [h.words["NUM_THREAD_X"], h.words["NUM_THREAD_Y"], h.words["NUM_THREAD_Z"]]
            compute.append(w)
            continue
        prof = {"state": J.state_of_signature(sig, r["regs"]),
                "colfmt": ix.headers[sig["ps"]].words["COL_FORMAT"] if sig["ps"] else 0}
        for name, sw in (("VS", 3), ("PS", 0)):
            s = st.get(sw)
            prof[name] = _slot_witnesses(ix.headers[s["hash"]], s["flat"]) if s else {}
            for k, v in prof[name].items():
                per_key[k][json.dumps(v)] += 1
        sem = J.vs_input_semantics(ix.headers[sig["vs"]])
        for i, (_sg, _off, words) in enumerate(st[3]["vsharps"]):
            fetch[json.dumps(J.witness_of(0, words))] += 1
            if i < len(sem):
                fetch_sem[f"fetch/{sem[i][0]}/{sem[i][2]}"][json.dumps(J.witness_of(0, words))] += 1

        for p in pid.get((sig["vs"], sig["ps"]), {255}):
            engine[p].append(prof)
    fallback = {k: json.loads(c.most_common(1)[0][0]) for k, c in per_key.items()}
    wild = defaultdict(Counter)
    for k, c in per_key.items():
        u, _a, n = k.split("/")
        wild[f"{u}/*/{n}"].update(c)
    fallback.update({k: json.loads(c.most_common(1)[0][0]) for k, c in wild.items()})
    if fetch:
        fallback["fetch"] = json.loads(fetch.most_common(1)[0][0])
    for k, c in fetch_sem.items():
        fallback[k] = json.loads(c.most_common(1)[0][0])

    return {"source": ET.INFERRED, "engine": dict(engine), "compute": compute, "fallback": fallback}


# --- rules: what explains each part of the capture ----------------------------------------
def rules(records, ix: J.Index, out=sys.stdout):
    H = ix.headers
    g_recs = [r for r in records if r["kind"] == 0]
    c_recs = [r for r in records if r["kind"] == 1]
    res = Counter()
    tot = Counter()

    def mark(name, ok, n=1):
        tot[name] += n
        res[name] += n if ok else 0

    pairs = set()
    for r in g_recs:
        st = {s["sw"]: s for s in r["stages"]}
        vh, ph = H[st[3]["hash"]], H[st[0]["hash"]]
        pairs.add((st[3]["hash"], st[0]["hash"]))
        g = r["regs"].get
        for name, idx in S.VS_HDR_REG.items():
            mark("hdr VS " + name, g(idx, 0) == vh.words[name])
        for name, idx in S.PS_HDR_REG.items():
            want = ph.words[name]
            if name == "DB_SHADER_CONTROL":
                mark("hdr PS DB_SHADER_CONTROL raw", g(idx, 0) == want)
                want = ET.db_shader_control(want)
            mark("hdr PS " + name, g(idx, 0) == want)
        n = S.bits(g(S.SPI_PS_IN_CONTROL, 0), 0, 6)
        mark("SPI_PS_INPUT_CNTL from semantics",
             tuple(g(S.SPI_PS_INPUT_CNTL_0 + i, 0) & 0x7FF for i in range(n)) ==
             tuple(J.ps_input_cntl(vh, ph)))
    for r in c_recs:
        cs = r["stages"][0]
        h = H[cs["hash"]]
        for name, idx in S.CS_HDR_REG.items():
            mark("hdr CS " + name, r["regs"].get(idx, 0) & (0xFFFF if "THREAD" in name else ~0) ==
                 h.words[name] & (0xFFFF if "THREAD" in name else ~0))

    # sharps: material textures, effect samplers, constant buffers, fetch V# (index rules)
    ctx = J.load_graphics_context(ix, pairs)
    groups, effects, mats, meshes, tex, _vse = ctx
    by_pair = defaultdict(list)
    mapped_by_rec = {}
    for (vs, ps, pid), rows in groups.items():
        by_pair[(vs, ps)].extend(rows)
    for r in g_recs:
        st = {s["sw"]: s for s in r["stages"]}
        sig = S.signature(r, H)
        rows = by_pair[(sig["vs"], sig["ps"])]
        cand_t, cand_t_raw, cand_s, mapped, eng_desc = defaultdict(set), defaultdict(set), defaultdict(set), set(), {}
        fetch_cands = set()
        loads = J.fetch_loads(st[3]["fetch"]) if st[3]["fetch"] else []
        for (f, o, fo, fs, tp, sp, eff, *_x) in rows:
            if eff not in effects:
                continue
            e = effects[eff]
            mat_map, desc = J.material_bindings(e, tp)
            mapped.update(mat_map)
            for k, d in desc.items():
                if k not in mat_map:
                    eng_desc[k] = d
            for k, w in J.effect_samplers(e, sp).items():
                cand_s[k].add(S.proj_sampler(w))
            for m in mats.get(eff, []):
                for k, w in J.material_textures(mat_map, desc, m, tex).items():
                    cand_t[k].add(S.proj_image(w, False))
                _f, _o, _n, tl = m
                for k, i in mat_map.items():
                    if (i + 1) * ET.MAT_TEX_ENTRY <= len(tl):

                        (hh,) = struct.unpack_from("<q", tl, i * ET.MAT_TEX_ENTRY)
                        if hh in tex:
                            cand_t_raw[k].add(S.proj_image(list(tex[hh][0]), False))
                for vsb in meshes.get((m[0], m[1]), []):
                    n = len(vsb) // 16
                    attrs = []
                    for _sg, off in loads:
                        if off // 4 >= n:
                            attrs = None
                            break

                        attrs.append(tuple(struct.unpack_from("<4I", vsb, 16 * (off // 4))))
                    if attrs is not None:
                        fetch_cands.add(S.fetch_projection({"fetch": st[3]["fetch"], "vsharps": [
                            (sg, off, a) for (sg, off), a in zip(loads, attrs)]}))
        mapped_by_rec[id(r)] = mapped
        for stage_name, sw, part in (("VS", 3, "vs_res"), ("PS", 0, "ps_res")):
            for usage, api, p in sig[part]:
                key = (stage_name, api)
                if usage in (0, 4) and p[0] == "I":
                    if key in mapped:
                        mark(f"{stage_name} T# material (rule + sRGB swap)", p in cand_t[key])
                        tot[f"{stage_name} T# material without the sRGB swap"] += 1
                        res[f"{stage_name} T# material without the sRGB swap"] += p in cand_t_raw[key]
                    else:
                        d = eng_desc.get(key)
                        mark(f"{stage_name} T# engine: type from descriptor +8",
                             d is not None and p[1] == 1 and ET.DESC_TYPE_TO_IMAGE.get(d[0]) == p[2])
                elif usage == S.USAGE_SAMPLER:
                    if key in cand_s:
                        mark(f"{stage_name} S# effect sampler", p in cand_s[key])
                    else:
                        mark(f"{stage_name} S# not an effect sampler (0,0)", p == ("S", 0, 0))
                elif usage == S.USAGE_CB:
                    mark(f"{stage_name} CB canonical", p == S.proj_buffer(J.CB_WORDS, True))
        if st[3]["fetch"]:
            mark("fetch V# from a mesh of the effect", sig["fetch"] in fetch_cands)
            if not fetch_cands:
                tot["fetch: no mesh for the effect"] += 1
                res["fetch: no mesh for the effect"] += 1
            mark("fetch loads parsed from the fetch code", sorted(J.fetch_loads(st[3]["fetch"])) ==
                 sorted((sg, off) for sg, off, _w in st[3]["vsharps"]))

    # inferred parts, leave-one-pair-out (only other pairs feed the tables)
    pid = pass_ids(ix.db)
    state_by_pid = defaultdict(lambda: defaultdict(set))
    res_by_key = defaultdict(lambda: defaultdict(set))
    for r in g_recs:
        sig = S.signature(r, H)
        pair = (sig["vs"], sig["ps"])
        mask = S.mrt_written(H[sig["ps"]])
        stt = J.state_of_signature(sig, r["regs"])
        for p in pid.get(pair, {255}):
            state_by_pid[p][J.project_state(stt, 0xFF)].add(pair)
        for stage_name, part in (("VS", "vs_res"), ("PS", "ps_res")):
            for usage, api, p in sig[part]:
                if usage in (0, 4) or (usage == "implicit"):
                    res_by_key[(stage_name, usage, api)][p].add(pair)
    for r in g_recs:
        sig = S.signature(r, H)
        pair = (sig["vs"], sig["ps"])
        mask = S.mrt_written(H[sig["ps"]])
        mine = J.project_state(J.state_of_signature(sig, r["regs"]), mask)
        ok = False
        for p in pid.get(pair, {255}):
            for full, prs in state_by_pid[p].items():
                if prs - {pair}:
                    cbs = full[0] if full[0] == ("off",) else tuple(x for x in full[0] if mask >> x[0] & 1)
                    if (cbs,) + full[1:] == mine:
                        ok = True
        mark("state (RT/depth/raster/clip) seen for the pass id in other pairs", ok)
        for stage_name, part in (("VS", "vs_res"), ("PS", "ps_res")):
            for usage, api, p in sig[part]:
                if usage in (0, 4) and p[0] == "B":
                    mark(f"{stage_name} V# engine buffer seen at the api slot in other pairs",
                         bool(res_by_key[(stage_name, usage, api)].get(p, set()) - {pair}))
                if usage in (0, 4) and p[0] == "I" and (stage_name, api) not in mapped_by_rec[id(r)]:
                    mark(f"{stage_name} T# engine texture seen at the api slot in other pairs",
                         bool(res_by_key[(stage_name, usage, api)].get(p, set()) - {pair}))
    cres = defaultdict(lambda: defaultdict(set))
    for r in c_recs:
        sig = S.signature(r, H)
        for usage, api, p in sig["cs_res"]:
            cres[(usage, api)][p].add(sig["cs"])
    for r in c_recs:
        sig = S.signature(r, H)
        for usage, api, p in sig["cs_res"]:
            name = "CS CB canonical" if usage == S.USAGE_CB else "CS sharp seen at the slot in other programs"
            ok = p == S.proj_buffer(J.CB_WORDS, True) if usage == S.USAGE_CB else \
                bool(cres[(usage, api)].get(p, set()) - {sig["cs"]})
            mark(name, ok)
    w = max(len(k) for k in tot)
    for k in tot:
        print(f"{k:<{w}}  {res[k]:6d} / {tot[k]:6d}  {100.0 * res[k] / tot[k]:6.1f}%", file=out)
    return res, tot


# --- eval ---------------------------------------------------------------------------------
def _blame(target: dict, cands, parts) -> list:
    best, best_n = None, None
    for c in cands:
        d = [p for p in parts if c.get(p) != target.get(p)]
        if best_n is None or len(d) < best_n:
            best, best_n = d, len(d)
    return best or []


def coverage(cap_sigs, gen_by_prog, parts, prog_of) -> dict:
    found, miss, causes = 0, 0, Counter()
    for sig in cap_sigs:
        cands = gen_by_prog.get(prog_of(sig), [])
        if any(S.freeze(c) == S.freeze(sig) for c in cands):
            found += 1
            continue
        miss += 1
        if not cands:
            causes["no record for the program"] += 1
            continue
        for p in _blame(sig, cands, parts):
            causes[p] += 1
    return {"needed": len(cap_sigs), "found": found, "missing": miss, "causes": dict(causes)}


def evaluate(records, index_path, tables: dict, *, cv=0, seed=1, caps=None, out=sys.stdout):
    caps = caps or {}
    ix = J.Index(index_path)
    H = ix.headers
    uniq = {}
    for r in records:
        s = S.signature(r, H)
        if s is not None:
            uniq.setdefault(S.freeze(s), (r, s))
    g = [(r, s) for r, s in uniq.values() if r["kind"] == 0]
    c = [(r, s) for r, s in uniq.values() if r["kind"] == 1]
    gprog = lambda s: (s["vs"], s["ps"])
    cprog = lambda s: s["cs"]
    folds = [None]
    if cv:
        progs = sorted({gprog(s) for _r, s in g}) + sorted({cprog(s) for _r, s in c})
        rnd = random.Random(seed)
        rnd.shuffle(progs)
        fold_of = {p: i % cv for i, p in enumerate(progs)}
        folds = list(range(cv))
    totals = {"graphics": Counter(), "compute": Counter()}
    causes = {"graphics": Counter(), "compute": Counter()}
    generated = Counter()
    for fold in folds:
        if fold is None:
            t = ET.Tables(tables) if tables is not None else None
            gsel, csel = g, c
        else:
            train = [r for r, s in uniq.values()
                     if fold_of[gprog(s) if r["kind"] == 0 else cprog(s)] != fold]
            t = ET.Tables(infer(train, ix))
            gsel = [(r, s) for r, s in g if fold_of[gprog(s)] == fold]
            csel = [(r, s) for r, s in c if fold_of[cprog(s)] == fold]
        pairs = {gprog(s) for _r, s in gsel}
        css = {cprog(s) for _r, s in csel}
        recs, b = J.build(index_path, t, pairs=pairs, cs=css, **caps)
        by = defaultdict(list)
        for rec in recs:
            s = S.signature(rec, H)
            by[gprog(s) if rec["kind"] == 0 else cprog(s)].append(s)
            generated["graphics" if rec["kind"] == 0 else "compute"] += 1
        for kind, sel, parts, prog in (("graphics", gsel, S.PARTS_GRAPHICS, gprog),
                                       ("compute", csel, S.PARTS_COMPUTE, cprog)):
            cv_res = coverage([s for _r, s in sel], by, parts, prog)
            totals[kind]["needed"] += cv_res["needed"]
            totals[kind]["found"] += cv_res["found"]
            causes[kind].update(cv_res["causes"])
        generated.update({"stat_" + k: v for k, v in b.stats.items()})
    ix.close()
    report = {}
    for kind in ("graphics", "compute"):
        n, f = totals[kind]["needed"], totals[kind]["found"]
        report[kind] = {"needed": n, "found": f, "coverage_pct": round(100.0 * f / n, 1) if n else None,
                        "generated_for_these_programs": generated[kind],
                        "cost": round(generated[kind] / n, 2) if n else None,
                        "missing_by_part": dict(causes[kind].most_common())}
    report["builder"] = {k[5:]: v for k, v in generated.items() if k.startswith("stat_")}
    print(json.dumps(report, indent=1, sort_keys=True), file=out)
    return report


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(prog="python -m xpps.jobs_eval")
    sub = ap.add_subparsers(dest="cmd", required=True)
    a1 = sub.add_parser("infer")
    a1.add_argument("capture"); a1.add_argument("index"); a1.add_argument("out")
    a2 = sub.add_parser("rules")
    a2.add_argument("capture"); a2.add_argument("index")
    a3 = sub.add_parser("eval")
    a3.add_argument("capture"); a3.add_argument("index")
    a3.add_argument("--tables", help="INFERRED tables (v1); without them the product model (eboot only)")
    a3.add_argument("--cv", type=int, default=0)
    a3.add_argument("--cost", action="store_true")
    a3.add_argument("--max-state", type=int, default=12)
    a3.add_argument("--max-material", type=int, default=12)
    a3.add_argument("--max-compute", type=int, default=8)
    a3.add_argument("--compute-model", choices=("re", "profiles"), default="re")
    a3.add_argument("--compute-wide", action="store_true")
    a3.add_argument("--eboot", help="eboot.bin for the product model (default: next to the index root)")
    a = ap.parse_args(argv)
    records = jobfile.read(a.capture)
    if a.cmd == "infer":
        ix = J.Index(a.index)
        t = infer(records, ix)
        with open(a.out, "w") as f:
            json.dump(t, f, sort_keys=True)
        print(f"{a.out}: {sum(len(v) for v in t['engine'].values())} graphics profiles in "
              f"{len(t['engine'])} pass ids, {len(t['compute'])} compute profiles, "
              f"{len(t['fallback'])} fallback keys", file=sys.stderr)
    elif a.cmd == "rules":
        rules(records, J.Index(a.index))
    else:
        caps = {"max_state": a.max_state, "max_material": a.max_material, "max_compute": a.max_compute,
                "compute_model": a.compute_model, "compute_wide": a.compute_wide}
        if a.eboot:
            caps["eboot"] = a.eboot
        if a.cv and not a.tables:
            ap.error("--cv needs --tables (cross-validation of the inferred tables)")
        evaluate(records, a.index, json.load(open(a.tables)) if a.tables else None, cv=a.cv, caps=caps)
        if a.cost:
            recs, b = J.build(a.index, ET.Tables.load(a.tables) if a.tables else None, **caps)
            n = Counter("graphics" if r["kind"] == 0 else "compute" for r in recs)
            print(json.dumps({"whole_game_records": dict(n), "builder": dict(b.stats)}, sort_keys=True))
    return 0


if __name__ == "__main__":
    sys.exit(main())
