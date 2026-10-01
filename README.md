# libshowplan

Clean-room SQL Server showplan XML parser in C++17.

Parses `.sqlplan` / `.queryplan` documents (Microsoft's public showplan schema) into strongly typed C++ trees. Pure data. No rendering, layout, or analysis. Bring your own UI / heuristics.

## Status

The parser retains the statement hierarchy as well as operator trees. Consumers that concatenate or reorder parsed statements must preserve their parent links.

## Build

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
ctest --test-dir build --output-on-failure
```

## Use from CMake

```cmake
add_subdirectory(extern/libshowplan)   # or FetchContent / submodule
target_link_libraries(myapp PRIVATE showplan::showplan)
```

```cpp
#include <showplan/showplan.hpp>

try {
    auto plan = showplan::parse_file("query.sqlplan");
    for (const auto& stmt : plan.statements) {
        if (!stmt.root) continue; // structural and non-query statements
        // walk stmt.root->children recursively (read-only)
    }
} catch (const showplan::ParseError& e) {
    // bad XML, not a ShowPlanXML document, or I/O error
}
```

## What's parsed

### Statements and hierarchy
- `Plan::statements` is preorder across batches. `parent_statement_index` is the nearest enclosing statement's index in that same vector, or `-1` for a batch-level row. It is independent of `StatementId` and SQL execution nest level. Rebase parent indexes when concatenating plans, and remap them when reordering.
- Retains `<StmtSimple>`, `<StmtCond>`, `<StmtCursor>`, `<StmtReceive>`, and `<StmtUseDb>`, plus real `<StoredProc>` / `<UDF>` contexts and cursor/receive `<Operation>` rows. `<Statements>`, `<Then>`, `<Else>`, and `<Condition>` blocks preserve ancestry without introducing invented statements.
- `structural` marks containers and procedure/operation rows. Labels preserve `StatementText`, falling back to `ProcName`, `OperationType`, or the XML statement type; the parser never manufactures `EXEC` text.
- `StatementId`, `StatementText`, `StatementType`, `StatementSubTreeCost` are preserved when supplied. Costs and runtime are not borrowed from descendants.
- `statement_start_offset` / `statement_end_offset` preserve `StatementStartOffset` / `StatementEndOffset` extensions when supplied; `-1` means absent. The public schema does not promise these attributes.
- Conditional roots come from `Condition/QueryPlan`; cursor/receive operation roots stay on their individual operation rows. Operator XML is never traversed as statements. `root` may be null, and is a `shared_ptr<PlanNode>` so captured invocations can share an operator tree without copying it. Consumers must treat parsed trees as read-only.
- `compile_time_ms`, `compile_cpu_ms`, `compile_memory_kb`, `cached_plan_size_kb` from `<QueryPlan>`
- `optm_early_abort_reason` (`TimeOut` / `MemoryLimitExceeded` / `GoodEnoughPlanFound` / empty)
- `optm_level` (`TRIVIAL` / `FULL`)
- `retrieved_from_cache` (default `true` when the attribute is absent)
- `parent_object_id`: supplied `ParentObjectId`, inherited through enclosing statement blocks when absent. Entering `<StoredProc>` or `<UDF>` starts a new module context rather than inheriting the caller's object ID. Explicit IDs on those contexts are honored, but `ProcName` alone cannot identify an object numerically: the [public ShowPlan schema](https://schemas.microsoft.com/sqlserver/2004/07/showplan/sql2022/showplanxml.xsd) does not define a `ProcID` attribute. `0` means no module identity was supplied, including ad-hoc batches. Operator `<Object>` IDs are not emitting-module IDs.

### Operator tree (`<RelOp>`)
- `PhysicalOp`, `LogicalOp`, `parallel`, `is_lookup` (Key/RID Lookup)
- Estimates: `est_rows`, `est_rows_per_exec`, `est_executions`, `est_row_size_bytes`, `est_io_cost`, `est_cpu_cost`, `est_op_cost`, `est_subtree_cost`
- `execution_mode`: `Row` / `Batch` (Estimated or runtime variant)
- `est_rows_to_be_read`: optimizer-projected SCAN rows (distinct from `est_rows` which is rows RETURNED)
- `partitioned` (RelOp attribute), `ordered` (IndexScan attribute)
- `seek_predicates`: pre-formatted human-readable strings extracted from `<SeekPredicates>`. Equality prefixes render as `"<col> = <expr>"`; paired `StartRange` + `EndRange` collapse into `"<col>: <start> - <end>"` (saves ~50% of lines on range-partition seeks); standalone bounds keep their operator (`>`, `<`, `>=`, `<=`).
- `predicate`: residual filter expression
- `output_list`: fully-qualified column references
- `warnings`: per-operator `<Warnings>` children
- `Object` references → `database` / `schema` / `table` / `index_name` / `index_kind` / `storage` / `alias`

### Runtime stats (actual plans, `<RunTimeInformation>`)
- Per thread: `actual_rows`, `actual_rows_read`, `actual_executions`, `actual_logical_reads`, `actual_physical_reads`, `actual_read_aheads`, `actual_lob_logical_reads`, `actual_cpu_ms`, `actual_elapsed_ms`, `actual_rebinds`, `actual_rewinds`, `actual_batches`
- Aggregated onto the `PlanNode`: `act_rows`, `act_rows_read`, `act_executions`, `act_logical_reads`, `act_physical_reads`, `act_cpu_ms`, `act_elapsed_ms` (max across threads, not sum), `act_rebinds`, `act_rewinds`, `act_batches`
- `has_runtime` flag distinguishes actual plans from estimated

### Plan-level
- `MissingIndexes` with generated `CREATE INDEX` DDL
- `Warnings` (plan-level and per-operator)
- `ParameterList` (compile-time and runtime values)
- `OptimizerStatsUsage` / `StatisticsInfo`
- `predominant_database(plan)` helper: most-referenced database across all RelOps

### Cross-platform path handling
`parse_file()` routes through `std::filesystem::u8path` so UTF-8 paths with characters outside the system ACP (Cyrillic, CJK, accented filenames) open correctly on Windows.

## License

MIT. Vendors pugixml (MIT) under `third_party/pugixml/`.
