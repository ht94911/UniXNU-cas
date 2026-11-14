// SPDX-License-Identifier: MIT
#ifndef UMI_USER_WIRE_H
#define UMI_USER_WIRE_H
static int umi_user_wire_repeat(volatile uint32_t *h,const char tag[4],unsigned event,const uint64_t *v,unsigned fields,unsigned copies) {
  const uint32_t cap=0x200000-12;
  if(!h || !event || event>255 || !fields || fields>64 || !copies || copies>5 || h[0]!=0x43474244 || h[1]>h[2] || h[2]>cap)return 0;
  uint32_t at=h[1]==cap?0:h[1],size=h[2];volatile unsigned char *out=(volatile unsigned char *)(h+3);
  const char hex[]="0123456789abcdef";
  for(unsigned copy=0;copy<copies;++copy)for(unsigned f=0;f<fields;++f) {
    uint64_t value=v[f];uint32_t crc=0xffffffff;
    for(unsigned i=0;i<10;++i){unsigned byte=i==0?event:i==1?f:(value>>((i-2)*8))&255;crc^=byte;for(unsigned j=0;j<8;++j)crc=(crc>>1)^(0xedb88320u&(0u-(crc&1)));}crc^=0xffffffff;
    char row[42]="\nUMI ";for(unsigned i=0;i<4;++i)row[5+i]=tag[i];row[9]=' ';unsigned n=10;
    row[n++]=hex[event>>4];row[n++]=hex[event&15];row[n++]=' ';row[n++]=hex[f>>4];row[n++]=hex[f&15];row[n++]=' ';
    for(int s=60;s>=0;s-=4)row[n++]=hex[(value>>s)&15];row[n++]=' ';
    for(int s=28;s>=0;s-=4)row[n++]=hex[(crc>>s)&15];row[n++]='\n';
    for(unsigned i=0;i<n;++i){out[at]=(unsigned char)row[i];at=at+1==cap?0:at+1;if(size<cap)++size;}
  }
  h[2]=size;h[1]=at;return 1;
}
static int umi_user_wire(volatile uint32_t *h,const char tag[4],unsigned event,const uint64_t *v,unsigned fields) {
  return umi_user_wire_repeat(h,tag,event,v,fields,5);
}
#endif
