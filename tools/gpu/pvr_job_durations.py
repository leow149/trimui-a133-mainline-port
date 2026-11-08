import re,sys,statistics as st
rx=re.compile(r"^\s*(.+?)-(\d+)\s+\[\d+\]\s+\S+\s+([\d.]+):\s+(\w+):\s*(.*)$")
jt={};sub={};res={}
vb=[]
for l in open(sys.argv[1]):
    m=rx.match(l)
    if not m: continue
    comm,t,ev,rest=m.group(1).strip().split(':')[0],float(m.group(3)),m.group(4),m.group(5)
    if ev=="pvr_job_create":
        j=re.search(r"job=(\w+)",rest).group(1); jt[j]=(comm,re.search(r"job_type=(\w+)",rest).group(1))
    elif ev=="pvr_job_submit_fw":
        j=re.search(r"job=(\w+)",rest).group(1); s=int(re.search(r"done_seqno=(\d+)",rest).group(1)); sub[j]=(t,s)
    elif ev=="pvr_job_done":
        j=re.search(r"job=(\w+)",rest).group(1)
        if j in sub and j in jt:
            res.setdefault((jt[j][0],jt[j][1]),[]).append((t-sub[j][0])*1e3)
for k,v in sorted(res.items()):
    v2=sorted(v); print(f"{k[0]:10s} {k[1]:9s} n={len(v):3d} median {st.median(v):6.2f} ms  min {v2[0]:6.2f}  max {v2[-1]:6.2f}")
