/* SPDX-License-Identifier: BSD-2-Clause */
/* Included in the existing locked bridge; identity data is never logged. */
static struct nb_device_identity bb_device_identity;
static uint8_t bb_act_known, bb_act_channel, bb_act_cid;
static void bb_activation_indicate(void)
{
  if (!bb_act_known) return;
  uint8_t tlvs[NB_SIM_ACTIVATION_BYTES + 2u * (3u + NB_DEVICE_IMEI_BYTES)];
  uint32_t t = nb_sim_activation(&bb_sim, tlvs, sizeof tlvs);
  if (!t) return;
  t += nb_device_activation(&bb_device_identity, tlvs + t, sizeof tlvs - t);
  const uint32_t n = cbq_private_indication(CBD_BSP_ACTIVATION_REGISTER, CBD_SVC_BSP, bb_act_cid, tlvs, t, bb_out, sizeof bb_out);
  if (n) bb_push(bb_act_channel, bb_out, n);
}
static void bb_sim_request(const struct cbq_result &r, const uint8_t *sdu)
{
  if (r.service == CBD_SVC_UIM) nb_sim_read_begin(&bb_sim, r.local_port, r.node, r.port, sdu, r.len);
  if (r.service == NB_DEVICE_DMS && r.local_port >= CBQ_PORT_BASE) {
    nb_device_sync(&bb_device_identity, &bb_qmux);
    nb_device_request(&bb_device_identity, r.local_port - CBQ_PORT_BASE,
      r.channel, r.cid, r.node, r.port, sdu, r.len);
  }
}
static void bb_sim_answer(const struct cbq_result &r, const uint8_t *sdu, uint32_t n)
{
  if (r.service == NB_DEVICE_DMS && r.local_port >= CBQ_PORT_BASE) {
    const auto *endpoint = cbq_client_server(&bb_qmux, r.local_port - CBQ_PORT_BASE);
    nb_device_sync(&bb_device_identity, &bb_qmux);
    if (endpoint && nb_device_answer(&bb_device_identity, r.local_port - CBQ_PORT_BASE,
        r.channel, r.cid, endpoint->node, endpoint->port, sdu, n))
      bb_activation_indicate();
    return;
  }
  if (r.service != CBD_SVC_UIM || r.local_port < CBQ_PORT_BASE ||
      r.local_port - CBQ_PORT_BASE >= CBQ_MAX_CLIENTS) return;
  const auto *endpoint = cbq_client_server(&bb_qmux, r.local_port - CBQ_PORT_BASE);
  if (!endpoint || !nb_sim_message(&bb_sim, r.local_port, endpoint->node, endpoint->port, sdu, n)) return;
  printf("COACHZ baseband: SIM identity state changed, generation %u; activation %s\n",
         bb_sim.generation, bb_act_known ? "updated" : "awaits registration");
  bb_activation_indicate();
}
