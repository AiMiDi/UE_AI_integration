#include "UECommandCli/SkillCatalog.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <fstream>
#include <initializer_list>
#include <set>
#include <sstream>
#include <string_view>

#ifndef UE_CLI_SOURCE_ROOT
#define UE_CLI_SOURCE_ROOT ""
#endif

namespace ue::command
{
namespace
{

using json = nlohmann::json;

std::optional<std::string> Environment(const std::string_view name)
{
    const std::string key(name);
#if defined(_WIN32)
    char* value = nullptr;
    std::size_t length = 0;
    if (_dupenv_s(&value, &length, key.c_str()) != 0
        || !value
        || length <= 1)
    {
        std::free(value);
        return std::nullopt;
    }
    std::string result(value);
    std::free(value);
    return result;
#else
    const char* value = std::getenv(key.c_str());
    return value && *value
        ? std::optional<std::string>(value)
        : std::nullopt;
#endif
}

bool IsSkillRoot(const std::filesystem::path& root)
{
    std::error_code error;
    if (!std::filesystem::is_directory(root, error) || error)
    {
        return false;
    }
    for (const auto& entry :
         std::filesystem::directory_iterator(root, error))
    {
        if (error)
        {
            return false;
        }
        std::error_code entry_error;
        if (entry.is_directory(entry_error)
            && !entry_error
            && std::filesystem::is_regular_file(
                entry.path() / "skill.json",
                entry_error)
            && !entry_error)
        {
            return true;
        }
    }
    return false;
}

bool NonEmptyString(
    const json& object,
    const std::string& field,
    const std::string& context,
    std::string& error)
{
    const auto value = object.find(field);
    if (value == object.end()
        || !value->is_string()
        || value->get_ref<const std::string&>().empty())
    {
        error = context + "." + field + " must be a non-empty string.";
        return false;
    }
    return true;
}

bool StringArray(
    const json& object,
    const std::string& field,
    const std::string& context,
    const bool allow_empty,
    std::string& error)
{
    const auto values = object.find(field);
    if (values == object.end()
        || !values->is_array()
        || (!allow_empty && values->empty()))
    {
        error = context + "." + field
            + (allow_empty
                ? " must be an array."
                : " must be a non-empty array.");
        return false;
    }
    std::set<std::string> unique;
    for (const auto& value : *values)
    {
        if (!value.is_string()
            || value.get_ref<const std::string&>().empty())
        {
            error = context + "." + field
                + " must contain only non-empty strings.";
            return false;
        }
        if (!unique.insert(value.get<std::string>()).second)
        {
            error = context + "." + field
                + " contains a duplicate value.";
            return false;
        }
    }
    return true;
}

bool IsOneOf(
    const std::string& value,
    const std::initializer_list<std::string_view> allowed)
{
    return std::any_of(
        allowed.begin(),
        allowed.end(),
        [&](const std::string_view candidate)
        {
            return value == candidate;
        });
}

bool ValidRisk(const std::string& value)
{
    return IsOneOf(
        value,
        { "readOnly", "safeWrite", "confirmWrite", "mixed" });
}

bool ValidDomain(const std::string& value)
{
    return IsOneOf(
        value,
        {
            "blueprint",
            "scene",
            "content",
            "animation",
            "ai",
            "production",
        });
}

bool ValidInputType(const std::string& value)
{
    return IsOneOf(
        value,
        {
            "string",
            "integer",
            "number",
            "boolean",
            "object",
            "array",
        });
}

bool ValidSkillId(const std::string& value)
{
    if (value.empty()
        || !std::islower(static_cast<unsigned char>(value.front())))
    {
        return false;
    }
    return std::all_of(
        value.begin(),
        value.end(),
        [](const unsigned char character)
        {
            return std::islower(character)
                || std::isdigit(character)
                || character == '-';
        });
}

bool SkillFrontmatterMatches(
    const std::filesystem::path& entrypoint,
    const std::string& expected,
    std::string& error)
{
    std::ifstream stream(entrypoint);
    if (!stream)
    {
        error = "Skill entrypoint could not be opened: "
            + entrypoint.generic_string();
        return false;
    }
    std::string line;
    bool opened = false;
    while (std::getline(stream, line))
    {
        if (!line.empty() && line.back() == '\r')
        {
            line.pop_back();
        }
        if (line == "---")
        {
            if (!opened)
            {
                opened = true;
                continue;
            }
            break;
        }
        if (opened && line.starts_with("name:"))
        {
            std::string name = line.substr(5);
            name.erase(
                name.begin(),
                std::find_if(
                    name.begin(),
                    name.end(),
                    [](const unsigned char character)
                    {
                        return !std::isspace(character);
                    }));
            if (name == expected)
            {
                return true;
            }
        }
    }
    error = "Skill entrypoint frontmatter name must equal manifest id: "
        + expected;
    return false;
}

bool IsContainedRelativeFile(
    const std::filesystem::path& skill_directory,
    const std::string& relative,
    std::string& error)
{
    const std::filesystem::path resource(relative);
    if (resource.empty()
        || resource.is_absolute()
        || resource.has_root_name()
        || std::find(
               resource.begin(),
               resource.end(),
               std::filesystem::path(".."))
            != resource.end())
    {
        error = "Skill resource path must stay inside its skill directory: "
            + relative;
        return false;
    }
    std::error_code path_error;
    const auto resolved = std::filesystem::weakly_canonical(
        skill_directory / resource,
        path_error);
    const auto canonical_directory = std::filesystem::weakly_canonical(
        skill_directory,
        path_error);
    if (path_error
        || !std::filesystem::is_regular_file(resolved, path_error)
        || path_error)
    {
        error = "Skill resource file does not exist: "
            + (skill_directory / resource).generic_string();
        return false;
    }
    const auto relative_to_skill = std::filesystem::relative(
        resolved,
        canonical_directory,
        path_error);
    if (path_error
        || relative_to_skill.empty()
        || relative_to_skill.is_absolute()
        || relative_to_skill.begin() == relative_to_skill.end()
        || *relative_to_skill.begin() == std::filesystem::path(".."))
    {
        error = "Skill resource resolves outside its skill directory: "
            + relative;
        return false;
    }
    return true;
}

bool ValidateSkill(
    const json& skill,
    const std::filesystem::path& manifest,
    const CapabilityCatalog& capabilities,
    std::string& error)
{
    const std::string context = manifest.generic_string();
    if (!skill.is_object()
        || !skill.contains("schema")
        || !skill["schema"].is_string()
        || skill["schema"].get_ref<const std::string&>()
            != "ue.agent-skill.v1"
        || !skill.contains("schemaVersion")
        || !skill["schemaVersion"].is_number_integer()
        || skill["schemaVersion"] != 1)
    {
        error = context
            + " must declare schema ue.agent-skill.v1 version 1.";
        return false;
    }
    for (const std::string field : {
             "id",
             "version",
             "title",
             "description",
             "risk",
             "entrypoint",
         })
    {
        if (!NonEmptyString(skill, field, context, error))
        {
            return false;
        }
    }
    if (!StringArray(skill, "domains", context, false, error)
        || !StringArray(skill, "triggers", context, false, error))
    {
        return false;
    }
    const std::string id = skill["id"].get<std::string>();
    if (!ValidSkillId(id))
    {
        error = context + ".id must be a lowercase kebab-case id.";
        return false;
    }
    if (manifest.parent_path().filename() != id)
    {
        error = context + ".id must match its directory name.";
        return false;
    }
    if (skill["entrypoint"] != "SKILL.md")
    {
        error = context + ".entrypoint must equal SKILL.md.";
        return false;
    }
    if (!ValidRisk(skill["risk"].get<std::string>()))
    {
        error = context
            + ".risk must be readOnly, safeWrite, confirmWrite, or mixed.";
        return false;
    }
    for (const auto& value : skill["domains"])
    {
        if (!ValidDomain(value.get<std::string>()))
        {
            error = context + ".domains contains an unsupported domain.";
            return false;
        }
    }
    std::set<std::string> skill_domains;
    for (const auto& value : skill["domains"])
    {
        skill_domains.insert(value.get<std::string>());
    }

    const auto requirements = skill.find("requirements");
    if (requirements == skill.end() || !requirements->is_object())
    {
        error = context + ".requirements must be an object.";
        return false;
    }
    if (!StringArray(
            *requirements,
            "capabilities",
            context + ".requirements",
            true,
            error))
    {
        return false;
    }
    if (requirements->contains("optionalCapabilities")
        && !StringArray(
            *requirements,
            "optionalCapabilities",
            context + ".requirements",
            true,
            error))
    {
        return false;
    }

    std::set<std::string> required_capabilities;
    std::set<std::string> optional_capabilities;
    for (const auto& value : (*requirements)["capabilities"])
    {
        required_capabilities.insert(value.get<std::string>());
    }
    for (const auto& value :
         requirements->value("optionalCapabilities", json::array()))
    {
        const std::string operation = value.get<std::string>();
        if (required_capabilities.contains(operation))
        {
            error = context
                + ".requirements declares a capability as both required "
                  "and optional: "
                + operation;
            return false;
        }
        optional_capabilities.insert(operation);
    }
    for (const std::string& operation : required_capabilities)
    {
        if (!capabilities.Find(operation))
        {
            error = context + " references missing required capability: "
                + operation;
            return false;
        }
        const auto* descriptor = capabilities.Find(operation);
        const std::string domain = descriptor
            ? descriptor->value("domain", std::string{})
            : std::string{};
        if (!domain.empty() && !skill_domains.contains(domain))
        {
            error = context + ".domains does not include " + domain
                + " for capability " + operation;
            return false;
        }
    }
    for (const std::string& operation : optional_capabilities)
    {
        if (!capabilities.Find(operation))
        {
            error = context + " references missing optional capability: "
                + operation;
            return false;
        }
        const auto* descriptor = capabilities.Find(operation);
        const std::string domain = descriptor
            ? descriptor->value("domain", std::string{})
            : std::string{};
        if (!domain.empty() && !skill_domains.contains(domain))
        {
            error = context + ".domains does not include " + domain
                + " for capability " + operation;
            return false;
        }
    }

    const auto recipes = skill.find("recipes");
    if (recipes == skill.end()
        || !recipes->is_array()
        || recipes->empty())
    {
        error = context + ".recipes must be a non-empty array.";
        return false;
    }
    std::set<std::string> recipe_ids;
    std::set<std::string> recipe_risks;
    for (std::size_t recipe_index = 0;
         recipe_index < recipes->size();
         ++recipe_index)
    {
        const auto& recipe = (*recipes)[recipe_index];
        const std::string recipe_context =
            context + ".recipes[" + std::to_string(recipe_index) + "]";
        if (!recipe.is_object())
        {
            error = recipe_context + " must be an object.";
            return false;
        }
        for (const std::string field :
             { "id", "title", "description", "risk" })
        {
            if (!NonEmptyString(
                    recipe,
                    field,
                    recipe_context,
                    error))
            {
                return false;
            }
        }
        if (!recipe_ids.insert(recipe["id"].get<std::string>()).second)
        {
            error = recipe_context + ".id must be unique within the skill.";
            return false;
        }
        const std::string recipe_risk =
            recipe["risk"].get<std::string>();
        if (!ValidRisk(recipe_risk))
        {
            error = recipe_context
                + ".risk must be readOnly, safeWrite, confirmWrite, or "
                  "mixed.";
            return false;
        }
        recipe_risks.insert(recipe_risk);
        const auto inputs = recipe.find("inputs");
        if (inputs == recipe.end() || !inputs->is_array())
        {
            error = recipe_context + ".inputs must be an array.";
            return false;
        }
        std::set<std::string> input_names;
        for (std::size_t input_index = 0;
             input_index < inputs->size();
             ++input_index)
        {
            const auto& input = (*inputs)[input_index];
            const std::string input_context =
                recipe_context + ".inputs[" + std::to_string(input_index)
                + "]";
            if (!input.is_object()
                || !NonEmptyString(
                    input,
                    "name",
                    input_context,
                    error)
                || !NonEmptyString(
                    input,
                    "type",
                    input_context,
                    error)
                || !NonEmptyString(
                    input,
                    "description",
                    input_context,
                    error)
                || !input.contains("required")
                || !input["required"].is_boolean())
            {
                if (error.empty())
                {
                    error = input_context
                        + ".required must be a boolean.";
                }
                return false;
            }
            if (!ValidInputType(input["type"].get<std::string>()))
            {
                error = input_context + ".type is unsupported.";
                return false;
            }
            if (!input_names.insert(
                    input["name"].get<std::string>()).second)
            {
                error = input_context + ".name must be unique.";
                return false;
            }
        }
        const auto steps = recipe.find("steps");
        if (steps == recipe.end()
            || !steps->is_array()
            || steps->empty())
        {
            error = recipe_context + ".steps must be a non-empty array.";
            return false;
        }
        std::set<std::string> step_ids;
        std::set<std::string> phases;
        bool has_guarded_operation = false;
        for (std::size_t step_index = 0;
             step_index < steps->size();
             ++step_index)
        {
            const auto& step = (*steps)[step_index];
            const std::string step_context =
                recipe_context + ".steps[" + std::to_string(step_index)
                + "]";
            if (!step.is_object())
            {
                error = step_context + " must be an object.";
                return false;
            }
            for (const std::string field :
                 { "id", "phase", "purpose" })
            {
                if (!NonEmptyString(
                        step,
                        field,
                        step_context,
                        error))
                {
                    return false;
                }
            }
            if (!step_ids.insert(step["id"].get<std::string>()).second)
            {
                error = step_context + ".id must be unique within a recipe.";
                return false;
            }
            const std::string phase = step["phase"].get<std::string>();
            if (!IsOneOf(phase, { "discover", "execute", "verify" }))
            {
                error = step_context
                    + ".phase must be discover, execute, or verify.";
                return false;
            }
            phases.insert(phase);
            if (!StringArray(
                    step,
                    "operations",
                    step_context,
                    false,
                    error))
            {
                return false;
            }
            if (step.contains("optional")
                && !step["optional"].is_boolean())
            {
                error = step_context + ".optional must be boolean.";
                return false;
            }
            if (step.contains("route")
                && (!step["route"].is_string()
                    || !IsOneOf(
                        step["route"].get<std::string>(),
                        { "domain", "workflow" })))
            {
                error = step_context
                    + ".route must be domain or workflow.";
                return false;
            }
            const std::string route =
                step.value("route", std::string("domain"));
            if (route == "workflow" && phase != "execute")
            {
                error = step_context
                    + ".route workflow is only valid in the execute phase.";
                return false;
            }
            const bool step_optional =
                step.value("optional", false);
            for (const auto& value : step["operations"])
            {
                const std::string operation = value.get<std::string>();
                const bool required =
                    required_capabilities.contains(operation);
                const bool optional =
                    optional_capabilities.contains(operation);
                if (!required && !optional)
                {
                    error = step_context
                        + " uses an operation not declared in requirements: "
                        + operation;
                    return false;
                }
                const auto* descriptor = capabilities.Find(operation);
                if (!descriptor)
                {
                    error = step_context
                        + " references missing capability: "
                        + operation;
                    return false;
                }
                const json traits = descriptor->value(
                    "traits",
                    json::object());
                const auto effects = descriptor->find("effects");
                const bool read_only = effects != descriptor->end()
                    && effects->is_object()
                    && effects->value("asset", std::string("write")) != "write"
                    && effects->value("world", std::string("write")) != "write"
                    && effects->value("external", std::string("write")) != "write";
                const bool destructive =
                    traits.value("destructive", false);
                const std::string dsl_risk =
                    descriptor
                        ->value("dsl", json::object())
                        .value("risk", std::string{});
                const bool guarded =
                    destructive || dsl_risk == "confirmWrite";
                has_guarded_operation =
                    has_guarded_operation || guarded;
                if (route == "workflow")
                {
                    const std::string admission =
                        descriptor
                            ->value("dsl", json::object())
                            .value(
                                "admission",
                                std::string{});
                    if (admission != "editStep")
                    {
                        error = step_context
                            + ".route workflow requires editStep "
                              "admission for "
                            + operation;
                        return false;
                    }
                }
                if (phase == "verify"
                    && !step_optional
                    && !read_only)
                {
                    error = step_context
                        + " contains a write in a non-optional "
                          "verify step.";
                    return false;
                }
                if (recipe_risk == "readOnly"
                    && (!read_only || destructive))
                {
                    error = recipe_context
                        + ".risk readOnly may reference only "
                          "read-only, non-destructive capabilities.";
                    return false;
                }
                if (recipe_risk == "safeWrite" && guarded)
                {
                    error = recipe_context
                        + ".risk safeWrite may not reference "
                          "destructive or confirmWrite capabilities.";
                    return false;
                }
            }
        }
        for (const std::string phase :
             { "discover", "execute", "verify" })
        {
            if (!phases.contains(phase))
            {
                error = recipe_context + " is missing " + phase
                    + " phase.";
                return false;
            }
        }
        if (recipe_risk == "confirmWrite"
            && !has_guarded_operation)
        {
            error = recipe_context
                + ".risk confirmWrite must include a destructive "
                  "or confirmWrite capability.";
            return false;
        }
        const auto result = recipe.find("result");
        if (result == recipe.end() || !result->is_object())
        {
            error = recipe_context + ".result must be an object.";
            return false;
        }
        if (!NonEmptyString(
                *result,
                "summary",
                recipe_context + ".result",
                error)
            || !StringArray(
                *result,
                "evidence",
                recipe_context + ".result",
                false,
                error)
            || !StringArray(
                *result,
                "success",
                recipe_context + ".result",
                false,
                error))
        {
            return false;
        }
    }
    const std::string expected_risk =
        recipe_risks.size() == 1
            ? *recipe_risks.begin()
            : std::string("mixed");
    if (skill["risk"].get<std::string>() != expected_risk)
    {
        error = context + ".risk must be " + expected_risk
            + " for its declared recipe risks.";
        return false;
    }

    const auto resources = skill.find("resources");
    if (resources == skill.end() || !resources->is_array())
    {
        error = context + ".resources must be an array.";
        return false;
    }
    const auto skill_directory = manifest.parent_path();
    if (!IsContainedRelativeFile(
            skill_directory,
            skill["entrypoint"].get<std::string>(),
            error))
    {
        return false;
    }
    if (!SkillFrontmatterMatches(
            skill_directory / skill["entrypoint"].get<std::string>(),
            id,
            error))
    {
        return false;
    }
    std::set<std::string> resource_paths;
    for (std::size_t index = 0; index < resources->size(); ++index)
    {
        const auto& resource = (*resources)[index];
        const std::string resource_context =
            context + ".resources[" + std::to_string(index) + "]";
        if (!resource.is_object()
            || !NonEmptyString(
                resource,
                "path",
                resource_context,
                error)
            || !NonEmptyString(
                resource,
                "description",
                resource_context,
                error))
        {
            if (error.empty())
            {
                error = resource_context + " must be an object.";
            }
            return false;
        }
        const std::string path = resource["path"].get<std::string>();
        if (!resource_paths.insert(path).second)
        {
            error = resource_context + ".path must be unique.";
            return false;
        }
        if (!IsContainedRelativeFile(skill_directory, path, error))
        {
            return false;
        }
    }
    return true;
}

} // namespace

std::optional<SkillCatalog> SkillCatalog::Load(
    const std::filesystem::path& root,
    const CapabilityCatalog& capabilities,
    std::string& error)
{
    std::error_code path_error;
    auto normalized_root =
        std::filesystem::weakly_canonical(root, path_error);
    if (path_error)
    {
        normalized_root = std::filesystem::absolute(root, path_error);
    }
    if (path_error || !IsSkillRoot(normalized_root))
    {
        error = "Skill root does not contain readable skill.json files: "
            + root.generic_string();
        return std::nullopt;
    }

    std::vector<std::filesystem::path> manifests;
    for (const auto& entry :
         std::filesystem::directory_iterator(
             normalized_root,
             path_error))
    {
        if (path_error)
        {
            break;
        }
        std::error_code entry_error;
        if (entry.is_directory(entry_error)
            && !entry_error)
        {
            const auto manifest = entry.path() / "skill.json";
            if (std::filesystem::is_regular_file(
                    manifest,
                    entry_error)
                && !entry_error)
            {
                manifests.push_back(manifest);
            }
        }
    }
    if (path_error)
    {
        error = "Skill root could not be enumerated: "
            + normalized_root.generic_string();
        return std::nullopt;
    }
    std::sort(manifests.begin(), manifests.end());

    SkillCatalog catalog;
    catalog.root_ = normalized_root;
    for (const auto& manifest : manifests)
    {
        std::ifstream stream(manifest, std::ios::binary);
        if (!stream)
        {
            error = "Skill manifest could not be opened: "
                + manifest.generic_string();
            return std::nullopt;
        }
        json skill = json::parse(stream, nullptr, false, true);
        if (!ValidateSkill(skill, manifest, capabilities, error))
        {
            return std::nullopt;
        }
        const std::string id = skill["id"].get<std::string>();
        if (!catalog.descriptors_.emplace(id, std::move(skill)).second)
        {
            error = "Duplicate skill id: " + id;
            return std::nullopt;
        }
    }
    if (catalog.descriptors_.empty())
    {
        error = "Skill catalog is empty.";
        return std::nullopt;
    }
    return catalog;
}

const json* SkillCatalog::Find(const std::string& id) const
{
    const auto descriptor = descriptors_.find(id);
    return descriptor == descriptors_.end()
        ? nullptr
        : &descriptor->second;
}

const std::map<std::string, json>& SkillCatalog::Descriptors() const
{
    return descriptors_;
}

const std::filesystem::path& SkillCatalog::Root() const
{
    return root_;
}

std::size_t SkillCatalog::Size() const
{
    return descriptors_.size();
}

std::optional<std::filesystem::path> ResolveSkillRoot(
    const std::filesystem::path& executable,
    const std::filesystem::path& explicit_root,
    std::vector<std::filesystem::path>* checked,
    std::string& error)
{
    std::vector<std::filesystem::path> candidates;
    if (!explicit_root.empty())
    {
        candidates.push_back(explicit_root);
    }
    else
    {
        if (const auto environment = Environment("UE_SKILL_ROOT"))
        {
            candidates.emplace_back(*environment);
        }
        if (!executable.empty())
        {
            const auto bin = executable.parent_path();
            candidates.push_back(bin / ".." / ".." / "skills");
            candidates.push_back(
                bin / ".." / "share" / "ue-workflow" / "Skills");
        }
        if (std::string_view(UE_CLI_SOURCE_ROOT).size() > 0)
        {
            candidates.emplace_back(
                std::filesystem::path(UE_CLI_SOURCE_ROOT) / "skills");
        }
        candidates.push_back(
            std::filesystem::current_path() / "skills");
    }

    std::set<std::string> seen;
    for (const auto& candidate : candidates)
    {
        std::error_code path_error;
        auto absolute = std::filesystem::absolute(candidate, path_error);
        if (path_error)
        {
            absolute = candidate;
        }
        const std::string key = absolute.lexically_normal().generic_string();
        if (!seen.insert(key).second)
        {
            continue;
        }
        if (checked)
        {
            checked->push_back(absolute);
        }
        if (IsSkillRoot(absolute))
        {
            return absolute;
        }
    }
    error = explicit_root.empty()
        ? "Could not locate packaged Agent Skills. Set UE_SKILL_ROOT "
          "or pass --skill-root."
        : "The explicit skill root is not readable: "
          + explicit_root.generic_string();
    return std::nullopt;
}

} // namespace ue::command
