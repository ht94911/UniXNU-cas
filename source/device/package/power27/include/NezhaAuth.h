/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#ifndef NEZHA_AUTH_H
#define NEZHA_AUTH_H
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif

#define NP_AUTH_MATERIAL_BYTES 528u
/* Supplied by the local boot's device tree, never included in source delivery.
 * OS4 nezha selects FG key2 (C038), not either public fallback FG key. */
typedef struct {
    uint8_t magic[8], version[4], fg_selector[4];
    uint8_t seeds[10][16], adapter_keys[10][32], battery_key[32];
} NpAuthMaterial;
typedef struct {
    void *context;
    int (*random_bytes)(void *,uint8_t *,size_t);
    int (*hmac_sha256)(void *,const uint8_t *,size_t,const uint8_t *,size_t,uint8_t [32]);
    /* One volatile MAC challenge and fresh32B response; no retries or DF writes. */
    int (*battery_digest)(void *,const uint8_t [32],uint8_t [32]);
} NpAuthOps;
enum {
    NP_AUTH_IDLE, NP_AUTH_SVID, NP_AUTH_ID, NP_AUTH_PROCESS_CHECK, NP_AUTH_BATTERY,
    NP_AUTH_CLEAR_VERIFIED, NP_AUTH_VERIFY_CLEAR, NP_AUTH_PROCESS_START,
    NP_AUTH_VERIFY_START, NP_AUTH_COMMAND, NP_AUTH_POLL, NP_AUTH_RESPONSE,
    NP_AUTH_PUBLISH, NP_AUTH_VERIFY_PUBLISH, NP_AUTH_PROCESS_END,
    NP_AUTH_VERIFY_END, NP_AUTH_DONE, NP_AUTH_CLEAN_VERIFIED_READ,
    NP_AUTH_CLEAN_VERIFIED_WRITE, NP_AUTH_CLEAN_PROCESS_READ,
    NP_AUTH_CLEAN_PROCESS_WRITE, NP_AUTH_STOPPED, NP_AUTH_FAILED,
    NP_AUTH_ROLE_READ, NP_AUTH_ROLE_REQUEST, NP_AUTH_ROLE_WAIT
};
enum { NP_AUTH_NO_ERROR, NP_AUTH_UNAVAILABLE, NP_AUTH_SOURCE, NP_AUTH_PROTOCOL,
    NP_AUTH_BATTERY_ERROR, NP_AUTH_CRYPTO_ERROR, NP_AUTH_MISMATCH,
    NP_AUTH_TRANSPORT, NP_AUTH_TIMEOUT, NP_AUTH_CLEANUP_ERROR };
/* A completed host report is distinct from the possible firmware effect of
 * an accepted final VDM6 whose authentication has not completed yet. */
enum { NP_AUTH_OWNS_PROCESS=1, NP_AUTH_OWNS_VERIFIED=2,
       NP_AUTH_OWNS_VDM_EFFECT=4 };
typedef struct {
    uint64_t id,epoch,attachment,deadline_ms;
    uint32_t property;
    uint8_t write,write_bytes,reply_bytes;
    uint8_t data[16];
} NpAuthCommand;
typedef struct {
    uint64_t epoch,attachment,now_ms;
    uint32_t online,real_type,sample_valid,pd_ready;
} NpAuthFacts;
typedef struct {
    const NpAuthMaterial *material;
    NpAuthOps io;
    uint64_t epoch,attachment,last_epoch,last_attachment,next_id,poll_until,poll_at,verified_ms,discovery_until;
    uint32_t state,error,svid,adapter_id,owned,attempts,battery_checks,battery_error;
    uint32_t data_role,role_requests;
    uint8_t enabled,pending,sent,stop,battery_ok,hash_ok,verified,index,seed_index,battery_checked;
    uint8_t challenge[16],expected[32];
    uint64_t clear_deadline[2];
    uint64_t role_until;
    uint8_t clear_pending;
    NpAuthCommand command;
} NpAuth;
int np_auth_material_valid(const void *,size_t);
void np_auth_init(NpAuth *,const NpAuthMaterial *,const NpAuthOps *,int);
int np_auth_step(NpAuth *,const NpAuthFacts *,NpAuthCommand *);
int np_auth_sent(NpAuth *,uint64_t);
int np_auth_complete(NpAuth *,const NpAuthCommand *,int,const uint8_t *,size_t,uint64_t);
int np_auth_write_allowed(const NpAuth *,const NpAuthCommand *,uint64_t,const uint8_t *,size_t);
void np_auth_stop(NpAuth *);
int np_auth_resume(NpAuth *);
void np_auth_source(NpAuth *,uint64_t,uint64_t);
int np_auth_busy(const NpAuth *);
/* Native adapters use the already initialized XNU corecrypto/random APIs. */
int np_auth_native_material(NpAuthMaterial *);
int np_auth_native_random(void *,uint8_t *,size_t);
int np_auth_native_hmac(void *,const uint8_t *,size_t,const uint8_t *,size_t,uint8_t [32]);
#ifdef __cplusplus
}
#endif
#endif
