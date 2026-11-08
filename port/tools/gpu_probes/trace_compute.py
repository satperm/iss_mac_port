import re,collections,sys
s=open(sys.argv[1]).read()
ids={}
rows=re.findall(r'<row>(.*?)</row>',s,re.S)
tot=collections.Counter(); cnt=collections.Counter(); chan=collections.Counter()
t0=None;t1=0
for r in rows:
    for t,i,f,v in re.findall(r"<([a-z0-9-]+) id=\"(\d+)\" fmt=\"([^\"]*)\">([^<]*)",r): ids.setdefault((t,i),(f,v))
    def g(tag):
        m=re.search(r'<%s (?:id="(\d+)"[^>]*>|ref="(\d+)"/>)'%tag,r)
        return ids.get((tag,m.group(1) or m.group(2))) if m else None
    st=g('start-time'); du=g('duration'); ch=g('gpu-channel-name'); lab=g('formatted-label')
    if not st or not du: continue
    l=lab[0] if lab else ''
    if 'tlggpu' not in l: continue
    st=int(st[1]); du=int(du[1]); t0=st if t0 is None else min(t0,st); t1=max(t1,st+du)
    c=ch[1] if ch else '?'
    chan[c]+=du
    m=re.search(r':(csi?) vs ([0-9a-f]+)',l)
    if m and c=='Compute':
        k=m.group(1)+' '+m.group(2); tot[k]+=du; cnt[k]+=1
    elif c=='Compute':
        k='other: '+re.sub(r'\s+\(tlggpu.*','',l).split(':')[-1][:50]; tot[k]+=du; cnt[k]+=1
span=(t1-t0)/1e9
print('span %.2fs'%span)
for c,v in chan.most_common(): print(' %s busy %.1f%%'%(c,100*v/1e9/span))
allc=sum(tot.values())
for k,v in tot.most_common(25): print('%-28s %7.2f ms/s %5.1f%%  n/s %6.0f  avg %6.1f us'%(k,v/1e6/span,100*v/allc,cnt[k]/span,v/cnt[k]/1e3))
print('labelled shaders', len(tot))
