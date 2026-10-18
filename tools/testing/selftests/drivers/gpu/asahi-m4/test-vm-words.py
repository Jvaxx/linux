#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only OR MIT
"""Actual shared Rust TTBAT/generation helpers against the working C M4 contract."""
from pathlib import Path
import subprocess,json
from common import kernel_root, output_dir, rustc, unpack_fixtures
ROOT=kernel_root();OUT=output_dir()
uat=(ROOT/'drivers/gpu/drm/asahi/g16/uat.rs').read_text()
def body(source,marker):
 start=source.index(marker);i=source.index('{',start)+1;depth=1
 while depth:
  depth+=(source[i]=='{')-(source[i]=='}');i+=1
 return source[start:i]
unpack_fixtures(OUT)
mmu=(ROOT/'drivers/gpu/drm/asahi/mmu.rs').read_text()
helpers='\n'.join(body(mmu,x) for x in ['const fn binding_wire_generation(', 'const fn application_ttb1('])
r='''#![allow(dead_code)]\nconst TTBR_VALID:u64=1;const TTBR_ASID_SHIFT:usize=48;\n'''+helpers+'''
fn main(){let expected=std::fs::read(std::env::args().nth(1).unwrap()).unwrap();let words:Vec<u64>=expected.chunks_exact(8).map(|b|u64::from_le_bytes(b.try_into().unwrap())).collect();let epochs=[0,1,254,255,256,257,u64::MAX];let mut i=0;for asid in 1usize..64{for epoch in epochs{let pa=0x804000000+asid as u64*0x4000;assert_eq!(pa|1|(asid as u64)<<48,words[i]);assert_eq!(binding_wire_generation(epoch)as u64,words[i+1]);assert_eq!(application_ttb1(pa,asid,false),words[i+2]);assert_eq!(application_ttb1(pa,asid,true),pa|1|(asid as u64)<<48);i+=3;}}assert_eq!(i,words.len());println!("441 M4 TTBAT/epoch C golden cases pass; all 63 application upper words zero; G15 shared upper words preserved");}
'''
(OUT/'check-vm-words.rs').write_text(r)
subprocess.run(rustc()+['--edition','2021',str(OUT/'check-vm-words.rs'),'-o',str(OUT/'check-vm-words')],check=True)
p=subprocess.run([str(OUT/'check-vm-words'),str(OUT/'vm-words.bin')],check=True,text=True,capture_output=True)
assert 'new_adopted_with_user_upper(dev, geometry, &regions, false)' in uat
assert 'application_ttb1(uat_inner.kernel_ttb1, idx, uat_inner.map_kernel_to_user)' in mmu
assert 'binding.generation = binding_wire_generation(incarnation)' in mmu
print(p.stdout,end='');(OUT/'vm-words-tests.log').write_text(p.stdout)
