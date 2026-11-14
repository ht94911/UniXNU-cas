/* SPDX-License-Identifier: GPL-2.0-only */
/* H4 framing and Qualcomm IBS, following Linux hci_qca and h4_recv_buf.
 * Byte storage is bounded. The executor sleeps on actual work/deadline events. */
#include "qbt_internal.h"
#include <stdarg.h>
#define SLEEP_IND 0xfe
#define WAKE_IND 0xfd
#define WAKE_ACK 0xfc
static uint64_t now(struct qbt *q) { return q->ops.now_ns(q->ops.context); }
static bool drained(struct qbt *q) { return q->ops.uart_drained(q->ops.context); }
static int clock_on(struct qbt *q) {
    if (q->clock_on) return 0;
    int e = q->ops.clock(q->ops.context, true);
    if (!e) q->clock_on = true;
    return e;
}
static bool flush(struct qbt *q) {
    while (q->forward_offset < q->forward_length) {
        size_t n = q->ops.receive(q->ops.context, q->forward + q->forward_offset,
                                q->forward_length - q->forward_offset);
        if (!n) { q->stats.backpressure++; return false; }
        if (n > q->forward_length - q->forward_offset) n = q->forward_length - q->forward_offset;
        q->forward_offset += (unsigned)n; q->stats.delivered_bytes += n;
    }
    q->forward_offset = q->forward_length = 0;
    return true;
}
static void control(struct qbt *q, uint8_t b) {
    if (q->control_length < sizeof(q->control)) q->control[q->control_length++] = b;
    else q->stats.malformed++;
}
static uint16_t metadata_handle(const uint8_t *p) {
    uint16_t handle=(uint16_t)(p[0]|(unsigned)p[1]<<8);
    return handle<=0x0eff?handle:UINT16_MAX;
}
static void link_recent(struct qbt *q,uint8_t kind,uint16_t opcode,uint8_t code,
                        uint8_t status,uint32_t value,uint16_t handle) {
    struct qbt_link_stats *s=&q->stats.link;
    struct qbt_link_recent *r=&s->recent[s->recent_next];
    memset(r,0,sizeof(*r));
    r->sequence=++s->recent_total;r->ns=now(q);r->kind=kind;r->opcode=opcode;
    r->code=code;r->status=status;r->value=value;r->handle=handle;
    s->recent_next=(s->recent_next+1)%QBT_LINK_RECENT_CAPACITY;
    if(s->recent_count<QBT_LINK_RECENT_CAPACITY)s->recent_count++;
}
static bool link_opcode(uint16_t opcode) {
    switch(opcode) {
    case 0x0405:case 0x0406:case 0x0408:case 0x0409:case 0x040a:
    case 0x040b:case 0x040c:case 0x040d:case 0x040e:case 0x0411:case 0x0413:
    case 0x0419:case 0x041a:case 0x041b:case 0x041c:case 0x041d:
    case 0x042b:case 0x042c:case 0x042d:case 0x042e:case 0x042f:
    case 0x0430:case 0x0433:case 0x0434:case 0x0445:
    case 0x0c03:case 0x0c0d:case 0x0c11:case 0x0c12:case 0x0c20:
    case 0x0c55:case 0x0c56:case 0x0c60:case 0x0c79:case 0x0c7a:
    case 0x1408:case 0x200d:case 0x200e:case 0x2016:case 0x2019:
    case 0x201a:case 0x201b:case 0x2043:return true;
    default:return false;
    }
}
static bool link_command_handle(uint16_t opcode) {
    return opcode==0x0406 || opcode==0x0411 || opcode==0x0413 ||
        opcode==0x041b || opcode==0x041c || opcode==0x041d || opcode==0x1408 ||
        opcode==0x2016 || opcode==0x2019 || opcode==0x201a || opcode==0x201b;
}
static void link_tx_parameters(struct qbt_tx_frame *f,const uint8_t *p,unsigned count) {
    uint16_t opcode=(uint16_t)(f->header[0]|(unsigned)f->header[1]<<8);
    unsigned first=f->header[2]-f->body;
    /* Inspect only known non-secret offsets; never buffer an address/PIN/key
     * prefix while waiting to discover which command is being sent. */
    for(unsigned i=0;i<count;i++) {
        unsigned at=first+i;
        if(link_command_handle(opcode) && at<2) {
            unsigned shift=8*at;
            f->link_handle=(uint16_t)((f->link_handle&~(0xffu<<shift))|((unsigned)p[i]<<shift));
        } else if((opcode==0x0406 || opcode==0x0413) && at==2)f->link_value=p[i];
        else if(opcode==0x042b && f->header[2]==9 && at>=6 && at<=8) {
            unsigned shift=8*(at-6);
            if(at==6)f->link_value=0;
            f->link_value=(f->link_value&~(0xffu<<shift))|((uint32_t)p[i]<<shift);
        } else if((opcode==0x040d || opcode==0x0434 || opcode==0x0c60) && at==6)f->link_value=p[i];
        else if((opcode==0x0c20 || opcode==0x0c56 || opcode==0x0c7a) && at==0)f->link_value=p[i];
    }
}
static void acl_clear(struct qbt *q) {
    memset(&q->tx_acl,0,sizeof(q->tx_acl));memset(&q->rx_acl,0,sizeof(q->rx_acl));
    q->tx_acl.current=q->rx_acl.current=QBT_ACL_OBSERVER_SLOTS;
}
static void acl_forget(struct qbt *q,uint16_t handle) {
    struct qbt_acl_direction *directions[]={&q->tx_acl,&q->rx_acl};
    for(unsigned d=0;d<2;d++)for(unsigned i=0;i<QBT_ACL_OBSERVER_SLOTS;i++) {
        struct qbt_acl_observer *s=&directions[d]->slots[i];
        if(s->active && s->handle==handle) {
            memset(s,0,sizeof(*s));
            if(directions[d]->current==i)directions[d]->current=QBT_ACL_OBSERVER_SLOTS;
        }
    }
}
static void acl_begin(struct qbt *q,struct qbt_acl_direction *d,const uint8_t header[4]) {
    d->current=QBT_ACL_OBSERVER_SLOTS;
    d->packet_remaining=header[2]|(unsigned)header[3]<<8;
    if(!q->service || q->info.state!=2)return;
    unsigned word=header[0]|(unsigned)header[1]<<8,pb=(word>>12)&3;
    uint16_t handle=(uint16_t)(word&0x0fff);
    /* SMP is unicast Basic L2CAP. Broadcast packet flags are not evidence of
     * SMP, but the original packet still goes to the stack. PB3 is a complete
     * L2CAP PDU, whereas PB0/2 can begin a fragmented PDU. */
    d->complete=pb==3;
    if(handle>0x0eff)return;
    unsigned index=QBT_ACL_OBSERVER_SLOTS;
    for(unsigned i=0;i<QBT_ACL_OBSERVER_SLOTS;i++)
        if(d->slots[i].active && d->slots[i].handle==handle){index=i;break;}
    /* Every new PDU boundary invalidates the preceding partial observation,
     * including a zero-length or malformed new packet. A later continuation
     * must not turn bytes from before that boundary into a pairing failure. */
    if(pb!=1 && index!=QBT_ACL_OBSERVER_SLOTS) {
        q->stats.link.acl_observation_dropped++;memset(&d->slots[index],0,sizeof(d->slots[index]));
    }
    if(!d->packet_remaining || (word&0xc000))return;
    if(d->complete && d->packet_remaining<4){q->stats.link.acl_observation_dropped++;return;}
    if(pb==1) {
        if(index==QBT_ACL_OBSERVER_SLOTS)return;
        struct qbt_acl_observer *s=&d->slots[index];
        unsigned left=s->header_have<4?UINT16_MAX:s->remaining;
        if(d->packet_remaining>left) {
            q->stats.link.acl_observation_dropped++;memset(s,0,sizeof(*s));return;
        }
    } else {
        if(index==QBT_ACL_OBSERVER_SLOTS)
            for(unsigned i=0;i<QBT_ACL_OBSERVER_SLOTS;i++)if(!d->slots[i].active){index=i;break;}
        if(index==QBT_ACL_OBSERVER_SLOTS){q->stats.link.acl_observation_dropped++;return;}
        memset(&d->slots[index],0,sizeof(d->slots[index]));
        d->slots[index].handle=handle;d->slots[index].active=true;
    }
    d->current=index;
}
static unsigned smp_length(uint8_t code) {
    switch(code) {
    case 1:case 2:return 7;
    case 3:case 4:case 6:case 8:case 10:case 13:return 17;
    case 5:case 11:case 14:return 2;
    case 7:return 11;
    case 9:return 8;
    case 12:return 65;
    default:return 0;
    }
}
static void smp_record(struct qbt *q,const struct qbt_acl_observer *s,bool tx) {
    unsigned expected=smp_length(s->code);
    if(expected && s->length!=expected){q->stats.link.acl_observation_dropped++;return;}
    uint32_t value=UINT32_MAX;uint8_t status=0xff;
    if(s->code==1 || s->code==2) {
        value=s->parameters[0]|(uint32_t)s->parameters[1]<<8|
            (uint32_t)s->parameters[2]<<16|(uint32_t)s->parameters[3]<<24;
    } else if(s->code==5 || s->code==11 || s->code==14)value=s->parameters[0];
    if(s->code==5){status=s->parameters[0];q->stats.link.smp_failures++;}
    q->stats.link.smp_packets++;
    link_recent(q,tx?QBT_LINK_SMP_TX:QBT_LINK_SMP_RX,s->cid,s->code,status,value,s->handle);
}
static void acl_bytes(struct qbt *q,struct qbt_acl_direction *d,const uint8_t *p,unsigned n,bool tx) {
    if(d->current>=QBT_ACL_OBSERVER_SLOTS)return;
    struct qbt_acl_observer *s=&d->slots[d->current];
    if(!s->active)return;
    for(unsigned i=0;i<n;i++) {
        if(d->packet_remaining)d->packet_remaining--;
        if(s->header_have<4) {
            s->header[s->header_have++]=p[i];
            if(s->header_have!=4)continue;
            s->length=s->remaining=(uint16_t)(s->header[0]|(unsigned)s->header[1]<<8);
            s->cid=(uint16_t)(s->header[2]|(unsigned)s->header[3]<<8);
            if(s->cid!=6 && s->cid!=7) {
                memset(s,0,sizeof(*s));d->current=QBT_ACL_OBSERVER_SLOTS;return;
            }
            if(!s->remaining || d->packet_remaining>s->remaining ||
               (d->complete && d->packet_remaining!=s->remaining)) {
                q->stats.link.acl_observation_dropped++;memset(s,0,sizeof(*s));
                d->current=QBT_ACL_OBSERVER_SLOTS;return;
            }
            continue;
        }
        if(!s->position)s->code=p[i];
        else if((s->code==1 || s->code==2) && s->position<=4)s->parameters[s->position-1]=p[i];
        else if((s->code==5 || s->code==11 || s->code==14) && s->position==1)s->parameters[0]=p[i];
        /* All unlisted payload bytes, including keys and randoms, are skipped. */
        s->position++;s->remaining--;
        if(!s->remaining) {
            smp_record(q,s,tx);memset(s,0,sizeof(*s));
            d->current=QBT_ACL_OBSERVER_SLOTS;return;
        }
    }
}
static void link_hci_rx(struct qbt *q,const uint8_t *e,unsigned n) {
    if(!q->service || q->info.state!=2 || n<2)return;
    struct qbt_link_stats *s=&q->stats.link;
    uint16_t handle=UINT16_MAX,opcode=0;uint8_t kind=QBT_LINK_EVENT,code=e[0],status=0xff;
    uint32_t value=UINT32_MAX;
    if((e[0]==0x0e && n>=5) || (e[0]==0x0f && n>=6)) {
        bool cc=e[0]==0x0e,has_status=!cc || n>=6;
        opcode=(uint16_t)(e[cc?3:4]|(unsigned)e[cc?4:5]<<8);
        status=has_status?e[cc?5:2]:0xff;kind=cc?QBT_LINK_CC:QBT_LINK_CS;
        if(has_status && status)s->command_errors++;
        if(!link_opcode(opcode) && (!has_status || !status))return;
        value=e[cc?2:3];
        if(cc && n>=8 && (opcode==0x201a || opcode==0x201b || opcode==0x1408))handle=metadata_handle(e+6);
        if(cc && opcode==0x1408 && !status && n>=9)value=e[8];
        if(cc && opcode==0x0c03 && has_status && !status)acl_clear(q);
    } else {
    switch(e[0]) {
    case 0x03:
        if(n<13)return;
        status=e[2];s->classic_connections++;
        if(status)s->classic_connection_errors++;
        else {handle=metadata_handle(e+3);acl_forget(q,handle);value=e[11]|(uint32_t)e[12]<<8;}
        break;
    case 0x04:if(n<12)return;value=e[11];break;
    case 0x05:
        if(n<6)return;
        status=e[2];handle=metadata_handle(e+3);value=e[5];s->disconnects++;
        if(!status)acl_forget(q,handle);
        break;
    case 0x06:
        if(n<5)return;
        status=e[2];handle=metadata_handle(e+3);s->authentication_complete++;
        if(status)s->authentication_errors++;
        break;
    case 0x07:if(n<9)return;status=e[2];break;
    case 0x08:case 0x30:case 0x59:
        if(n<(e[0]==0x30?5u:e[0]==0x59?7u:6u))return;
        status=e[2];handle=metadata_handle(e+3);s->encryption_change++;
        if(status)s->encryption_errors++;
        if(e[0]!=0x30)value=e[5]|(e[0]==0x59?(uint32_t)e[6]<<8:0);
        break;
    case 0x09:case 0x0b:case 0x0c:
        if(n<(e[0]==0x09?5u:e[0]==0x0b?13u:10u))return;
        status=e[2];handle=metadata_handle(e+3);break;
    case 0x10:if(n<3)return;status=e[2];break;
    case 0x16:case 0x17:case 0x31:case 0x34:case 0x35:if(n<8)return;break;
    case 0x18:if(n<25)return;value=e[24];break; /* key type, not the key */
    case 0x32:if(n<11)return;value=e[8]|(uint32_t)e[9]<<8|(uint32_t)e[10]<<16;break;
    case 0x33:case 0x3b:if(n<12)return;break; /* never retain the displayed number */
    case 0x36:
        if(n<9)return;
        status=e[2];s->pairing_complete++;
        if(status)s->pairing_errors++;
        break;
    case 0x3c:if(n<9)return;value=e[8];break;
    case 0x3e:
        if(n<3)return;
        kind=QBT_LINK_LE_EVENT;code=e[2];
        if(code==1 || code==0x0a) {
            if(n<(code==1?21u:33u))return;
            status=e[3];s->le_connections++;
            if(status)s->le_connection_errors++;
            else {handle=metadata_handle(e+4);acl_forget(q,handle);value=e[6];}
        } else if(code==5) {if(n<15)return;handle=metadata_handle(e+3);}
        else if(code==3 || code==4 || code==0x0c) {
            if(n<(code==3?12u:code==4?14u:8u))return;
            status=e[3];handle=metadata_handle(e+4);
            if(code==0x0c)value=e[6]|(uint32_t)e[7]<<8;
        } else return;
        break;
    default:return;
    }
    }
    link_recent(q,kind,opcode,code,status,value,handle);
}
static void hci_recent(struct qbt *q,uint16_t opcode,uint8_t kind,uint8_t code,uint8_t status,uint8_t value) {
    struct qbt_hci_stats *s=&q->stats.hci;
    struct qbt_hci_recent *r=&s->recent[s->recent_next];
    memset(r,0,sizeof(*r));
    r->sequence=++s->recent_total;r->ns=now(q);r->opcode=opcode;
    r->kind=kind;r->code=code;r->status=status;r->value=value;
    s->recent_next=(s->recent_next+1)%QBT_HCI_RECENT_CAPACITY;
    if(s->recent_count<QBT_HCI_RECENT_CAPACITY)s->recent_count++;
}
static bool hci_scan_opcode(uint16_t opcode) {
    return opcode==0x200b || opcode==0x200c || opcode==0x2041 || opcode==0x2042;
}
static void hci_tx(struct qbt *q,const struct qbt_tx_frame *f) {
    if(!q->service || q->info.state!=2)return;
    uint16_t opcode=(uint16_t)(f->header[0]|(unsigned)f->header[1]<<8);
    struct qbt_hci_stats *s=&q->stats.hci;
    s->commands_tx++;
    if(opcode==0x0401)s->inquiry_tx++;
    else if(opcode==0x0402)s->inquiry_cancel_tx++;
    else if(opcode==0x200b || opcode==0x2041)s->le_scan_parameters_tx++;
    else if(opcode==0x200c || opcode==0x2042) {
        if(f->value==1)s->le_scan_enable_tx++;
        else if(f->value==0)s->le_scan_disable_tx++;
    }
    hci_recent(q,opcode,QBT_HCI_TX,1,0xff,f->value);
    if(link_opcode(opcode))link_recent(q,QBT_LINK_TX,opcode,1,0xff,f->link_value,
        f->link_handle<=0x0eff?f->link_handle:UINT16_MAX);
}
static void hci_rx(struct qbt *q,const uint8_t *e,unsigned n) {
    if(!q->service || q->info.state!=2)return;
    struct qbt_hci_stats *s=&q->stats.hci;
    if((e[0]==0x0e && n>=5) || (e[0]==0x0f && n>=6)) {
        bool cc=e[0]==0x0e,has_status=!cc || n>=6;
        uint16_t opcode=(uint16_t)(e[cc?3:4]|(unsigned)e[cc?4:5]<<8);
        uint8_t status=has_status?e[cc?5:2]:0xff,value=e[cc?2:3];
        if(cc)s->command_complete++;else s->command_status++;
        if(has_status && status)s->command_errors++;
        if(hci_scan_opcode(opcode)) {
            s->scan_responses++;
            if(has_status && status)s->scan_errors++;
        }
        hci_recent(q,opcode,cc?QBT_HCI_CC:QBT_HCI_CS,e[0],status,value);
        return;
    }
    if(e[0]==0x01 || e[0]==0x02 || e[0]==0x22 || e[0]==0x2f || e[0]==0x07) {
        uint8_t status=0xff,value=0xff;
        if(e[0]==0x01) {s->inquiry_complete++;if(n>=3)status=e[2];}
        else if(e[0]==0x07) {
            s->remote_name_completions++;
            if(n>=3) {status=e[2];if(status)s->remote_name_errors++;}
        } else {
            s->inquiry_result_events++;
            if(n>=3) {value=e[2];s->announced_devices+=value;}
        }
        hci_recent(q,0,QBT_HCI_EVENT,e[0],status,value);
    } else if(e[0]==0x3e && n>=3 && (e[2]==0x02 || e[2]==0x0d)) {
        uint8_t value=0xff;
        if(e[2]==0x02)s->legacy_report_events++;else s->extended_report_events++;
        if(n>=4) {value=e[3];s->announced_advertisements+=value;}
        hci_recent(q,0,QBT_HCI_LE_REPORT,e[2],0xff,value);
    }
}
/* Stop each DMA chunk at a packet boundary. IBS bytes may never be inserted
 * into the middle of a command, ACL, SCO or ISO frame spanning several slots. */
static size_t tx_advance(struct qbt_tx_frame *f,const uint8_t *p,size_t n,struct qbt *observe) {
    size_t i=0;
    while(i<n) {
        if(!f->type) {
            uint8_t t=p[i++];if(t!=1 && t!=2 && t!=3 && t!=5)return 0;
            f->type=t;f->have=0;f->need=t==1||t==3?3:4;f->value=0xff;
            f->link_handle=UINT16_MAX;f->link_value=UINT32_MAX;
        } else if(f->need) {
            f->header[f->have++]=p[i++];if(--f->need)continue;
            f->body=f->type==1||f->type==3?f->header[2]:
                (f->header[2]|(unsigned)f->header[3]<<8)&(f->type==5?0x3fff:0xffff);
            if(f->type==2 && observe)acl_begin(observe,&observe->tx_acl,f->header);
            if(!f->body){if(f->type==1 && observe)hci_tx(observe,f);f->type=0;return i;}
        } else {
            if(f->type==1 && f->body==f->header[2]) {
                uint16_t opcode=(uint16_t)(f->header[0]|(unsigned)f->header[1]<<8);
                if(opcode==0x2042 || opcode==0x200c || opcode==0x0c1a)f->value=p[i];
            }
            size_t take=n-i;if(take>f->body)take=f->body;
            if(f->type==1)link_tx_parameters(f,p+i,(unsigned)take);
            else if(f->type==2 && observe)acl_bytes(observe,&observe->tx_acl,p+i,(unsigned)take,true);
            i+=take;f->body-=(unsigned)take;
            if(!f->body){if(f->type==1 && observe)hci_tx(observe,f);f->type=0;return i;}
        }
    }
    return i;
}
static void event(struct qbt *q) {
    const uint8_t *e = q->event; unsigned n = q->event_length;
    link_hci_rx(q,e,n);
    hci_rx(q,e,n);
    /* Observe credits; the native stack still owns runtime HCI flow control. */
    if (e[0] == 0x0e && n >= 5) { q->stats.command_credit_events++; q->stats.command_credits = e[2]; }
    else if (e[0] == 0x0f && n >= 6) { q->stats.command_credit_events++; q->stats.command_credits = e[3]; }
    if (e[0] == 0x13 && n >= 3) {
        if (n != 3u + 4u * e[2]) q->stats.malformed++;
        else for (unsigned i = 0; i < e[2]; i++) q->stats.completed_packets += e[5 + 4 * i] | (unsigned)e[6 + 4 * i] << 8;
    }
    if (n >= 3 && e[0] == 0x10) { q->stats.hardware_errors++; q->stats.last_hardware_error = e[2]; }
    /* The speed change completes after the switch, at the new rate. */
    if (n >= 6 && e[0] == 0x0e && e[3] == 0x48 && e[4] == 0xfc) {
        q->speed_reply = true; q->speed_status = e[5];
    }
    else if (n >= 4 && e[0] == 0xff && e[2] == 0x92) {
        q->speed_reply = true; q->speed_status = e[3];
    }
    /* A nonzero TLV report describes an earlier segment; only status 0 answers a wait. */
    if (n >= 7 && e[0] == 0x0e && e[3] == 0x00 && e[4] == 0xfc && e[6] == 0x1e) {
        q->stats.tlv_acks++; q->stats.tlv_final_status = e[5];
        if (e[5]) return;
    }
    if (q->waiting && !q->complete) {
        bool match = false;
        if (!q->wait_event && n >= 5 && e[0] == 0x0e) {
            match = (e[3] | (unsigned)e[4] << 8) == q->wait_opcode;
            /* FC00 carries several unrelated replies under one opcode.
             * A delayed download reply must not answer a version/board query. */
            if (match && q->wait_subcommand) {
                /* Unknown/unsupported commands can have only an HCI status,
                 * without an EDL subcommand. Preserve that real failure. */
                match = (n==6 && e[5]!=0) || (n>=7 && e[6]==q->wait_subcommand);
            }
        } else if (q->wait_event && q->wait_event == e[0]) match = true;
        if (match) {
            /* Matching replies have independent storage from parser scratch. */
            memcpy(q->reply, e, n); q->reply_length = n; q->complete = true;
            return;
        }
        if (n >= 6 && e[0] == 0x0f && (e[4] | (unsigned)e[5] << 8) == q->wait_opcode && e[2]) {
            memcpy(q->reply, e, n); q->reply_length = n; q->complete = true;
            return;
        }
    }
    if (q->service && !q->drop) {
        q->forward[0] = 4; memcpy(q->forward + 1, e, n);
        q->forward_length = n + 1;
    } else q->stats.unmatched_events++;
    if (n >= 3 && e[0] == 0x10) qbt_fail(q, -EIO);
}
struct qbt *qbt_create(const struct qbt_platform *p) {
    if (!p || !p->allocate || !p->release || !p->now_ns || !p->uart_write ||
        !p->uart_drained || !p->uart_speed || !p->uart_flow || !p->clock ||
        !p->pump || !p->wait_until || !p->receive || !p->firmware_get || !p->firmware_put) return NULL;
    struct qbt *q = p->allocate(p->context, sizeof(*q));
    if (q) { memset(q, 0, sizeof(*q)); q->ops = *p; q->clock_on = true; q->info.baud = 115200; }
    return q;
}
void qbt_destroy(struct qbt *q) {
    if (!q) return;
    struct qbt_platform p = q->ops; memset(q, 0, sizeof(*q)); p.release(p.context, q, sizeof(*q));
}
size_t qbt_receive(struct qbt *q, const uint8_t *data, size_t length) {
    if (q->info.state == 3) return 0;
    size_t i = 0;
    while (i < length) {
        if (!flush(q)) break;
        if (!q->header_need && !q->body) {
            uint8_t b = data[i++];
            if (b == WAKE_IND) {
                int e = clock_on(q);
                if (e) { qbt_fail(q, e); break; }
                q->rx_awake = true; control(q, WAKE_ACK); q->stats.wake_ack++;
            } else if (b == WAKE_ACK) {
                if (q->tx_state == QBT_WAKING) { q->tx_state = QBT_AWAKE; q->retry_at = 0; q->idle_at = now(q) + QBT_TX_IDLE; }
            } else if (b == SLEEP_IND) q->rx_awake = false;
            else if (b >= 2 && b <= 5) {
                q->type = b; q->header_length = 0; q->header_need = b == 4 ? 2 : b == 3 ? 3 : 4;
                q->drop = !q->service;
            } else { q->stats.malformed++; i = length; }
            continue;
        }
        if (q->header_need) {
            q->header[q->header_length++] = data[i++];
            if (--q->header_need) continue;
            uint8_t t = q->type;
            q->body = t == 4 ? q->header[1] : t == 3 ? q->header[2] :
                (q->header[2] | (unsigned)q->header[3] << 8) & (t == 5 ? 0x3fff : 0xffff);
            q->drop = q->drop || !q->service || (t == 2 &&
                (q->header[0] | (unsigned)q->header[1] << 8) == 0x2edc);
            if(t==2 && !q->drop)acl_begin(q,&q->rx_acl,q->header);
            if (t == 4) { memcpy(q->event, q->header, 2); q->event_length = 2; }
            else if (!q->drop) { q->forward[0] = t; memcpy(q->forward + 1, q->header, q->header_length); q->forward_length = q->header_length + 1; }
            if (!q->body && t == 4) event(q);
            continue;
        }
        size_t n = length - i;
        if (n > q->body) n = q->body;
        if (q->type == 4) {
            if (n > sizeof(q->event) - q->event_length) { q->stats.malformed++; break; }
            memcpy(q->event + q->event_length, data + i, n); q->event_length += (unsigned)n;
        } else if (!q->drop) {
            if (n > sizeof(q->forward)) n = sizeof(q->forward);
            if(q->type==2)acl_bytes(q,&q->rx_acl,data+i,(unsigned)n,false);
            memcpy(q->forward, data + i, n); q->forward_length = (unsigned)n;
        }
        i += n; q->body -= (unsigned)n;
        if (!q->body && q->type == 4) event(q);
    }
    q->stats.rx_bytes += i; (void)flush(q); return i;
}
int qbt_transmit(struct qbt *q, const uint8_t *p, size_t n) {
    if (!q->service || q->info.state != 2) return -ENETDOWN;
    if (!n || !p || n > QBT_TX_CAPACITY - q->tx_length) { q->stats.tx_rejected++; return -ENOBUFS; }
    unsigned tail = (q->tx_head + q->tx_length) % QBT_TX_CAPACITY;
    size_t first = QBT_TX_CAPACITY - tail; if (first > n) first = n;
    memcpy(q->tx + tail, p, first); memcpy(q->tx, p + first, n - first);
    q->tx_length += (unsigned)n; q->stats.host_bytes += n; return 0;
}
size_t qbt_transmit_capacity(const struct qbt *q) {
    return q->service && q->info.state==2?QBT_TX_CAPACITY-q->tx_length:0;
}
void qbt_step(struct qbt *q) {
    if (q->info.state == 3) return;
    (void)flush(q);
    uint64_t t = now(q); bool pending = q->command_length || (q->service && q->tx_length);
    if (pending) {
        int e = clock_on(q);
        if (e) { qbt_fail(q, e); return; }
    }
    if (q->ibs && pending && q->tx_state == QBT_ASLEEP) {
        q->tx_state = QBT_WAKING; q->wake_attempts = 1; control(q, WAKE_IND);
        q->retry_at = t + QBT_WAKE_RETRY; q->stats.wake_ind++;
    } else if (q->ibs && q->tx_state == QBT_WAKING && t >= q->retry_at) {
        if (++q->wake_attempts > 10) {
            acl_clear(q);
            q->info.error = -ETIMEDOUT; q->info.state = 3; q->service = false;
            q->retry_at = 0; q->stats.timeouts++; return;
        }
        control(q, WAKE_IND); q->retry_at = t + QBT_WAKE_RETRY; q->stats.wake_retries++;
    }
    if (q->control_length && q->clock_on && !q->tx_frame.type) {
        size_t n = q->ops.uart_write(q->ops.context, q->control, q->control_length);
        if (n > q->control_length) n = q->control_length;
        memmove(q->control, q->control + n, q->control_length - n); q->control_length -= (unsigned)n;
        if (q->control_length) return;
    }
    if (!q->ibs || q->tx_state == QBT_AWAKE) {
        const uint8_t *p = NULL; size_t n = 0; bool cmd = q->command_length != 0;
        if (cmd) { p = q->command + q->command_offset; n = q->command_length - q->command_offset; }
        else if (q->service && q->tx_length) { p = q->tx + q->tx_head; n = QBT_TX_CAPACITY - q->tx_head; if (n > q->tx_length) n = q->tx_length; }
        if (n) {
            if (n > QBT_CHUNK) n = QBT_CHUNK;
            struct qbt_tx_frame preview=q->tx_frame;
            n=tx_advance(&preview,p,n,NULL);
            if(!n){acl_clear(q);q->info.error=-EPROTO;q->info.state=3;q->service=false;return;}
            size_t used = q->ops.uart_write(q->ops.context, p, n); if (used > n) used = n;
            /* Only the actually consumed bytes commit observations. The
             * packet-boundary preview above has no metadata side effects. */
            if(used)(void)tx_advance(&q->tx_frame,p,used,q);
            q->stats.tx_bytes += used;
            if (cmd) { q->command_offset += (unsigned)used; if (q->command_offset == q->command_length) q->command_offset = q->command_length = 0; }
            else { q->tx_head = (q->tx_head + (unsigned)used) % QBT_TX_CAPACITY; q->tx_length -= (unsigned)used; }
            if (used) q->idle_at = t + QBT_TX_IDLE;
        }
        if (q->ibs && !q->command_length && !q->tx_length && !q->control_length &&
            !q->tx_frame.type && q->tx_state == QBT_AWAKE && q->idle_at && t >= q->idle_at && drained(q)) {
            control(q, SLEEP_IND); q->tx_state = QBT_ASLEEP; q->idle_at = 0; q->stats.sleep_ind++;
        }
    }
    if (q->ibs && q->tx_state == QBT_ASLEEP && !q->rx_awake && !q->tx_length &&
        !q->command_length && !q->control_length && !q->tx_frame.type && q->clock_on && drained(q)) {
        if (!q->ops.clock(q->ops.context, false)) { q->clock_on = false; q->stats.clock_sleeps++; }
    }
}
uint64_t qbt_deadline(const struct qbt *q) {
    if (q->info.state == 3) return UINT64_MAX;
    bool idle = q->ops.uart_drained(q->ops.context);
    if (q->control_length && !q->tx_frame.type && idle) return 0;
    if ((q->command_length || (q->service && q->tx_length)) && idle && q->tx_state != QBT_WAKING) return 0;
    if (q->tx_state == QBT_WAKING && q->retry_at) return q->retry_at;
    if (q->ibs && !q->tx_frame.type && q->tx_state == QBT_AWAKE && q->idle_at && idle) return q->idle_at;
    return UINT64_MAX;
}
void qbt_set_service(struct qbt *q, bool enabled) {
    q->service = enabled && q->info.state == 2;
    if (!q->service) {
        acl_clear(q);
        if(q->tx_frame.type){q->info.state=3;q->info.error=-ECANCELED;}
        q->drop = true;
        q->tx_head = q->tx_length = 0; q->forward_length = q->forward_offset = 0;
    }
}
void qbt_rx_wake(struct qbt *q) {
    if (q->info.state == 3) return;
    int e = clock_on(q);
    if (e) qbt_fail(q, e);
}
void qbt_fail(struct qbt *q,int error) {
    acl_clear(q);
    q->info.state=3;q->info.error=error<0?error:-EIO;q->service=false;
    q->retry_at=q->idle_at=0;
}
static int run(struct qbt *q, uint64_t deadline, unsigned predicate) {
    for (;;) {
        q->ops.pump(q->ops.context); qbt_step(q);
        if (q->info.error) return q->info.error;
        if ((predicate == 0 && !q->command_length) || (predicate == 1 && q->complete) ||
            (predicate == 2 && !q->command_length && drained(q))) return 0;
        uint64_t t = now(q); if (t >= deadline) { q->stats.timeouts++; return -ETIMEDOUT; }
        uint64_t next = qbt_deadline(q);
        /* Wire drain has no IRQ of its own: bytes waiting on it re-check here. */
        if (predicate == 2 || q->command_length || q->control_length) {
            if (!next || next > t + QBT_TX_POLL) next = t + QBT_TX_POLL;
        }
        if (!next || next > deadline) next = deadline;
        /* UART completion and ring-space producers signal this wait. */
        q->ops.wait_until(q->ops.context, next);
    }
}
int qbt_send_command(struct qbt *q, uint16_t op, const void *data, unsigned n) {
    if (n > 255 || (n && !data)) return -EINVAL;
    uint64_t deadline = now(q) + QBT_COMMAND_TIMEOUT;
    int e = run(q, deadline, 0); if (e) return e;
    q->command[0] = 1; q->command[1] = (uint8_t)op; q->command[2] = op >> 8; q->command[3] = (uint8_t)n;
    if (n) memcpy(q->command + 4, data, n);
    q->command_length = n + 4; q->command_offset = 0; q->stats.commands++;
    if (op == 0xfc00 && n && ((const uint8_t *)data)[0] == 0x1e) q->stats.tlv_sent++;
    return run(q, deadline, 0);
}
int qbt_command(struct qbt *q, uint16_t op, const void *data, unsigned n, uint8_t ev,
                uint8_t *reply, size_t *length) {
    if (q->waiting) return -EBUSY;
    q->wait_opcode = op; q->wait_event = ev; q->waiting = true; q->complete = false; q->reply_length = 0;
    q->wait_subcommand = op == 0xfc00 && n && data ? ((const uint8_t *)data)[0] : 0;
    uint64_t deadline = now(q) + QBT_COMMAND_TIMEOUT;
    int e = qbt_send_command(q, op, data, n);
    if (!e) e = run(q, deadline, 1);
    q->waiting = false;
    if (e) return e;
    unsigned skip = q->reply[0] == 0x0e ? 5 : 2;
    if (q->reply[0] == 0x0f) return -EIO;
    if (q->reply_length < skip || *length < q->reply_length - skip) return -EMSGSIZE;
    *length = q->reply_length - skip; memcpy(reply, q->reply + skip, *length); return 0;
}
int qbt_drain(struct qbt *q, uint64_t deadline) { return run(q, deadline, 2); }
void qbt_delay(struct qbt *q, unsigned ms) {
    uint64_t end = now(q) + (uint64_t)ms * 1000000;
    while (now(q) < end) {
        q->ops.pump(q->ops.context); qbt_step(q);
        if (q->info.error) break;
        q->ops.wait_until(q->ops.context, end);
    }
}
void qbt_get_info(const struct qbt *q, struct qbt_info *out) { *out = q->info; }
void qbt_get_stats(const struct qbt *q, struct qbt_stats *out) { *out = q->stats; }
void qbt_linux_log(struct qbt *q, int level, const char *format, ...) {
    if (!q->ops.log) return;
    char text[160]; va_list ap; va_start(ap, format); vsnprintf(text, sizeof(text), format, ap); va_end(ap);
    q->ops.log(q->ops.context, level, text);
}
