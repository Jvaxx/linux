#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Synthetic country-package tests; no firmware or board data is embedded."""
import importlib.util
from pathlib import Path
import struct
import unittest

SPEC = importlib.util.spec_from_file_location('six_policy',
    Path(__file__).with_name('mt7932-policy-6ghz.py'))
gen = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(gen)
CHANNELS = [2] + [ch for ch in range(1, 234, 2)
                  if gen.power.channel_width_class(3, ch) <= 3 and ch != 231]


def legacy(country='AU'):
    out = bytearray(b'J7RP' + struct.pack('<H', 1) + country.encode() + b'\x02\x0d\0\0\0\0\0\0')
    for _ in range(2):
        mode = bytearray(328)
        struct.pack_into('<I', mode, 4, 1)
        out += mode
    for index in range(13):
        if 1 <= index <= 9:
            count = 5 if index in (2, 9) else 8
            table = bytearray(44 + count * 122)
            table[:2] = b'\x03\x0a'
            table[4:8] = bytes((count, 1 if index <= 2 else 2, int(index in (2, 9)), 0))
            table[8:10] = country.encode()
        else:
            slot = index - 9 if index else 0
            table = bytearray((532, 144, 58, 265)[slot])
            table[1] = 2
            table[7] = (5, 1, 4, 2)[slot]
            if index == 10:
                table[8:10] = country.encode()
        struct.pack_into('<H', table, 2, len(table))
        out += table
    struct.pack_into('<I', out, 12, len(out))
    return bytes(out)


def groups():
    return {name: [(f'ch{ch:03d}', ['20'] * gen.power.GROUP_WIDTH[name])
                   for ch in CHANNELS] for name in gen.GROUPS}


class SixPolicyTest(unittest.TestCase):
    def test_channel_namespace(self):
        self.assertEqual(len(CHANNELS), 110)
        self.assertEqual(CHANNELS[:9], [2, 1, 3, 5, 7, 9, 11, 13, 15])
        self.assertEqual(CHANNELS[-4:], [225, 227, 229, 233])

    def test_four_country_identity_and_permissions(self):
        parsed = gen.rows(groups(), CHANNELS)
        allowed = ['0'] * 110
        allowed[CHANNELS.index(1)] = '3/1'
        allowed[CHANNELS.index(5)] = '1/2'
        allowed[CHANNELS.index(37)] = '2'
        for country in ('AU', 'US', 'CA', 'DE'):
            with self.subTest(country=country):
                base = legacy(country)
                data, mask, messages = gen.extend(base, country, CHANNELS, allowed, parsed)
                self.assertEqual(mask, (1 << 1) | (1 << 9))
                self.assertEqual(data[6:8], country.encode())
                self.assertEqual(struct.unpack_from('<H', data, 4)[0], 2)
                self.assertEqual(data[len(base):len(base) + 4], b'6GRP')
                self.assertEqual(data[len(base) + 18], 1)
                self.assertEqual(len(messages), 28)
                self.assertTrue(all(body[8:10] == country.encode()
                                    for cid, body in messages if cid == 0x5d))

    def test_pages_and_association_selectors(self):
        messages = gen.pages('AU', CHANNELS, gen.rows(groups(), CHANNELS))
        self.assertEqual([cid for cid, _ in messages], [0x5d] * 14 + [0x95] * 14)
        self.assertEqual([messages[n][1][44] for n in (0, 1)], [2, 15])
        self.assertEqual([len(messages[n][1]) for n in (0, 1)], [1020, 1020])
        for cid, page in messages:
            self.assertEqual(page[5], 3)
            self.assertEqual(len(page), 44 + page[4] * (122 if cid == 0x5d else 33))
            if cid == 0x95:
                self.assertEqual(page[:4], bytes(4))
                self.assertEqual(page[8:44], bytes(36))
        self.assertEqual(messages[13][1][6], 1)
        self.assertEqual(messages[27][1][6], 1)

    def test_missing_power_does_not_grant_permission(self):
        parsed = gen.rows(groups(), CHANNELS)
        parsed[5] = {name: ['X'] * width for name, width in gen.power.GROUP_WIDTH.items()}
        allowed = ['2'] * 110
        _, mask, _ = gen.extend(legacy(), 'AU', CHANNELS, allowed, parsed)
        self.assertFalse(mask & (1 << 1))
        self.assertFalse(mask & (1 << 59))

    def test_no_vlp_country_is_refused(self):
        with self.assertRaisesRegex(ValueError, 'no VLP'):
            gen.extend(legacy(), 'AU', CHANNELS, ['1/3'] * 110,
                       gen.rows(groups(), CHANNELS))

    def test_wrong_legacy_country_is_refused(self):
        with self.assertRaisesRegex(ValueError, 'header'):
            gen.validate_legacy(legacy('US'), 'AU')

    def test_all_legacy_truncations_are_refused(self):
        data = legacy()
        for size in range(len(data)):
            with self.assertRaises(ValueError):
                gen.validate_legacy(data[:size], 'AU')

    def test_legacy_internal_table_corruption(self):
        data = bytearray(legacy())
        data[16 + 2 * 328 + 7] = 0
        with self.assertRaisesRegex(ValueError, 'table'):
            gen.validate_legacy(data, 'AU')

    def test_row_overwrite_and_channel_two_padding(self):
        data = groups()
        data['ofdm'].append(('ch002', ['11'] * 8))
        parsed = gen.rows(data, CHANNELS)
        self.assertEqual(parsed[2]['ofdm'], ['11'] * 8)
        self.assertEqual(parsed[2]['ht20'][:8], ['11'] * 8)
        self.assertEqual(parsed[2]['ht40'], ['X'] * 24)

    def test_bonded_and_cdd_rows(self):
        data = groups()
        data['ofdm'].extend([('ch007-003', ['9'] * 8), ('ch007-003-cdd', ['8'] * 8)])
        parsed = gen.rows(data, CHANNELS)
        self.assertEqual(parsed[3]['ht20'][16:24], ['9'] * 8)
        self.assertEqual(parsed[3]['ht40'][:8], ['8'] * 8)

    def test_unrecognized_rows_and_missing_groups(self):
        data = groups()
        data['ofdm'].append(('ch234', ['1'] * 8))
        with self.assertRaisesRegex(ValueError, 'namespace'):
            gen.rows(data, CHANNELS)
        del data['ofdm']
        with self.assertRaisesRegex(ValueError, 'incomplete'):
            gen.rows(data, CHANNELS)

    def test_ascii_grammar_duplicate_and_range(self):
        for text in ('[AU,VLP]\n[AU,VLP]\n',
                     '[AU,VLP]\n<unknown,x>\n',
                     '[AU,VLP]\n<ofdm,x>\nch005,999,1,1,1,1,1,1,1\n</ofdm>\n',
                     '[AU,VLP]\n<ofdm,x>\nch005,1,1\n</ofdm>\n'):
            with self.subTest(text=text), self.assertRaises(ValueError):
                gen.sections(text)

    def test_wire_projection_distinguishes_groups(self):
        row = {name: ['X'] * width for name, width in gen.power.GROUP_WIDTH.items()}
        row['ofdm'] = [str(i) for i in range(8)]
        wire = gen.power.expand_record(gen.power.value_image(row))
        self.assertEqual(len(wire), 121)
        self.assertEqual(wire[:4], bytes((0xc4, 0, 4, 6)))
        self.assertEqual(wire[-9:], bytes([0xc4]) * 9)


if __name__ == '__main__':
    unittest.main(verbosity=2)
