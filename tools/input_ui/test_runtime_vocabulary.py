"""
The UI and the runtime must name the same things.

    py -3 -m unittest tools.input_ui.test_runtime_vocabulary

src/input/input_bindings.c has its own copies of the control names, the pad
source names, the key names and the default mapping, because it cannot import
Python. Nothing but this test stops the two drifting, and drift is silent in
the worst way: the UI writes a binding, the file saves, the runtime does not
recognise the name and the control simply never fires.

The C tables are read out of the source rather than a generated header, so the
runtime file stays readable on its own.
"""

import os
import re
import unittest

from tools.input_ui import bindings

ROOT = os.path.join(os.path.dirname(__file__), "..", "..")
SOURCE = os.path.join(ROOT, "src", "input", "input_bindings.c")


def read_source():
    with open(SOURCE, encoding="utf-8") as handle:
        return handle.read()


def table(text, name):
    """The body of a C initialiser list `... NAME[...] = { ... };`."""
    match = re.search(re.escape(name) + r"\[[^\]]*\]\s*=\s*\{(.*?)\n\};",
                      text, re.S)
    assert match, "%s is not in %s any more" % (name, SOURCE)
    return match.group(1)


class Vocabulary(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.text = read_source()

    def test_the_control_names_are_the_same_and_in_the_same_order(self):
        names = re.findall(r'\{\s*"([a-z_0-9]+)"', table(self.text, "CONTROLS"))
        self.assertEqual(names, bindings.CONTROL_NAMES)

    def test_the_pad_button_names_are_the_same(self):
        names = re.findall(r'"([a-z_0-9]+)"', table(self.text, "PAD_BUTTONS"))
        self.assertEqual(names, bindings.PAD_BUTTONS)

    def test_the_mouse_button_names_are_the_same(self):
        names = re.findall(r'"([a-z_0-9]+)"', table(self.text, "MOUSE_BUTTONS"))
        self.assertEqual(names, bindings.MOUSE_BUTTONS)

    def test_the_defaults_are_the_same(self):
        rows = re.findall(r'\{\s*"([a-z_0-9]+)"\s*,\s*"([^"]+)"\s*,\s*'
                          r'(?:"([^"]+)"|NULL)\s*\}',
                          table(self.text, "DEFAULTS"))
        self.assertEqual([(control, pad, key or None)
                          for control, pad, key in rows], bindings.DEFAULTS)

    def test_the_key_names_are_the_same(self):
        rows = re.findall(r'\{\s*"([A-Z0-9_]+)"\s*,\s*(0x[0-9A-Fa-f]+)\s*\}',
                          table(self.text, "KEYS"))
        self.assertEqual({name: int(code, 16) for name, code in rows},
                         bindings.KEY_NAMES)

    def test_the_deadzone_default_is_the_same(self):
        match = re.search(r"#define DEADZONE_DEFAULT (\d+)", self.text)
        self.assertTrue(match)
        self.assertEqual(int(match.group(1)), bindings.DEADZONE_DEFAULT)

    def test_the_runtime_reads_the_same_environment_variable(self):
        self.assertIn('getenv("RECOMP_INPUT_CONFIG")', self.text)

    def test_the_runtime_looks_in_the_same_per_user_place(self):
        # find_config() in the C takes the directory from src/config's
        # recomp_config_user_dir() and adds input_bindings.json; the UI's
        # config_path() does the same with bindings.user_dir().
        self.assertIn("recomp_config_user_dir(", self.text)
        self.assertIn('input_bindings.json"', self.text)
        self.assertTrue(bindings.config_path().endswith(
            os.path.join("xboxrecomp", "input_bindings.json"))
            or os.environ.get("RECOMP_INPUT_CONFIG"))

    def test_the_per_user_directory_is_src_configs_on_every_platform(self):
        # recomp_config.c's user_dir() is one dir_from() call per platform:
        #   dir_from(out, n, <environment variable or NULL>, <home-relative or NULL>)
        # Read those back and check bindings.user_dir() agrees on each.
        with open(os.path.join(ROOT, "src", "config", "recomp_config.c"),
                  encoding="utf-8") as handle:
            c = handle.read()
        body = c[c.index("static int user_dir(char *out, size_t n)"):]
        body = body[:body.index("\n}\n")]
        calls = re.findall(r'dir_from\(out, n, (NULL|"[^"]*"), (NULL|"[^"]*")\)', body)
        branches = dict(zip(("win32", "darwin", "linux"), calls))
        self.assertEqual(len(calls), 3, body)

        home = os.path.join(os.sep, "home", "player")
        env = {"APPDATA": os.path.join(os.sep, "appdata"),
               "XDG_CONFIG_HOME": os.path.join(os.sep, "xdg")}
        for platform, (var, rel) in branches.items():
            with self.subTest(platform=platform):
                var, rel = var.strip('"'), rel.strip('"')
                base = env[var] if var != "NULL" else os.path.join(home, *rel.split("/"))
                self.assertEqual(bindings.user_dir(platform, env, home),
                                 os.path.join(base, "xboxrecomp"))
        # The fallback when the variable is unset, where there is one; on
        # Windows dir_from(out, n, "APPDATA", NULL) has none and gives no
        # directory, and so does the UI.
        self.assertEqual(bindings.user_dir("linux", {}, home),
                         os.path.join(home, ".config", "xboxrecomp"))
        self.assertEqual(branches["win32"][1], "NULL")
        self.assertIsNone(bindings.user_dir("win32", {}, home))

    def test_both_sides_honour_recomp_user_dir(self):
        with open(os.path.join(ROOT, "src", "config", "recomp_config.c"),
                  encoding="utf-8") as handle:
            c = handle.read()
        body = c[c.index("static int user_dir(char *out, size_t n)"):]
        body = body[:body.index("\n}\n")]
        self.assertIn('getenv("RECOMP_USER_DIR")', c)
        self.assertIn('override_dir(out, n, "")', body)
        for platform in ("win32", "darwin", "linux"):
            with self.subTest(platform=platform):
                self.assertEqual(bindings.user_dir(
                    platform, {"RECOMP_USER_DIR": "/scratch/u",
                               "APPDATA": "/appdata"}, "/home/p"),
                    "/scratch/u")


if __name__ == "__main__":
    unittest.main()
