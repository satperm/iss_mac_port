import re,sys,collections
# streaming parse of xctrace rows with id/ref dedup
s=open('gpu.xml').read()
ids={}
rows=re.findall(r'<row>(.*?)</row>',s,re.S)
iv=collections.defaultdict(list); procs=collections.Counter()
def val(tag,frag):
    m=re.search(r'<%s (?:id="(\d+)"[^>]*>([^<]*)|ref="(\d+)"/>)'%tag,frag)
    if not m: return None
    if m.group(1): ids[(tag,m.group(1))]=m.group(2); return m.group(2)
    return ids.get((tag,m.group(3)))
for r in rows:
    # register all ids in row for later refs
    for t,i,f,v in re.findall(r"<([a-z0-9-]+) id=\"(\d+)\" fmt=\"([^\"]*)\">([^<]*)",r): ids.setdefault((t,i),v if t in ("start-time","duration") else f)
    st=val('start-time',r); du=val('duration',r); ch=val('gpu-channel-name',r)
    pm=re.search(r'<process (?:id="(\d+)" fmt="([^"]*)"|ref="(\d+)")',r)
    p= pm.group(2) if pm and pm.group(2) else (ids.get(('process',pm.group(3))) if pm else None)
    if pm and pm.group(1): ids[('process',pm.group(1))]=pm.group(2)
    if st is None or du is None: continue
    if p and 'tlggpu' not in p: procs[p]+=1; continue
    iv[ch].append((int(st),int(st)+int(du)))
def union(l):
    l=sorted(l); tot=0; cs,ce=None,None
    for a,b in l:
        if ce is None or a>ce:
            if ce is not None: tot+=ce-cs
            cs,ce=a,b
        else: ce=max(ce,b)
    if ce is not None: tot+=ce-cs
    return tot
allv=[x for v in iv.values() for x in v]
span=max(b for a,b in allv)-min(a for a,b in allv)
print('span %.2f s'%(span/1e9))
for ch,v in iv.items(): print(ch,len(v),'busy %.1f%%'%(100*union(v)/span))
print('any channel busy %.1f%%'%(100*union(allv)/span))
print('other procs',procs.most_common(3))
for ch in ('Fragment','Vertex','Compute'):
    d=sorted(b-a for a,b in iv[ch]); n=len(d)
    small=sum(x for x in d if x<50000)
    print(ch,'median %.1f us p90 %.1f us; <50us: %d of %d, %.1f%% of busy'%(d[n//2]/1e3,d[int(n*.9)]/1e3,sum(1 for x in d if x<50000),n,100*small/sum(d)))
