/* SPDX-License-Identifier: MIT */
#ifndef QCOM_USB_DIAGNOSTIC_H
#define QCOM_USB_DIAGNOSTIC_H
#include <stdint.h>
#include <stddef.h>
#define QUD_DIAG_VERSION 2u
#define QUD_DIAG_BYTES 2048u
#define QUD_DIAG_CHUNK 128u
#define QUD_DIAG_PROVIDERS 8u
enum qud_diag_op { QUD_RESULT=0, QUD_CAPS=1, QUD_SNAPSHOT=2, QUD_HISTORY=3,
 QUD_CONFIG=4, QUD_BEGIN=5, QUD_END=6, QUD_EXPERIMENT_STATUS=7, QUD_SUBMIT=8, QUD_CANCEL=9 };
enum qud_diag_result { QUD_OK=0, QUD_UNSUPPORTED=1, QUD_BUSY=2, QUD_INVALID=3,
 QUD_FAILED=4, QUD_TIMEOUT=5, QUD_EXPIRED=6 };
struct qud_diag_request { uint32_t op, provider, arg0, arg1; uint64_t experiment, cursor; };
/* Lifetime: descriptor, name and cookie static until reboot. Registration is
 * one-way, before first use. No arbitrary pointer/address arrives over USB.
 * Called from normal boot-CPU worker OUTSIDE ALL USB guards, never IRQ/panic.
 * Return in <=1 ms; no allocation, logging, waiting or recursive USB calls.
 * Snapshot must copy consistent software state under the owner's brief lock,
 * never traverse unowned/volatile object pointers. Cap output at capacity.
 * submit only validates/enqueues to the subsystem's OWN work queue/state
 * machine. Report busy/failed/unsupported, never pretend hardware recovery.
 * Running work, cancellation and deadlines belong to that subsystem. */
struct qud_diag_provider {
 uint32_t id, version, operations, max_arg0, max_arg1;
 const char *name;
 void *cookie;
 int (*snapshot)(void *, char *, size_t, size_t *, unsigned *);
 int (*submit)(void *, const struct qud_diag_request *, char *, size_t, size_t *, unsigned *);
};
struct qud_diag_view {
 uint64_t ticks, experiment;
 uint32_t id, total, result, truncated, generation, pending, provider, provider_version;
 const char *data;
};
int qcom_usb_diag_register(const struct qud_diag_provider *provider);
/* Parser/transport only stages CPU jobs, no provider callback here. */
int qcom_usb_diag_enqueue(uint64_t session, uint32_t id, const struct qud_diag_request *request, uint32_t generation);
int qcom_usb_diag_result(uint64_t session, uint32_t id, uint32_t offset, struct qud_diag_view *view);
/* Exactly one job/tick. Called after qcom_usb_debug_poll() returns. */
void qcom_usb_diag_service(void);
uint64_t qcom_usb_debug_ticks(void);
#endif
