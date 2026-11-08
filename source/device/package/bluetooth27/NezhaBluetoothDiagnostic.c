/* SPDX-License-Identifier: BSD-2-Clause-Patent */
/* Existing r008 diagnostic protocol; scalar state and startup-only bytes.
 * Runtime ACL/SCO/ISO, addresses and pairing keys are not exposed here. */
#include "NezhaBluetoothPlatform.h"
#include "qbt_transport.h"
#include <sys/systm.h>
#include <QcomUsbDiagnostic.h>
extern uint8_t native_bluetooth_boot_rx[512],native_bluetooth_boot_tail[256];
extern volatile uint32_t native_bluetooth_boot_rx_length,native_bluetooth_boot_tail_length,native_bluetooth_boot_sequence;
extern volatile uint64_t native_bluetooth_command_state[2];
static int snapshot(void *cookie,char *out,size_t capacity,size_t *bytes,unsigned *truncated) {
    (void)cookie;
    uint64_t state[32];
    for(unsigned i=0;i<32;i++)state[i]=__atomic_load_n(&native_bluetooth_stats[i],__ATOMIC_ACQUIRE);
    struct qbt_transport_stats pipe;qbt_transport_get_stats(&pipe);
    unsigned before=__atomic_load_n(&native_bluetooth_boot_sequence,__ATOMIC_ACQUIRE);
    if(before&1u)return QUD_BUSY;
    unsigned prefix=native_bluetooth_boot_rx_length,tail=native_bluetooth_boot_tail_length;
    if(prefix>128)prefix=128;if(tail>128)tail=128;
    uint8_t rx[128],last[128];
    memcpy(rx,native_bluetooth_boot_rx,prefix);
    unsigned tail_offset=native_bluetooth_boot_tail_length-tail;
    memcpy(last,native_bluetooth_boot_tail+tail_offset,tail);
    if(before!=__atomic_load_n(&native_bluetooth_boot_sequence,__ATOMIC_ACQUIRE))return QUD_BUSY;
    int n=snprintf(out,capacity,
        "{\"version\":1,\"state\":%llu,\"error\":%lld,\"baud\":%llu,\"tx\":%llu,\"rx\":%llu,"
        "\"irq\":%llu,\"timeouts\":%llu,\"phase\":%llu,\"line_errors\":%llu,\"dma_errors\":%llu,"
        "\"product\":%llu,\"soc\":%llu,\"rom_patch\":%llu,\"board\":%llu,\"owners\":%llu,"
        "\"malformed\":%llu,\"unmatched\":%llu,\"tlv_acks\":%llu,\"worker_exited\":%llu,"
        "\"cleanup\":%lld,\"pipe_boot\":%d,\"pipe_flags\":%u,\"pipe_power_result\":%d,"
        "\"tx_buffered\":%u,\"rx_buffered\":%u,\"last_opcode\":%llu,\"last_status\":%llu,\"boot_prefix\":\"",
        state[0],(int64_t)state[1],state[2],state[3],state[4],state[5],state[6],state[14],state[15],state[16],
        state[17],state[18],state[19],state[28],state[21],state[26],state[27],state[29],state[30],(int64_t)state[31],
        pipe.boot,pipe.flags,pipe.power_result,pipe.tx_buffered,pipe.rx_buffered,
        native_bluetooth_command_state[0],native_bluetooth_command_state[1]);
    if(n<0 || (size_t)n>=capacity)return QUD_FAILED;
    size_t at=(size_t)n;
    if(at+2u*(prefix+tail)+32u>=capacity){*bytes=0;*truncated=1;return QUD_FAILED;}
    static const char hex[]="0123456789abcdef";
    for(unsigned i=0;i<prefix;i++){out[at++]=hex[rx[i]>>4];out[at++]=hex[rx[i]&15];}
    static const char middle[]="\",\"boot_tail\":\"";
    memcpy(out+at,middle,sizeof(middle)-1);at+=sizeof(middle)-1;
    for(unsigned i=0;i<tail;i++){out[at++]=hex[last[i]>>4];out[at++]=hex[last[i]&15];}
    out[at++]='"';out[at++]='}';out[at]=0;*bytes=at;*truncated=0;return QUD_OK;
}
static const struct qud_diag_provider provider={
    .id=10,.version=1,.operations=1u<<QUD_SNAPSHOT,.max_arg0=0,.max_arg1=0,
    .name="nezha-bluetooth",.cookie=NULL,.snapshot=snapshot,
};
void nezha_bluetooth_diagnostic_register(void) {
    static unsigned registered;
    if(__atomic_exchange_n(&registered,1u,__ATOMIC_ACQ_REL))return;
    if(!qcom_usb_diag_register(&provider))printf("[nezha bluetooth] provider10 registration failed\n");
}
