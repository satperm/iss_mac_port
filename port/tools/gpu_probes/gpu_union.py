import re,sys
s=open(sys.argv[1]).read(); frames=float(sys.argv[2])
ids={}
iv=[]
for r in re.findall(r'<row>(.*?)</row>',s,re.S):
    for t,i,v in re.findall(r'<([a-z0-9-]+) id="(\d+)" fmt="[^"]*">([^<]*)',r): ids[(t,i)]=v
    def g(tag):
        m=re.search(r'<%s (?:id="(\d+)"[^>]*>([^<]*)|ref="(\d+)"/>)'%tag,r)
        if not m: return None
        return m.group(2) if m.group(1) else ids.get((tag,m.group(3)))
    pm=re.search(r'<process (?:id="(\d+)" fmt="([^"]*)"|ref="(\d+)")',r)
    if pm and pm.group(1): ids[('pfmt',pm.group(1))]=pm.group(2)
    p=pm.group(2) if pm and pm.group(2) else (ids.get(('pfmt',pm.group(3))) if pm else '')
    if 'tlggpu' not in (p or ''): continue
    st=g('start-time'); du=g('duration')
    if st and du: iv.append((int(st),int(st)+int(du)))
iv.sort(); 
if not iv: sys.exit("no intervals")
t0,t1=iv[0][0],max(e for _,e in iv)
busy=0; gaps=[]; cs,ce=iv[0]
for s_,e in iv[1:]:
    if s_>ce: busy+=ce-cs; gaps.append(s_-ce); cs,ce=s_,e
    else: ce=max(ce,e)
busy+=ce-cs
span=t1-t0
print(f"span {span/1e6:.0f} ms, union busy {busy/span*100:.1f}%, per frame busy {busy/frames/1e6:.2f} ms idle {(span-busy)/frames/1e6:.2f} ms")
import collections
b=collections.Counter()
tot=collections.Counter()
for gp in gaps:
    k='<50us' if gp<5e4 else '<0.2ms' if gp<2e5 else '<0.5ms' if gp<5e5 else '<1ms' if gp<1e6 else '<2ms' if gp<2e6 else '<4ms' if gp<4e6 else '>=4ms'
    b[k]+=1; tot[k]+=gp
for k in ['<50us','<0.2ms','<0.5ms','<1ms','<2ms','<4ms','>=4ms']:
    print(f"  gaps {k:7} n={b[k]/frames:6.1f}/frame  idle {tot[k]/frames/1e6:5.2f} ms/frame")
