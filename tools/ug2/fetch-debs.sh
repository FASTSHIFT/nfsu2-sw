#!/usr/bin/env bash
# 在宿主机下载 OpenUG2 交叉编译需要的 arm64 deb（绕开容器内很慢的 apt）
set -u
mkdir -p /tmp/ug2debs && cd /tmp/ug2debs  # 下载到临时目录，再复制到 ug2-r36s/debs/
M=http://mirrors.aliyun.com/ubuntu-ports
for s in focal focal-updates focal-security; do
  curl -s --retry 3 -m 40 -o P_$s.gz $M/dists/$s/main/binary-arm64/Packages.gz
  echo "index $s: $(stat -c %s P_$s.gz 2>/dev/null)"
done
python3 - <<'E'
import gzip,re
want={'zlib1g','zlib1g-dev','libpng16-16','libpng-dev','libssl1.1','libssl-dev','libgles2','libgles-dev','libglvnd0','libglvnd-dev','libgl-dev'}
best={}
for f in ['P_focal.gz','P_focal-updates.gz','P_focal-security.gz']:
    for blk in gzip.open(f,'rt').read().split('\n\n'):
        m=re.search(r'^Package: (.+)$',blk,re.M)
        if m and m.group(1) in want:
            best[m.group(1)]=re.search(r'^Filename: (.+)$',blk,re.M).group(1)
for k in sorted(want): print(k,best.get(k))
open('urls.txt','w').write('\n'.join('http://mirrors.aliyun.com/ubuntu-ports/'+v for v in best.values())+'\n')
E
while read -r u; do curl -s --retry 3 -m 120 -O "$u" && echo "got $(basename $u)"; done < urls.txt
ls -la *.deb
echo FETCH_DONE
