// SPDX-License-Identifier: MIT
#include "showplan/showplan.hpp"
#include "showplan/profile.hpp"

#include "pugixml.hpp"

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <sstream>
#include <utility>
#include <vector>

namespace showplan {

namespace {

double attr_d(const pugi::xml_node& n, const char* name, double def = 0.0) {
    auto a = n.attribute(name);
    return a ? a.as_double(def) : def;
}

int64_t attr_i(const pugi::xml_node& n, const char* name, int64_t def = 0) {
    auto a = n.attribute(name);
    return a ? a.as_llong(def) : def;
}

std::string attr_s(const pugi::xml_node& n, const char* name) {
    auto a = n.attribute(name);
    return a ? std::string(a.value()) : std::string();
}

bool attr_b(const pugi::xml_node& n, const char* name, bool def = false) {
    auto a = n.attribute(name);
    if (!a) return def;
    const char* v = a.value();
    return v && (v[0] == '1' || v[0] == 't' || v[0] == 'T');
}

std::string unbracket(std::string s) {
    if (s.size() >= 2 && s.front() == '[' && s.back() == ']') {
        return s.substr(1, s.size() - 2);
    }
    return s;
}

std::string format_col_ref(const pugi::xml_node& cr) {
    std::string db = attr_s(cr, "Database");
    std::string sc = attr_s(cr, "Schema");
    std::string tb = attr_s(cr, "Table");
    std::string col = attr_s(cr, "Column");
    std::string alias = attr_s(cr, "Alias");

    std::string out;
    auto add = [&](const std::string& s) {
        if (s.empty()) return;
        if (!out.empty()) out += '.';
        out += s;
    };
    add(db);
    add(sc);
    add(tb);
    add(col);
    if (!alias.empty()) {
        out += " AS ";
        out += alias;
    }
    return out;
}

void collect_child_relops(const pugi::xml_node& parent,
                          std::vector<pugi::xml_node>& out) {
    for (auto child : parent.children()) {
        if (child.type() != pugi::node_element) continue;
        if (std::strcmp(child.name(), "RelOp") == 0) {
            out.push_back(child);
        } else {
            collect_child_relops(child, out);
        }
    }
}

pugi::xml_node find_object_no_relop(const pugi::xml_node& root) {
    for (auto child : root.children()) {
        if (child.type() != pugi::node_element) continue;
        const char* n = child.name();
        if (std::strcmp(n, "RelOp") == 0) continue;
        if (std::strcmp(n, "Object") == 0) return child;
        auto inner = find_object_no_relop(child);
        if (inner) return inner;
    }
    return {};
}

pugi::xml_node find_first_object(const pugi::xml_node& relop) {
    for (auto child : relop.children()) {
        if (child.type() != pugi::node_element) continue;
        const char* n = child.name();
        if (std::strcmp(n, "OutputList") == 0) continue;
        if (std::strcmp(n, "RunTimeInformation") == 0) continue;
        if (std::strcmp(n, "Warnings") == 0) continue;
        if (std::strcmp(n, "MemoryFractions") == 0) continue;
        if (std::strcmp(n, "RelOp") == 0) continue;
        if (auto found = find_object_no_relop(child)) return found;
    }
    return {};
}

std::string read_predicate(const pugi::xml_node& relop) {
    auto pred = relop.child("Predicate");
    if (!pred) return {};
    auto scalar = pred.child("ScalarOperator");
    if (!scalar) return {};
    return attr_s(scalar, "ScalarString");
}

void parse_runtime(const pugi::xml_node& relop, PlanNode& node) {
    auto rti = relop.child("RunTimeInformation");
    if (!rti) return;
    node.has_runtime = true;
    for (auto rt : rti.children("RunTimeCountersPerThread")) {
        RuntimeStats s;
        s.thread = static_cast<int>(attr_i(rt, "Thread"));
        s.actual_rows = attr_i(rt, "ActualRows");
        s.actual_rows_read = attr_i(rt, "ActualRowsRead");
        s.actual_executions = attr_i(rt, "ActualExecutions");
        s.actual_logical_reads = attr_i(rt, "ActualLogicalReads");
        s.actual_physical_reads = attr_i(rt, "ActualPhysicalReads");
        s.actual_read_aheads = attr_i(rt, "ActualReadAheads");
        s.actual_lob_logical_reads = attr_i(rt, "ActualLobLogicalReads");
        s.actual_cpu_ms = attr_i(rt, "ActualCPUms");
        s.actual_elapsed_ms = attr_i(rt, "ActualElapsedms");
        s.actual_rebinds = attr_i(rt, "ActualRebinds");
        s.actual_rewinds = attr_i(rt, "ActualRewinds");
        s.actual_batches = attr_i(rt, "ActualBatches");
        node.runtimes.push_back(s);
        node.act_rows += s.actual_rows;
        node.act_rows_read += s.actual_rows_read;
        node.act_executions += s.actual_executions;
        node.act_logical_reads += s.actual_logical_reads;
        node.act_physical_reads += s.actual_physical_reads;
        node.act_cpu_ms += s.actual_cpu_ms;
        node.act_rebinds += s.actual_rebinds;
        node.act_rewinds += s.actual_rewinds;
        node.act_batches += s.actual_batches;
        if (s.actual_elapsed_ms > node.act_elapsed_ms) {
            node.act_elapsed_ms = s.actual_elapsed_ms;
        }
    }
}

// Render a <Prefix> / <StartRange> / <EndRange> as one human-friendly
// line: "<columns> <op> <expressions>" - same idea SSMS uses. The XML
// shape is:
//   <Prefix | StartRange | EndRange ScanType="EQ|GT|GE|LT|LE">
//     <RangeColumns><ColumnReference .../>+</RangeColumns>
//     <RangeExpressions><ScalarOperator ScalarString="..."/>+</RangeExpressions>
//   </Prefix>
// Multi-column composite seeks pair columns with expressions positionally
// (col[0] op expr[0], col[1] op expr[1], ...).
// Returns one entry per (col op expr) pair so the tooltip can
// render a multi-line bulleted list - multi-column composite seeks
// otherwise produce a single comma-joined line that's hard to scan.
std::vector<std::string> format_seek_range_lines(const pugi::xml_node& range) {
    const char* scan_type = range.attribute("ScanType").value();
    const char* op = " = ";
    if (scan_type && *scan_type) {
        if      (!std::strcmp(scan_type, "EQ"))  op = " = ";
        else if (!std::strcmp(scan_type, "GT"))  op = " > ";
        else if (!std::strcmp(scan_type, "GE"))  op = " >= ";
        else if (!std::strcmp(scan_type, "LT"))  op = " < ";
        else if (!std::strcmp(scan_type, "LE"))  op = " <= ";
        else if (!std::strcmp(scan_type, "NE"))  op = " <> ";
    }
    std::vector<std::string> cols;
    if (auto rc = range.child("RangeColumns")) {
        for (auto cr : rc.children("ColumnReference")) {
            cols.push_back(format_col_ref(cr));
        }
    }
    std::vector<std::string> exprs;
    if (auto re = range.child("RangeExpressions")) {
        for (auto so : re.children("ScalarOperator")) {
            std::string s = attr_s(so, "ScalarString");
            if (s.empty()) s = "?";
            exprs.push_back(std::move(s));
        }
    }
    std::vector<std::string> out;
    const size_t n = std::max(cols.size(), exprs.size());
    out.reserve(n);
    for (size_t i = 0; i < n; ++i) {
        std::string line;
        line  = i < cols.size()  ? cols[i]  : std::string("?");
        line += op;
        line += i < exprs.size() ? exprs[i] : std::string("?");
        out.push_back(std::move(line));
    }
    return out;
}

std::vector<std::string> seek_exprs_vec(const pugi::xml_node& range) {
    std::vector<std::string> out;
    if (auto re = range.child("RangeExpressions")) {
        for (auto so : re.children("ScalarOperator")) {
            std::string s = attr_s(so, "ScalarString");
            if (s.empty()) s = "?";
            out.push_back(std::move(s));
        }
    }
    return out;
}

std::vector<std::string> seek_cols_vec(const pugi::xml_node& range) {
    std::vector<std::string> out;
    if (auto rc = range.child("RangeColumns")) {
        for (auto cr : rc.children("ColumnReference")) {
            out.push_back(format_col_ref(cr));
        }
    }
    return out;
}

// Pull <SeekPredicates> off an IndexScan / IndexSeek body child. Each
// <SeekPredicateNew> holds one set of seek keys: Prefix (equality),
// StartRange + EndRange (a range), or just one of those. Display
// rules - one entry per column so the tooltip can render bullets:
//   - Prefix             →  "<col> = <expr>"
//   - StartRange+EndRange → "<col>: <start_expr> - <end_expr>"
//     (per-column line, partition-function-friendly - pairs each
//     range column with its own boundary pair.)
//   - StartRange alone   →  "<col> > <expr>" (operator preserved)
//   - EndRange alone     →  "<col> < <expr>" (operator preserved)
// Don't recurse into ScalarOperator subtrees, those are sub-pieces of
// a single expression, not separate predicates.
void collect_seek_predicates(const pugi::xml_node& container,
                             PlanNode& node) {
    auto sps = container.child("SeekPredicates");
    if (!sps) return;
    for (auto spn : sps.children()) {
        if (spn.type() != pugi::node_element) continue;
        auto keys = spn.child("SeekKeys");
        // Some legacy plans put Prefix/StartRange/EndRange directly
        // under SeekPredicateNew (no SeekKeys wrapper) - accept both.
        const pugi::xml_node& src = keys ? keys : spn;
        pugi::xml_node start_range, end_range;
        for (auto r : src.children()) {
            if (r.type() != pugi::node_element) continue;
            const char* nm = r.name();
            if      (!std::strcmp(nm, "StartRange")) start_range = r;
            else if (!std::strcmp(nm, "EndRange"))   end_range   = r;
            else if (!std::strcmp(nm, "Prefix")) {
                for (auto& line : format_seek_range_lines(r)) {
                    node.seek_predicates.push_back(std::move(line));
                }
            }
        }
        // Range collapse: one line per column "<col>: <start> - <end>"
        // instead of two ("Start: ...", "End: ..."). Pairs cols and
        // exprs positionally so a 3-key range emits 3 bullets.
        if (start_range && end_range) {
            auto cols = seek_cols_vec(start_range);
            if (cols.empty()) cols = seek_cols_vec(end_range);
            auto starts = seek_exprs_vec(start_range);
            auto ends   = seek_exprs_vec(end_range);
            const size_t n = std::max({cols.size(),
                                       starts.size(),
                                       ends.size()});
            for (size_t i = 0; i < n; ++i) {
                std::string line;
                if (i < cols.size() && !cols[i].empty()) {
                    line = cols[i];
                    line += ": ";
                }
                line += i < starts.size() ? starts[i] : std::string("?");
                line += " - ";
                line += i < ends.size()   ? ends[i]   : std::string("?");
                node.seek_predicates.push_back(std::move(line));
            }
        } else if (start_range) {
            for (auto& line : format_seek_range_lines(start_range)) {
                node.seek_predicates.push_back(std::move(line));
            }
        } else if (end_range) {
            for (auto& line : format_seek_range_lines(end_range)) {
                node.seek_predicates.push_back(std::move(line));
            }
        }
    }
}

void parse_relop_warnings(const pugi::xml_node& relop, PlanNode& node) {
    auto w = relop.child("Warnings");
    if (!w) return;
    for (auto child : w.children()) {
        if (child.type() != pugi::node_element) continue;
        node.warnings.emplace_back(child.name());
    }
}

void parse_output_list(const pugi::xml_node& relop, PlanNode& node) {
    auto ol = relop.child("OutputList");
    if (!ol) return;
    for (auto cr : ol.children("ColumnReference")) {
        node.output_list.push_back(format_col_ref(cr));
    }
}

std::unique_ptr<PlanNode> parse_relop(const pugi::xml_node& relop) {
    auto node = std::make_unique<PlanNode>();
    node->node_id = static_cast<int>(attr_i(relop, "NodeId", -1));
    node->physical_op = attr_s(relop, "PhysicalOp");
    node->logical_op = attr_s(relop, "LogicalOp");
    node->est_rows = attr_d(relop, "EstimateRows");
    node->est_rows_per_exec = attr_d(relop, "EstimatedRowsRead",
                                     node->est_rows);
    node->est_executions = attr_d(relop, "EstimateRebinds", 0.0) +
                           attr_d(relop, "EstimateRewinds", 0.0) + 1.0;
    node->est_row_size_bytes = attr_d(relop, "AvgRowSize");
    node->est_io_cost = attr_d(relop, "EstimateIO");
    node->est_cpu_cost = attr_d(relop, "EstimateCPU");
    node->est_op_cost = attr_d(relop, "EstimatedOperatorCost",
                               node->est_io_cost + node->est_cpu_cost);
    node->est_subtree_cost = attr_d(relop, "EstimatedTotalSubtreeCost");
    node->parallel = attr_b(relop, "Parallel");
    // EstimatedExecutionMode is the plan-time choice; ExecutionMode
    // appears on runtime-augmented plans.
    node->execution_mode = attr_s(relop, "EstimatedExecutionMode");
    if (node->execution_mode.empty()) {
        node->execution_mode = attr_s(relop, "ExecutionMode");
    }
    node->partitioned = attr_b(relop, "Partitioned");

    parse_output_list(relop, *node);
    parse_runtime(relop, *node);
    parse_relop_warnings(relop, *node);

    node->predicate = read_predicate(relop);

    if (node->physical_op == "RID Lookup" ||
        node->physical_op == "Key Lookup") {
        node->is_lookup = true;
    } else {
        for (auto child : relop.children()) {
            if (child.type() != pugi::node_element) continue;
            if (std::strcmp(child.name(), "RelOp") == 0) continue;
            auto is_ix = child.child("IndexScan");
            if (is_ix && attr_b(is_ix, "Lookup")) {
                node->is_lookup = true;
                break;
            }
        }
    }

    if (auto obj = find_first_object(relop)) {
        node->database   = unbracket(attr_s(obj, "Database"));
        node->schema     = unbracket(attr_s(obj, "Schema"));
        node->table      = unbracket(attr_s(obj, "Table"));
        node->index_name = unbracket(attr_s(obj, "Index"));
        node->index_kind = attr_s(obj, "IndexKind");
        node->storage    = attr_s(obj, "Storage");
        node->alias      = unbracket(attr_s(obj, "Alias"));
    }

    // Operator-specific extras live on the body child of RelOp
    // (IndexScan / IndexSeek / TableScan / NestedLoops / Sort / ...).
    for (auto child : relop.children()) {
        if (child.type() != pugi::node_element) continue;
        const char* name = child.name();
        if (std::strcmp(name, "RelOp") == 0) continue;
        // EstimatedRowsRead + Ordered live on IndexScan / IndexSeek.
        double err = attr_d(child, "EstimatedRowsRead", 0.0);
        if (err > 0.0 && node->est_rows_to_be_read == 0.0) {
            node->est_rows_to_be_read = err;
        }
        if (auto attr = child.attribute("Ordered"); attr) {
            node->ordered = attr_b(child, "Ordered");
        }
        collect_seek_predicates(child, *node);
    }

    std::vector<pugi::xml_node> child_ops;
    collect_child_relops(relop, child_ops);
    node->children.reserve(child_ops.size());
    for (auto& co : child_ops) {
        node->children.push_back(parse_relop(co));
    }
    return node;
}

void parse_missing_indexes(const pugi::xml_node& qp, Statement& stmt) {
    auto mis = qp.child("MissingIndexes");
    if (!mis) return;
    for (auto grp : mis.children("MissingIndexGroup")) {
        double impact = attr_d(grp, "Impact");
        for (auto mi : grp.children("MissingIndex")) {
            MissingIndex idx;
            idx.impact = impact;
            idx.database = attr_s(mi, "Database");
            idx.schema = attr_s(mi, "Schema");
            idx.table = attr_s(mi, "Table");
            for (auto cg : mi.children("ColumnGroup")) {
                std::string usage = attr_s(cg, "Usage");
                std::vector<std::string>* dest = nullptr;
                if (usage == "EQUALITY") dest = &idx.equality_cols;
                else if (usage == "INEQUALITY") dest = &idx.inequality_cols;
                else if (usage == "INCLUDE") dest = &idx.include_cols;
                if (!dest) continue;
                for (auto col : cg.children("Column")) {
                    dest->emplace_back(attr_s(col, "Name"));
                }
            }
            std::ostringstream ddl;
            ddl << "CREATE NONCLUSTERED INDEX [<name>] ON "
                << idx.database << '.' << idx.schema << '.' << idx.table
                << " (";
            bool first = true;
            auto emit = [&](const std::vector<std::string>& cols) {
                for (auto& c : cols) {
                    if (!first) ddl << ", ";
                    ddl << c;
                    first = false;
                }
            };
            emit(idx.equality_cols);
            emit(idx.inequality_cols);
            ddl << ')';
            if (!idx.include_cols.empty()) {
                ddl << " INCLUDE (";
                first = true;
                emit(idx.include_cols);
                ddl << ')';
            }
            ddl << ';';
            idx.ddl = ddl.str();
            stmt.missing_indexes.push_back(std::move(idx));
        }
    }
}

void parse_query_warnings(const pugi::xml_node& qp, Statement& stmt) {
    auto w = qp.child("Warnings");
    if (!w) return;
    for (auto child : w.children()) {
        if (child.type() != pugi::node_element) continue;
        PlanWarning pw;
        pw.kind = child.name();
        std::ostringstream detail;
        bool any = false;
        for (auto attr : child.attributes()) {
            if (any) detail << ", ";
            detail << attr.name() << '=' << attr.value();
            any = true;
        }
        if (std::strcmp(child.name(), "ColumnsWithNoStatistics") == 0) {
            for (auto cr : child.children("ColumnReference")) {
                if (any) detail << "; ";
                detail << format_col_ref(cr);
                any = true;
            }
        }
        pw.detail = detail.str();
        stmt.warnings.push_back(std::move(pw));
    }
}

void parse_parameters(const pugi::xml_node& qp, Statement& stmt) {
    auto pl = qp.child("ParameterList");
    if (!pl) return;
    for (auto cr : pl.children("ColumnReference")) {
        Parameter p;
        p.name = attr_s(cr, "Column");
        p.compiled_value = attr_s(cr, "ParameterCompiledValue");
        p.runtime_value = attr_s(cr, "ParameterRuntimeValue");
        stmt.parameters.push_back(std::move(p));
    }
}

void parse_statistics(const pugi::xml_node& qp, Statement& stmt) {
    auto usage = qp.child("OptimizerStatsUsage");
    if (!usage) return;
    for (auto si : usage.children("StatisticsInfo")) {
        Statistic st;
        st.database = unbracket(attr_s(si, "Database"));
        st.schema   = unbracket(attr_s(si, "Schema"));
        st.table    = unbracket(attr_s(si, "Table"));
        st.name     = unbracket(attr_s(si, "Statistics"));
        st.last_update = attr_s(si, "LastUpdate");
        st.modification_count = attr_i(si, "ModificationCount");
        st.sampling_percent = attr_d(si, "SamplingPercent");
        stmt.stats.push_back(std::move(st));
    }
}

void parse_statement(const pugi::xml_node& stmt_xml, Statement& stmt,
                     int64_t module_id) {
    stmt.statement_id = static_cast<int>(attr_i(stmt_xml, "StatementId"));
    stmt.text = attr_s(stmt_xml, "StatementText");
    stmt.stmt_type = attr_s(stmt_xml, "StatementType");
    stmt.subtree_cost = attr_d(stmt_xml, "StatementSubTreeCost");
    stmt.optm_early_abort_reason =
        attr_s(stmt_xml, "StatementOptmEarlyAbortReason");
    stmt.optm_level = attr_s(stmt_xml, "StatementOptmLevel");
    // Default true matches SQL Server when the attribute is absent.
    if (auto a = stmt_xml.attribute("RetrievedFromCache")) {
        stmt.retrieved_from_cache = attr_b(stmt_xml, "RetrievedFromCache");
    }
    stmt.parent_object_id = attr_i(stmt_xml, "ParentObjectId", module_id);
    stmt.statement_start_offset = attr_i(stmt_xml, "StatementStartOffset", -1);
    stmt.statement_end_offset = attr_i(stmt_xml, "StatementEndOffset", -1);

    auto qp = stmt_xml.child("QueryPlan");
    if (!qp) qp = stmt_xml.child("Condition").child("QueryPlan");
    if (!qp) return;

    stmt.cached_plan_size_kb = static_cast<int>(attr_i(qp, "CachedPlanSize"));
    stmt.compile_time_ms = static_cast<int>(attr_i(qp, "CompileTime"));
    stmt.compile_cpu_ms = static_cast<int>(attr_i(qp, "CompileCPU"));
    stmt.compile_memory_kb = static_cast<int>(attr_i(qp, "CompileMemory"));
    if (auto timing = qp.child("QueryTimeStats")) {
        stmt.query_cpu_ms = attr_i(timing, "CpuTime", -1);
        stmt.query_elapsed_ms = attr_i(timing, "ElapsedTime", -1);
        stmt.query_udf_cpu_ms = attr_i(timing, "UdfCpuTime", -1);
        stmt.query_udf_elapsed_ms = attr_i(timing, "UdfElapsedTime", -1);
    }

    parse_missing_indexes(qp, stmt);
    parse_query_warnings(qp, stmt);
    parse_parameters(qp, stmt);
    parse_statistics(qp, stmt);

    auto root = qp.child("RelOp");
    if (root) stmt.root = parse_relop(root);
}

bool is_statement(const pugi::xml_node& node) {
    const char* name = node.name();
    return std::strcmp(name, "StmtSimple") == 0 ||
           std::strcmp(name, "StmtCond") == 0 ||
           std::strcmp(name, "StmtCursor") == 0 ||
           std::strcmp(name, "StmtReceive") == 0 ||
           std::strcmp(name, "StmtUseDb") == 0;
}

void walk_statements(const pugi::xml_node& container, Plan& plan,
                     int parent = -1, int64_t module_id = 0) {
    for (auto child : container.children()) {
        if (child.type() != pugi::node_element) continue;
        const char* name = child.name();
        // QueryPlan is parsed exactly once by its owning statement. In
        // particular, an operator's Object is never a procedure context.
        if (std::strcmp(name, "QueryPlan") == 0 ||
            std::strcmp(name, "RelOp") == 0) continue;

        const bool procedure = std::strcmp(name, "StoredProc") == 0 ||
                               std::strcmp(name, "UDF") == 0;
        const bool operation = std::strcmp(name, "Operation") == 0 &&
            (std::strcmp(container.name(), "CursorPlan") == 0 ||
             std::strcmp(container.name(), "ReceivePlan") == 0);
        if (is_statement(child) || procedure || operation) {
            Statement stmt;
            // A call enters a new emitting module. ProcName is not an
            // object ID, and the caller's module must not leak into it.
            parse_statement(child, stmt, procedure ? 0 : module_id);
            stmt.parent_statement_index = parent;
            stmt.structural = std::strcmp(name, "StmtSimple") != 0 &&
                              std::strcmp(name, "StmtUseDb") != 0;
            if (child.child("StoredProc") || child.child("UDF"))
                stmt.structural = true;
            if (stmt.structural) {
                if (stmt.stmt_type.empty()) stmt.stmt_type = name;
                if (stmt.text.empty()) {
                    if (procedure) stmt.text = attr_s(child, "ProcName");
                    else if (operation) stmt.text = attr_s(child, "OperationType");
                    if (stmt.text.empty()) stmt.text = stmt.stmt_type;
                }
            }
            const int index = static_cast<int>(plan.statements.size());
            const int64_t child_module = stmt.parent_object_id;
            plan.statements.push_back(std::move(stmt));
            walk_statements(child, plan, index, child_module);
        } else {
            // Statements, Then, Else and Condition are transparent XML
            // blocks: ancestry follows statements, not SQL nest levels.
            walk_statements(child, plan, parent, module_id);
        }
    }
}

void collect_databases(const PlanNode* n,
                       std::vector<std::pair<std::string, int>>& counts) {
    if (!n) return;
    if (!n->database.empty()) {
        bool found = false;
        for (auto& kv : counts) {
            if (kv.first == n->database) { kv.second++; found = true; break; }
        }
        if (!found) counts.push_back({n->database, 1});
    }
    for (auto& c : n->children) collect_databases(c.get(), counts);
}

}  // namespace

Plan parse_xml(std::string_view xml) {
    SP_ZONE_N("showplan::parse_xml");
    pugi::xml_document doc;
    auto load = doc.load_buffer(xml.data(), xml.size(),
                                pugi::parse_default,
                                pugi::encoding_auto);
    if (!load) {
        throw ParseError(std::string("XML parse error: ") + load.description());
    }
    auto root = doc.child("ShowPlanXML");
    if (!root) {
        throw ParseError("Document is not a ShowPlanXML");
    }
    Plan plan;
    plan.build = attr_s(root, "Build");
    plan.server_version = attr_s(root, "Version");
    auto bs = root.child("BatchSequence");
    if (bs) walk_statements(bs, plan);
    if (plan.statements.empty()) {
        throw ParseError("No statements found in plan");
    }
    return plan;
}

Plan parse_file(const std::string& path) {
    // u8path routes through _wfopen on MSVC so Unicode paths (Cyrillic,
    // CJK, anything outside the system ACP) open correctly on Windows.
    std::ifstream f(std::filesystem::u8path(path), std::ios::binary);
    if (!f) {
        throw ParseError("Cannot open file: " + path + " (" +
                         std::strerror(errno) + ')');
    }
    std::ostringstream buf;
    buf << f.rdbuf();
    auto s = buf.str();
    Plan plan = parse_xml(s);
    plan.source_path = path;
    return plan;
}

std::string predominant_database(const Plan& plan) {
    std::vector<std::pair<std::string, int>> counts;
    for (auto& s : plan.statements) collect_databases(s.root.get(), counts);
    std::string best;
    int best_n = 0;
    for (auto& kv : counts) {
        if (kv.second > best_n) { best_n = kv.second; best = kv.first; }
    }
    return best;
}

}  // namespace showplan
