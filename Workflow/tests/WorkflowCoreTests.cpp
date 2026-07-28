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
    ue::workflow::Engine engine;
    const auto loaded = engine.Load({
        root / "Workflow" / "Contracts",
        { root / "Resources" / "Capabilities" },
    });
    require(loaded.ok, "contracts load");
    require(engine.CapabilityCount() > 0, "capabilities discovered dynamically");
    require(engine.ComposableOperationCount() >= 20, "v1 composable admission loaded");

    const auto widget_text = read_file(
        root / "Workflow" / "tests" / "fixtures" / "widget.workflow.json");
    const auto widget_plan = engine.PlanJson(widget_text);
    require(widget_plan.ok, "documented widget workflow plans");
    const auto widget = parse_result(widget_plan);
    require(widget["normalizedWorkflow"]["operations"][0]["params"].contains("child_class"),
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
    require(widget["finalizers"].size() == 5, "widget compile/readback/diff finalizers emitted");
    require(
        widget["finalizers"][0]["operationType"] == "blueprint.asset.compile",
        "widget workflow uses the lifecycle compile finalizer");
    for (std::size_t index = 1; index + 1 < widget["finalizers"].size(); ++index)
    {
        require(
            widget["finalizers"][index]["dependsOn"] ==
                json::array({ "$finalizer.compile" }),
            "read-back finalizers depend on compile");
    }
    json read_back_ids = json::array();
    for (std::size_t index = 1; index + 1 < widget["finalizers"].size(); ++index)
    {
        read_back_ids.push_back(widget["finalizers"][index]["id"]);
    }
    require(
        widget["finalizers"].back()["dependsOn"] == read_back_ids,
        "diff depends on every read-back finalizer");
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
                { "/params/widget_bp", {
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
    widget_scope_escape["operations"][0]["params"]["package_path"] = "/Game/Other";
    const auto widget_scope_escape_result =
        parse_result(engine.PlanJson(widget_scope_escape.dump()));
    require(widget_scope_escape_result["ok"] == false,
            "authored widget package_path scope parameter rejected");
    require(has_diagnostic(widget_scope_escape_result, "scope_parameter_authored"),
            "widget package_path escape has diagnostic");

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
    second_scope["operations"][0]["params"]["widget_bp"] =
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
        { "params", { { "child_name", "Title" } } },
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
