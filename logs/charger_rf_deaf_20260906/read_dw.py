"""Bench-only halted CPU DW register reads. No DW writes, wake, or reset.
Borrow SPIM only with CS high and radio awake; restore DMA RAM/config/events.
Halting extends the current RX operation, so this is configuration evidence only.
"""
import time,json,sys
from pathlib import Path
from pyocd.core.helpers import ConnectHelper
root=Path('logs/charger_rf_deaf_20260906');base=0x4002f000;gpio=0x50000000;scratch=0x2001f000
role=sys.argv[1];uid={'anchor_a':'E46070D247233537','anchor_b':'E46070D247394D36'}[role]
s=ConnectHelper.session_with_chosen_probe(unique_id=uid,target_override='nrf52833',connect_mode='attach',options={'frequency':4000000,'resume_on_disconnect':True});s.open();t=s.board.target
saved=None;ram=None;halted=False
try:
 end=time.monotonic()+30
 while time.monotonic()<end:
  if True:
   t.halt();halted=True
   state=[t.read8(536933579),t.read32(base+0x500),t.read32(gpio+0x504)&8,t.read32(base+0x508)]
   print(state,flush=True)
   if state[0] and state[1]==0 and state[2] and state[3]<32:
    break
   t.resume();halted=False;time.sleep(.003)
 else:raise RuntimeError('no awake idle SPI capture opportunity')
 offsets=[0x500,0x104,0x110,0x118,0x120,0x14c,0x200,0x304,0x524,0x534,0x538,0x544,0x548]
 saved={o:t.read32(base+o) for o in offsets};ram=bytes(t.read_memory_block8(scratch,256))
 t.write32(base+0x500,7);t.write32(base+0x308,0xffffffff);t.write32(base+0x200,0);t.write32(base+0x524,0x2000000)
 def read_dw(reg,length=4):
  f=(reg>>16)&31;o=reg&127;addr=(f<<9)|(o<<2)
  h=bytes([(addr>>8)|0x40,addr&255]) if o else bytes([addr>>8])
  payload=h+bytes(length);t.write_memory_block8(scratch,payload)
  t.write32(base+0x544,scratch);t.write32(base+0x548,len(payload));t.write32(base+0x534,scratch+128);t.write32(base+0x538,len(payload));t.write32(base+0x118,0)
  t.write32(gpio+0x50c,8);t.write32(base+0x10,1)
  limit=time.monotonic()+1
  while not t.read32(base+0x118):
   if time.monotonic()>limit:raise RuntimeError('SPI timeout')
  t.write32(gpio+0x508,8)
  return bytes(t.read_memory_block8(scratch+128+len(h),length)).hex()
 result={'cpu_pc':hex(t.read_core_register('pc')),'saved_spim':saved,'time':time.monotonic(),'registers':{}}
 for name,reg in [('DEV_ID',0),('SYS_CFG',0x10),('TX_FCTRL',0x24),('SYS_STATUS',0x44),('CHAN_CTRL',0x10014),('DGC_CFG',0x30018),('DTUNE0',0x60000),('DTUNE1',0x60004),('DTUNE3',0x6000c),('RX_CTRL_HI',0x70010),('LDO_CTRL',0x70048),('PLL_CFG',0x90000),('SYS_STATE',0xf0030),('SEQ_CTRL',0x110008)]:result['registers'][name]=read_dw(reg)
 for f in [0,1,3,4,6,7,9,0xe,0x11]:result['registers']['file_'+hex(f)]=read_dw(f<<16,112)
 (root/(role+'-dw.json')).write_text(json.dumps(result,indent=2));print(json.dumps({k:v for k,v in result['registers'].items() if not k.startswith('file_')},indent=2))
finally:
 if saved:
  t.write32(gpio+0x508,8)
  if ram:t.write_memory_block8(scratch,ram)
  for o,v in saved.items():
   if o!=0x304:t.write32(base+o,v)
  t.write32(base+0x304,saved[0x304])
 if halted:t.resume()
 s.close()
