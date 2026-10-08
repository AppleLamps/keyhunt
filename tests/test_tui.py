#!/usr/bin/env python3
import tempfile
import unittest
from pathlib import Path

import keyhunt_tui


class CommandTests(unittest.TestCase):
    def test_default_address_command(self):
        config = keyhunt_tui.default_config()
        command = keyhunt_tui.build_command(config)
        self.assertEqual(command[:5], ["./keyhunt", "-m", "address", "-f", "tests/66.txt"])
        self.assertIn("-b", command)
        self.assertIn("66", command)
        self.assertIn("-l", command)
        self.assertIn("compress", command)
        self.assertIn("-R", command)
        self.assertIn("-q", command)

    def test_bsgs_only_emits_bsgs_options(self):
        config = keyhunt_tui.default_config()
        config.update({
            "mode": "bsgs", "target_file": "tests/63.pub", "bsgs_order": "dance",
            "bsgs_factor": "512", "compact_filter": True, "skip_checksum": True,
            "endomorphism": True, "stride": "10",
        })
        command = keyhunt_tui.build_command(config)
        self.assertIn("dance", command)
        self.assertIn("512", command)
        self.assertIn("-F", command)
        self.assertIn("-6", command)
        self.assertIn("-o", command)
        self.assertIn(str(config["cache_dir"]), command)
        self.assertNotIn("-e", command)
        self.assertNotIn("-I", command)
        self.assertNotIn("-l", command)

    def test_vanity_prefixes_are_repeated(self):
        config = keyhunt_tui.default_config()
        config.update({"mode": "vanity", "target_file": "", "vanity_prefixes": "1Good, 1Fast"})
        command = keyhunt_tui.build_command(config)
        pairs = list(zip(command, command[1:]))
        self.assertIn(("-v", "1Good"), pairs)
        self.assertIn(("-v", "1Fast"), pairs)
        self.assertNotIn("-f", command)

    def test_minikey_base_suppresses_random_flag(self):
        config = keyhunt_tui.default_config()
        config.update({"mode": "minikeys", "target_file": "tests/minikeys.txt",
                       "minikey_base": "SG64GZqySYwBm9KxE1wJ28"})
        command = keyhunt_tui.build_command(config)
        self.assertIn("-C", command)
        self.assertNotIn("-R", command)
        self.assertNotIn("-L", command)
        self.assertNotIn("-b", command)

    def test_mode_change_updates_untouched_example_defaults(self):
        config = keyhunt_tui.default_config()
        config["puzzle"] = "manual"
        mode_field = next(field for field in keyhunt_tui.visible_fields(config) if field.key == "mode")
        keyhunt_tui.cycle_field(config, mode_field, 1)
        self.assertEqual(config["mode"], "rmd160")
        self.assertEqual(config["target_file"], "tests/66.rmd")
        for _ in range(4):
            mode_field = next(field for field in keyhunt_tui.visible_fields(config) if field.key == "mode")
            keyhunt_tui.cycle_field(config, mode_field, 1)
        self.assertEqual(config["mode"], "minikeys")
        self.assertEqual(config["target_file"], "tests/minikeys.txt")
        self.assertEqual(config["range_kind"], "default")

    def test_catalog_has_all_puzzles(self):
        catalog = keyhunt_tui.load_puzzle_catalog()
        self.assertEqual(set(catalog), set(range(1, 161)))
        self.assertEqual(catalog[66]["bits"], "66")
        self.assertRegex(catalog[66]["hash160_compressed"], r"^[0-9a-f]{40}$")

    def test_machine_tuned_bsgs_preset(self):
        catalog = keyhunt_tui.load_puzzle_catalog()
        hardware = keyhunt_tui.HardwareProfile("test CPU", 12, 6, 12.0, "AVX2")
        number = next(n for n, record in catalog.items()
                      if n >= 66 and len(record.get("public_key", "")) in (66, 130))
        config = keyhunt_tui.default_config()
        with tempfile.TemporaryDirectory() as directory:
            keyhunt_tui.apply_puzzle_preset(config, number, catalog, hardware, Path(directory))
            self.assertTrue(Path(config["target_file"]).exists())
        self.assertEqual(config["mode"], "bsgs")
        self.assertEqual(config["threads"], "12")
        self.assertEqual(config["bsgs_factor"], "1024")
        self.assertTrue(config["compact_filter"])
        self.assertFalse(config["endomorphism"])

    def test_address_only_preset_uses_rmd160_without_endomorphism(self):
        catalog = keyhunt_tui.load_puzzle_catalog()
        hardware = keyhunt_tui.HardwareProfile("test CPU", 12, 6, 12.0, "AVX2")
        number = next(n for n, record in catalog.items()
                      if n >= 22 and not record.get("public_key", "").strip())
        config = keyhunt_tui.default_config()
        with tempfile.TemporaryDirectory() as directory:
            keyhunt_tui.apply_puzzle_preset(config, number, catalog, hardware, Path(directory))
            self.assertTrue(Path(config["target_file"]).exists())
        self.assertEqual(config["mode"], "rmd160")
        self.assertEqual(config["search_type"], "compress")
        self.assertFalse(config["endomorphism"])
        self.assertEqual(config["search_order"], "random")

    def test_every_puzzle_produces_a_complete_command(self):
        catalog = keyhunt_tui.load_puzzle_catalog()
        hardware = keyhunt_tui.HardwareProfile("test CPU", 12, 6, 12.0, "AVX2")
        with tempfile.TemporaryDirectory() as directory:
            for number in range(1, 161):
                config = keyhunt_tui.default_config()
                keyhunt_tui.apply_puzzle_preset(config, number, catalog, hardware, Path(directory))
                command = keyhunt_tui.build_command(config)
                self.assertIn("-f", command, number)
                self.assertIn("-b", command, number)
                self.assertIn("-n", command, number)
                self.assertIn("-t", command, number)
                self.assertNotIn("-e", command, number)
                self.assertTrue(Path(config["target_file"]).exists(), number)

    def test_settings_round_trip(self):
        config = keyhunt_tui.default_config()
        config["mode"] = "xpoint"
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "settings.json"
            keyhunt_tui.save_config(config, path)
            loaded, _ = keyhunt_tui.load_config(path)
        self.assertEqual(loaded, config)

    def test_bsgs_cache_directory_is_created(self):
        config = keyhunt_tui.default_config()
        with tempfile.TemporaryDirectory() as directory:
            cache = Path(directory) / "nested" / "cache"
            config.update({"mode": "bsgs", "save_tables": True, "cache_dir": str(cache)})
            self.assertEqual(keyhunt_tui.prepare_bsgs_cache(config), cache)
            self.assertTrue(cache.is_dir())

    def test_private_keys_are_zero_padded_to_full_256_bit_hex(self):
        full = "e9ae4933d6c972c4c318d244865dc27c6299b8d8d3f9166f275640a9dfc3e21d"
        output = f"Hit! Private Key: 1\n[+] Thread Key found privkey {full}\n"
        self.assertEqual(keyhunt_tui.extract_private_keys(output), ["0" * 63 + "1", full])
        self.assertEqual(keyhunt_tui.extract_private_keys("pubkey: 02" + "a" * 64), [])

    def test_long_output_is_hard_wrapped_without_losing_characters(self):
        line = "Private Key: " + "f" * 64
        wrapped = keyhunt_tui.wrap_output_lines([line], 23)
        self.assertEqual("".join(wrapped), line)
        self.assertTrue(all(len(part) <= 23 for part in wrapped))

    def test_carriage_return_replaces_live_status(self):
        lines = []
        current, pending = keyhunt_tui.feed_output("start\nold status\rnew status", lines, "")
        self.assertEqual(lines, ["start"])
        self.assertEqual(current, "new status")
        self.assertFalse(pending)

    def test_pty_crlf_preserves_lines_across_chunks(self):
        lines = []
        current, pending = keyhunt_tui.feed_output("first line\r", lines, "")
        current, pending = keyhunt_tui.feed_output("\nsecond line\r\n", lines, current, pending)
        self.assertEqual(lines, ["first line", "second line"])
        self.assertEqual(current, "")
        self.assertFalse(pending)


if __name__ == "__main__":
    unittest.main()
