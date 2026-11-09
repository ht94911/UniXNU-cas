/* SPDX-License-Identifier: BSD-2-Clause-Patent */
/* Read-only VFS and SHA gate reuse sheng BluetoothFirmware.c and nezha
 * NezhaWifiFirmware.c. The controller still authenticates its signed patch. */
#include "NezhaBluetoothFirmware.h"
#include "qbt_firmware.h"
#include <sys/param.h>
#include <sys/systm.h>
#include <sys/proc.h>
#include <sys/errno.h>
#include <sys/fcntl.h>
#include <sys/vnode.h>
#include <sys/uio.h>
#include <sys/kauth.h>
#include <IOKit/IOLib.h>
#include <pexpert/device_tree.h>
#include <corecrypto/ccdigest.h>
#include <corecrypto/ccsha2.h>
static int property(DTEntry chosen, const char *name, void *out, unsigned size) {
    const void *data; unsigned bytes = 0;
    if (SecureDTGetProperty(chosen, name, &data, &bytes) != kSuccess || bytes != size) return -ENOENT;
    memcpy(out, data, size); return 0;
}
static int filename(DTEntry chosen, const char *key, char out[60]) {
    const void *data; unsigned bytes = 0;
    if (SecureDTGetProperty(chosen, key, &data, &bytes) != kSuccess || bytes < 2 || bytes > 60) return -ENOENT;
    const char *p = data;
    if (p[bytes - 1] || p[0] == '.' || memchr(p, 0, bytes - 1)) return -EINVAL;
    for (unsigned i = 0; i + 1 < bytes; i++) if ((p[i] == '.' && p[i + 1] == '.') ||
        !((p[i] >= 'a' && p[i] <= 'z') ||
        (p[i] >= '0' && p[i] <= '9') || p[i] == '.' || p[i] == '-' || p[i] == '_')) return -EINVAL;
    memcpy(out, p, bytes); return 0;
}
static int digest_valid(const uint8_t digest[32]) {
    unsigned nonzero=0;for(unsigned j=0;j<32;j++)nonzero|=digest[j];return nonzero!=0;
}
static int nvm_catalog(DTEntry chosen,const char *key,const char *prefix,
    struct nezha_bt_nvm_entry entries[NEZHA_BT_NVM_RECORDS],uint32_t *count) {
    const void *catalog;unsigned bytes=0;
    _Static_assert(sizeof(struct nezha_bt_nvm_entry)==92,"NVM manifest wire entry");
    if(SecureDTGetProperty(chosen,key,&catalog,&bytes)!=kSuccess || !bytes ||
        bytes%sizeof(entries[0]) || bytes>NEZHA_BT_NVM_RECORDS*sizeof(entries[0]))return -EINVAL;
    memcpy(entries,catalog,bytes);*count=bytes/sizeof(entries[0]);
    size_t start=strlen(prefix);
    for(unsigned i=0;i<*count;i++) {
        const struct nezha_bt_nvm_entry *entry=&entries[i];
        size_t length=strnlen(entry->name,sizeof(entry->name));
        if(length==sizeof(entry->name) || length<=start+2 || strncmp(entry->name,prefix,start))return -EINVAL;
        /* Linux selects this exact stock filename for a successful board0
         * reply. Board still rejects .bin for nonzero/unknown board IDs. */
        bool zero_board=length==start+4 && !strcmp(entry->name+start,".bin");
        if(!zero_board) {
            if(entry->name[start]!='.' || entry->name[start+1]!='b' || length-start-2>4)return -EINVAL;
            for(size_t j=start+2;j<length;j++)if(!((entry->name[j]>='0' && entry->name[j]<='9') ||
                (entry->name[j]>='a' && entry->name[j]<='f')))return -EINVAL;
        }
        if(!digest_valid(entry->digest))return -EINVAL;
        for(unsigned j=0;j<i;j++)if(!strcmp(entry->name,entries[j].name))return -EINVAL;
    }
    return 0;
}
int nezha_bt_inputs_read(struct nezha_bt_inputs *in) {
    if (!in) return -EINVAL;
    memset(in, 0, sizeof(*in));
    DTEntry chosen; uint32_t version = 0, method = 0;
    if (SecureDTLookupEntry(NULL, "/chosen", &chosen) != kSuccess) return -ENOENT;
    int e = property(chosen, "nezha-bt-input-version", &version, 4);
    if (!e && version != 2 && version != 3) e = -EINVAL;
    if (!e) e = filename(chosen, "nezha-bt-patch-name", in->patch);
    if (!e) e = filename(chosen, "nezha-bt-nvm-prefix", in->nvm);
    if (!e) e = property(chosen, "nezha-bt-patch-sha256", in->patch_digest, sizeof(in->patch_digest));
    if (!e) e = property(chosen, "mac-address-bluetooth0", in->address, sizeof(in->address));
    if (!e) e = property(chosen, "nezha-bt-initial-baud", &in->boot.initial_baud, 4);
    if (!e) e = property(chosen, "nezha-bt-operating-baud", &in->boot.operating_baud, 4);
    if (!e) e = property(chosen, "nezha-bt-baud-method", &method, 4);
    if (!e) e = property(chosen, "nezha-bt-product", &in->boot.expected_product, 4);
    if (!e) e = property(chosen, "nezha-bt-soc", &in->boot.expected_soc, 4);
    uint32_t rom = 0;
    if (!e) e = property(chosen, "nezha-bt-rom", &rom, 4);
    if (e) return e;
    if (rom > 65535 || method > QBT_BAUD_PERI ||
        (version==2 && method==QBT_BAUD_PERI) || (version==3 && method!=QBT_BAUD_PERI) || in->boot.expected_product != 0x1e ||
        !rom || (in->address[0] & 1) ||
        !(in->address[0] | in->address[1] | in->address[2] | in->address[3] | in->address[4] | in->address[5])) return -EINVAL;
    if(!digest_valid(in->patch_digest))return -EINVAL;
    e=nvm_catalog(chosen,"nezha-bt-nvm-manifest",in->nvm,in->nvm_entries,&in->nvm_count);
    if(e)return e;
    if(version==3) {
        uint32_t config=0;
        e=filename(chosen,"nezha-bt-peri-patch-name",in->ganges.patch);
        if(!e)e=filename(chosen,"nezha-bt-peri-nvm-prefix",in->ganges.nvm);
        if(!e)e=filename(chosen,"nezha-bt-tme-patch-name",in->ganges.tme);
        if(!e)e=property(chosen,"nezha-bt-peri-patch-sha256",in->peri_patch_digest,32);
        if(!e)e=property(chosen,"nezha-bt-tme-patch-sha256",in->tme_patch_digest,32);
        if(!e)e=property(chosen,"nezha-bt-peri-elf-config",&config,4);
        if(!e)e=property(chosen,"nezha-bt-peri-elf-value",&in->ganges.elf_value,4);
        if(!e)e=nvm_catalog(chosen,"nezha-bt-peri-nvm-manifest",in->ganges.nvm,in->peri_nvm_entries,&in->peri_nvm_count);
        if(e)return e;
        if(!digest_valid(in->peri_patch_digest) || !digest_valid(in->tme_patch_digest) ||
            (config!=QBT_PERI_ELF_NO_VALUE && config!=QBT_PERI_ELF_EXPLICIT) ||
            (config==QBT_PERI_ELF_NO_VALUE && in->ganges.elf_value))return -EINVAL;
        in->ganges.elf_config=(enum qbt_peri_elf_config)config;in->ganges.enabled=true;
    }
    in->boot.controller = QBT_WCN7860; in->boot.baud_method = (enum qbt_baud_method)method;
    in->boot.expected_rom = (uint16_t)rom; in->boot.rampatch = in->patch; in->boot.nvm = in->nvm;
    /* The locked Linux selector reads the actual board ID after the patch.
     * Only that selected, hash-locked NVM can load. Unknown SoC is observed by
     * the version query rather than filled with an inferred ID. */
    in->boot.nvm_by_board = true;
    return 0;
}
int nezha_bt_firmware_read(void *context, const char *name, const uint8_t **out, size_t *size) {
    if (!context || !name || !out || !size) return -EINVAL;
    const struct nezha_bt_inputs *in = context;
    *out = NULL; *size = 0;
    if (strncmp(name, "qca/", 4)) return -ENOENT;
    bool patch = !strcmp(name+4,in->patch);
    bool peri_patch=in->ganges.enabled && !strcmp(name+4,in->ganges.patch);
    bool tme_patch=in->ganges.enabled && !strcmp(name+4,in->ganges.tme);
    const uint8_t *expected=patch?in->patch_digest:NULL;
    if(peri_patch)expected=in->peri_patch_digest;
    if(tme_patch)expected=in->tme_patch_digest;
    if(!expected)for(unsigned i=0;i<in->nvm_count;i++)
        if(!strcmp(name+4,in->nvm_entries[i].name)){expected=in->nvm_entries[i].digest;break;}
    if(!expected && in->ganges.enabled)for(unsigned i=0;i<in->peri_nvm_count;i++)
        if(!strcmp(name+4,in->peri_nvm_entries[i].name)){expected=in->peri_nvm_entries[i].digest;break;}
    if(!expected)return -ENOENT; /* includes Linux's board-to-generic NVM retry */
    char path[160];
    if (snprintf(path, sizeof(path), "%s/%s", NEZHA_BT_FIRMWARE_ROOT, name + 4) >= (int)sizeof(path)) return -EINVAL;
    vfs_context_t ctx = vfs_context_kernel(); vnode_t file = NULLVP;
    int error = vnode_open(path, FREAD | O_NOFOLLOW, 0, 0, &file, ctx);
    if (error) return -error;
    struct vnode_attr va; VATTR_INIT(&va); VATTR_WANTED(&va, va_data_size);
    error = vnode_getattr(file, &va, ctx);
    uint8_t *bytes = NULL; size_t length = 0;
    if (!error && (!VATTR_IS_SUPPORTED(&va, va_data_size) || vnode_vtype(file) != VREG ||
        !va.va_data_size || va.va_data_size > 4u * 1024 * 1024)) error = EINVAL;
    if (!error) {
        length = (size_t)va.va_data_size; bytes = IOMallocData(length);
        if (!bytes) error = ENOMEM;
    }
    if (!error) {
        int residual = (int)length;
        error = vn_rdwr(UIO_READ, file, (caddr_t)bytes, (int)length, 0, UIO_SYSSPACE, IO_NOAUTH,
                        vfs_context_ucred(ctx), &residual, vfs_context_proc(ctx));
        if (!error && residual) error = EIO;
    }
    int close_error = vnode_close(file, FREAD, ctx);
    if (!error) error = close_error;
    if (!error) {
        uint8_t digest[32]; ccdigest(ccsha256_di(), length, bytes, digest);
        if (memcmp(digest, expected, sizeof(digest))) error = EAUTH;
    }
    if (!error) {
        const char *dot = strrchr(name+4, '.');
        enum qbt_firmware_kind kind = !(patch || peri_patch || tme_patch) ? QBT_FW_NVM :
            dot && (!strcmp(dot, ".mbn") || !strcmp(dot, ".elf")) ? QBT_FW_ELF_PATCH : QBT_FW_TLV_PATCH;
        int e = qbt_firmware_validate(kind, bytes, length); if (e) error = -e;
    }
    if (error) { if (bytes) IOFreeData(bytes, length); return -error; }
    *out = bytes; *size = length; return 0;
}
void nezha_bt_firmware_release(void *context, const uint8_t *data, size_t size) {
    (void)context; if (data) IOFreeData((void *)data, size);
}
