#!/usr/bin/env bash
set -u
cd /tmp/sdlbuild
M=http://mirrors.aliyun.com/ubuntu-ports
for s in focal focal-updates focal-security; do
  [ -s P_$s.gz ] || curl -s --retry 3 -m 60 -o P_$s.gz $M/dists/$s/main/binary-arm64/Packages.gz
done
python3 - <<'P'
import gzip,re
want={'libdrm-dev','libdrm2','libgbm-dev','libgbm1','libasound2-dev','libasound2','libegl-dev','libgles-dev','libglvnd-dev','libgl-dev','libudev-dev','libudev1'}
best={}
for f in ['P_focal.gz','P_focal-updates.gz','P_focal-security.gz']:
    for blk in gzip.open(f,'rt').read().split('\n\n'):
        m=re.search(r'^Package: (.+)$',blk,re.M)
        if m and m.group(1) in want: best[m.group(1)]=re.search(r'^Filename: (.+)$',blk,re.M).group(1)
for k in sorted(want): print(k,best.get(k))
open('urls.txt','w').write('\n'.join('http://mirrors.aliyun.com/ubuntu-ports/'+v for v in best.values())+'\n')
P
cd debs && while read -r u; do [ -s "$(basename $u)" ] || curl -s --retry 3 -m 120 -O "$u"; done < ../urls.txt
cd .. && curl -s --retry 3 -m 300 -o sdl2.orig.tar.xz "http://mirrors.aliyun.com/ubuntu/pool/universe/libs/libsdl2/libsdl2_2.0.10+dfsg1.orig.tar.xz"   # github is slow from here
ls -la debs sdl2.orig.tar.xz
echo FETCH_DONE
