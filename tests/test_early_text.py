"""Actual diagnostic C text/blits and serial wrapper; controlled host RAM only."""
import ctypes as c
import os
from pathlib import Path
import subprocess
import tempfile
import unittest

UEFI = Path(os.environ.get('CAS_UEFI_ROOT', str(Path(__file__).resolve().parents[2]/'uefi-cas')))
PKG = UEFI/'Platforms/Xiaomi/casPkg'
U, Q = c.c_uint32, c.c_uint64

class Text(c.Structure):
    _fields_ = [(n,U) for n in 'magic version size row column busy bytes frames frozen match'.split()] + [('cells',(c.c_ubyte*56)*16)] + [('pc_match',U),('pc_pending',U)]

class Frame(c.Structure):
    _fields_ = [('base',Q),('bytes',Q)] + [(n,U) for n in 'width height stride red green blue reserved rshift gshift bshift pipes ctls manual intfs'.split()]

class EarlyText(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.tmp=tempfile.TemporaryDirectory()
        out=Path(cls.tmp.name)/'text.so'
        subprocess.run(['cc','-shared','-fPIC','-O2','-Wall','-Wextra','-Werror','-fno-builtin',
                        '-I',str(PKG/'Include'),str(PKG/'Library/SerialDiagLib/EarlyText.c'),
                        str(PKG/'Library/CasDisplayLib/DisplayBlt.c'),'-o',str(out)],check=True,capture_output=True)
        cls.lib=c.CDLL(str(out))
        cls.lib.CasEarlyTextReset.argtypes=[c.POINTER(Text)]
        cls.lib.CasEarlyTextFeed.argtypes=[c.POINTER(Text),c.c_void_p,Q]
        cls.lib.CasEarlyTextValid.argtypes=[c.POINTER(Text)]
        cls.lib.CasEarlyTextRender.argtypes=[c.POINTER(Text),c.POINTER(Frame)]

    @classmethod
    def tearDownClass(cls):cls.tmp.cleanup()

    def setUp(self):
        self.t=Text()
        self.lib.CasEarlyTextReset(c.byref(self.t))

    def feed(self,b):self.lib.CasEarlyTextFeed(c.byref(self.t),b,len(b))
    def line(self,n=None):return bytes(self.t.cells[self.t.row if n is None else n]).rstrip(b'\0')

    def test_layout_and_corrupt_cursor_recovery(self):
        self.assertLessEqual(c.sizeof(Text),1024)
        self.t.row=16;self.feed(b'recovered')
        self.assertEqual(self.line(),b'recovered')
        self.t.column=57;self.feed(b'bounded')
        self.assertEqual(self.line(),b'bounded')

    def test_chunked_crlf_tab_backspace_and_controls(self):
        self.feed(b'abc\r');self.feed(b'Q\nhi\t!\b?\x01\x7f')
        self.assertEqual(self.line(0),b'Qbc')
        self.assertEqual(self.line(),b'hi  ??')

    def test_exact_width_newline_does_not_double_wrap(self):
        self.feed(b'a'*56);self.feed(b'\nB')
        self.assertEqual((self.t.row,self.t.column),(1,1))
        self.assertEqual(self.line(),b'B')

    def test_wrap_and_scroll_keep_recent_lines(self):
        self.feed(b'x'*57)
        self.assertEqual((self.t.row,self.t.column),(1,1))
        for i in range(20):self.feed(f'\nline{i:02d}'.encode())
        rows=[self.line((self.t.row+1+i)%16) for i in range(16)]
        self.assertEqual(rows,[f'line{i:02d}'.encode() for i in range(4,20)])

    def test_non_ascii_is_replaced_and_counter_saturates(self):
        self.t.bytes=0xfffffffe
        self.feed(b'\x80\xff')
        self.assertEqual(self.line(),b'??')
        self.assertEqual(self.t.bytes,0xffffffff)

    def test_latch_preserves_registers_before_stack_overwrites_them(self):
        self.feed(b'ELR 0x9bb6f418\nESR 0x9600000d FAR 0x80900000\nStack dump:\n'+b'stack data\n'*30)
        self.assertEqual(self.t.frozen,1)
        saved=bytes(self.t)
        self.feed(b'ASSERT handler438\n')
        self.assertEqual(bytes(self.t),saved)
        self.assertIn(b'ESR 0x9600000d FAR 0x80900000',bytes(self.t.cells))
        self.assertNotIn(b'stack data',bytes(self.t.cells))

    def test_line_marker_handles_every_chunk_split_and_rejects_embedded_phrase(self):
        marker=b'Stack dump:'
        for split in range(len(marker)+1):
            self.lib.CasEarlyTextReset(c.byref(self.t))
            self.feed(b'ESR test\n'+marker[:split]);self.feed(marker[split:]+b'\ntrash')
            self.assertEqual(self.t.frozen,1)
            self.assertNotIn(b'trash',bytes(self.t.cells))
        self.lib.CasEarlyTextReset(c.byref(self.t))
        self.feed(b'ordinary Stack dump: text\nStack dummy:\nSTaCk DUMP:')
        self.assertEqual(self.t.frozen,1)
        self.assertIn(b'ordinary Stack dump: text',bytes(self.t.cells))

    def frame(self):
        width,height,stride=1080,600,1088*4
        self.mem=(U*(32+1088*height+32))(*([0x12345678]*(64+1088*height)))
        f=Frame();f.base=c.addressof(self.mem)+128;f.bytes=stride*height
        f.width=width;f.height=height;f.stride=stride;f.rshift=16;f.gshift=8;f.bshift=0;f.reserved=0xff000000
        return f

    def test_first_pc_freezes_only_after_the_complete_line(self):
        self.feed(b'Loading driver at 0x9bb00000 EntryPoint=0x9bb01264\nSynchronous Exception at 0x9bb09500\n')
        pc=b'PC 0x00009bb09500 (0x00009bb00000+0x00009500) [ 0] module'
        self.feed(pc)
        self.assertEqual((self.t.frozen,self.t.pc_pending),(0,1))
        self.feed(b'\nV0 register spam\nStack dump:\n')
        self.assertEqual(self.t.frozen,1)
        self.assertIn(b'Loading driver at 0x9bb00000',bytes(self.t.cells))
        self.assertIn(b'0x00009500',bytes(self.t.cells))
        self.assertNotIn(b'V0 register spam',bytes(self.t.cells))

    def test_pc_marker_is_chunk_safe_and_keeps_wrapped_crlf_line(self):
        line=b'PC 0x00009bb09500 (0x00009bb00000+0x00009500) [ 0] very-long-module-name.pdb\r\n'
        for split in range(len(line)+1):
            self.lib.CasEarlyTextReset(c.byref(self.t))
            self.feed(line[:split]);self.feed(line[split:]+b'V register\n'*30)
            self.assertEqual(self.t.frozen,1)
            self.assertIn(b'0x00009500',bytes(self.t.cells))
            self.assertIn(b'very-long-module-name.pdb',bytes(self.t.cells))
            self.assertNotIn(b'V register',bytes(self.t.cells))

    def test_embedded_or_display_wrapped_pc_is_not_a_handler_line(self):
        self.feed(b'ordinary PC 0x123 text\n'+b'x'*56+b'PC 0x456 wrapped\n')
        self.assertEqual(self.t.frozen,0)
        self.feed(b'PC 0x00009bb09500\nignored')
        self.assertEqual(self.t.frozen,1)
        self.assertNotIn(b'ignored',bytes(self.t.cells))

    def test_corrupt_pc_state_is_reset_before_use(self):
        self.t.pc_match=6;self.feed(b'recovered\n')
        self.assertTrue(self.lib.CasEarlyTextValid(c.byref(self.t)))
        self.assertEqual(self.t.pc_pending,0)
        self.t.pc_pending=2;self.feed(b'PC 0x1\n')
        self.assertEqual(self.t.frozen,1)

    def test_render_preserves_guard_padding_and_stage_bands(self):
        self.feed(b'ASSERT: 0x80900000\n')
        f=self.frame();self.assertEqual(self.lib.CasEarlyTextRender(c.byref(self.t),c.byref(f)),0)
        self.assertEqual(list(self.mem[:32]),[0x12345678]*32)
        self.assertEqual(list(self.mem[-32:]),[0x12345678]*32)
        for y in range(600):
            row=self.mem[32+y*1088:32+(y+1)*1088]
            if 136 <= y < 520:
                self.assertEqual(row[:12],[0x12345678]*12)
                self.assertEqual(row[1020:],[0x12345678]*68)
            else:self.assertEqual(row,[0x12345678]*1088)
        self.assertIn(0xffe4edf8,self.mem)

    def test_lowercase_screen_matches_uppercase(self):
        f=self.frame();self.feed(b'assert: abcd1234')
        self.assertEqual(self.lib.CasEarlyTextRender(c.byref(self.t),c.byref(f)),0)
        first=bytes(self.mem)
        self.lib.CasEarlyTextReset(c.byref(self.t));self.feed(b'ASSERT: ABCD1234')
        self.assertEqual(self.lib.CasEarlyTextRender(c.byref(self.t),c.byref(f)),0)
        self.assertEqual(bytes(self.mem),first)

    def test_invalid_geometry_refuses_before_pixel_write(self):
        f=self.frame();self.feed(b'FAIL')
        for field,value in [('width',1000),('height',400),('stride',4),('bytes',1),('rshift',32)]:
            old=getattr(f,field);setattr(f,field,value)
            self.assertNotEqual(self.lib.CasEarlyTextRender(c.byref(self.t),c.byref(f)),0)
            self.assertTrue(all(v==0x12345678 for v in self.mem))
            setattr(f,field,old)

BASE_H = r"""
#include <stddef.h>
#include <stdint.h>
typedef void VOID; typedef uint8_t UINT8, BOOLEAN; typedef uint32_t UINT32;
typedef uintptr_t UINTN; typedef uint64_t UINT64; typedef intptr_t RETURN_STATUS;
#define EFIAPI
#define STATIC static
#define FALSE 0
#define RETURN_SUCCESS 0
#define RETURN_DEVICE_ERROR -2
#define RETURN_INVALID_PARAMETER -3
#define RETURN_UNSUPPORTED -4
#define RETURN_ERROR(x) ((x)<0)
#define MAX_UINT32 UINT32_MAX
#define SIGNATURE_32(a,b,c,d) ((a)|((b)<<8)|((c)<<16)|((d)<<24))
BOOLEAN GetInterruptState(void); void EnableInterrupts(void); void DisableInterrupts(void); void MemoryFence(void);
"""
SERIAL_H = r"""
typedef int EFI_PARITY_TYPE, EFI_STOP_BITS_TYPE;
#define EFI_SERIAL_INPUT_BUFFER_EMPTY 1
#define EFI_SERIAL_OUTPUT_BUFFER_EMPTY 2
UINTN SerialPortWrite(UINT8 *,UINTN);
"""
HARNESS = r"""
#include <sys/mman.h>
#include <string.h>
#include <Library/BaseLib.h>
#include <CasEarlyText.h>
static int Mapped, Irq, Refreshes, Nested;
UINTN SerialPortWrite(UINT8 *,UINTN);
BOOLEAN GetInterruptState(void) {return Irq;}
void EnableInterrupts(void) {Irq=1;}
void DisableInterrupts(void) {Irq=0;}
void MemoryFence(void) {__sync_synchronize();}
CAS_DISPLAY_STATE *CasDisplayState(void) {return (void *)(uintptr_t)CAS_DISPLAY_ADDRESS;}
void CasDisplayRefresh(void) {++Refreshes;if(Nested) {Nested=0;SerialPortWrite((UINT8 *)"nested",6);}}
int Setup(void) {
 CAS_BOOT_INFO *B=(void *)(uintptr_t)CAS_LOG_BASE;
 CAS_DISPLAY_STATE *S=CasDisplayState();
 if(!Mapped) {
  if(mmap((void *)(uintptr_t)CAS_LOG_BASE,CAS_LOG_SIZE,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS|MAP_FIXED_NOREPLACE,-1,0)==MAP_FAILED)return -1;
  if(mmap((void *)(uintptr_t)CAS_SPLASH_BASE,1080*2340*4,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS|MAP_FIXED_NOREPLACE,-1,0)==MAP_FAILED)return -2;
  Mapped=1;
 }
 memset(B,0,CAS_LOG_SIZE);memset((void *)(uintptr_t)CAS_SPLASH_BASE,0,1080*2340*4);
 B->Magic=CAS_BOOT_MAGIC;B->Version=CAS_BOOT_VERSION;B->Size=sizeof(*B);B->Stage=7;
 S->Magic=CAS_DISPLAY_MAGIC;S->Version=CAS_DISPLAY_VERSION;S->Size=sizeof(*S);S->Ready=1;
 S->Framebuffer.Base=CAS_SPLASH_BASE;S->Framebuffer.Bytes=1080*2340*4;
 S->Framebuffer.Width=1080;S->Framebuffer.Height=2340;S->Framebuffer.Stride=1080*4;
 S->Framebuffer.RedShift=16;S->Framebuffer.GreenShift=8;S->Framebuffer.ReservedMask=0xff000000;
 Irq=1;Refreshes=0;Nested=0;return 0;
}
void SetIrq(int x) {Irq=x;}
int RefreshCount(void) {return Refreshes;}
void NestOnRefresh(void) {Nested=1;}
void SetStage(unsigned x) {((CAS_BOOT_INFO *)(uintptr_t)CAS_LOG_BASE)->Stage=x;}
void BadFrame(void) {CasDisplayState()->Framebuffer.Base=CAS_HEAP_BASE;}
"""

class SerialIntegration(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.tmp=tempfile.TemporaryDirectory();p=Path(cls.tmp.name);(p/'Library').mkdir()
        (p/'Library/BaseLib.h').write_text('#pragma once\n'+BASE_H)
        (p/'Library/SerialPortLib.h').write_text('#pragma once\n'+SERIAL_H)
        (p/'harness.c').write_text(HARNESS)
        out=p/'serial.so'
        subprocess.run(['cc','-shared','-fPIC','-O2','-Wall','-Wextra','-Werror','-Wno-unused-parameter','-fno-builtin',
                        '-DCAS_XNU_DIAGNOSTICS=1','-I',str(p),'-I',str(PKG/'Include'),str(p/'harness.c'),
                        str(PKG/'Library/SerialDiagLib/SerialDiagLib.c'),str(PKG/'Library/SerialDiagLib/EarlyText.c'),
                        str(PKG/'Library/CasDisplayLib/DisplayBlt.c'),'-o',str(out)],check=True,capture_output=True)
        cls.lib=c.CDLL(str(out));cls.lib.SerialPortWrite.argtypes=[c.c_void_p,c.c_size_t];cls.lib.SerialPortWrite.restype=c.c_size_t

    @classmethod
    def tearDownClass(cls):cls.tmp.cleanup()

    def setUp(self):
        self.assertEqual(self.lib.Setup(),0)
        self.t=Text.from_address(0x9f940c00)

    def write(self,b):self.assertEqual(self.lib.SerialPortWrite(b,len(b)),len(b))

    def test_actual_serial_keeps_case_and_mirrors_then_stops_at_bds(self):
        self.write(b'caseSensitive\n')
        self.assertEqual(self.t.bytes,14)
        self.assertEqual(c.string_at(0x9f941010,14),b'caseSensitive\n')
        self.assertEqual(self.lib.RefreshCount(),1)
        self.lib.SetStage(8);self.write(b'afterBDS')
        self.assertEqual(self.t.bytes,14);self.assertEqual(self.lib.RefreshCount(),1)
        self.assertEqual(U.from_address(0x9f941004).value,22)

    def test_interrupt_state_and_reentrant_refresh(self):
        self.lib.SetIrq(0);self.lib.NestOnRefresh();self.write(b'outer')
        self.assertEqual(self.lib.GetInterruptState(),0)
        self.assertEqual(self.t.bytes,5);self.assertEqual(self.t.busy,0)
        self.assertEqual(self.lib.RefreshCount(),1)
        self.assertEqual(c.string_at(0x9f941010,11),b'outernested')
        self.lib.SetIrq(1);self.write(b'A')
        self.assertEqual(self.lib.GetInterruptState(),1)

    def test_unknown_framebuffer_and_empty_write_do_not_refresh(self):
        self.lib.BadFrame();self.write(b'bad frame')
        self.assertEqual(self.lib.RefreshCount(),0)
        self.assertEqual(self.lib.SerialPortWrite(None,10),0)
        self.assertEqual(self.lib.GetInterruptState(),1)

    def test_latched_screen_keeps_full_raw_log_and_stops_refreshing(self):
        self.write(b'ELR pc\nESR syndrome FAR address\n')
        self.write(b'Stack dump:\n' + b'dump line\n'*32)
        self.assertEqual(self.t.frozen,1)
        saved=bytes(self.t);frames=self.lib.RefreshCount()
        self.write(b'ASSERT handler438\n')
        self.assertEqual(bytes(self.t),saved)
        self.assertEqual(self.lib.RefreshCount(),frames)
        used=U.from_address(0x9f941004).value
        raw=c.string_at(0x9f941010,used)
        self.assertIn(b'dump line\n'*32,raw)
        self.assertTrue(raw.endswith(b'ASSERT handler438\n'))

    def test_raw_log_capacity_preserves_structured_tail(self):
        tail=(c.c_ubyte*65536).from_address(0x9f970000)
        for i in range(len(tail)):tail[i]=0xa5
        self.lib.SetStage(8);self.write(b'x'*210000)
        self.assertEqual(U.from_address(0x9f941004).value,0x30000-0x1000-16)
        self.assertGreater(U.from_address(0x9f941008).value,0)
        self.assertEqual(bytes(tail),b'\xa5'*65536)

    def test_first_pc_screen_capture_still_logs_all_registers_and_stack(self):
        context=b'Loading driver at 0x9bb00000 EntryPoint=0x9bb01264\nPC 0x00009bb09500\n'
        self.write(context)
        self.assertEqual(self.t.frozen,1)
        saved=bytes(self.t);frames=self.lib.RefreshCount()
        rest=b'X0 original\nV31 original\nESR 0x02000000\nStack dump:\n'+b'full stack\n'*20
        self.write(rest)
        self.assertEqual(bytes(self.t),saved)
        self.assertEqual(self.lib.RefreshCount(),frames)
        used=U.from_address(0x9f941004).value
        self.assertEqual(c.string_at(0x9f941010,used),context+rest)

if __name__=='__main__':unittest.main()
