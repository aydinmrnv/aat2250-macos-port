set -e
cd /opt/cuda-redist
B=https://developer.download.nvidia.com/compute/cuda/redist
python3 -I - <<'PY' > list.txt
import json
j=json.load(open('redistrib_13.0.3.json'))
for k in ['cuda_nvcc','cuda_cudart','cuda_cccl','cuda_crt','libnvvm','libcublas']:
    a=j[k]['linux-x86_64']; print(a['relative_path'], a['sha256'])
PY
mkdir -p /opt/cuda-13.0
while read p s; do
  f=$(basename $p)
  [ -f $f ] || curl -fsSL -o $f $B/$p
  echo "$s  $f" | sha256sum -c -
  tar -xJf $f -C /opt/cuda-13.0 --strip-components=1
  echo "unpacked $f"
done < list.txt
ls /opt/cuda-13.0 /opt/cuda-13.0/bin
du -sh /opt/cuda-13.0
