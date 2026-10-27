#!/usr/bin/env python3
"""AI column argument-contract guards (static; SQL behavior needs the kernel suite).

Regression guards for the AI column argument semantics fixed in this change (see
deepseek-ai-argument-review.md and deepseek-ai-argument-fix-progress.md).  The fix
is a versioned JSON input envelope: the enqueuing statement evaluates the prompt
and config expressions of the AI function in the user transaction with the native
JSON_OBJECT constructor, which keeps their SQL types apart, and the worker decodes
the envelope:

  A1  __all_ai_column_task.prompt stays NOT NULL, but what it holds is never a bare
      expression: a JSON_OBJECT(...) envelope is never NULL, so a NULL source
      column can no longer abort the business DML with ER_BAD_NULL_ERROR (1048).
      The worker owns the NULL / empty prompt outcome instead (AI_COMPLETE fails
      the task terminally, no request; AI_EMBED propagates SQL NULL).
  A2  a prompt that materializes to a prompt object (nested AI_PROMPT(...)) is
      validated and rendered in the worker before the model call; a plain string
      prompt is never parsed for a prompt-like shape and is sent verbatim.
  A3  the third argument of AI_COMPLETE (the config) is accepted, persisted in
      the definition as SQL text, snapshotted by the enqueuing statement (its
      column references are dependencies too) and passed to the model call.
  A4  a config that is not a JSON object is a terminal task failure (never a
      crash, never a silent default), and an omitted config equals a NULL config.
  A5  AI_EMBED of a NULL input propagates SQL NULL: READY with a NULL result, no
      request, and the publication writes NULL rather than an empty vector.

These are source-shape guards, the same style as test_ai_column_dml_safety.py and
test_ai_column_worker_invariants.py: they never substitute sources and never need
a build.  The behavioural proof lives in the mysqltest suite
test_suite/ai_column/t/ai_column_argument_contract.test.
"""
from pathlib import Path
import re
import sys

ROOT = Path(__file__).resolve().parents[3]


def read(relative):
    return (ROOT / relative).read_text()


def section(source, start_marker, end_marker, label):
    """Return the text between two unique markers (fail loudly if they moved)."""
    if start_marker not in source:
        raise AssertionError(f'{label}: start marker %r not found' % start_marker)
    body = source.split(start_marker, 1)[1]
    if end_marker not in body:
        raise AssertionError(f'{label}: end marker %r not found' % end_marker)
    return body.split(end_marker, 1)[0]


resolver = read('src/sql/resolver/ddl/ob_ai_column_resolver.cpp')
task_h = read('src/observer/ai_service/ai_column/ob_ai_column_task.h')
task_sql = read('src/observer/ai_service/ai_column/ob_ai_column_task.cpp')
worker = read('src/observer/ai_service/ai_column/ob_ai_column_worker.cpp')
service = read('src/observer/ai_service/ai_column/ob_ai_column_service.cpp')
definition = read('src/share/schema/ob_ai_column_definition.h')
schema_def = read('src/share/inner_table/ob_inner_table_schema_def.py')

enqueue = section(task_sql, 'int AiColumnTaskSql::enqueue_task(',
                  'int AiColumnTaskSql::delete_tasks_of_row(', 'enqueue_task')
call_model = section(worker, 'int AiColumnWorker::call_model(',
                     'int AiColumnWorker::stage_success(', 'call_model')
decode = section(worker, 'int decode_input_envelope(',
                 '} // anonymous namespace', 'decode_input_envelope')
claimed = section(worker, 'int AiColumnWorker::handle_claimed_task(',
                  'int AiColumnWorker::decode_task_input(', 'handle_claimed_task')

violations = []


def check(condition, message):
    if not condition:
        violations.append(message)


# ------------------------------------------------ A1: no bare prompt expression
check('def.prompt_expr_' in enqueue,
      'A1: enqueue_task no longer materializes the prompt expression at all')
check('JSON_OBJECT(' in enqueue and 'AI_COLUMN_INPUT_KEY_PROMPT' in enqueue,
      'A1: enqueue_task must snapshot the prompt inside the JSON input envelope '
      '(JSON_OBJECT), so a NULL prompt can not violate the NOT NULL task column')
check(re.search(r'AI_COLUMN_INPUT_KEY_PROMPT.{0,300}?sql\.append\(\s*def\.prompt_expr_',
                enqueue, re.S) is not None,
      'A1: the prompt expression must be appended inside the JSON envelope literal')
check('decode_task_input' in worker and 'prompt_is_null_' in worker,
      'A1: the worker must own the NULL prompt outcome instead of the SQL layer')
check('prompt expression evaluated to NULL' in claimed,
      'A1: a NULL COMPLETE prompt must fail the task with a self-explanatory message')

# ------------------------------------------- A2: render only genuine object prompts
for marker in ('is_valid_prompt_object', 'replace_all_str_args_in_template'):
    check(marker in decode,
          'A2: the worker never calls %s on a JSON prompt object' % marker)
check(decode.index('is_valid_prompt_object') < decode.index('replace_all_str_args_in_template'),
      'A2: a prompt object must be validated before it is rendered')
check('J_STRING' in decode and 'get_str()' in decode,
      'A2: a plain string prompt must be taken verbatim, never parsed for a '
      'prompt-like shape')
check('call_completion(' in call_model, 'A2: the COMPLETE model call moved')
check('render' not in call_model.lower(),
      'A2: the prompt must be rendered by decode_task_input, not by call_model')

# ------------------------------------------------------ A3: config is carried
check(re.search(r'\bObString\s+config_expr_\s*;', definition) is not None,
      'A3: AIColumnDefinition must carry the config expression (config_expr_)')
check('META_KEY_CONFIG' in definition,
      'A3: the persisted definition payload must encode the config expression')
check(re.search(r'AI_COLUMN_CALL_COMPLETE\s*==\s*def\.call_type_', resolver) is not None,
      'A3: the resolver has no AI_COMPLETE third-argument branch')
check(re.search(r'collect_column_refs\(\s*third_node', resolver) is not None,
      'A3: the config expression must be collected as a dependency, so updating a '
      'config column re-enqueues the AI column')
check('AI_COLUMN_INPUT_KEY_CONFIG' in enqueue and 'def.config_expr_' in enqueue,
      'A3: the enqueuing statement must snapshot the config expression in the '
      'same user transaction')
check('config' in service and 'config_expr_' in service,
      'A3: the service must read the config expression from the persisted definition')
check('call_completion(prompt_copy, config' in call_model,
      'A3: the config must reach the model call (call_completion(prompt, config, ...))')

# ------------------------------------------- A4: invalid config is a hard failure
check('config must be a JSON object' in decode,
      'A4: a config that is not a JSON object must be reported explicitly')
check('J_NULL' in decode and 'input.config_ = NULL' in decode,
      'A4: an omitted config and a JSON null config must both mean "no config"')

# ----------------------------------------- A5: EMBED NULL propagates SQL NULL
check('stage_success(task, ObString(), affected_rows, true)' in claimed,
      'A5: AI_EMBED of a NULL input must stage READY with a SQL NULL result')
check('publish_nulls' in worker and 'result_is_null_ = staged' not in worker,
      'A5: the publish payload must carry the staged NULL flag')
check('result_is_null' in task_h,
      'A5: the staged task row must distinguish SQL NULL from an empty result')

# the catalog keeps the literal prompt expression of the DDL
check('prompt_expr = ' in schema_def or 'prompt_expr' in schema_def,
      'catalog: __all_ai_column must keep exposing prompt_expr (literal DDL text)')

if violations:
    for item in violations:
        print('FAILED ' + item)
    print('\nAI argument contract: %d violation(s) - see deepseek-ai-argument-review.md'
          % len(violations))
    sys.exit(1)

print('AI argument contract: 5 contracts passed (static only)')
