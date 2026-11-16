/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#include "../platform/AudioInputPack.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
int main(int argc,char **argv) {
    assert(argc==2);FILE *f=fopen(argv[1],"rb");assert(f);assert(!fseek(f,0,SEEK_END));long n=ftell(f);assert(n>4096);rewind(f);
    unsigned char *data=malloc((size_t)n),*metadata=malloc(65536);assert(data && metadata);
    assert(fread(data,1,(size_t)n,f)==(size_t)n);fclose(f);
    NaInputPack pack;assert(!na_input_pack(&pack,data,(uint64_t)n));
    for (unsigned kind=0;kind<2;++kind) {
        NaFirmwarePiece pieces[QCOM_MDT_MAX_SEGMENTS],mdt;uint32_t count;NaSplitFirmware *fw=calloc(1,sizeof *fw);assert(fw);
        assert(!na_input_firmware(&pack,kind,pieces,QCOM_MDT_MAX_SEGMENTS,&count,&mdt));
        assert(!na_split_firmware(fw,pieces,count));
        assert(qcom_mdt_metadata_size(&fw->image)==mdt.bytes && mdt.bytes<=65536);
        assert(!qcom_mdt_metadata(&fw->image,na_split_read,fw,metadata,65536));
        assert(!memcmp(metadata,mdt.data,(size_t)mdt.bytes));free(fw);
    }
    NaFirmwarePiece missing;assert(na_input_find(&pack,"not-present",&missing)==NA_MISSING);
    unsigned char saved[128];memcpy(saved,data+16,128);
    memset(data+16,'a',48);assert(na_input_pack(&pack,data,(uint64_t)n)==NA_WIRE);memcpy(data+16,saved,128);
    memcpy(data+80,data+16,48);assert(na_input_pack(&pack,data,(uint64_t)n)==NA_WIRE);memcpy(data+16,saved,128);
    memset(data+16+48,0xff,8);assert(na_input_pack(&pack,data,(uint64_t)n)==NA_WIRE);memcpy(data+16,saved,128);
    memset(data+80+48,0,8);assert(na_input_pack(&pack,data,(uint64_t)n)==NA_WIRE);memcpy(data+16,saved,128);
    memset(data+16+56,0xff,8);assert(na_input_pack(&pack,data,(uint64_t)n)==NA_WIRE);memcpy(data+16,saved,128);
    assert(na_input_pack(&pack,data,4096)==NA_WIRE);
    free(metadata);free(data);puts("PASS: both actual firmware families and exact signed metadata; malformed container rejected");
}
