// SPDX-License-Identifier: MIT
// libshowplan - clean-room SQL Server showplan XML parser.
//
// Parses .sqlplan / .queryplan documents (Microsoft public schema,
// http://schemas.microsoft.com/sqlserver/2004/07/showplan) into strongly
// typed C++ trees. Pure data - no rendering, layout, or analysis here;
// those belong in the consuming application.
#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace showplan {

struct RuntimeStats {
    int thread = -1;
    int64_t actual_rows = 0;
    int64_t actual_rows_read = 0;
    int64_t actual_executions = 0;
    int64_t actual_logical_reads = 0;
    int64_t actual_physical_reads = 0;
    int64_t actual_read_aheads = 0;
    int64_t actual_lob_logical_reads = 0;
    int64_t actual_cpu_ms = 0;
    int64_t actual_elapsed_ms = 0;
    int64_t actual_rebinds = 0;
    int64_t actual_rewinds = 0;
    int64_t actual_batches = 0;
};

struct PlanNode {
    int node_id = -1;
    std::string physical_op;
    std::string logical_op;
    std::string database;
    std::string schema;
    std::string table;
    std::string index_name;
    std::string index_kind;
    std::string storage;
    std::string alias;
    std::string predicate;
    std::vector<std::string> output_list;

    double est_rows = 0.0;
    double est_rows_per_exec = 0.0;
    double est_executions = 1.0;
    double est_row_size_bytes = 0.0;
    double est_io_cost = 0.0;
    double est_cpu_cost = 0.0;
    double est_op_cost = 0.0;
    double est_subtree_cost = 0.0;
    bool parallel = false;
    bool is_lookup = false;
    // "Row" / "Batch". Empty on legacy plans that omit the attribute.
    std::string execution_mode;
    // Rows the operator will SCAN, vs est_rows which is rows RETURNED.
    // Sourced from IndexScan@EstimatedRowsRead.
    double est_rows_to_be_read = 0.0;
    bool partitioned = false;
    bool ordered = false;
    // Seek-time conditions - distinct from `predicate` which is the
    // residual filter applied after the seek.
    std::vector<std::string> seek_predicates;

    std::vector<RuntimeStats> runtimes;
    bool has_runtime = false;
    int64_t act_rows = 0;
    int64_t act_rows_read = 0;
    int64_t act_executions = 0;
    int64_t act_logical_reads = 0;
    int64_t act_physical_reads = 0;
    int64_t act_cpu_ms = 0;
    // Max across per-thread RunTimeCountersPerThread, NOT sum. The
    // wall-clock window is the longest-running thread.
    int64_t act_elapsed_ms = 0;
    int64_t act_rebinds = 0;
    int64_t act_rewinds = 0;
    int64_t act_batches = 0;

    std::vector<std::string> warnings;
    std::vector<std::unique_ptr<PlanNode>> children;
};

struct MissingIndex {
    double impact = 0.0;
    std::string database;
    std::string schema;
    std::string table;
    std::vector<std::string> equality_cols;
    std::vector<std::string> inequality_cols;
    std::vector<std::string> include_cols;
    std::string ddl;
};

struct PlanWarning {
    std::string kind;
    std::string detail;
};

struct Parameter {
    std::string name;
    std::string compiled_value;
    std::string runtime_value;
};

struct Statistic {
    std::string database;
    std::string schema;
    std::string table;
    std::string name;
    std::string last_update;
    int64_t modification_count = 0;
    double sampling_percent = 0.0;
};

// Memory quantities are KB; wait time is milliseconds. -1 means absent.
struct MemoryGrantInfo {
    int64_t desired_memory_kb = -1;
    int64_t granted_memory_kb = -1;
    int64_t grant_wait_time_ms = -1;
    int64_t max_used_memory_kb = -1;
    int64_t requested_memory_kb = -1;
    int64_t required_memory_kb = -1;
    int64_t serial_desired_memory_kb = -1;
    int64_t serial_required_memory_kb = -1;
};

struct OptimizerHardware {
    int64_t estimated_degree_of_parallelism = -1;
    int64_t estimated_available_memory_grant_kb = -1;
    int64_t estimated_pages_cached = -1;
};

struct ThreadReservation {
    int64_t node_id = -1;
    int64_t reserved_threads = -1;
};

struct ParallelThreadUsage {
    int64_t branches = -1;
    int64_t used_threads = -1;
    std::vector<ThreadReservation> reservations;
};

// Disengaged options were not supplied; false is a captured value.
struct StatementSetOptions {
    std::optional<bool> ansi_nulls;
    std::optional<bool> ansi_padding;
    std::optional<bool> ansi_warnings;
    std::optional<bool> arithabort;
    std::optional<bool> concat_null_yields_null;
    std::optional<bool> numeric_roundabort;
    std::optional<bool> quoted_identifier;
};

struct Statement {
    int statement_id = 0;
    // Index in Plan::statements of the nearest enclosing statement;
    // -1 for a batch-level statement. Statements are stored in preorder.
    int parent_statement_index = -1;
    // Source byte offsets supplied by ShowPlan; -1 when absent.
    int64_t statement_start_offset = -1;
    int64_t statement_end_offset = -1;
    // True for XML statement containers and procedure/cursor operations,
    // which need not have an operator tree.
    bool structural = false;
    std::string text;
    std::string stmt_type;
    double subtree_cost = 0.0;
    int compile_time_ms = 0;
    int compile_cpu_ms = 0;
    int compile_memory_kb = 0;
    int cached_plan_size_kb = 0;
    // Optional statement/query-plan metadata: -1 means absent, zero is valid.
    double est_rows = -1.0;
    int64_t degree_of_parallelism = -1;
    int64_t memory_grant_kb = -1;
    MemoryGrantInfo memory_grant;
    OptimizerHardware optimizer_hardware;
    ParallelThreadUsage parallel_threads;
    StatementSetOptions set_options;
    // QueryTimeStats uses milliseconds; -1 distinguishes missing from zero.
    int64_t query_cpu_ms = -1;
    int64_t query_elapsed_ms = -1;
    int64_t query_udf_cpu_ms = -1;
    int64_t query_udf_elapsed_ms = -1;
    // "TimeOut" / "MemoryLimitExceeded" → plan is likely suboptimal;
    // "GoodEnoughPlanFound" is benign; empty means full optimisation.
    std::string optm_early_abort_reason;
    std::string optm_level;
    // Default true matches SQL Server's behaviour when the attribute
    // is absent.
    bool retrieved_from_cache = true;
    // sys.objects.object_id of the emitting module, inherited through
    // enclosing statement blocks. StoredProc/UDF enters a new context;
    // 0 means no module identity was supplied (including ad-hoc batches).
    int64_t parent_object_id = 0;
    // Parsed operator trees may be shared by multiple captured invocations.
    // Consumers must treat the tree as read-only after parsing.
    std::shared_ptr<PlanNode> root;
    std::vector<MissingIndex> missing_indexes;
    std::vector<PlanWarning> warnings;
    std::vector<Parameter> parameters;
    std::vector<Statistic> stats;
};

struct Plan {
    std::string server_version;
    std::string build;
    std::string source_path;
    std::vector<Statement> statements;
};

class ParseError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

// Parse a ShowPlanXML document. Throws ParseError on malformed input or
// when the document is not a ShowPlanXML root.
Plan parse_xml(std::string_view xml);

// Read and parse a .sqlplan / .queryplan file. Throws ParseError on I/O
// or parse failure.
Plan parse_file(const std::string& path);

// Returns the most-referenced database name across all RelOps in the
// plan, with surrounding [brackets] stripped. Empty if no table references.
std::string predominant_database(const Plan& plan);

}  // namespace showplan
