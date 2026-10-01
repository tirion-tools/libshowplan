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

static int check_query_metadata() {
    int failures = 0;
    auto require = [&](bool ok, const char* message) {
        if (!ok) {
            std::fprintf(stderr, "query metadata contract: %s\n", message);
            ++failures;
        }
    };
    const auto plan = showplan::parse_xml(R"xml(
<ShowPlanXML><BatchSequence><Batch><Statements>
  <StmtCond StatementId="1" StatementEstRows="12.5">
    <StatementSetOptions ANSI_NULLS="true" ANSI_PADDING="false"
      ANSI_WARNINGS="1" ARITHABORT="0" CONCAT_NULL_YIELDS_NULL="true"
      NUMERIC_ROUNDABORT="false" QUOTED_IDENTIFIER="true"/>
    <Condition><QueryPlan DegreeOfParallelism="8" MemoryGrant="4096">
      <MemoryGrantInfo DesiredMemory="8192" GrantedMemory="4096" GrantWaitTime="3"
        MaxUsedMemory="1024" RequestedMemory="6144" RequiredMemory="512"
        SerialDesiredMemory="2048" SerialRequiredMemory="256"/>
      <OptimizerHardwareDependentProperties EstimatedAvailableDegreeOfParallelism="16"
        EstimatedAvailableMemoryGrant="65536" EstimatedPagesCached="1048576"/>
      <ThreadStat Branches="2" UsedThreads="8">
        <ThreadReservation NodeId="0" ReservedThreads="6"/>
        <ThreadReservation NodeId="1" ReservedThreads="2"/>
      </ThreadStat>
      <RelOp NodeId="0" PhysicalOp="Constant Scan" EstimateRows="7"/>
    </QueryPlan></Condition>
    <Then><Statements>
      <StmtSimple StatementId="2" StatementEstRows="0">
        <StatementSetOptions ANSI_NULLS="false"/>
        <QueryPlan DegreeOfParallelism="0" MemoryGrant="0">
          <MemoryGrantInfo DesiredMemory="0" GrantedMemory="0" GrantWaitTime="0"
            MaxUsedMemory="0" RequestedMemory="0" RequiredMemory="0"
            SerialDesiredMemory="0" SerialRequiredMemory="0"/>
          <OptimizerHardwareDependentProperties EstimatedAvailableDegreeOfParallelism="0"
            EstimatedAvailableMemoryGrant="0" EstimatedPagesCached="0"/>
          <ThreadStat Branches="0" UsedThreads="0">
            <ThreadReservation NodeId="0" ReservedThreads="0"/>
          </ThreadStat>
        </QueryPlan>
      </StmtSimple>
    </Statements></Then>
  </StmtCond>
  <StmtSimple StatementId="3"><QueryPlan>
    <RelOp NodeId="1" PhysicalOp="Constant Scan">
      <QueryPlan DegreeOfParallelism="99" MemoryGrant="99">
        <MemoryGrantInfo DesiredMemory="99"/>
        <OptimizerHardwareDependentProperties EstimatedPagesCached="99"/>
        <ThreadStat Branches="99"><ThreadReservation NodeId="99"/></ThreadStat>
        <StatementSetOptions ANSI_NULLS="true"/>
      </QueryPlan>
    </RelOp>
  </QueryPlan></StmtSimple>
  <StmtSimple StatementId="4">
    <StatementSetOptions QUOTED_IDENTIFIER="false"/>
  </StmtSimple>
  <StmtSimple StatementId="5"><QueryPlan>
    <MemoryGrantInfo GrantedMemory="0"/>
    <OptimizerHardwareDependentProperties EstimatedPagesCached="0"/>
    <ThreadStat UsedThreads="0"><ThreadReservation ReservedThreads="0"/></ThreadStat>
  </QueryPlan></StmtSimple>
</Statements></Batch></BatchSequence></ShowPlanXML>)xml");
    require(plan.statements.size() == 5, "nested operator metadata adds no statements");
    if (plan.statements.size() != 5) return failures;
    const auto& full = plan.statements[0];
    require(full.est_rows == 12.5 && full.root && full.root->est_rows == 7 &&
            full.degree_of_parallelism == 8 && full.memory_grant_kb == 4096,
            "conditional owns statement estimates and its Condition/QueryPlan metadata");
    const auto& grant = full.memory_grant;
    require(grant.desired_memory_kb == 8192 && grant.granted_memory_kb == 4096 &&
            grant.grant_wait_time_ms == 3 && grant.max_used_memory_kb == 1024 &&
            grant.requested_memory_kb == 6144 && grant.required_memory_kb == 512 &&
            grant.serial_desired_memory_kb == 2048 && grant.serial_required_memory_kb == 256,
            "retain distinct memory grant quantities without conversion");
    const auto& hardware = full.optimizer_hardware;
    require(hardware.estimated_degree_of_parallelism == 16 &&
            hardware.estimated_available_memory_grant_kb == 65536 &&
            hardware.estimated_pages_cached == 1048576,
            "retain optimizer hardware estimates separately from runtime grants");
    const auto& threads = full.parallel_threads;
    require(threads.branches == 2 && threads.used_threads == 8 &&
            threads.reservations.size() == 2 &&
            threads.reservations[0].node_id == 0 &&
            threads.reservations[0].reserved_threads == 6 &&
            threads.reservations[1].node_id == 1 &&
            threads.reservations[1].reserved_threads == 2,
            "retain every NUMA reservation with its owning node");
    const auto& options = full.set_options;
    require(options.ansi_nulls == true && options.ansi_padding == false &&
            options.ansi_warnings == true && options.arithabort == false &&
            options.concat_null_yields_null == true && options.numeric_roundabort == false &&
            options.quoted_identifier == true, "capture true/false and numeric set options");
    const auto& zero = plan.statements[1];
    const auto& zg = zero.memory_grant;
    require(zero.parent_statement_index == 0 && zero.est_rows == 0 &&
            zero.degree_of_parallelism == 0 && zero.memory_grant_kb == 0 &&
            zg.desired_memory_kb == 0 && zg.granted_memory_kb == 0 &&
            zg.grant_wait_time_ms == 0 && zg.max_used_memory_kb == 0 &&
            zg.requested_memory_kb == 0 && zg.required_memory_kb == 0 &&
            zg.serial_desired_memory_kb == 0 && zg.serial_required_memory_kb == 0 &&
            zero.optimizer_hardware.estimated_degree_of_parallelism == 0 &&
            zero.optimizer_hardware.estimated_available_memory_grant_kb == 0 &&
            zero.optimizer_hardware.estimated_pages_cached == 0 &&
            zero.parallel_threads.branches == 0 && zero.parallel_threads.used_threads == 0 &&
            zero.parallel_threads.reservations.size() == 1 &&
            zero.parallel_threads.reservations[0].node_id == 0 &&
            zero.parallel_threads.reservations[0].reserved_threads == 0,
            "captured zeros remain known, not absent or inherited from enclosing statement");
    require(zero.set_options.ansi_nulls == false && !zero.set_options.ansi_padding,
            "captured false differs from absent without inheriting parent options");
    const auto& absent = plan.statements[2];
    const auto& ag = absent.memory_grant;
    require(absent.est_rows == -1 && absent.degree_of_parallelism == -1 &&
            absent.memory_grant_kb == -1 && ag.desired_memory_kb == -1 &&
            ag.granted_memory_kb == -1 && ag.grant_wait_time_ms == -1 &&
            ag.max_used_memory_kb == -1 && ag.requested_memory_kb == -1 &&
            ag.required_memory_kb == -1 && ag.serial_desired_memory_kb == -1 &&
            ag.serial_required_memory_kb == -1 &&
            absent.optimizer_hardware.estimated_degree_of_parallelism == -1 &&
            absent.optimizer_hardware.estimated_available_memory_grant_kb == -1 &&
            absent.optimizer_hardware.estimated_pages_cached == -1 &&
            absent.parallel_threads.branches == -1 &&
            absent.parallel_threads.used_threads == -1 &&
            absent.parallel_threads.reservations.empty(),
            "missing metadata stays absent rather than borrowing nested or sibling values");
    require(!absent.set_options.ansi_nulls && !absent.set_options.ansi_padding &&
            !absent.set_options.ansi_warnings && !absent.set_options.arithabort &&
            !absent.set_options.concat_null_yields_null &&
            !absent.set_options.numeric_roundabort && !absent.set_options.quoted_identifier,
            "missing set options remain disengaged");
    require(!plan.statements[3].root &&
            plan.statements[3].set_options.quoted_identifier == false &&
            !plan.statements[3].set_options.ansi_nulls,
            "set options do not require an operator query plan");
    const auto& partial = plan.statements[4];
    require(partial.memory_grant.granted_memory_kb == 0 &&
            partial.memory_grant.desired_memory_kb == -1 &&
            partial.optimizer_hardware.estimated_pages_cached == 0 &&
            partial.optimizer_hardware.estimated_degree_of_parallelism == -1 &&
            partial.parallel_threads.branches == -1 &&
            partial.parallel_threads.used_threads == 0 &&
            partial.parallel_threads.reservations.size() == 1 &&
            partial.parallel_threads.reservations[0].node_id == -1 &&
            partial.parallel_threads.reservations[0].reserved_threads == 0,
            "partially supplied elements preserve absence at attribute level");
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
        if (check_query_metadata() != 0) return 1;
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
