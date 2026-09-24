"""ANTS-5313 — draw a seeded sample of doc_symbols locator entries for hand
classification. Reads out.jsonl from the harness in the current directory.
Usage: doc_symbols_locator_sample.py [n] [seed]   (seed default 20260924)
Prints bucket totals, a mechanical check over the whole population (does the
cited line contain the identifier at all), and n entries with the document
line beside the cited code line."""
import json, random, re, sys
import os
R=os.environ.get('ANTS_ROOT', os.getcwd())+'/'
rows=[json.loads(l) for l in open('out.jsonl')]
pop=[]; agg={'located':0,'ambiguous':0,'unresolved':0,'not_checked':0,'declared_only':0}; trunc=0
for r in rows:
    trunc+=r['truncated']
    for k,src in (('located','locators'),('ambiguous','ambiguous'),('unresolved','unresolved'),('not_checked','not_checked'),('declared_only','declared_only')):
        agg[k]+=len(r.get(src,[]))
    for s,loc in r['locators'].items():
        pop.append((r['doc'],s,loc,r['doc_line'].get(s)))
print('docs',len(rows),'truncated_docs',trunc,agg)
lines={}
def L(path):
    if path not in lines: lines[path]=open(R+path,errors='replace').read().split('\n')
    return lines[path]
# mechanical: cited line contains the final identifier
miss=[]
for d,s,loc,dl in pop:
    f,n=loc.rsplit(':',1); leaf=s.replace('()','').split('::')[-1]
    if not re.search(r'\b'+re.escape(leaf)+r'\b', L(f)[int(n)-1]): miss.append((d,s,loc))
print('population',len(pop),'mechanical_miss',len(miss))
for m in miss[:20]: print('  MISS',m)
if len(sys.argv)>1:
    n=int(sys.argv[1]); random.seed(int(sys.argv[2]) if len(sys.argv)>2 else 20260924)
    for i,(d,s,loc,dl) in enumerate(random.sample(sorted(pop),n)):
        f,ln=loc.rsplit(':',1); ln=int(ln)
        print(f'\n#{i+1} {s}  doc={d}:{dl}  ->  {loc}')
        print('  DOC :', L(d)[dl-1].strip()[:300])
        print('  CODE:', L(f)[ln-1].strip()[:200])
