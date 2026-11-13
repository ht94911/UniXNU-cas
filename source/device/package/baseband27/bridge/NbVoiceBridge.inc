/* SPDX-License-Identifier: BSD-2-Clause */
/* Included on the existing locked QMUX owner; no second transport/thread. */
static struct nb_voice bb_voice;
static void bb_voice_indicate(void)
{
  nb_voice_sync(&bb_voice, &bb_qmux);
  for (unsigned i = 0; i < CBQ_MAX_CLIENTS; ++i) {
    const struct cbq_client *c = &bb_qmux.client[i];
    if (!c->used || c->service != NB_AUDIO_SERVICE || c->channel >= CBQ_CHANNELS) continue;
    uint8_t tlvs[5];
    unsigned bytes = nb_voice_vocoder(&bb_voice, c->channel, tlvs, sizeof tlvs);
    if (!bytes) {
      /* An ended/rebound call must withdraw the previous speech format. */
      tlvs[0] = 1; cbd_wr16(tlvs + 1, 2); tlvs[3] = 0xff; tlvs[4] = 0; bytes = 5;
    }
    unsigned n = cbq_private_indication(NB_AUDIO_INFO, NB_AUDIO_SERVICE, c->cid, tlvs, bytes, bb_out, sizeof bb_out);
    if (n) bb_push(c->channel ? CBH_QMUX2 : CBH_QMUX1, bb_out, n);
  }
}
static void bb_voice_request(const struct cbq_result &r, const uint8_t *sdu)
{
  if (r.service != NB_VOICE_SERVICE) return;
  nb_voice_sync(&bb_voice, &bb_qmux);
  nb_voice_request(&bb_voice, r.local_port - CBQ_PORT_BASE, r.channel, r.cid, r.node, r.port, sdu, r.len);
}
static void bb_voice_answer(const struct cbq_result &r, uint32_t node, uint32_t port, const uint8_t *sdu, uint32_t size)
{
  if (r.service != NB_VOICE_SERVICE) return;
  nb_voice_sync(&bb_voice, &bb_qmux);
  if (nb_voice_message(&bb_voice, r.local_port - CBQ_PORT_BASE, r.channel, r.cid, node, port, sdu, size))
    bb_voice_indicate();
}
static int bb_audio_answer(uint32_t channel, const struct cbq_result &r, const uint8_t *sdu)
{
  if (r.service != NB_AUDIO_SERVICE || cbd_rd16(sdu + 3) != NB_AUDIO_QUERY) return 0;
  nb_voice_sync(&bb_voice, &bb_qmux);
  uint8_t tlvs[5];
  const bool valid = nb_voice_frame(sdu, r.len);
  unsigned bytes = valid ? nb_voice_vocoder(&bb_voice, r.channel, tlvs, sizeof tlvs) : 0;
  unsigned n = cbq_private_answer(sdu, r.service, r.cid, !valid ? CBQ_ERR_MALFORMED_MESSAGE : bytes ? 0 : CBQ_ERR_NOT_SUPPORTED,
                                 bytes ? tlvs : nullptr, bytes, bb_out, sizeof bb_out);
  if (n) bb_push(channel, bb_out, n);
  return 1;
}
