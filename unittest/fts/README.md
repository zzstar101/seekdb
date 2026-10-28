# Focused FTS optimization regressions

These checks exercise the production implementations and the SQL behavior affected
by the FTS optimizations. The official benchmark and its fixed baseline are unchanged.

## Native regression tests

First finish a normal seekdb build with its pinned dependencies installed. The helper
uses that build's actual compiler flags and production link command, substitutes the
regression test translation units for the server entry point, and links the installed
Google Test libraries. It does not rebuild seekdb or substitute mock implementations.

From the repository root:

```bash
python3 unittest/fts/build_regressions.py \
  --build build_release \
  --out /tmp/seekdb-fts-regressions \
  --execute

/tmp/seekdb-fts-regressions/fts_regression_tests
```

Omit `--execute` to validate the build metadata and show the intended outputs without
compiling. The output directory must be outside the source and build trees. The
executed compiler and linker commands are saved in `build-plan.json` there.

Both Makefiles and Ninja builds are supported. For a Google Test installation
outside the production dependency directories, pass its prefix (for example,
`--gtest-prefix /opt/homebrew/opt/googletest` on Apple Silicon). Add further
production-linked regression sources with repeated `--test-source` arguments;
paths are relative to the repository root.

The tests cover:

- Word aggregation: first-occurrence frequency, repeated words, collation equality,
  baseline representative spelling, forced bucket collisions, error propagation, and parser-helper
  reuse across populated and empty documents.
- IK classification: all 1,112,064 valid UTF-8 Unicode scalar values, every UTF-16
  code unit in both byte orders, and malformed, truncated, and null inputs. The
  unchanged production predicates in the old order provide the comparison oracle.
- String comparison: every supported collation, both trailing-space modes, empty
  strings, embedded NUL, Unicode and malformed byte sequences, and invalid inputs.
  Results are compared with the original production collation handler.
- Count-only batch merging: single, two-way, three-way and loser-tree unions; empty inputs,
  NULL IDs, duplicate final IDs, partial consumption, and allocation reuse. Bulk counts also
  cover overwritten datum/payload storage, long binary IDs, allocator replacement, refill
  failures, and extreme iterator offsets without adding the offset to the batch capacity.
- Production token-iterator batches: two token iterators share an expression frame and a
  test DAS source, then consume the public batch API. Refills overwrite both descriptors and
  payloads; the returned batches must retain private copies. The fixture also covers row/advance
  transitions, reuse, reset, and source-error propagation. Separate tests check long payload growth.
- Fixed-width document-ID comparison: every differing byte position and byte value,
  unaligned addresses, random pairs, NULL, and length fallbacks against the generic comparator.
- Post-build word rows: all generated columns against the original builder,
  input lifetime, partial batches, tokenless documents, and scratch-memory reuse.
- IK dictionary character codes: DAT-local mappings, missing and malformed
  characters, overlapping prefixes, and overflow of the bounded cache compared
  with the original matching loop. Range lookup checks both boundaries and gaps, and
  explicitly verifies that unordered, overlapping or invalid ranges use linear first-match
  lookup while ordered disjoint ranges use binary lookup.

## SQL regression tests

The focused cases are in the existing `fts_index` mysqltest suite:

- `no_score_projection`: independent expected counts and IDs; observable score
  materialization as an oracle for threshold and top-k behavior; prepared statements;
  joins, residual predicates, grouping, distinct and multiple aggregates; limit and
  offset; heap document IDs; DML and rollback.
- `count_only_boundaries`: 255/256/257/512/513 matching documents, duplicate and
  overlapping postings, batch-boundary limits, prepared-statement reuse, bulk-built
  and incrementally maintained indexes, updates/deletes, transaction visibility,
  rollback, and an independently materialized scored result.

- `postbuild_word_rows`: partitioned and nonpartitioned post-build versus incremental membership,
  documents crossing output-batch boundaries, empty/NULL input, frequency-sensitive
  scores on small documents, and a secondary-index smoke check. Partitioned builds
  use the existing scalar path to preserve per-source partition metadata. The large
  document
  occupies its own partition because the existing average-length estimator differs
  between SSTables and memtables; its full row fields are checked by the native test.
- `tokenize_function`: accent-, case-, and emoji-equivalent spellings plus n-gram
  representatives compared with the competition baseline.
- `postbuild_sstable_scan`: waits for a nonempty source tablet's current major
  compaction, then builds two indexes with parallel sampling and rescan. It checks
  exact membership and the persisted document-ID lookup path, and restores the
  temporarily changed compaction, sampling and PX settings.

They run through the repository's existing mysqltest discovery. To execute only these
cases against a dedicated local test instance, adjust the client connection as needed:

```bash
export LD_LIBRARY_PATH="$PWD/deps/3rd/u01/obclient/lib:$PWD/deps/3rd/u01/obclient/lib64${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
FTS_ROOT="$PWD"
FTS_SUITE="$FTS_ROOT/tools/deploy/mysql_test/test_suite/fts_index"
FTS_LOG_DIR=$(mktemp -d /tmp/seekdb-fts-mysqltest.XXXXXX)
cd "$FTS_ROOT/tools/deploy"

for FTS_CASE in no_score_projection count_only_boundaries postbuild_word_rows postbuild_sstable_scan tokenize_function; do
  "$FTS_ROOT/deps/3rd/u01/obclient/bin/mysqltest" \
    --host=127.0.0.1 --port=2881 --user=root \
    --tmpdir="$FTS_LOG_DIR" --logdir="$FTS_LOG_DIR" --silent \
    --test-file="$FTS_SUITE/t/$FTS_CASE.test" \
    --result-file="$FTS_SUITE/r/mysql/$FTS_CASE.result" || exit
done
```

These fixtures create and drop their own `fts_no_score_projection`,
`fts_count_boundaries`, `fts_postbuild_word_rows`, `fts_postbuild_sstable_scan`, and `fts`
databases. Run the compaction case on a dedicated instance with administrative access;
its major freeze also compacts other tables on that instance.

## Exact TOKENIZE differential

Capture a reference using the original server binary, then run the same command with
`--reference` against the candidate server:

```bash
python3 unittest/storage/fts_word_aggregation_diff.py \
  --mysql 'mysql -h127.0.0.1 -P2881 -uroot' \
  --save-reference /tmp/fts-tokenize-reference.json

python3 unittest/storage/fts_word_aggregation_diff.py \
  --mysql 'mysql -h127.0.0.1 -P2881 -uroot' \
  --reference /tmp/fts-tokenize-reference.json
```

This compares 533 complete JSON results byte for byte, including token order and
baseline representative spelling. It covers five parsers, the three supported UTF-8 collations, both
output modes, a binary-collation BENG fallback, empty/NULL input, Unicode, embedded
NUL, repeated words, long words, and alternating input rows. It also checks independent
frequency and empty-input invariants. All statements in this differential are reads.
