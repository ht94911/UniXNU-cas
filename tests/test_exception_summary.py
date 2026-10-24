"""Actual diagnostic C: first-PC context plus bounded numeric serial summary."""
import ctypes as c
import test_early_text as early

CONTEXT = (b'Loading driver at 0x9bb05000 EntryPoint=0x9bb061cc\n'
           b'ProtectUefiImage 0x9bb05000 size0x10000\n'
           b'ReplaceTableEntry: splitting block entry with MMU disabled\n'
           b'Synchronous Exception at 0x9bb07500\nPC 0x00009bb07500\n')
SP = b'  SP 0x000000009f93e900  ELR 0x000000009bb07500  SPSR 0x600002c5  FPSR 0x00000010\n'
ESR = b' ESR 0x9600000d          FAR 0x0000000080900000\n'
VALUES = [0x9f93e900, 0x9bb07500, 0x600002c5, 0x10, 0x9600000d, 0x80900000]


class Summary(early.unittest.TestCase):
    setUpClass = classmethod(early.EarlyText.setUpClass.__func__)
    tearDownClass = classmethod(early.EarlyText.tearDownClass.__func__)
    setUp = early.EarlyText.setUp
    feed = early.EarlyText.feed
    frame = early.EarlyText.frame

    def test_pinned_lines_at_every_chunk_split_keep_original_pc_and_loading_cells(self):
        raw = CONTEXT + b'X0 junk\n' * 12 + b'V0 junk\n' * 32 + SP + ESR
        for split in range(len(raw) + 1):
            self.lib.CasEarlyTextReset(c.byref(self.t))
            self.feed(CONTEXT)
            cells = bytes(self.t.cells)
            self.lib.CasEarlyTextReset(c.byref(self.t))
            self.feed(raw[:split]); self.feed(raw[split:])
            self.assertEqual(self.t.fault_mask, 0x8000003f)
            self.assertEqual(list(self.t.fault_values), VALUES)
            self.assertEqual(bytes(self.t.cells), cells)
            self.assertTrue(self.lib.CasEarlyTextValid(c.byref(self.t)))
        self.assertEqual(c.sizeof(early.Text), 1024)
        self.assertEqual(self.t.version, 4)

    def test_hex_requires_delimiter_and_rejects_overlong_bad_or_empty_values(self):
        for text in (b'  SP 0x\n', b'  SP 0x1G\n', b'  SP 0x' + b'f' * 17 + b'\n'):
            self.lib.CasEarlyTextReset(c.byref(self.t)); self.feed(CONTEXT + text)
            self.assertEqual(self.t.fault_mask, 0x80000000)
        self.lib.CasEarlyTextReset(c.byref(self.t))
        self.feed(CONTEXT + b'  SP 0x123')
        self.assertEqual(self.t.fault_mask, 0x80000000)
        self.feed(b'\n')
        self.assertEqual((self.t.fault_mask, self.t.fault_values[0]), (0x80000001, 0x123))
        self.feed(ESR)
        self.assertEqual(self.t.fault_mask, 0x80000031)

    def test_only_exact_source_prefixes_after_first_pc_can_start_summary(self):
        for prefix in (b'', b' ', b'   ', b'ordinary ', b'X' * 56):
            self.lib.CasEarlyTextReset(c.byref(self.t))
            self.feed(CONTEXT + prefix + b'SP 0x1  ELR 0x2\n')
            self.assertEqual(self.t.fault_mask, 0x80000000)
        self.lib.CasEarlyTextReset(c.byref(self.t)); self.feed(SP + ESR)
        self.assertEqual(self.t.fault_mask, 0)
        self.feed(b'Stack dump:\n'); saved = bytes(self.t)
        self.feed(SP + ESR)
        self.assertEqual(bytes(self.t), saved)

    def test_completed_values_never_change_on_later_exception_or_big_spam(self):
        self.feed(CONTEXT + SP.replace(b'\n', b'\r\n') + ESR)
        saved = bytes(self.t)
        self.feed(CONTEXT + SP.replace(b'9f93e900', b'ffffffff') + ESR + b'junk' * 10000)
        self.assertEqual(bytes(self.t), saved)

    def test_corrupt_summary_cursor_or_label_index_resets_before_access(self):
        for field, value in [('fault_state',5), ('fault_slot',6), ('fault_match',9),
                             ('fault_digits',17), ('fault_mask',0x20000000),
                             ('fault_exception_match',16)]:
            self.lib.CasEarlyTextReset(c.byref(self.t)); setattr(self.t, field, value)
            self.feed(b'recovered\n')
            self.assertTrue(self.lib.CasEarlyTextValid(c.byref(self.t)))
            self.assertEqual(self.t.fault_mask, 0)
        self.t.fault_state = 1; self.t.fault_slot = 0; self.t.fault_match = 5
        self.feed(b'recovered\n')
        self.assertEqual(self.t.fault_mask, 0)

    def test_nested_exception_cannot_pair_later_esr_with_first_pc(self):
        nested = b'Synchronous Exception at 0x9f000000\nPC 0x9f000000\n'
        for split in range(len(nested)+1):
            self.lib.CasEarlyTextReset(c.byref(self.t))
            self.feed(CONTEXT+SP)
            self.feed(nested[:split]); self.feed(nested[split:]+ESR)
            self.assertEqual(self.t.fault_mask,0xc000000f)
            self.assertEqual(list(self.t.fault_values)[4:],[0,0])
            self.assertIn(b'PC 0x00009bb07500',bytes(self.t.cells))

    def test_summary_draws_only_in_extra_band_and_leaves_frozen_pc_pixels_identical(self):
        self.feed(CONTEXT)
        frame = self.frame()
        self.assertEqual(self.lib.CasEarlyTextRender(c.byref(self.t),c.byref(frame)),0)
        before = bytes(self.mem)
        self.feed(SP + ESR)
        self.assertEqual(self.lib.CasEarlyTextRender(c.byref(self.t),c.byref(frame)),0)
        # At600 pixels: original text y136..519, new summary y28..123.
        for y in range(600):
            start = (32+y*1088)*4
            if not 28 <= y < 124:
                self.assertEqual(bytes(self.mem)[start:start+1088*4], before[start:start+1088*4])
        self.assertNotEqual(bytes(self.mem),before)
        self.assertEqual(list(self.mem[:32]),[0x12345678]*32)
        self.assertEqual(list(self.mem[-32:]),[0x12345678]*32)

    def test_reset_clears_old_summary_band_instead_of_showing_stale_fault(self):
        self.feed(CONTEXT+SP+ESR); frame = self.frame()
        self.assertEqual(self.lib.CasEarlyTextRender(c.byref(self.t),c.byref(frame)),0)
        self.lib.CasEarlyTextReset(c.byref(self.t))
        self.assertEqual(self.lib.CasEarlyTextRender(c.byref(self.t),c.byref(frame)),0)
        for y in range(28,124):
            self.assertEqual(list(self.mem[32+y*1088+12:32+y*1088+1020]),[0xff101820]*1008)


class SummarySerial(early.unittest.TestCase):
    setUpClass = classmethod(early.SerialIntegration.setUpClass.__func__)
    tearDownClass = classmethod(early.SerialIntegration.tearDownClass.__func__)
    setUp = early.SerialIntegration.setUp
    write = early.SerialIntegration.write

    def test_refresh_only_on_new_context_fields_and_raw_keeps_every_byte(self):
        self.write(CONTEXT)
        cells = bytes(self.t.cells); frames = self.lib.RefreshCount()
        spam = b'X0 original\nV31 original\n' * 40
        self.write(spam)
        self.assertEqual(self.lib.RefreshCount(),frames)
        self.write(SP)
        self.assertEqual(self.lib.RefreshCount(),frames+1)
        self.write(ESR)
        self.assertEqual(self.lib.RefreshCount(),frames+2)
        saved = bytes(self.t)
        self.write(b'Stack dump:\n' + spam)
        self.assertEqual(bytes(self.t),saved)
        self.assertEqual(bytes(self.t.cells),cells)
        used = early.U.from_address(0x9f941004).value
        self.assertEqual(c.string_at(0x9f941010,used),CONTEXT+spam+SP+ESR+b'Stack dump:\n'+spam)

    def test_summary_preserves_reentrancy_irq_and_bds_gate(self):
        self.write(CONTEXT)
        self.lib.SetIrq(0); self.lib.NestOnRefresh(); self.write(SP)
        self.assertEqual(self.lib.GetInterruptState(),0)
        self.assertEqual(self.t.busy,0)
        self.assertEqual(self.t.fault_mask,0x8000000f)
        self.assertEqual(list(self.t.fault_values)[:4],VALUES[:4])
        self.lib.SetStage(8); self.write(ESR)
        self.assertEqual(self.t.fault_mask,0x8000000f)
        used = early.U.from_address(0x9f941004).value
        self.assertIn(b'nested',c.string_at(0x9f941010,used))

    def test_nested_summary_refreshes_warning_once_and_raw_is_complete(self):
        self.write(CONTEXT)
        count = self.lib.RefreshCount()
        nested = b'Synchronous Exception at 0x9f000000\n'
        self.write(nested)
        self.assertEqual(self.t.fault_mask,0xc0000000)
        self.assertEqual(self.lib.RefreshCount(),count+1)
        self.write(SP+ESR)
        self.assertEqual(self.t.fault_mask,0xc0000000)
        used = early.U.from_address(0x9f941004).value
        self.assertEqual(c.string_at(0x9f941010,used),CONTEXT+nested+SP+ESR)
