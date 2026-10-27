#!/usr/bin/env python3
"""Static call-site guard; SQL behavior is covered by ai_column_quoted_key.test.

This is deliberately not a substitute for the kernel SQL regression. It prevents
reintroducing raw schema identifier interpolation into the AI row-key predicate.
"""
from pathlib import Path
import re

root = Path(__file__).resolve().parents[3]
source = (root / 'src/sql/engine/dml/ob_table_modify_op.cpp').read_text()
body = source.split('int ObTableModifyOp::ai_column_build_rowkey(', 1)[1].split(
    'int ObTableModifyOp::ai_column_collect_row(', 1)[0]
unsafe = re.search(r'databuff_printf\([^;]*key\.column_name_\.ptr\(\)', body)
assert unsafe is None, 'AI row-key predicate interpolates an unescaped identifier'
assert 'generate_new_name_with_escape_character(' in body, 'AI row-key identifier must use kernel escaping'
assert 'escaped_key_name.ptr()' in body, 'escaped identifier must reach the predicate formatter'
prepare = source.split('int ObTableModifyOp::ai_column_prepare_table(', 1)[1].split(
    'int ObTableModifyOp::ai_column_build_rowkey(', 1)[0]
assert 'const uint64_t table_id = dml_ctdef.das_base_ctdef_.index_tid_;' in prepare, (
    'AI schema lookup must use the physical table ID, not the negative SQL alias ID')
print('AI DML identifier/physical-ID call-site guards: 4 passed (static only)')
