#!/usr/bin/env python3
"""Execute the prepared image selection block and the original plane fallback."""
import argparse
import hashlib
import json
from pathlib import Path
import subprocess

p=argparse.ArgumentParser(description=__doc__)
p.add_argument('--source',type=Path,required=True)
p.add_argument('--out',type=Path,required=True)
p.add_argument('--cxx',default='clang++')
a=p.parse_args();a.out.mkdir(parents=True,exist_ok=False)
source=a.source/'src/freedreno/vulkan/tu_image.cc'
text=source.read_text()
start=text.index('   /* A combined Gen8 descriptor uses the luma tile mode')
end=text.index('   for (uint32_t i = 0; i < tu6_plane_count(image->vk.format); i++) {',start)
block=text[start:end]
layout=a.source/'src/freedreno/fdl/fd6_layout.c'
body=layout.read_text()
start=body.index('   if (!params->force_ubwc &&')
end=body.index('\n   /* Linear D/S is not supported by HW. */',start)
fallback=body[start:end]
fixture=r'''
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <initializer_list>
enum chip { A6XX=6,A7XX=7,A8XX=8 };
enum TestFormat { SINGLE=0,YUV420=1,YUV422=2,YUV444=3,COMPRESSED_COLOR=4 };
enum { TILE6_LINEAR=0,TILE6_3=3 };
struct fdl_layout {TestFormat format;uint32_t width0,linear_fallback_threshold_texels,tile_mode;bool ubwc;};
struct fd_dev_info {};
struct physical {fd_dev_info dev_info;};
struct device {physical *physical_device;};
struct image {struct {TestFormat format;struct {uint32_t width,height;}extent;}vk;};
static unsigned vk_format_get_plane_count(TestFormat f){return f>=YUV420&&f<=YUV444?2:1;}
static TestFormat tu6_plane_format(TestFormat f,unsigned){return f;}
static uint32_t vk_format_get_plane_width(TestFormat f,unsigned plane,uint32_t width){return plane&&(f==YUV420||f==YUV422)?(width+1)/2:width;}
static uint32_t fdl_linear_fallback_threshold_texels(fdl_layout *,fd_dev_info *){return 16;}
static bool util_format_is_depth_or_stencil(TestFormat){return false;}
template<chip CHIP> static uint32_t selected(device *device,image *image,uint32_t tile_mode,bool force_ubwc,bool &ubwc_enabled){
SELECTION_BLOCK
return tile_mode;
}
struct params {bool force_ubwc;TestFormat format;};
static uint32_t actual_fallback(uint32_t width,uint32_t mode,bool forced,bool &ubwc){
 fdl_layout backing={SINGLE,width,16,mode,ubwc};fdl_layout *layout=&backing;
 params input={forced,SINGLE};params *params=&input;
FALLBACK_BLOCK
 ubwc=layout->ubwc;return layout->tile_mode;
}
int main(){
 physical physical{};device device{&physical};unsigned checks=0,old_mismatches=0;
 for(TestFormat f:{SINGLE,YUV420,YUV422,YUV444,COMPRESSED_COLOR})
 for(unsigned width:{2u,8u,14u,16u,18u,30u,32u,64u,1200u,1600u})
 for(bool forced:{false,true}){
  image image{{f,{width,16}}};bool compression=true;
  const auto current=selected<A8XX>(&device,&image,TILE6_3,forced,compression);
  const auto planes=vk_format_get_plane_count(f);
  bool small=false;for(unsigned i=0;i<planes;++i)small|=vk_format_get_plane_width(f,i,width)<16;
  const bool should_change=planes>1&&!forced&&small;
  assert(current==(should_change?TILE6_LINEAR:TILE6_3));assert(compression==!should_change);
  bool ycomp=compression,ccomp=compression;
  const auto y=actual_fallback(width,current,forced,ycomp);
  const auto c=actual_fallback(vk_format_get_plane_width(f,1,width),current,forced,ccomp);
  if(planes>1)assert(y==c);
  bool oldy=true,oldc=true;
  const auto ybefore=actual_fallback(width,TILE6_3,forced,oldy);
  const auto cbefore=actual_fallback(vk_format_get_plane_width(f,1,width),TILE6_3,forced,oldc);
  old_mismatches+=planes>1&&ybefore!=cbefore;
  for(chip older:{A6XX,A7XX}){
   bool enabled=true;
   const auto result=older==A6XX?selected<A6XX>(&device,&image,TILE6_3,forced,enabled):selected<A7XX>(&device,&image,TILE6_3,forced,enabled);
   assert(result==TILE6_3&&enabled);++checks;
  }
  // An explicit linear layout stays linear.
  bool disabled=false;assert(selected<A8XX>(&device,&image,TILE6_LINEAR,forced,disabled)==TILE6_LINEAR&&!disabled);
  ++checks;
 }
 assert(checks==300&&old_mismatches==6);
 printf("PASS %u original image-selection checks; %u old luma/chroma fallback mismatches; planes agree\n",checks,old_mismatches);
}
'''
fixture=fixture.replace('SELECTION_BLOCK',block).replace('FALLBACK_BLOCK',fallback)
cpp=a.out/'layout.cpp';cpp.write_text(fixture)
exe=a.out/'layout';commands=[[a.cxx,'-std=c++17','-O1','-g','-Wall','-Wextra','-Werror','-Wno-unused-parameter','-fsanitize=address,undefined',str(cpp),'-o',str(exe)],[str(exe)]]
for command in commands:
    result=subprocess.run(command,capture_output=True,text=True,timeout=60)
    (a.out/('compile.log' if command==commands[0] else 'execution.log')).write_text(result.stdout+result.stderr)
    assert result.returncode==0,result.stdout+result.stderr
sha=lambda p:hashlib.sha256(p.read_bytes()).hexdigest()
report=dict(passed=True,checks=300,old_plane_mode_mismatches=6,actual_image_selection_block=True,actual_fdl_fallback_block=True,format_and_threshold_callbacks_modeled=True,A6XX_A7XX_unchanged=True,forced_UBWC_unchanged=True,source_sha256=sha(source),fdl_source_sha256=sha(layout),commands=commands,hardware_pixels_verified=False,phone_operations=False)
(a.out/'report.json').write_text(json.dumps(report,indent=2)+'\n');print(result.stdout,end='')
