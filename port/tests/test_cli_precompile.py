# SPDX-License-Identifier: GPL-2.0-or-later
import sys, unittest
from pathlib import Path
from unittest.mock import patch, Mock

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "launcher"))
import iss_run as run
import precompile


class CliPreparation(unittest.TestCase):
    def check(self, outcome):
        order = []
        with (
            patch.object(run, "load_settings", return_value={"game_dir": "synthetic"}),
            patch.object(run, "check_game", return_value=[]),
            patch.object(
                run, "prepare", side_effect=lambda *a, **k: order.append("prepare")
            ),
            patch.object(
                precompile,
                "prepare_jobs",
                side_effect=lambda *a, **k: order.append("jobs") or outcome,
            ),
            patch.object(
                run,
                "launch",
                side_effect=lambda *a, **k: (
                    order.append("launch")
                    or (Mock(poll=lambda: 0, wait=lambda: 0), Path("missing.log"))
                ),
            ),
            patch.object(
                run.logtail, "LogTail", return_value=Mock(poll=lambda **k: [])
            ),
        ):
            code = run.main([])
        return code, order

    def test_jobs_before_launch(self):
        self.assertEqual(self.check("done"), (0, ["prepare", "jobs", "launch"]))

    def test_failure_remains_playable(self):
        self.assertEqual(self.check("failed"), (0, ["prepare", "jobs", "launch"]))

    def test_cancel_does_not_launch(self):
        self.assertEqual(self.check("cancelled"), (1, ["prepare", "jobs"]))


if __name__ == "__main__":
    unittest.main()
