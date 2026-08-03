#include "UEWorkflowCore/WorkflowCore.h"

#include <nlohmann/json.hpp>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>

#ifndef UE_WORKFLOW_TEST_ROOT
#define UE_WORKFLOW_TEST_ROOT "."
#endif

namespace
{

using json = nlohmann::json;

int failures = 0;

void require(bool condition, std::string_view message)
{
    if (!condition)
    {
        ++failures;
        std::cerr << "FAILED: " << message << "\n";
    }
}

std::string read_file(const std::filesystem::path& path)
{
    std::ifstream stream(path, std::ios::binary);
    return std::string(
        std::istreambuf_iterator<char>(stream),
        std::istreambuf_iterator<char>());
}

json parse_result(const ue::workflow::Result& result)
{
    const auto value = json::parse(result.json, nullptr, false, true);
    require(!value.is_discarded(), "core result is JSON");
    return value;
}

bool has_diagnostic(const json& result, std::string_view code)
{
    if (!result.contains("diagnostics") || !result["diagnostics"].is_array())
    {
        return false;
    }
    for (const auto& diagnostic : result["diagnostics"])
    {
        if (diagnostic.value("code", std::string{}) == code)
        {
            return true;
        }
    }
    return false;
}

json minimal_workflow(std::string scope_kind, std::string asset)
{
    return {
        { "dsl", "ue.workflow" },
        { "dslVersion", "1.0" },
        { "workflowKind", "assetEdit" },
        { "workflowId", "test-workflow" },
        { "scope", {
            { "kind", std::move(scope_kind) },
            { "asset", std::move(asset) },
            { "createIfMissing", false },
        } },
        { "operations", json::array() },
    };
}

} // namespace

int main()
{
    const std::filesystem::path root = UE_WORKFLOW_TEST_ROOT;
    const auto hash_fixture =
        std::filesystem::temp_directory_path()
        / ("ue-workflow-sha256-"
            + std::to_string(
                std::chrono::steady_clock::now()
                    .time_since_epoch()
                    .count())
            + ".txt");
    {
        std::ofstream stream(hash_fixture, std::ios::binary | std::ios::trunc);
        stream << "abc";
    }
    const auto file_digest = ue::workflow::Sha256File(hash_fixture);
    require(
        file_digest
            == "sha256:ba7816bf8f01cfea414140de5dae2223"
               "b00361a396177a9cb410ff61f20015ad",
        "streaming file SHA-256 is stable");
    std::error_code hash_cleanup_error;
    std::filesystem::remove(hash_fixture, hash_cleanup_error);

    ue::workflow::Engine engine;
    const auto loaded = engine.Load({
        root / "Workflow" / "Contracts",
        { root / "Resources" / "Capabilities" },
    });
    require(loaded.ok, "contracts load");
    require(engine.CapabilityCount() > 0, "capabilities discovered dynamically");
    require(engine.ComposableOperationCount() >= 20, "v1 composable admission loaded");
    require(
        !engine.ContractSetDigest().empty(),
        "v1 contract digest is exposed");
    require(
        !engine.ContractSetDigestV2().empty(),
        "v2 contract digest is exposed");
    require(
        engine.ContractSetDigestV2() != engine.ContractSetDigest(),
        "v1 and v2 contract surfaces have distinct digests");

    const auto widget_text = read_file(
        root / "Workflow" / "tests" / "fixtures" / "widget.workflow.json");
    const auto widget_plan = engine.PlanJson(widget_text);
    require(widget_plan.ok, "documented widget workflow plans");
    const auto widget = parse_result(widget_plan);
    require(widget["normalizedWorkflow"]["operations"][0]["params"].contains("childClass"),
            "child class alias normalized");
    require(!widget["normalizedWorkflow"]["operations"][0]["params"].contains("class"),
            "child class alias removed");
    require(widget["normalizedWorkflow"]["operations"][1]["bindings"].contains("/params/target"),
            "shorthand /target binding canonicalized to capability params");
    require(widget["initializers"].size() == 1, "createIfMissing emits initializer");
    require(widget["initializers"][0]["operationType"] == "content.widget.blueprint.create",
            "widget initializer uses widget create capability");
    require(widget["operations"][0]["dependsOn"][0] == "$initializer.create",
            "authored operations depend on initializer");
    require(widget["finalizers"].size() == 3,
            "widget compile/grouped readback/diff finalizers emitted");
    require(
        widget["finalizers"][0]["operationType"] == "blueprint.asset.compile",
        "widget workflow uses the lifecycle compile finalizer");
    const auto& grouped_readback = widget["finalizers"][1];
    require(grouped_readback["operationType"] == "content.widget.hierarchy.get",
            "widget read-back uses one hierarchy query");
    require(grouped_readback["dependsOn"] == json::array({"$finalizer.compile"}),
            "grouped read-back depends on compile");
    require(grouped_readback["readBackKeys"] == json::array({"widgetTree", "bindings", "layout"}),
            "grouped read-back preserves deterministic projection order");
    require(widget["finalizers"].back()["dependsOn"] == json::array({grouped_readback["id"]}),
            "diff depends on the grouped read-back finalizer");
    require(widget["approval"]["required"] == true, "write plan requires approval");
    require(widget["approval"]["confirmWriteRequired"] == false,
            "safe widget plan does not require confirmWrite");

    auto explicit_create = json::parse(widget_text);
    const json explicit_create_operation = {
        { "id", "createWidget" },
        { "type", "content.widget.blueprint.create" },
        { "params", json::object() },
    };
    explicit_create["operations"].insert(
        explicit_create["operations"].begin(),
        explicit_create_operation);
    const auto explicit_create_plan = parse_result(engine.PlanJson(explicit_create.dump()));
    require(explicit_create_plan["ok"] == true, "explicit scope initializer plans");
    require(explicit_create_plan.contains("initializers") &&
                explicit_create_plan["initializers"].empty(),
            "explicit scope initializer suppresses automatic initializer");
    require(explicit_create_plan["operations"][0]["kind"] == "scopeInitializer",
            "explicit scope initializer is unambiguous in the plan");

    const auto widget_plan_again = parse_result(engine.PlanJson(widget_text));
    require(widget["planDigest"] == widget_plan_again["planDigest"],
            "identical workflow has deterministic digest");
    require(widget["schema"] == "ue.workflow-plan.v1" &&
                widget["plannerVersion"] == "1.0",
            "v1 workflows retain the v1 planner and plan contract");

    const auto multi_asset_text = read_file(
        root / "Workflow" / "tests" / "fixtures" /
            "multi-asset.workflow.v2.json");
    const auto multi_asset_plan_result =
        engine.PlanJson(multi_asset_text);
    require(multi_asset_plan_result.ok, "multi-asset v2 workflow plans");
    const auto multi_asset = parse_result(multi_asset_plan_result);
    require(
        multi_asset["schema"] == "ue.workflow-plan.v2" &&
            multi_asset["plannerVersion"] == "2.0",
        "v2 workflow uses the v2 plan contract");
    require(
        multi_asset["assetSet"].size() == 2 &&
            multi_asset["assetSet"][0]["asset"] <
                multi_asset["assetSet"][1]["asset"],
        "v2 asset set is in canonical asset-path order");
    require(
        multi_asset["operations"][0]["id"] ==
            "createProducerGraph" &&
            multi_asset["operations"][1]["id"] ==
                "createConsumerGraph",
        "v2 operations use a stable topological order");
    require(
        multi_asset["operations"][1]["dependsOn"].back() ==
            "createProducerGraph",
        "cross-scope typed binding contributes a DAG edge");
    require(
        multi_asset["initializers"].size() == 2 &&
            multi_asset["finalizers"].size() == 6,
        "v2 plans one initializer and compile/readback/diff chain per asset");
    const auto multi_asset_plan_again =
        parse_result(engine.PlanJson(multi_asset_text));
    require(
        multi_asset["planDigest"] ==
            multi_asset_plan_again["planDigest"],
        "v2 plan digest is deterministic");

    auto cyclic_v2 = json::parse(multi_asset_text);
    cyclic_v2["operations"][1]["dependsOn"] =
        json::array({ "createConsumerGraph" });
    const auto cyclic_v2_result =
        parse_result(engine.PlanJson(cyclic_v2.dump()));
    require(
        cyclic_v2_result["ok"] == false &&
            has_diagnostic(cyclic_v2_result, "dependency_cycle"),
        "v2 cross-scope dependency cycles are rejected");

    auto duplicate_asset_v2 = json::parse(multi_asset_text);
    duplicate_asset_v2["scopes"]["consumer"]["asset"] =
        duplicate_asset_v2["scopes"]["producer"]["asset"];
    const auto duplicate_asset_result =
        parse_result(engine.PlanJson(duplicate_asset_v2.dump()));
    require(
        duplicate_asset_result["ok"] == false &&
            has_diagnostic(
                duplicate_asset_result,
                "duplicate_scope_asset"),
        "v2 rejects two scope names for the same asset");

    auto too_many_scopes_v2 = json::parse(multi_asset_text);
    for (int index = 0; index < 15; ++index)
    {
        too_many_scopes_v2["scopes"][
            "extra" + std::to_string(index)] = {
            { "kind", "blueprint" },
            { "asset", "/Game/Automation/BP_Extra" +
                std::to_string(index) },
        };
    }
    const auto too_many_scopes_result =
        parse_result(engine.PlanJson(too_many_scopes_v2.dump()));
    require(
        too_many_scopes_result["ok"] == false &&
            has_diagnostic(
                too_many_scopes_result,
                "scope_limit_exceeded"),
        "v2 enforces the sixteen-scope limit");

    ue::workflow::CapabilityQuery capability_query;
    capability_query.domain = "blueprint";
    capability_query.kind = "query";
    capability_query.limit = 3;
    const auto capability_page =
        parse_result(engine.CapabilitiesJson(capability_query));
    require(
        capability_page["ok"] == true &&
            capability_page["capabilities"].size() <= 3 &&
            capability_page["detail"] == "summary",
        "portable capability catalog filters and pages summaries");
    if (capability_page["capabilities"].size() > 1)
    {
        require(
            capability_page["capabilities"][0]["id"] <
                capability_page["capabilities"][1]["id"],
            "portable capability catalog sorts by id");
    }
    ue::workflow::CapabilityQuery ranked_query;
    ranked_query.domain = "blueprint";
    ranked_query.query = "align layout";
    ranked_query.limit = 5;
    const auto ranked_capabilities =
        parse_result(engine.CapabilitiesJson(ranked_query));
    require(
        ranked_capabilities["ok"] == true
            && ranked_capabilities["total"] >= 1
            && ranked_capabilities["capabilities"][0]["id"]
                == "blueprint.layout.align"
            && ranked_capabilities["capabilities"][0]["match"]["score"]
                == 10000
            && ranked_capabilities["capabilities"][0]["match"]
                    ["matchedTokens"]
                == json::array({ "align", "layout" }),
        "portable capability search uses token AND ranking");
    ue::workflow::CapabilityQuery exact_query;
    exact_query.operation = "blueprint.asset.get";
    const auto exact_capability =
        parse_result(engine.CapabilitiesJson(exact_query));
    require(
        exact_capability["ok"] == true &&
            exact_capability["detail"] == "full" &&
            exact_capability["total"] == 1 &&
            exact_capability["capabilities"][0].contains(
                "inputSchema"),
        "exact operation lookup returns one full descriptor");
    ue::workflow::CapabilityQuery invalid_query;
    invalid_query.domain = "invalid-domain";
    const auto invalid_capability_query =
        parse_result(engine.CapabilitiesJson(invalid_query));
    require(
        invalid_capability_query["ok"] == false &&
            has_diagnostic(
                invalid_capability_query,
                "capability_domain_invalid"),
        "invalid capability filters return structured diagnostics");

    const auto canonical_vectors = json::parse(read_file(
        root / "Resources" / "Contracts" /
            "canonical-json-vectors.v1.json"));
    for (const auto& vector : canonical_vectors["vectors"])
    {
        const auto canonical =
            ue::workflow::CanonicalizeJsonText(
                vector["value"].dump());
        const auto digest =
            ue::workflow::CanonicalJsonSha256(
                vector["value"].dump());
        require(
            canonical &&
                *canonical ==
                    vector["canonical"].get<std::string>(),
            "canonical JSON matches the shared golden vector");
        require(
            digest &&
                *digest ==
                    "sha256:" +
                    vector["sha256"].get<std::string>(),
            "canonical JSON digest matches the shared golden vector");
    }

    const auto search_vectors = json::parse(read_file(
        root / "Resources" / "Contracts" /
            "capability-search-v1.json"));
    for (const auto& vector : search_vectors["vectors"])
    {
        json actual = json::array();
        for (const auto& descriptor : search_vectors["documents"])
        {
            ue::workflow::CapabilitySearchDocument document;
            document.id = descriptor.value("id", std::string{});
            document.description =
                descriptor.value("description", std::string{});
            const auto search =
                descriptor.value("search", json::object());
            document.title =
                search.value("title", std::string{});
            document.keywords =
                search.value(
                    "keywords",
                    std::vector<std::string>{});
            document.aliases =
                search.value(
                    "aliases",
                    std::vector<std::string>{});
            const auto match =
                ue::workflow::MatchCapabilitySearch(
                    vector.value("query", std::string{}),
                    document);
            if (match)
            {
                actual.push_back({
                    { "id", document.id },
                    { "score", match->score },
                    { "matchedFields", match->matched_fields },
                    { "matchedTokens", match->matched_tokens },
                });
            }
        }
        std::sort(
            actual.begin(),
            actual.end(),
            [](const json& left, const json& right)
            {
                if (left["score"] != right["score"])
                {
                    return left["score"].get<int>()
                        > right["score"].get<int>();
                }
                return left["id"].get<std::string>()
                    < right["id"].get<std::string>();
            });
        require(
            actual == vector["matches"],
            "capability search matches the shared golden vector");
    }

    const json valid_receipt = {
        { "schema", "ue.workflow-run.v1" },
        { "runId", "run-1" },
        { "planDigest", widget["planDigest"] },
        { "contractSetDigest", engine.ContractSetDigest() },
        { "status", "completed" },
    };
    require(
        engine.ValidateReceiptJson(valid_receipt.dump()).ok,
        "journal receipt validates against receipt contract");
    auto valid_result = valid_receipt;
    valid_result["schema"] = "ue.workflow-result.v1";
    valid_result["receipt"] = valid_receipt;
    require(
        engine.ValidateResultJson(valid_result.dump()).ok,
        "HTTP result with nested receipt validates against result contract");
    auto invalid_receipt_schema = valid_receipt;
    invalid_receipt_schema["schema"] = "ue.workflow-result.v1";
    const auto invalid_receipt_validation =
        parse_result(engine.ValidateReceiptJson(invalid_receipt_schema.dump()));
    require(invalid_receipt_validation["ok"] == false,
            "HTTP result cannot masquerade as a persisted receipt");

    const auto material_text = read_file(
        root / "Workflow" / "tests" / "fixtures" / "material.workflow.json");
    const auto material_plan = engine.PlanJson(material_text);
    require(material_plan.ok, "material nodeId binding plans");
    const auto material = parse_result(material_plan);
    require(material["operations"][1]["dependsOn"].back() == "constant",
            "binding creates a dependency");
    require(material["operations"][1]["bindings"].contains("/params/nodeId"),
            "typed nodeId binding canonicalized to capability params");

    auto bypass_finalizers = json::parse(material_text);
    bypass_finalizers["verify"] = {
        { "compile", false },
        { "readBack", json::array() },
    };
    const auto bypass_validation =
        parse_result(engine.ValidateJson(bypass_finalizers.dump()));
    require(bypass_validation["ok"] == true,
            "compatibility-shaped finalizer opt-out validates through normalization");
    require(
        bypass_validation["normalizedWorkflow"]["verify"]["compile"] == true,
        "compile false is normalized to the mandatory v1 compile finalizer");
    require(
        bypass_validation["normalizedWorkflow"]["verify"]["readBack"] ==
            json::array({ "graph" }),
        "empty material readBack is normalized to the scope default");

    const auto bypass_plan = parse_result(engine.PlanJson(bypass_finalizers.dump()));
    require(bypass_plan["ok"] == true,
            "finalizer opt-out syntax cannot prevent planning");
    require(bypass_plan["finalizers"].size() == 3,
            "material plan retains compile, read-back, and diff finalizers");
    require(bypass_plan["finalizers"][0]["kind"] == "compile",
            "mandatory compile finalizer is first");
    require(bypass_plan["finalizers"][1]["kind"] == "readBack" &&
                bypass_plan["finalizers"][1]["readBackKey"] == "graph",
            "mandatory scope read-back precedes diff");
    require(bypass_plan["finalizers"][2]["kind"] == "diff",
            "mandatory diff finalizer is last");
    require(
        bypass_plan["planDigest"] == material["planDigest"],
        "normalized finalizer opt-out syntax has the canonical plan digest");

    auto invalid_output = json::parse(material_text);
    invalid_output["operations"][1]["bindings"]["/nodeId"]["path"] = "/missingNodeId";
    const auto invalid_output_result = parse_result(engine.PlanJson(invalid_output.dump()));
    require(invalid_output_result["ok"] == false, "unknown output path rejected");
    require(has_diagnostic(invalid_output_result, "binding_source_untyped"),
            "unknown output path has typed diagnostic");

    auto nested_destination = json::parse(widget_text);
    nested_destination["operations"][1]["bindings"]["/anchors/0"] =
        nested_destination["operations"][1]["bindings"]["/target"];
    const auto nested_destination_result =
        parse_result(engine.PlanJson(nested_destination.dump()));
    require(nested_destination_result["ok"] == false,
            "nested binding destination rejected before runtime");
    require(has_diagnostic(
                nested_destination_result,
                "binding_destination_depth_unsupported"),
            "nested binding destination has v1 constraint diagnostic");

    auto scope_escape = json::parse(material_text);
    scope_escape["operations"][0]["params"]["materialFunction"] =
        "/Game/Materials/MF_Other";
    const auto scope_escape_result = parse_result(engine.PlanJson(scope_escape.dump()));
    require(scope_escape_result["ok"] == false, "materialFunction scope escape rejected");
    require(has_diagnostic(scope_escape_result, "scope_parameter_forbidden"),
            "material scope escape has contract-driven diagnostic");

    auto explicit_scope_parameter = json::parse(material_text);
    explicit_scope_parameter["operations"][0]["params"]["material"] =
        "/Game/Materials/M_Other";
    const auto explicit_scope_result =
        parse_result(engine.PlanJson(explicit_scope_parameter.dump()));
    require(explicit_scope_result["ok"] == false, "authored scope parameter rejected");
    require(has_diagnostic(explicit_scope_result, "scope_parameter_authored"),
            "authored scope parameter has diagnostic");

    auto widget_binding_escape =
        minimal_workflow("widgetBlueprint", "/Game/UI/WBP_Login");
    widget_binding_escape["operations"] = json::array({
        {
            { "id", "sourceValue" },
            { "type", "content.widget.property.set" },
            { "params", {
                { "widget", "Title" },
                { "property", "Text" },
                { "value", "/Game/UI/WBP_Other" },
            } },
        },
        {
            { "id", "redirectWidget" },
            { "type", "content.widget.property.set" },
            { "params", {
                { "widget", "Title" },
                { "property", "Text" },
                { "value", "Updated" },
            } },
            { "bindings", {
                { "/params/widgetBp", {
                    { "from", "sourceValue" },
                    { "path", "/value" },
                } },
            } },
        },
    });
    const auto widget_binding_escape_result =
        parse_result(engine.PlanJson(widget_binding_escape.dump()));
    require(widget_binding_escape_result["ok"] == false,
            "widget scope cannot be overridden by a binding");
    require(has_diagnostic(
                widget_binding_escape_result,
                "scope_binding_forbidden"),
            "widget scope binding escape has diagnostic");

    auto blueprint_binding_escape =
        minimal_workflow("blueprint", "/Game/Blueprints/BP_Test");
    blueprint_binding_escape["operations"] = json::array({
        {
            { "id", "sourceCategory" },
            { "type", "blueprint.variable.add" },
            { "params", {
                { "variableName", "Source" },
                { "variableType", "String" },
                { "category", "/Game/Blueprints/BP_Other" },
            } },
        },
        {
            { "id", "redirectBlueprint" },
            { "type", "blueprint.variable.add" },
            { "params", {
                { "variableName", "Target" },
                { "variableType", "String" },
            } },
            { "bindings", {
                { "/blueprint", {
                    { "from", "sourceCategory" },
                    { "path", "/category" },
                } },
            } },
        },
    });
    const auto blueprint_binding_escape_result =
        parse_result(engine.PlanJson(blueprint_binding_escape.dump()));
    require(blueprint_binding_escape_result["ok"] == false,
            "Blueprint scope cannot be overridden by a binding");
    require(has_diagnostic(
                blueprint_binding_escape_result,
                "scope_binding_forbidden"),
            "Blueprint scope binding escape has diagnostic");

    auto material_binding_escape =
        minimal_workflow("material", "/Game/Materials/M_Test");
    material_binding_escape["operations"] = json::array({
        {
            { "id", "sourceMaterial" },
            { "type", "content.material.expression.add" },
            { "params", {
                { "expressionClass", "Constant" },
            } },
        },
        {
            { "id", "redirectMaterial" },
            { "type", "content.material.expression.move" },
            { "params", {
                { "nodeId", "00000000-0000-0000-0000-000000000001" },
                { "posX", 10 },
                { "posY", 20 },
            } },
            { "bindings", {
                { "/params/materialFunction", {
                    { "from", "sourceMaterial" },
                    { "path", "/material" },
                } },
            } },
        },
    });
    const auto material_binding_escape_result =
        parse_result(engine.PlanJson(material_binding_escape.dump()));
    require(material_binding_escape_result["ok"] == false,
            "materialFunction cannot be introduced by a binding");
    require(has_diagnostic(
                material_binding_escape_result,
                "scope_binding_forbidden"),
            "material forbidden-parameter binding has diagnostic");

    auto widget_scope_escape = json::parse(widget_text);
    widget_scope_escape["operations"].insert(
        widget_scope_escape["operations"].begin(),
        explicit_create_operation);
    widget_scope_escape["operations"][0]["params"]["packagePath"] = "/Game/Other";
    const auto widget_scope_escape_result =
        parse_result(engine.PlanJson(widget_scope_escape.dump()));
    require(widget_scope_escape_result["ok"] == false,
            "authored widget packagePath scope parameter rejected");
    require(has_diagnostic(widget_scope_escape_result, "scope_parameter_authored"),
            "widget packagePath escape has diagnostic");

    auto graph_binding = minimal_workflow("blueprint", "/Game/Blueprints/BP_Test");
    graph_binding["operations"] = json::array({
        {
            { "id", "makeFunction" },
            { "type", "blueprint.graph.create" },
            { "params", {
                { "graphName", "DoThing" },
                { "graphType", "function" },
            } },
        },
        {
            { "id", "moveNode" },
            { "type", "blueprint.node.move" },
            { "params", {
                { "graph", "DoThing" },
                { "posX", 10 },
                { "posY", 20 },
            } },
            { "bindings", {
                { "/nodeId", {
                    { "from", "makeFunction" },
                    { "path", "/nodeId" },
                } },
            } },
        },
    });
    const auto graph_binding_result = parse_result(engine.PlanJson(graph_binding.dump()));
    require(graph_binding_result["ok"] == false,
            "graph.create does not promise an optional custom-event nodeId");
    require(has_diagnostic(graph_binding_result, "binding_source_untyped"),
            "optional graph nodeId binding is rejected at plan time");

    auto call_function =
        minimal_workflow("blueprint", "/Game/Blueprints/BP_Test");
    call_function["operations"].push_back({
        { "id", "printString" },
        { "type", "blueprint.node.add" },
        { "params", {
            { "graph", "EventGraph" },
            { "nodeType", "CallFunction" },
            { "functionName", "PrintString" },
        } },
    });
    const auto call_function_plan_result = engine.PlanJson(call_function.dump());
    require(call_function_plan_result.ok,
            "CallFunction workflow with deterministic EventGraph plans");
    const auto call_function_plan = parse_result(call_function_plan_result);
    std::vector<std::string> call_function_graphs;
    for (const auto& finalizer : call_function_plan["finalizers"])
    {
        if (finalizer.value("readBackKey", std::string{}) == "graphs")
        {
            call_function_graphs.push_back(
                finalizer.value("params", json::object())
                    .value("graph", std::string{}));
        }
    }
    require(
        call_function_graphs == std::vector<std::string>{ "EventGraph" },
        "node.add read-back uses graph and never called functionName");

    auto organize_layout =
        minimal_workflow("blueprint", "/Game/Blueprints/BP_Test");
    organize_layout["operations"].push_back({
        { "id", "organizeEventGraph" },
        { "type", "blueprint.layout.organize" },
        { "params", {
            { "graph", "EventGraph" },
            { "groups", json::array({
                {
                    { "id", "inputFlow" },
                    { "nodeIds", json::array({
                        "00000000-0000-0000-0000-000000000001",
                        "00000000-0000-0000-0000-000000000002",
                    }) },
                    { "actions", json::array({
                        {
                            { "kind", "align" },
                            { "alignment", "top" },
                        },
                    }) },
                },
            }) },
        } },
    });
	organize_layout["verify"] = {
		{ "compile", true },
		{ "readBack", json::array({ "graphs" }) },
	};
	const auto missing_layout_approval =
		parse_result(engine.PlanJson(organize_layout.dump()));
	require(
		missing_layout_approval["ok"] == false,
		"layout.organize requires its Graph hash and layout digest in Workflow");
	require(
		has_diagnostic(
			missing_layout_approval,
			"workflow_required_parameter_missing"),
		"missing organizer approval has a stable Workflow diagnostic");
	organize_layout["operations"][0]["params"]["expectedGraphHash"] =
		"sha256:aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa";
	organize_layout["operations"][0]["params"]["approvePlanDigest"] =
		"sha256:bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb";
	const auto organize_layout_plan_result =
		engine.PlanJson(organize_layout.dump());
    require(
        organize_layout_plan_result.ok,
        "layout.organize is admitted as a Blueprint editStep");
    if (organize_layout_plan_result.ok)
    {
        const auto organize_layout_plan =
            parse_result(organize_layout_plan_result);
        require(
            organize_layout_plan["operations"].size() == 1
                && organize_layout_plan["operations"][0].value(
                    "type",
                    std::string{}) == "blueprint.layout.organize",
            "layout.organize remains an authored workflow operation");
        require(
            !organize_layout_plan["operations"][0]["params"].contains(
                "dryRun"),
            "portable planner does not inject a preview-only dryRun");
        bool found_organize_graph_read_back = false;
        for (const auto& finalizer :
             organize_layout_plan["finalizers"])
        {
            found_organize_graph_read_back =
                found_organize_graph_read_back ||
                (finalizer.value(
                     "readBackKey",
                     std::string{}) == "graphs" &&
                 finalizer.value(
                     "params",
                     json::object())
                     .value("graph", std::string{}) ==
                     "EventGraph");
        }
        require(
            found_organize_graph_read_back,
            "layout.organize supplies its deterministic graph read-back target");
    }

	// A replaceManaged BuildGraph may compile to metadata-only operations after
	// obsolete managed nodes are removed. The metadata operation still carries
	// the authoritative graph target and must be sufficient for the required
	// graph read-back finalizer.
	auto buildgraph_metadata =
		minimal_workflow("blueprint", "/Game/Blueprints/BP_Test");
	buildgraph_metadata["operations"].push_back({
		{ "id", "persistManagedDefinition" },
		{ "type", "blueprint.graph.build.metadata.set" },
		{ "params", {
			{ "graph", "EventGraph" },
			{ "buildId", "automation-build" },
			{ "definition", json::object() },
			{ "ref", "managed-node" },
			{ "nodeId", "00000000-0000-0000-0000-000000000001" },
		} },
	});
	buildgraph_metadata["verify"] = {
		{ "compile", true },
		{ "readBack", json::array({ "graphs" }) },
	};
	const auto buildgraph_metadata_plan_result =
		engine.PlanJson(buildgraph_metadata.dump());
	require(
		buildgraph_metadata_plan_result.ok,
		"BuildGraph metadata-only workflow has a deterministic graph target");
	if (buildgraph_metadata_plan_result.ok)
	{
		const auto buildgraph_metadata_plan =
			parse_result(buildgraph_metadata_plan_result);
		bool found_metadata_graph_read_back = false;
		for (const auto& finalizer : buildgraph_metadata_plan["finalizers"])
		{
			found_metadata_graph_read_back =
				found_metadata_graph_read_back ||
				(finalizer.value("readBackKey", std::string{}) == "graphs" &&
				 finalizer.value("params", json::object())
					 .value("graph", std::string{}) == "EventGraph");
		}
		require(
			found_metadata_graph_read_back,
			"BuildGraph metadata supplies EventGraph read-back finalizer");
	}

    auto custom_event =
        minimal_workflow("blueprint", "/Game/Blueprints/BP_Test");
    custom_event["operations"].push_back({
        { "id", "createEvent" },
        { "type", "blueprint.graph.create" },
        { "params", {
            { "graphName", "OnReady" },
            { "graphType", "customEvent" },
        } },
    });
    const auto custom_event_plan_result = engine.PlanJson(custom_event.dump());
    require(custom_event_plan_result.ok,
            "customEvent create plans without treating event name as a graph");
    const auto custom_event_plan = parse_result(custom_event_plan_result);
    bool custom_event_graph_read_back = false;
    for (const auto& finalizer : custom_event_plan["finalizers"])
    {
        custom_event_graph_read_back =
            custom_event_graph_read_back ||
            finalizer.value("readBackKey", std::string{}) == "graphs";
    }
    require(!custom_event_graph_read_back,
            "customEvent name is not an automatic graph read-back target");
    custom_event["verify"] = {
        { "compile", true },
        { "readBack", json::array({ "graphs" }) },
    };
    const auto explicit_custom_event_graph =
        parse_result(engine.PlanJson(custom_event.dump()));
    require(explicit_custom_event_graph["ok"] == false,
            "explicit graph read-back for only a customEvent has no target");
    require(has_diagnostic(
                explicit_custom_event_graph,
                "graph_read_back_target_missing"),
            "customEvent graph read-back reports missing deterministic target");

    auto empty_pointer = json::parse(widget_text);
    empty_pointer["operations"][1]["bindings"][""] =
        empty_pointer["operations"][1]["bindings"]["/target"];
    empty_pointer["operations"][1]["bindings"].erase("/target");
    const auto empty_pointer_result = parse_result(engine.ValidateJson(empty_pointer.dump()));
    require(empty_pointer_result["ok"] == false, "empty binding destination rejected");
    require(has_diagnostic(empty_pointer_result, "pattern_mismatch") ||
                has_diagnostic(empty_pointer_result, "binding_destination_invalid"),
            "empty pointer has schema or binding diagnostic");

    auto extra_field = json::parse(widget_text);
    extra_field["operations"][0]["unexpected"] = true;
    const auto extra_field_result = parse_result(engine.ValidateJson(extra_field.dump()));
    require(extra_field_result["ok"] == false, "operation additional property rejected");
    require(has_diagnostic(extra_field_result, "additional_property_forbidden"),
            "additional property uses schema diagnostic");

    for (const auto* control_field : { "loop", "condition" })
    {
        auto control_flow = json::parse(widget_text);
        control_flow[control_field] = json::object();
        const auto control_flow_result =
            parse_result(engine.ValidateJson(control_flow.dump()));
        require(control_flow_result["ok"] == false,
                std::string("workflow ") + control_field + " rejected");
        require(has_diagnostic(control_flow_result, "additional_property_forbidden"),
                std::string("workflow ") + control_field + " is outside v1 schema");
    }

    auto second_scope = json::parse(widget_text);
    second_scope["operations"][0]["params"]["widgetBp"] =
        "/Game/UI/WBP_Other";
    const auto second_scope_result =
        parse_result(engine.PlanJson(second_scope.dump()));
    require(second_scope_result["ok"] == false,
            "operation-level second asset scope rejected");
    require(has_diagnostic(second_scope_result, "scope_parameter_authored"),
            "second asset scope has scope-bound parameter diagnostic");

    for (const auto* non_composable : {
             "scene.pie.start",
             "scene.output_log.get",
             "production.project.cook",
         })
    {
        auto invalid_workflow =
            minimal_workflow("blueprint", "/Game/Blueprints/BP_Test");
        invalid_workflow["operations"].push_back({
            { "id", "invalidStep" },
            { "type", non_composable },
            { "params", json::object() },
        });
        const auto invalid_result =
            parse_result(engine.PlanJson(invalid_workflow.dump()));
        require(invalid_result["ok"] == false,
                std::string(non_composable) + " cannot enter a workflow");
        require(has_diagnostic(invalid_result, "operation_not_composable"),
                std::string(non_composable) + " preserves admission boundary");
    }

    auto invalid_asset = json::parse(widget_text);
    invalid_asset["scope"]["asset"] = "Content/UI/WBP_Login";
    const auto invalid_asset_result = parse_result(engine.ValidateJson(invalid_asset.dump()));
    require(invalid_asset_result["ok"] == false, "non-/Game asset rejected");
    require(has_diagnostic(invalid_asset_result, "pattern_mismatch"),
            "asset path uses schema pattern diagnostic");

    auto interpolation = json::parse(widget_text);
    interpolation["operations"][0]["params"]["name"] = "${previous.name}";
    const auto interpolation_result = parse_result(engine.ValidateJson(interpolation.dump()));
    require(interpolation_result["ok"] == false, "string interpolation rejected");
    require(has_diagnostic(interpolation_result, "string_interpolation_forbidden"),
            "interpolation diagnostic emitted");

    auto confirm = minimal_workflow("widgetBlueprint", "/Game/UI/WBP_Existing");
    confirm["operations"].push_back({
        { "id", "removeTitle" },
        { "type", "content.widget.child.remove" },
        { "params", { { "childName", "Title" } } },
    });
    const auto confirm_plan = parse_result(engine.PlanJson(confirm.dump()));
    require(confirm_plan["ok"] == true, "confirmWrite workflow plans");
    require(confirm_plan["risk"]["overall"] == "confirmWrite",
            "destructive widget remove is confirmWrite");
    require(confirm_plan["approval"]["confirmWriteRequired"] == true,
            "confirmWrite plan requires explicit confirmation");

    auto variable = minimal_workflow("blueprint", "/Game/Blueprints/BP_Test");
    variable["operations"].push_back({
        { "id", "addHealth" },
        { "type", "blueprint.variable.add" },
        { "params", {
            { "variableName", "Health" },
            { "variableType", "Float" },
        } },
    });
    const auto variable_plan_result = engine.PlanJson(variable.dump());
    require(variable_plan_result.ok, "variable-only blueprint workflow plans");
    const auto variable_plan = parse_result(variable_plan_result);
    bool graph_finalizer = false;
    for (const auto& finalizer : variable_plan["finalizers"])
    {
        graph_finalizer =
            graph_finalizer ||
            finalizer.value("readBackKey", std::string{}) == "graphs";
    }
    require(!graph_finalizer, "variable-only workflow does not add graph readback");

    variable["verify"] = {
        { "compile", true },
        { "readBack", json::array({ "graphs" }) },
    };
    const auto missing_graph = parse_result(engine.PlanJson(variable.dump()));
    require(missing_graph["ok"] == false, "explicit graph readback needs a graph target");
    require(has_diagnostic(missing_graph, "graph_read_back_target_missing"),
            "missing graph target diagnostic emitted");

    const auto mutation_suffix =
        std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
    const auto mutation_root =
        std::filesystem::temp_directory_path() /
        ("ue-workflow-contract-test-" + mutation_suffix);
    std::error_code copy_error;
    std::filesystem::create_directories(mutation_root, copy_error);
    std::filesystem::copy(
        root / "Workflow" / "Contracts",
        mutation_root,
        std::filesystem::copy_options::recursive |
            std::filesystem::copy_options::overwrite_existing,
        copy_error);
    require(!copy_error, "contract mutation fixture copied");
    if (!copy_error)
    {
        const auto mutation_capabilities = mutation_root / "Capabilities";
        std::filesystem::copy(
            root / "Resources" / "Capabilities",
            mutation_capabilities,
            std::filesystem::copy_options::recursive |
                std::filesystem::copy_options::overwrite_existing,
            copy_error);
        require(!copy_error, "capability mutation fixture copied");
        if (!copy_error)
        {
            const auto content_path = mutation_capabilities / "content.json";
            auto content_manifest = json::parse(read_file(content_path));
            content_manifest["capabilities"].push_back({
                { "id", "content.test.nonworkflow" },
                { "domain", "content" },
                { "kind", "query" },
                { "description", "Test-only non-workflow capability." },
                { "inputSchema", {
                    { "type", "object" },
                    { "properties", json::object() },
                    { "additionalProperties", false },
                } },
                { "traits", {
                    { "readOnly", true },
                    { "destructive", false },
                    { "expensive", false },
                } },
                { "output", {
                    { "kind", "json" },
                } },
                { "dsl", {
                    { "admission", "none" },
                    { "scopeKinds", json::array() },
                    { "transactionDomain", "none" },
                    { "deferCompile", false },
                    { "risk", "readOnly" },
                } },
            });
            {
                std::ofstream stream(
                    content_path,
                    std::ios::binary | std::ios::trunc);
                stream << content_manifest.dump(2) << "\n";
            }

            ue::workflow::Engine nonworkflow_engine;
            const auto nonworkflow_loaded = nonworkflow_engine.Load({
                mutation_root,
                { mutation_capabilities },
            });
            require(nonworkflow_loaded.ok, "non-workflow capability fixture loads");
            require(
                nonworkflow_engine.CapabilityCount() == engine.CapabilityCount() + 1,
                "non-workflow capability remains discoverable");
            require(
                nonworkflow_engine.ContractSetDigest() == engine.ContractSetDigest(),
                "non-workflow capability does not churn workflow contract digest");

            auto& added = content_manifest["capabilities"].back();
            added["dsl"] = {
                { "admission", "observeOnly" },
                { "scopeKinds", json::array({ "widgetBlueprint" }) },
                { "transactionDomain", "asset" },
                { "deferCompile", false },
                { "risk", "readOnly" },
            };
            {
                std::ofstream stream(
                    content_path,
                    std::ios::binary | std::ios::trunc);
                stream << content_manifest.dump(2) << "\n";
            }
            ue::workflow::Engine workflow_surface_engine;
            const auto workflow_surface_loaded = workflow_surface_engine.Load({
                mutation_root,
                { mutation_capabilities },
            });
            require(
                workflow_surface_loaded.ok,
                "workflow-admitted capability fixture loads");
            require(
                workflow_surface_engine.ContractSetDigest()
                    != engine.ContractSetDigest(),
                "workflow-admitted capability participates in contract digest");
        }

        const auto receipt_path = mutation_root / "receipt.schema.v1.json";
        auto receipt = json::parse(read_file(receipt_path));
        receipt["title"] = "Mutated receipt contract";
        {
            std::ofstream stream(receipt_path, std::ios::binary | std::ios::trunc);
            stream << receipt.dump(2) << "\n";
        }

        ue::workflow::Engine mutated_engine;
        const auto mutated_loaded = mutated_engine.Load({
            mutation_root,
            { root / "Resources" / "Capabilities" },
        });
        require(mutated_loaded.ok, "mutated complete contract set loads");
        require(
            mutated_engine.ContractSetDigest() != engine.ContractSetDigest(),
            "receipt schema content participates in contract digest");
    }
    std::error_code cleanup_error;
    std::filesystem::remove_all(mutation_root, cleanup_error);

    std::cout << "UEWorkflowCore tests: "
              << (failures == 0 ? "PASS" : "FAIL")
              << " (" << failures << " failures)\n";
    return failures == 0 ? 0 : 1;
}
