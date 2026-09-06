"""Bench fault injection: reproduce measured DW PHY loss without nRF reset."""
import time
B=0x4002f000;G=0x50000000;SCRATCH=0x2001f000

def inject(t,awake,configured,phy):
 saved=None;ram=None;halted=False
 try:
  deadline=time.monotonic()+30
  while time.monotonic()<deadline:
   t.halt();halted=True
   if t.read8(awake) and t.read8(configured) and t.read8(phy)==2 and t.read32(B+0x500)==0 and t.read32(G+0x504)&8:break
   t.resume();halted=False;time.sleep(.003)
  else:raise RuntimeError('no safe awake SPI opportunity')
  offsets=[0x500,0x104,0x110,0x118,0x120,0x14c,0x200,0x304,0x524,0x534,0x538,0x544,0x548]
  saved={o:t.read32(B+o) for o in offsets};ram=bytes(t.read_memory_block8(SCRATCH,256))
  t.write32(B+0x500,7);t.write32(B+0x308,0xffffffff);t.write32(B+0x200,0);t.write32(B+0x524,0x2000000)
  def transfer(reg,data=None):
   f=(reg>>16)&31;o=reg&127;addr=(f<<9)|(o<<2);wr=0x80 if data is not None else 0
   h=bytes([(addr>>8)|0x40|wr,addr&255]) if o else bytes([(addr>>8)|wr])
   payload=h+(data if data is not None else bytes(4));t.write_memory_block8(SCRATCH,payload)
   t.write32(B+0x544,SCRATCH);t.write32(B+0x548,len(payload));t.write32(B+0x534,SCRATCH+128);t.write32(B+0x538,len(payload));t.write32(B+0x118,0)
   t.write32(G+0x50c,8);t.write32(B+0x10,1);limit=time.monotonic()+1
   while not t.read32(B+0x118):
    if time.monotonic()>limit:raise RuntimeError('SPI timeout')
   t.write32(G+0x508,8)
   return int.from_bytes(bytes(t.read_memory_block8(SCRATCH+128+len(h),4)),'little')
  assert transfer(0)==0xdeca0302,'unexpected DW identity'
  result={}
  for name,reg,mask,bits in [('CHAN_CTRL',0x10014,6,6),('DTUNE0',0x60000,0xffff0003,65<<16),('TX_FCTRL',0x24,0xf400,0x1400)]:
   before=transfer(reg);after=(before&~mask)|bits;transfer(reg,after.to_bytes(4,'little'));observed=transfer(reg)
   assert observed==after,(name,hex(observed),hex(after));result[name]={'before':hex(before),'injected':hex(after)}
  return result
 finally:
  if saved:
   t.write32(G+0x508,8)
   if ram:t.write_memory_block8(SCRATCH,ram)
   for o,v in saved.items():
    if o!=0x304:t.write32(B+o,v)
   t.write32(B+0x304,saved[0x304])
  if halted:t.resume()
