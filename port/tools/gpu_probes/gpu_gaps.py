import sys,re,collections,xml.etree.ElementTree as ET
text={}; cols=None; iv=[]
for kind,e in ET.iterparse(sys.argv[1],events=('start','end')):
    if kind=='start':
        if e.tag=='schema': cols=[]
        continue
    if e.get('id'): text[e.get('id')]=(e.text,e.get('fmt'))
    if e.tag=='mnemonic' and cols is not None: cols.append(e.text)
    if e.tag!='row': continue
    v=[text.get(x.get('ref')) if x.get('ref') else (x.text,x.get('fmt')) for x in e]
    d=dict(zip(cols,v)); e.clear()
    if 'tlggpu' not in str(d['process'][1]) or d['event-depth'][0]!='0': continue
    st=int(d['start'][0]) if 'start' in d else int(d['start-time'][0]); du=int(d['duration'][0])
    lab=(d.get('event-label',(None,None))[1] or '').split('  (tlggpu')[0]
    lab=lab.split('Queue 3-0:',1)[-1]; lab=re.sub(r' vs:\w+ ps:\w+','',lab); lab=re.sub(r'Command Buffer \d+','CB',lab)
    iv.append((st,st+du,lab[:90]))
iv.sort()
ce=iv[0][1]; prev=iv[0][2]; big=collections.Counter(); n=0
for s,e,l in iv[1:]:
    if s-ce>=1.5e6:
        n+=1; big[(prev,l)]+=1
        if n<=6: print(f"gap {(s-ce)/1e6:.2f} ms  after [{prev}]  before [{l}]")
    if e>ce: ce=e; prev=l
print("most common big-gap neighbours:")
for k,c in big.most_common(5): print(c,k)
