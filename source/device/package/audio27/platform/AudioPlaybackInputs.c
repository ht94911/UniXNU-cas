/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#include "AudioPlaybackInputs.h"
#include <string.h>
int na_playback_inputs(NaPlaybackInputs *out,const NaInputPack *pack,const NaTasSelection *selection) {
    if (!out || out->valid || !pack || !selection || !selection->profile ||
        !selection->rx_format || !selection->tx_format ||
        selection->revision_mask[0]!=0xff || selection->revision_mask[1]!=0xff) return NA_ARGUMENT;
    /* Recheck the view: callers cannot manufacture a trusted directory from
     * a partially parsed blob. This is CPU-only and bounded to 63 entries. */
    NaInputPack checked;int r=na_input_pack(&checked,pack->data,pack->bytes);if (r) return r;
    NaPlaybackInputs p={0};p.selection=*selection;p.selection.calibrated=0;
    const char *graphs[]={"speaker.graph","vi.graph"};
    for (unsigned i=0;i<2;++i) {
        r=na_input_find(&checked,graphs[i],&p.graph[i]);if (r) return r;
        if (p.graph[i].bytes>0x100000) return NA_WIRE;
        r=na_topology_resolve(p.graph[i].data,(uint32_t)p.graph[i].bytes,i ? 2:0,&p.media[i],&p.protection[i]);
        if (r) return r;
    }
    NaFirmwarePiece tuning;
    r=na_input_find(&checked,"tismartpa_driver_tuning.bin",&tuning);if (r) return r;
    if (tuning.bytes>0x100000) return NA_WIRE;
    r=na_tas_image(&p.tuning,tuning.data,(uint32_t)tuning.bytes);if (r) return r;
    const char *names[]={"ti_calr.bin","calib_re","calib_re_r","amb_temp","amb_temp_r"};
    NaCalibrationFile files[5];
    for (unsigned i=0;i<5;++i) {
        NaFirmwarePiece f;r=na_input_find(&checked,names[i],&f);if (r) return r;
        if (f.bytes>24) return NA_WIRE;
        files[i]=(NaCalibrationFile){f.data,(uint32_t)f.bytes};
    }
    r=na_ti_calibration_parse(&p.calibration,files[0],files+1,files+3,&p.tuning,selection->profile);
    if (r) return r;
    /* Resolve both exact serial-interface labels and safe gain controls now.
     * Endpoint initialization still validates every executable table later. */
    for (unsigned i=0;i<2;++i) {
        NaTasBytes program;NaTasControl gain;NaTasProgramInfo info;uint8_t bytes[17];
        r=na_tas_format(&p.tuning,i,0,selection->rx_format,&program);
        if (r || na_tas_validate(program,&info)) return r ? r:NA_WIRE;
        r=na_tas_format(&p.tuning,i,1,selection->tx_format,&program);
        if (r || na_tas_validate(program,&info)) return r ? r:NA_WIRE;
        r=na_tas_control(&p.tuning,i ? "DVC_GAIN_BOTTOM":"DVC_GAIN_TOP",&gain);if (r) return r;
        if (gain.channel!=i || gain.reg!=0x10c || gain.reg_type!=1 || selection->dvc_index[i]>gain.default_index) return NA_MISSING;
        r=na_tas_integer(&gain,selection->dvc_index[i],bytes,&program);if (r) return r;
    }
    p.selection.calibrated=1;p.valid=1;*out=p;return NA_OK;
}
int na_playback_plan(NaAssemblyPlan *p,const NaPlaybackInputs *in) {
    if (!p || !in || !in->valid || !in->calibration.valid || !in->selection.calibrated ||
        !in->protection[0] || !in->protection[1] || p->tuning || p->ti_instance ||
        p->stream[0].graph || p->stream[1].graph || p->stream[2].graph ||
        p->capture_proven || p->capture_probe_allowed) return NA_ARGUMENT;
    p->tuning=&in->tuning;p->tas=in->selection;p->calibration=in->calibration;p->ti_instance=in->protection[0];
    for (unsigned i=0;i<2;++i) {
        NaAssemblyStream *s=&p->stream[i ? 2:0];
        s->graph=in->graph[i].data;s->graph_bytes=(uint32_t)in->graph[i].bytes;s->media=in->media[i];
    }
    return NA_OK;
}
int na_capture_inputs(NaCaptureInputs *out,const NaInputPack *pack) {
    if (!out || out->valid || !pack) return NA_ARGUMENT;
    NaInputPack checked;int r=na_input_pack(&checked,pack->data,pack->bytes);if (r) return r;
    NaCaptureInputs in={0};uint32_t protection=0;
    r=na_input_find(&checked,"capture.graph",&in.graph);if (r) return r;
    if (in.graph.bytes>0x100000) return NA_WIRE;
    r=na_topology_resolve(in.graph.data,(uint32_t)in.graph.bytes,1,&in.media,&protection);
    if (r) return r;
    if (protection || in.media.capture!=1 || in.media.channels!=1) return NA_MISSING;
    in.valid=1;*out=in;return NA_OK;
}
int na_capture_plan(NaAssemblyPlan *p,const NaCaptureInputs *in) {
    if (!p || !in || !in->valid || p->stream[1].graph ||
        p->capture_proven || p->capture_probe_allowed || !p->stream[0].graph ||
        !p->stream[2].graph) return NA_ARGUMENT;
    p->stream[1].graph=in->graph.data;p->stream[1].graph_bytes=(uint32_t)in->graph.bytes;
    p->stream[1].media=in->media;p->capture_probe_allowed=1;return NA_OK;
}
