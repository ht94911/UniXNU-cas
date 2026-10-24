"""Execute the actual EFI permission preparation function with fault-injected protocols.

Synthetic host evidence only: no page tables or phone memory are accessed.
"""
import ctypes as c
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
UEFI = Path(os.environ.get('CAS_UEFI_ROOT', ROOT.parent / 'uefi-cas'))

HARNESS = r'''
#include <stdint.h>
#include <stddef.h>
#define STATIC static
#define CONST const
#define VOID void
#define TRUE 1
#define FALSE 0
typedef int BOOLEAN;
typedef uint64_t UINT64;
typedef uint64_t EFI_STATUS;
#define EFI_ERROR(s) (((s) >> 63) != 0)
#define ERROR (1ULL << 63)
#define EFI_MEMORY_RO 0x20000ULL
#define EFI_MEMORY_XP 0x4000ULL
#define EFI_MEMORY_RP 0x2000ULL
#define CAS_SMEM_BASE 0x80900000ULL
#define CAS_SMEM_SIZE 0x200000ULL
typedef struct Protocol Protocol;
struct Protocol {
  EFI_STATUS (*GetMemoryAttributes)(Protocol *, UINT64, UINT64, UINT64 *);
  EFI_STATUS (*SetMemoryAttributes)(Protocol *, UINT64, UINT64, UINT64);
};
typedef Protocol EFI_MEMORY_ATTRIBUTE_PROTOCOL;
static int mSmemMapped;
static unsigned mComplete;
static int gEfiMemoryAttributeProtocolGuid;
static unsigned mode, gets, sets, persists, events, bad;
static UINT64 before, after;
static void Line(const char *format, ...) {(void)format;}
static void Persist(void) {persists++; events=events*10+2;}
static EFI_STATUS Get(Protocol *p, UINT64 base, UINT64 size, UINT64 *out) {
  (void)p; gets++; events=events*10+1;
  if(base!=CAS_SMEM_BASE || size!=CAS_SMEM_SIZE)bad++;
  *out=gets==1 ? before : after;
  return ((mode&4) && gets==1) || ((mode&16) && gets==2) ? ERROR : 0;
}
static EFI_STATUS Set(Protocol *p, UINT64 base, UINT64 size, UINT64 attrs) {
  (void)p; sets++; events=events*10+3;
  if(base!=CAS_SMEM_BASE || size!=CAS_SMEM_SIZE || attrs!=(EFI_MEMORY_RO|EFI_MEMORY_XP) || persists!=1)bad++;
  return mode&8 ? ERROR : 0;
}
static Protocol protocol;
static EFI_STATUS Locate(void *guid, void *registration, void **out) {
  (void)guid; (void)registration;
  *out=mode&2 ? NULL : &protocol;
  return mode&1 ? ERROR : 0;
}
static struct {EFI_STATUS (*LocateProtocol)(void *, void *, void **);} services={Locate};
static __typeof__(services) *gBS=&services;
'''

WRAPPER = r'''
int run(int provider, int mapped, unsigned complete, unsigned failures,
        UINT64 first, UINT64 second) {
  mSmemMapped=mapped; mComplete=complete; mode=failures;
  gets=sets=persists=events=bad=0; before=first; after=second;
  protocol.GetMemoryAttributes=mode&32 ? NULL : Get;
  protocol.SetMemoryAttributes=mode&64 ? NULL : Set;
  return PrepareSmemPermissions(provider);
}
unsigned counts(unsigned which) {
  const unsigned values[]={gets,sets,persists,events,bad};
  return values[which];
}
'''


class SmemPermissions(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temp = tempfile.TemporaryDirectory(prefix='cas-smem-permissions-')
        cls.addClassCleanup(cls.temp.cleanup)
        cls.source = (UEFI / 'Platforms/Xiaomi/casPkg/Applications/CasDiag/CasDiag.c').read_text()
        function = cls.source.split('STATIC BOOLEAN PrepareSmemPermissions(', 1)[1].split('STATIC BOOLEAN RamFacts(', 1)[0]
        code = Path(cls.temp.name) / 'permissions.c'
        code.write_text(HARNESS + 'STATIC BOOLEAN PrepareSmemPermissions(' + function + WRAPPER)
        library = code.with_suffix('.so')
        compiler = shutil.which('clang-18') or shutil.which('cc')
        if not compiler:
            raise RuntimeError('actual C compiler required')
        subprocess.run([compiler, '-std=c11', '-shared', '-fPIC', '-O2', '-Wall', '-Wextra', '-Werror', str(code), '-o', str(library)], check=True, capture_output=True)
        cls.lib = c.CDLL(str(library))
        cls.lib.run.argtypes = [c.c_int, c.c_int, c.c_uint, c.c_uint, c.c_uint64, c.c_uint64]
        cls.lib.run.restype = c.c_int
        cls.lib.counts.argtypes = [c.c_uint]
        cls.lib.counts.restype = c.c_uint

    def run_gate(self, provider=1, mapped=1, complete=13, failures=0, before=0x4000, after=0x24000):
        result = self.lib.run(provider, mapped, complete, failures, before, after)
        counts = tuple(self.lib.counts(i) for i in range(5))
        self.assertEqual(counts[-1], 0, 'range/permission/order invariant violated')
        return result, counts[:4]

    def test_observed_005_xp_only_gets_ro_and_verified_readback(self):
        self.assertEqual(self.run_gate(), (1, (2, 1, 1, 1231)))

    def test_only_adds_protection_never_clears_and_range_is_exact(self):
        for before in (0, 0x4000, 0x20000):
            with self.subTest(before=before):
                self.assertEqual(self.run_gate(before=before), (1, (2, 1, 1, 1231)))

    def test_already_protected_is_idempotent_but_still_requeried(self):
        self.assertEqual(self.run_gate(before=0x24000), (1, (2, 0, 0, 11)))
        self.assertEqual(self.run_gate(before=0x24000, failures=64), (1, (2, 0, 0, 11)))

    def test_invalid_provider_or_incomplete_map_never_even_queries(self):
        for args in ({'provider': 0}, {'mapped': 0}, {'complete': 0}, {'complete': 1}, {'complete': 4}, {'complete': 12}):
            with self.subTest(args=args):
                self.assertEqual(self.run_gate(**args), (0, (0, 0, 0, 0)))

    def test_absent_or_failed_protocol_or_get_callback_refused(self):
        for failure in (1, 2, 32):
            self.assertEqual(self.run_gate(failures=failure), (0, (0, 0, 0, 0)))

    def test_initial_query_error_refuses_even_with_good_output(self):
        self.assertEqual(self.run_gate(failures=4, before=0x24000), (0, (1, 0, 0, 1)))

    def test_rp_and_unexpected_attributes_never_relaxed(self):
        for before in (0x2000, 0x26000, 0x8000000000024000, 0x24001):
            self.assertEqual(self.run_gate(before=before), (0, (1, 0, 0, 1)))

    def test_missing_setter_refuses_unprotected_region(self):
        self.assertEqual(self.run_gate(failures=64), (0, (1, 0, 0, 1)))

    def test_failed_setter_has_persisted_intent_and_never_passes(self):
        self.assertEqual(self.run_gate(failures=8), (0, (1, 1, 1, 123)))

    def test_set_success_with_wrong_live_permissions_refused(self):
        for after in (0, 0x4000, 0x20000, 0x26000, 0x24001):
            self.assertEqual(self.run_gate(after=after), (0, (2, 1, 1, 1231)))

    def test_readback_error_refuses_even_if_output_looks_good(self):
        self.assertEqual(self.run_gate(failures=16), (0, (2, 1, 1, 1231)))

    def test_already_protected_path_rejects_changed_or_failed_readback(self):
        self.assertEqual(self.run_gate(before=0x24000, after=0x4000), (0, (2, 0, 0, 11)))
        self.assertEqual(self.run_gate(before=0x24000, failures=16), (0, (2, 0, 0, 11)))

    def test_runtime_smem_read_still_after_all_gates(self):
        body = self.source.split('STATIC BOOLEAN RamFacts(VOID)', 1)[1].split('STATIC BOOLEAN GopFacts', 1)[0]
        self.assertLess(body.index('PrepareSmemPermissions(Provider)'), body.index('if(!Provider || !mSmemMapped || !PermissionsGood)return FALSE;'))
        self.assertLess(body.index('if(!Provider || !mSmemMapped || !PermissionsGood)return FALSE;'), body.index('CasDiagSmemRead('))
        helper = self.source.split('STATIC BOOLEAN PrepareSmemPermissions(', 1)[1].split('STATIC BOOLEAN RamFacts(', 1)[0]
        for forbidden in ('ClearMemoryAttributes', 'AddMemorySpace', 'AllocatePages', 'SmemRead(', 'volatile'):
            self.assertNotIn(forbidden, helper)


if __name__ == '__main__':
    unittest.main()
