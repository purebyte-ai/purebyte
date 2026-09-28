# Benchmarks

The example specialists of the release, measured on public benchmarks where one exists (PIIMB, CredData) and on our
own exams where none does (credentials inside binaries), with everything needed to check our numbers where the data
are public.

| Page | Contents |
|---|---|
| [RESULTS.md](RESULTS.md) | All published quality results, with their conditions and caveats |
| [creddata/](creddata/) | Step-by-step reproduction of the `secrets-code` results on CredData, and the scoring script |
| [piimb/](piimb/) | Step-by-step reproduction of the `pii` results on the PII Masking Benchmark, and the scoring script |
| [docs/performance.md](../docs/performance.md) | Speed, latency and memory |

Rules we follow for every published number:

1. The benchmark is public, and so are the split and the scoring code. The binary exams of `secrets-bin` are the
   exception, and [RESULTS.md](RESULTS.md#secrets-bin) says why.
2. The split is fixed before measuring. Released models are never trained on the test partition, and the one release
   choice made with test results in view is disclosed
   ([secrets-code](RESULTS.md#the-pre-registered-criteria-and-the-choice-of-the-released-model)).
3. Test data that overlaps a training corpus is left out of the published figures, and the overlap is disclosed with
   both results ([an example](RESULTS.md#training-data-overlap)).
4. Other tools are run by us on the same files with the same scoring code, and reported as measured, where we can
   run them (CredData, the binary probe). On PIIMB, the other models' figures are the public leaderboard's: same
   sentences and metric, their own inference code.
5. F1 comes with a 95 % confidence interval; the conditions (versions, scope, hardware) are written next to it.
6. A result we have not measured is written as **TBD**, never estimated, and a figure that can still change is marked
   *provisional*.
7. Every pre-registered verdict is published, failures included.

Want another benchmark here? Open an issue with a link to the dataset, its license and its official metric.
