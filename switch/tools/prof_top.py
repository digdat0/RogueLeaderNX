# RL-NX: top functions of the main thread in a hardware profile (profile-NNN.txt, R2_PROFILE).
#   py -3 -I switch/tools/prof_top.py <profile-NNN.txt> debug/RL-NX-HHMM.elf
# The elf must be the build named on the profile's "build" line (debug/RL-NX-HHMM.elf). Role 0 = main thread.
import sys, subprocess, collections
prof, elf = sys.argv[1], sys.argv[2]
anchor = None; pcs = collections.Counter()
for ln in open(prof):
    p = ln.split()
    if p and p[0] == 'anchor': anchor = int(p[1], 16)
    if len(p) == 4 and p[0] == '0':
        try: pcs[int(p[2], 16)] += int(p[1])
        except ValueError: pass
total = sum(pcs.values())
addrs = list(pcs)
a2l = 'C:/devkitPro/devkitA64/bin/aarch64-none-elf-addr2line.exe'
out = subprocess.run([a2l, '-f', '-C', '-e', elf], input='\n'.join(hex(a - anchor) for a in addrs) + '\n', capture_output=True, text=True).stdout.split('\n')
fn = collections.Counter(); where = {}
for i, a in enumerate(addrs):
    name = out[2 * i]; fn[name] += pcs[a]; where[name] = out[2 * i + 1].split('/')[-1]
print('main-thread samples', total)
for n, c in fn.most_common(45):
    print(f'{100*c/total:5.1f}%  {n[:70]}  {where[n][:40]}')
