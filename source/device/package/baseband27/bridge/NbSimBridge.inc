/* SPDX-License-Identifier: BSD-2-Clause */
/* Included in the existing locked bridge; identity data is never logged. */
static uint8_t bb_act_known, bb_act_channel, bb_act_cid;
static void bb_activation_indicate(void)
{
  if (!bb_act_known) return;
  uint8_t tlvs[NB_SIM_ACTIVATION_BYTES];
  const uint32_t t = nb_sim_activation(&bb_sim, tlvs, sizeof tlvs);
  const uint32_t n = cbq_private_indication(CBD_BSP_ACTIVATION_REGISTER, CBD_SVC_BSP, bb_act_cid, tlvs, t, bb_out, sizeof bb_out);
  if (n) bb_push(bb_act_channel, bb_out, n);
}
static void bb_sim_request(const struct cbq_result &r, const uint8_t *sdu)
{
  if (r.service == CBD_SVC_UIM) nb_sim_read_begin(&bb_sim, r.local_port, r.node, r.port, sdu, r.len);
}
static void bb_sim_answer(const struct cbq_result &r, const uint8_t *sdu, uint32_t n)
{
  if (r.service != CBD_SVC_UIM || r.local_port < CBQ_PORT_BASE ||
      r.local_port - CBQ_PORT_BASE >= CBQ_MAX_CLIENTS) return;
  const auto *endpoint = cbq_client_server(&bb_qmux, r.local_port - CBQ_PORT_BASE);
  if (!endpoint || !nb_sim_message(&bb_sim, r.local_port, endpoint->node, endpoint->port, sdu, n)) return;
  printf("COACHZ baseband: SIM identity state changed, generation %u; activation %s\n",
         bb_sim.generation, bb_act_known ? "updated" : "awaits registration");
  bb_activation_indicate();
}
