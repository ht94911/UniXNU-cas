#!/usr/bin/env python3
"""Check the distinctions needed by the actual036/037 IMS evidence."""
from pathlib import Path
import sys
import unittest

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT/'tools/baseband27'))
from ims_diagnostics import parse

BOOT = '39A17A98E916449EA958DCBD37D6930E'


def row(category, message, seconds=117.5, boot=BOOT):
    return dict(boot_uuid=boot, boot_seconds=seconds, category=category, message=message)


class IMSDiagnosticsTest(unittest.TestCase):
    def test_VoLTE_feature_and_WMS_ready_do_not_imply_registration_or_delivery(self):
        result = parse([
            row('5wi.ctr.1.1', 'IMS Feature: ON, VoLTE Feature support: kTrue, VoNR Feature support: kFalse'),
            row('5wi.ctr.1.1', 'ImsRegMaskUp: IMS Pref [Pref]: IMSRegistered[kNotRegistered] qualifiedToBringupPDN[0] qualifiedToRegister[0]'),
            row('msg.sms.ctr.1', 'SMS [ready] - over BB, encoder: k3GPP'),
            row('msg.sms.ctr.1', 'SMS AWD Send Start [type: kVVM transport: Default Transport]'),
            row('msg.sms.qmi.1', 'QMI_WMS_RAW_SEND failed with error 0x35 (QMI_ERR_NETWORK_NOT_READY)'),
        ], BOOT)
        self.assertTrue(result['latest_features']['5wi.ctr.1.1']['feature_enabled'])
        self.assertEqual(result['latest_registration_gates']['5wi.ctr.1.1']['registration_state'], 'kNotRegistered')
        self.assertEqual(result['SMS_raw_send_error_counts'], {'53': 1})
        self.assertNotIn('PDN_and_PCSCF', result['groups'])
        self.assertEqual(result['groups']['SMS_send_attempts'][0]['message_type'], 'kVVM')
        self.assertNotIn('IMS_registration_accepted', result)
        self.assertNotIn('SMS_delivery_accepted', result)

    def test_masks_keep_slot_and_pre_shutdown_operating_state(self):
        base = 'Enabled[1] Allowed[1] Telephony[1] SimFiles[1] Pref[1] Connectivity[0] TTY[1] E911[1] Power[1] DataContext[0] Proxy[0] Client[1] MSISDNPresent[1] AllowWhileOnCSCall[1] DCN[1] DeviceID[1]'
        result = parse([
            row('5wi.mdl.1.1', 'RegMasks[Pref]: '+base),
            row('5wi.mdl.2.2', 'RegMasks[Pref]: '+base.replace('Pref[1]', 'Pref[0]')),
            row('5wi.mdl.1.1', 'RegMasks[Power]: '+base.replace('Power[1]', 'Power[0]'), 426),
        ], BOOT)
        self.assertEqual(result['latest_masks']['5wi.mdl.1.1']['fields']['Power'], 0)
        self.assertEqual(result['last_operating_masks']['5wi.mdl.1.1']['fields']['Power'], 1)
        self.assertEqual(result['last_operating_masks']['5wi.mdl.2.2']['slot'], 2)
        self.assertEqual(result['last_operating_masks']['5wi.mdl.2.2']['fields']['Pref'], 0)

    def test_unassigned_context_and_unavailable_connection_are_retained(self):
        result = parse([
            row('DATA.ServiceController', 'updateStateCache: kDataConnectionIM: idByMask=-2, configuredByMask[BB]=true, configuredByMask[iW]=false, activeByMask=false, networkActive=false, activeOver=kDataContextUnknown/Any'),
            row('5wi.ct.1.1', 'ConnectionAvailability: size=36, ct=kDataConnectionIM, error=kDataNotSupported, available=false, contextType=kDataContextUnknown/Any'),
        ], BOOT)
        cached, = result['groups']['global_IM_context_cache']
        self.assertEqual(cached['context_id'], -2)
        self.assertIsNone(cached['slot'])
        self.assertFalse(result['groups']['connection_availability'][0]['available'])

    def test_other_boot_and_missing_corrected_time_are_rejected(self):
        with self.assertRaises(ValueError):
            parse([row('5wi', '', boot='0'*32)], BOOT)
        with self.assertRaises(ValueError):
            parse([dict(boot_uuid=BOOT, category='5wi', message='')], BOOT)

    def test_empty_capture_does_not_invent_success_or_failure(self):
        result = parse([], BOOT)
        self.assertEqual(result['native_record_count'], 0)
        self.assertEqual(result['latest_masks'], {})
        self.assertEqual(result['SMS_raw_send_error_counts'], {})


if __name__ == '__main__':
    unittest.main()
