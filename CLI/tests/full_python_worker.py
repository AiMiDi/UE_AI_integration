"""Regression checks for normal script entry and main-module identity."""
import argparse
import json
from pathlib import Path
import subprocess
import unittest


class FullScriptTests(unittest.TestCase):
    python = ""

    def execute(self, script):
        worker = Path(__file__).resolve().parents[2] / "Resources/Python/full_execute.py"
        process = subprocess.run(
            [self.python, "-I", "-E", "-u", str(worker)],
            input=json.dumps({"script": script, "data": {"value": 123}}),
            capture_output=True, text=True, timeout=10, check=True,
        )
        return json.loads(process.stdout)

    def test_standard_main_entry(self):
        output = self.execute("def main():\n    global result\n    result = input['value']\n"
                              "if __name__ == '__main__':\n    main()\n")
        self.assertTrue(output["ok"])
        self.assertEqual(output["result"], 123)

    def test_main_module_and_postponed_dataclass_annotations(self):
        output = self.execute(
            "from __future__ import annotations\n"
            "import __main__\nfrom dataclasses import dataclass\n"
            "@dataclass\nclass Value:\n    number: int\n"
            "assert __main__.Value is Value\n"
            "result = Value(data['value']).number\n"
        )
        self.assertTrue(output["ok"], output)
        self.assertEqual(output["result"], 123)

    def test_script_error_remains_a_protocol_error(self):
        output = self.execute("if __name__ == '__main__':\n    raise ValueError('entry failure')\n")
        self.assertFalse(output["ok"])
        self.assertEqual(output["exceptionType"], "ValueError")
        self.assertEqual(output["error"], "entry failure")


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("--python", required=True)
    args, remaining = parser.parse_known_args()
    FullScriptTests.python = args.python
    unittest.main(argv=[__file__, *remaining])
