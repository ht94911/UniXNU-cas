# SPDX-License-Identifier: BSD-2-Clause-Patent
"""Actual patched MenuRun and text/portrait/landscape draw functions on host."""
import ctypes as c,json,os,subprocess,sys,tempfile,unittest
from pathlib import Path
ROOT=Path(__file__).resolve().parents[1];sys.path.insert(0,str(ROOT/'scripts'))
from cas_menu_overlay import stage
class MenuScrollTests(unittest.TestCase):
 @classmethod
 def setUpClass(cls):
  cls.tmp=tempfile.TemporaryDirectory(prefix='cas-actual-menu-');cls.addClassCleanup(cls.tmp.cleanup);out=Path(cls.tmp.name)
  stage(ROOT.parent/'UniXNU',out/'overlay');lib=out/'overlay/UniXNUPkg/Library/UniXNULib'
  ui=(lib/'Ui.c').read_text();portrait='STATIC VOID DrawEntryPortrait('+ui.split('DrawEntryPortrait (',1)[1].split('STATIC\nVOID\nDrawStatusPortrait',1)[0]
  draw='VOID UniXNUUiDrawEntry('+ui.split('UniXNUUiDrawEntry (',1)[1].split('VOID\nUniXNUUiDrawStatus',1)[0]
  menu=(lib/'Menu.c').read_text().replace('#include "Internal.h"','').split('BOOLEAN\nUniXNUMenuConfirm',1)[0]
  wrapper=r"""
int menu_case(const char *Keys,int Layout,unsigned Default,unsigned Count,unsigned Rows,int HasInput,int *Output){
 UNIXNU_ENTRY Entries[64];UNIXNU_CONFIG C={0};UNIXNU_UI U={0};UINTN I,Result;
 if(!Count || Count>64 || !Rows)return -1;
 for(I=0;I<Count;I++){Entries[I].Title=L"Entry";Entries[I].Id="entry";}
 C.EntryCount=Count;C.DefaultIndex=Default;C.FallbackTimeout=3;C.RotateSeconds=0;C.Timeout=-1;C.Entries=Entries;
 U.Mode=Layout==0?UniXNUUiText:UniXNUUiGraphics;U.Portrait=Layout==1;U.Scale=1;U.Width=1080;U.Height=2340;U.Margin=12;U.CardHeight=70;U.MenuTop=100;U.RowHeight=80;U.MenuLeft=12;U.MenuWidth=400;U.VisibleRows=Rows<Count?Rows:Count;
 Script=Keys;Position=WaitCount=0;BoundsError=0;Physical=HasInput;
 Input.ReadKeyStroke=Read;Input.WaitForKey=(void *)1;gST->ConIn=&Input;
 gBS->LocateHandleBuffer=Locate;gBS->HandleProtocol=Handle;gBS->CreateEvent=Create;gBS->SetTimer=Timer;gBS->WaitForEvent=Wait;gBS->CloseEvent=Close;
 Result=UniXNUMenuRun(&U,&C,TRUE);Output[0]=(int)Result;Output[1]=(int)U.FirstVisible;Output[2]=BoundsError;
 for(I=0;I<Rows;I++){Output[3+I]=Drawn[I];Output[3+Rows+I]=Highlight[I];}return 0;
}
"""
  code=(ROOT/'tests/menu_host_prefix.c').read_text()+'\n'+portrait+'\n'+draw+'\n'+menu+'\n'+wrapper
  (out/'menu.c').write_text(code)
  p=subprocess.run(['clang-18','-std=c11','-shared','-fPIC','-fshort-wchar','-O1','-Wall','-Wextra','-Werror','-Wno-unused-variable','-Wno-unused-function','-Wno-unused-parameter','-Wno-misleading-indentation','-I'+str(lib),str(out/'menu.c'),'-o',str(out/'menu.so')],capture_output=True,text=True)
  if p.returncode:raise RuntimeError(p.stderr)
  cls.lib=c.CDLL(str(out/'menu.so'));cls.lib.menu_case.argtypes=[c.c_char_p,c.c_int,c.c_uint,c.c_uint,c.c_uint,c.c_int,c.POINTER(c.c_int)]
 def case(self,keys,layout=1,default=0,count=6,rows=4,physical=1):
  out=(c.c_int*(3+rows*2))();self.assertEqual(self.lib.menu_case(keys.encode(),layout,default,count,rows,physical,out),0);self.assertEqual(out[2],0,'actual draw emitted a row outside the viewport');return out[0],out[1],list(out[3:3+rows]),list(out[3+rows:])
 def test_entry5_and6_are_drawn_and_selectable_all_layouts(self):
  for layout in [0,1,2]:
   for keys,index,first,drawn in [('dddE',3,0,[1,2,3,4]),('ddddE',4,1,[2,3,4,5]),('dddddE',5,2,[3,4,5,6])]:
    with self.subTest(layout=layout,keys=keys):
     selected,start,actual,highlight=self.case(keys,layout);self.assertEqual((selected,start,actual),(index,first,drawn));self.assertEqual(sum(highlight),1);self.assertEqual(highlight[index-first],1)
 def test_wrap_home_end_and_reverse_navigation(self):
  for keys,index,first in [('uE',5,2),('ddddddE',0,0),('NuE',4,2),('NHdE',1,0),('dddddduE',5,2)]:
   selected,start,drawn,highlight=self.case(keys);self.assertEqual((selected,start),(index,first));self.assertEqual(drawn[index-first],index+1)
 def test_last_entry_as_default_is_visible_on_initial_draw(self):
  selected,start,drawn,highlight=self.case('E',default=5);self.assertEqual((selected,start,drawn),(5,2,[3,4,5,6]));self.assertEqual(highlight,[0,0,0,1])
 def test_short_menus_and_single_visible_row(self):
  self.assertEqual(self.case('dE',count=2)[:3],(1,0,[1,2,0,0]))
  self.assertEqual(self.case('dddE',rows=1)[:3],(3,3,[4]))
 def test_original_digit_boot_and_no_input_timeout_still_choose_same_entry(self):
  self.assertEqual(self.case('6')[0],5);self.assertEqual(self.case('',default=2,physical=0)[0],2)
if __name__=='__main__':unittest.main()
