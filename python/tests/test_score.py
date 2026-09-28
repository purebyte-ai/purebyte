"""The CLI's reports work with the public CredData scorer (benchmarks/creddata/score.py): a synthetic dataset laid
out like a CredData checkout, scanned by `purebyte scan` with a model forced to flag every line, then scored."""
import csv
import json
import os
import subprocess
import sys
import unittest

from support import CLI, HAVE_CLI, MARK_ALL, ROOT, TempDir, random_models, write_model

SCORE = os.path.join(ROOT, "benchmarks", "creddata", "score.py")
LINES = b"".join(b"key_%d = value number %d of this configuration file\n" % (i, i) for i in range(12))


@unittest.skipUnless(HAVE_CLI and random_models and os.path.isfile(SCORE), "needs the CLI, tests/gen and score.py")
class Score(unittest.TestCase):
    def test_scan_report_is_scored(self):
        tmp = TempDir()
        try:
            model = write_model(tmp)
            rows = []
            for repo, path in (("r1", "data/r1/src/a.py"), ("r2", "data/r2/cfg/b.yaml")):
                tmp.file(path, LINES)
                rows += [dict(Id=f"{repo}1", FilePath=path, LineStart="2", LineEnd="2", GroundTruth="T",
                              Category="Password", RepoName=repo),
                         dict(Id=f"{repo}2", FilePath=path, LineStart="7", LineEnd="8", GroundTruth="F",
                              Category="Key", RepoName=repo)]
                with open(tmp.file(f"meta/{repo}.csv"), "w", encoding="utf-8", newline="") as fh:
                    writer = csv.DictWriter(fh, fieldnames=list(rows[0]))
                    writer.writeheader()
                    writer.writerows(r for r in rows if r["RepoName"] == repo)
            report = os.path.join(tmp.path, "purebyte.json")
            files = [os.path.join(tmp.path, "data", "r1", "src", "a.py"),
                     os.path.join(tmp.path, "data", "r2", "cfg", "b.yaml")]
            scan = subprocess.run([CLI, "scan", "--model", model, "--threads", "2", "--bias", str(MARK_ALL),
                                   "--format", "json", "--out", report] + files, capture_output=True, timeout=300)
            self.assertEqual(scan.returncode, 1, scan.stderr)
            scores = os.path.join(tmp.path, "scores.json")
            scored = subprocess.run([sys.executable, SCORE, "score", "--creddata", tmp.path, "--purebyte",
                                     f"pb={report}", "--partitions", "all", "--json", scores],
                                    capture_output=True, timeout=300)
            self.assertEqual(scored.returncode, 0, scored.stderr)
            with open(scores, encoding="utf-8") as fh:
                row = json.load(fh)["results"]["all | all file sizes | pb"]
            self.assertEqual((row["T"], row["TP"], row["FP"], row["FN"]), (2, 2, 2, 0))
        finally:
            tmp.cleanup()


if __name__ == "__main__":
    unittest.main()
