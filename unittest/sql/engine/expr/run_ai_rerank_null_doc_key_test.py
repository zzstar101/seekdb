#!/usr/bin/env python3
"""Execute the production AI_RERANK evaluator with dependency doubles.

The evaluator's complete function body is copied verbatim and compiled; the
fixture supplies only its surrounding interfaces. This exercises NULL handling,
element validation and dispatch, not SQL type deduction, HTTP, JSON encoding or
the keyed-document extraction helper. Run ai_rerank_regression.test for those
integrated paths.

Examples:
  python3 unittest/sql/engine/expr/run_ai_rerank_null_doc_key_test.py
  python3 unittest/sql/engine/expr/run_ai_rerank_null_doc_key_test.py --baseline-ref HEAD
  ASAN_OPTIONS=detect_leaks=0 python3 unittest/sql/engine/expr/run_ai_rerank_null_doc_key_test.py --sanitize

Use a revision predating the fix with --baseline-ref. The same compiler, fixture
and runner must observe a targeted SQL NULL failure on that revision and success
on the working source; compile errors and crashes are never accepted as proof.
"""

import argparse
from pathlib import Path
import subprocess
import tempfile


ROOT = Path(__file__).resolve().parents[4]
SOURCE = Path('src/sql/engine/expr/ob_expr_ai/ob_expr_ai_rerank.cpp')
FIXTURE = Path(__file__).with_name('test_ai_rerank_null_doc_key.cpp')
START = 'int ObExprAIRerank::eval_ai_rerank('
END = '\nint ObExprAIRerank::eval_ai_rerank_with_doc_key('


def extract_evaluator(source):
    if source.count(START) != 1 or source.count(END) != 1:
        raise RuntimeError('expected exactly one evaluator and keyed-helper signature')
    start = source.index(START)
    end = source.index(END)
    if end <= start:
        raise RuntimeError('keyed helper must follow the evaluator')
    function = source[start:end]
    if not function.rstrip().endswith('}'):
        raise RuntimeError('evaluator boundary does not end at a closing brace')
    return 'namespace oceanbase { namespace sql {\n' + function + '\n}}\n'


def run_evaluator(source, label, args, expect_failure=False):
    with tempfile.TemporaryDirectory(prefix='seekdb-rerank-null-') as directory:
        work = Path(directory)
        (work / 'eval_under_test.inc').write_text(extract_evaluator(source))
        executable = work / 'test_ai_rerank_null_doc_key'
        flags = ['-std=c++17', '-Wall', '-Wextra', '-Werror']
        flags += (['-O1', '-g', '-fsanitize=address,undefined', '-fno-omit-frame-pointer']
                  if args.sanitize else ['-O2'])
        subprocess.run([args.compiler, *flags, '-I', str(work), str(FIXTURE),
                        '-o', str(executable)], check=True)
        result = subprocess.run([str(executable)], text=True, capture_output=True)
        if expect_failure:
            if result.returncode != 1 or 'SQL NULL key selects strings: ret=' not in result.stderr:
                raise RuntimeError(f'{label}: expected the targeted SQL NULL assertion failure; '
                                   f'got exit {result.returncode}\n{result.stdout}{result.stderr}')
            print(f'{label}: negative control failed on SQL NULL as expected')
        elif result.returncode != 0 or 'actual eval_ai_rerank function: 9/9 passed' not in result.stdout:
            raise RuntimeError(f'{label}: evaluator checks failed with exit {result.returncode}\n'
                               f'{result.stdout}{result.stderr}')
        else:
            print(f'{label}: PASS')
        print(result.stdout, end='')
        if result.stderr:
            print(result.stderr, end='')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--compiler', default='c++')
    parser.add_argument('--source', type=Path, default=ROOT / SOURCE)
    parser.add_argument('--baseline-ref', help='Git revision before the SQL NULL fix')
    parser.add_argument('--sanitize', action='store_true')
    args = parser.parse_args()
    if args.baseline_ref:
        baseline = subprocess.run(['git', 'show', f'{args.baseline_ref}:{SOURCE.as_posix()}'],
                                  cwd=ROOT, text=True, capture_output=True, check=True).stdout
        run_evaluator(baseline, 'baseline', args, expect_failure=True)
    run_evaluator(args.source.read_text(), 'working source', args)


if __name__ == '__main__':
    main()
