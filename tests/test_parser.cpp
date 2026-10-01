// SPDX-License-Identifier: MIT
// Stand-alone smoke test. Parses the file at argv[1] and dumps a summary.
// Exit code: 0 on success, 1 on parse failure, 2 on misuse.

#include "showplan/showplan.hpp"

#include <cstdio>

static int check_structure() {
    int failures = 0;
    auto require = [&](bool ok, const char* message) {
        if (!ok) {
            std::fprintf(stderr, "statement contract: %s\n", message);
            ++failures;
        }
    };
    // Public ShowPlan statement shapes, with source offsets carried by
    // producers that supply them. No SQL Server connection is needed.
    const auto plan = showplan::parse_xml(R"xml(
<ShowPlanXML Version="1" Build="test"><BatchSequence>
  <Batch><Statements>
    <StmtSimple StatementId="1" StatementText="EXEC dbo.outer_proc"
                StatementType="EXECUTE" ParentObjectId="91">
      <StoredProc ProcName="[dbo].[outer_proc]"><Statements>
        <StmtCond StatementId="2" StatementText="IF @value = 1"
                  StatementType="COND" ParentObjectId="42"
                  StatementStartOffset="100" StatementEndOffset="180">
          <Condition><QueryPlan CompileTime="3">
            <RelOp NodeId="10" PhysicalOp="Constant Scan" LogicalOp="Constant Scan"
                   EstimatedTotalSubtreeCost="0.01"/>
          </QueryPlan></Condition>
          <Then><Statements>
            <StmtSimple StatementId="3" StatementText="SELECT 1"
                        StatementType="SELECT" StatementSubTreeCost="0.25">
              <QueryPlan CompileTime="7">
                <RelOp NodeId="11" PhysicalOp="Compute Scalar" LogicalOp="Compute Scalar">
                  <ComputeScalar><RelOp NodeId="12" PhysicalOp="Constant Scan"
                                        LogicalOp="Constant Scan"/></ComputeScalar>
                </RelOp>
              </QueryPlan>
            </StmtSimple>
            <StmtCond StatementId="4" StatementText="WHILE @value &gt; 0"
                      StatementType="WHILE">
              <Condition/>
              <Then><Statements>
                <StmtSimple StatementId="5" StatementText="SET @value = 0"
                            StatementType="ASSIGN"/>
              </Statements></Then>
            </StmtCond>
          </Statements></Then>
          <Else><Statements>
            <StmtSimple StatementId="6" StatementText="EXEC dbo.inner_proc"
                        StatementType="EXECUTE">
              <StoredProc ProcName="[dbo].[inner_proc]"><Statements>
                <StmtSimple StatementId="7" StatementText="SELECT 2"
                            StatementType="SELECT" ParentObjectId="84"/>
                <StmtSimple StatementId="8" StatementText="RETURN"
                            StatementType="RETURN"/>
              </Statements></StoredProc>
            </StmtSimple>
          </Statements></Else>
        </StmtCond>
        <StmtSimple StatementId="9" StatementText="RETURN" StatementType="RETURN"/>
      </Statements></StoredProc>
    </StmtSimple>
    <StmtSimple StatementId="10" StatementText="SELECT 3" StatementType="SELECT"/>
  </Statements></Batch>
  <Batch><Statements>
    <StmtSimple StatementId="1" StatementText="SELECT 4" StatementType="SELECT">
      <QueryPlan><RelOp NodeId="20" PhysicalOp="Constant Scan"
                        LogicalOp="Constant Scan"/></QueryPlan>
    </StmtSimple>
  </Statements></Batch>
</BatchSequence></ShowPlanXML>)xml");
    require(plan.statements.size() == 13, "retain procedure and conditional rows");
    if (plan.statements.size() == 13) {
        const int parents[] = {-1, 0, 1, 2, 2, 4, 2, 6, 7, 7, 1, -1, -1};
        const int ids[] = {1, 0, 2, 3, 4, 5, 6, 0, 7, 8, 9, 10, 1};
        for (size_t i = 0; i < plan.statements.size(); ++i) {
            require(plan.statements[i].parent_statement_index == parents[i],
                    "nearest statement ancestry in preorder, including blocks and batches");
            require(plan.statements[i].statement_id == ids[i],
                    "XML statement identity is not a global array index");
        }
        const auto& proc = plan.statements[1];
        require(proc.structural && proc.text == "[dbo].[outer_proc]" &&
                proc.stmt_type == "StoredProc", "use real procedure label, not fabricated EXEC");
        require(!proc.root && proc.subtree_cost == 0 && proc.compile_time_ms == 0,
                "procedure does not borrow child costs or operator trees");
        require(proc.parent_object_id == 0, "procedure name does not imply module ID");
        const auto& cond = plan.statements[2];
        require(cond.structural && cond.root && cond.root->node_id == 10 &&
                cond.compile_time_ms == 3, "conditional owns its condition query plan");
        require(cond.statement_start_offset == 100 && cond.statement_end_offset == 180,
                "preserve supplied source offsets");
        const auto& leaf = plan.statements[3];
        require(!leaf.structural && leaf.root && leaf.root->node_id == 11 &&
                leaf.root->children.size() == 1 &&
                leaf.root->children[0]->node_id == 12 &&
                leaf.compile_time_ms == 7 && leaf.subtree_cost == 0.25,
                "ordinary statement operators and costs stay attached once");
        require(leaf.parent_object_id == 42 && plan.statements[5].parent_object_id == 42,
                "inherit emitting module across conditional blocks");
        require(plan.statements[6].parent_object_id == 42 &&
                plan.statements[7].parent_object_id == 0 &&
                plan.statements[8].parent_object_id == 84 &&
                plan.statements[9].parent_object_id == 0,
                "do not attribute a called procedure to its caller or previous sibling");
        require(leaf.statement_start_offset == -1 && leaf.statement_end_offset == -1,
                "absent offsets are unknown, not zero or inherited");
        require(plan.statements[11].parent_object_id == 0 &&
                plan.statements[12].parent_object_id == 0 &&
                plan.statements[12].root && plan.statements[12].root->node_id == 20,
                "reset statement and module ancestry between batches");
    }

    const auto cursor = showplan::parse_xml(R"xml(
<ShowPlanXML><BatchSequence><Batch><Statements>
  <StmtCursor StatementId="1" StatementText="DECLARE items CURSOR FOR SELECT 1"
              StatementType="DECLARE CURSOR">
    <CursorPlan CursorName="items">
      <Operation OperationType="PopulateQuery"><QueryPlan>
        <RelOp NodeId="1" PhysicalOp="Constant Scan" LogicalOp="Constant Scan"/>
      </QueryPlan></Operation>
      <Operation OperationType="FetchQuery"><QueryPlan>
        <RelOp NodeId="2" PhysicalOp="Constant Scan" LogicalOp="Constant Scan"/>
      </QueryPlan></Operation>
    </CursorPlan>
  </StmtCursor>
  <StmtReceive StatementId="2" StatementText="RECEIVE TOP (1) * FROM queue"
               StatementType="RECEIVE">
    <ReceivePlan>
      <Operation OperationType="ReceivePlanSelect"><QueryPlan>
        <RelOp NodeId="3" PhysicalOp="Constant Scan" LogicalOp="Constant Scan"/>
      </QueryPlan></Operation>
      <Operation OperationType="ReceivePlanUpdate"><QueryPlan>
        <RelOp NodeId="4" PhysicalOp="Constant Scan" LogicalOp="Constant Scan"/>
      </QueryPlan></Operation>
    </ReceivePlan>
  </StmtReceive>
  <StmtUseDb StatementId="3" StatementText="USE tempdb" StatementType="USE" Database="tempdb"/>
</Statements></Batch></BatchSequence></ShowPlanXML>)xml");
    require(cursor.statements.size() == 7, "retain both cursor and receive operation plans");
    if (cursor.statements.size() == 7) {
        require(!cursor.statements[0].root && !cursor.statements[3].root,
                "structural parents do not duplicate operation plans");
        const int indexes[] = {1, 2, 4, 5};
        for (int i = 0; i < 4; ++i) {
            const auto& op = cursor.statements[indexes[i]];
            require(op.structural && op.root && op.root->node_id == i + 1 &&
                    op.parent_statement_index == (i < 2 ? 0 : 3),
                    "operation roots have distinct ownership and correct ancestry");
        }
        require(cursor.statements[1].text == "PopulateQuery" &&
                cursor.statements[2].text == "FetchQuery",
                "cursor operation labels come from XML");
        require(cursor.statements[6].text == "USE tempdb" &&
                cursor.statements[6].parent_statement_index == -1,
                "retain non-operator USE statement");
    }
    const auto simple = showplan::parse_xml(R"xml(
<ShowPlanXML><BatchSequence><Batch><Statements>
  <StmtSimple StatementId="17" StatementText="SELECT 1" StatementType="SELECT"
              StatementSubTreeCost="0.125" RetrievedFromCache="false">
    <QueryPlan CompileTime="2" CompileCPU="1" CompileMemory="16" CachedPlanSize="8">
      <RelOp NodeId="0" PhysicalOp="Constant Scan" LogicalOp="Constant Scan"/>
    </QueryPlan>
  </StmtSimple>
</Statements></Batch></BatchSequence></ShowPlanXML>)xml");
    require(simple.statements.size() == 1, "standalone statement gains no wrapper rows");
    if (simple.statements.size() == 1) {
        const auto& s = simple.statements[0];
        require(s.statement_id == 17 && s.text == "SELECT 1" && s.stmt_type == "SELECT" &&
                !s.structural && s.parent_statement_index == -1 &&
                s.statement_start_offset == -1 && s.statement_end_offset == -1 &&
                s.parent_object_id == 0 && !s.retrieved_from_cache &&
                s.subtree_cost == 0.125 && s.compile_time_ms == 2 &&
                s.compile_cpu_ms == 1 && s.compile_memory_kb == 16 &&
                s.cached_plan_size_kb == 8 && s.root && s.root->node_id == 0 &&
                !s.root->has_runtime, "standalone statement metadata and estimates unchanged");
    }
    const auto udf = showplan::parse_xml(R"xml(
<ShowPlanXML><BatchSequence><Batch><Statements>
  <StmtSimple StatementId="1" StatementText="SELECT dbo.f()" StatementType="SELECT"
              ParentObjectId="10">
    <UDF ProcName="[dbo].[f]" ParentObjectId="20"><Statements>
      <StmtSimple StatementId="2" StatementText="RETURN 1" StatementType="RETURN"/>
    </Statements></UDF>
  </StmtSimple>
</Statements></Batch></BatchSequence></ShowPlanXML>)xml");
    require(udf.statements.size() == 3, "retain function context and nested statement");
    if (udf.statements.size() == 3) {
        require(udf.statements[1].text == "[dbo].[f]" &&
                udf.statements[1].parent_statement_index == 0 &&
                udf.statements[2].parent_statement_index == 1 &&
                udf.statements[1].parent_object_id == 20 &&
                udf.statements[2].parent_object_id == 20,
                "honor explicit module identity extensions on procedure contexts");
    }
    const auto lookup = showplan::parse_xml(R"xml(
<ShowPlanXML><BatchSequence><Batch><Statements>
  <StmtSimple StatementType="SELECT"><QueryPlan>
    <RelOp NodeId="0" PhysicalOp="Nested Loops"><NestedLoops>
      <RelOp NodeId="1" PhysicalOp="Index Seek">
        <IndexScan Lookup="false"/>
      </RelOp>
      <RelOp NodeId="2" PhysicalOp="Clustered Index Seek">
        <IndexScan Lookup="true"/>
      </RelOp>
    </NestedLoops></RelOp>
  </QueryPlan></StmtSimple>
</Statements></Batch></BatchSequence></ShowPlanXML>)xml");
    const auto& join = *lookup.statements.at(0).root;
    require(!join.is_lookup, "a child's lookup does not turn its parent into a lookup");
    require(!join.children.at(0)->is_lookup, "ordinary index seek remains a seek");
    require(join.children.at(1)->is_lookup &&
            join.children.at(1)->physical_op == "Clustered Index Seek",
            "IndexScan Lookup marks a key lookup without rewriting physical metadata");
    return failures;
}

static void dump_node(const showplan::PlanNode& n, int depth) {
    for (int i = 0; i < depth; ++i) std::printf("  ");
    std::printf("[%d] %s (%s)  est_rows=%.0f cost=%.4f",
                n.node_id, n.physical_op.c_str(), n.logical_op.c_str(),
                n.est_rows, n.est_op_cost);
    if (!n.table.empty()) {
        std::printf("  -> %s.%s", n.schema.c_str(), n.table.c_str());
        if (!n.index_name.empty()) std::printf(".%s", n.index_name.c_str());
    }
    if (n.is_lookup)   std::printf("  [lookup]");
    if (n.has_runtime) std::printf("  act=%lld", (long long)n.act_rows);
    std::printf("\n");
    for (const auto& c : n.children) dump_node(*c, depth + 1);
}

int main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr, "usage: %s <plan.sqlplan>\n", argv[0]);
        return 2;
    }

    try {
        if (check_structure() != 0) return 1;
    } catch (const showplan::ParseError& e) {
        std::fprintf(stderr, "statement contract parse failure: %s\n", e.what());
        return 1;
    }

    showplan::Plan plan;
    try {
        plan = showplan::parse_file(argv[1]);
    } catch (const showplan::ParseError& e) {
        std::fprintf(stderr, "parse error: %s\n", e.what());
        return 1;
    }

    std::printf("server: %s build %s   database: %s\n",
                plan.server_version.c_str(), plan.build.c_str(),
                showplan::predominant_database(plan).c_str());
    std::printf("statements: %zu\n", plan.statements.size());

    for (const auto& s : plan.statements) {
        std::printf("\n[stmt %d] type=%s subtree=%.4f compile=%dms "
                    "params=%zu stats=%zu\n",
                    s.statement_id, s.stmt_type.c_str(), s.subtree_cost,
                    s.compile_time_ms, s.parameters.size(), s.stats.size());
        if (!s.text.empty()) {
            std::printf("  sql: %.140s%s\n", s.text.c_str(),
                        s.text.size() > 140 ? "..." : "");
        }
        std::printf("  missing indexes: %zu, warnings: %zu\n",
                    s.missing_indexes.size(), s.warnings.size());
        for (const auto& mi : s.missing_indexes) {
            std::printf("    impact=%.1f  %s\n", mi.impact, mi.ddl.c_str());
        }
        for (const auto& w : s.warnings) {
            std::printf("    warn: %s  %s\n",
                        w.kind.c_str(), w.detail.c_str());
        }
        if (s.root) dump_node(*s.root, 1);
    }
    return 0;
}
