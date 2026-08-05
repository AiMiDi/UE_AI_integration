#include "UECommandCli/CapabilityCatalog.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <fstream>
#include <set>
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

bool IsCatalogRoot(const std::filesystem::path& root)
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
        if (entry.is_regular_file(error)
            && !error
            && entry.path().extension() == ".json")
        {
            return true;
        }
    }
    return false;
}

std::string NormalizeSearchMetadataValue(
    const std::string_view value)
{
    std::size_t begin = 0;
    while (begin < value.size()
        && static_cast<unsigned char>(value[begin]) <= 0x20)
    {
        ++begin;
    }
    std::size_t end = value.size();
    while (end > begin
        && static_cast<unsigned char>(value[end - 1]) <= 0x20)
    {
        --end;
    }
    std::string normalized(value.substr(begin, end - begin));
    std::transform(
        normalized.begin(),
        normalized.end(),
        normalized.begin(),
        [](const unsigned char character)
        {
            return static_cast<char>(
                character >= 'A' && character <= 'Z'
                    ? character - 'A' + 'a'
                    : character);
        });
    return normalized;
}

bool ValidateSearchMetadata(
    const json& descriptor,
    const std::filesystem::path& manifest,
    std::string& error)
{
    const auto search = descriptor.find("search");
    if (search == descriptor.end())
    {
        return true;
    }
    if (!search->is_object())
    {
        error =
            "Capability search metadata must be an object in "
            + manifest.generic_string();
        return false;
    }
    static const std::set<std::string> allowed = {
        "title",
        "keywords",
        "aliases",
    };
    for (const auto& [field, ignored] : search->items())
    {
        (void)ignored;
        if (!allowed.contains(field))
        {
            error =
                "Unsupported capability search field '" + field
                + "' in " + manifest.generic_string();
            return false;
        }
    }
    const auto title = search->find("title");
    if (title != search->end()
        && (!title->is_string()
            || NormalizeSearchMetadataValue(
                title->get_ref<const std::string&>()).empty()))
    {
        error =
            "Capability search title must be non-empty in "
            + manifest.generic_string();
        return false;
    }
    for (const char* field : { "keywords", "aliases" })
    {
        const auto values = search->find(field);
        if (values == search->end())
        {
            continue;
        }
        if (!values->is_array() || values->empty())
        {
            error =
                "Capability search " + std::string(field)
                + " must be a non-empty array in "
                + manifest.generic_string();
            return false;
        }
        std::set<std::string> unique;
        for (const auto& value : *values)
        {
            if (!value.is_string()
                || NormalizeSearchMetadataValue(
                    value.get_ref<const std::string&>()).empty())
            {
                error =
                    "Capability search " + std::string(field)
                    + " must contain non-empty strings in "
                    + manifest.generic_string();
                return false;
            }
            const std::string normalized =
                NormalizeSearchMetadataValue(
                    value.get_ref<const std::string&>());
            if (!unique.insert(normalized).second)
            {
                error =
                    "Capability search " + std::string(field)
                    + " must not contain duplicates in "
                    + manifest.generic_string();
                return false;
            }
        }
    }
    if (title == search->end()
        && search->find("keywords") == search->end()
        && search->find("aliases") == search->end())
    {
        error =
            "Capability search metadata must declare title, keywords, "
            "or aliases in " + manifest.generic_string();
        return false;
    }
    return true;
}

bool ValidateExecutionMetadata(
    const json& descriptor,
    const std::filesystem::path& manifest,
    std::string& error)
{
    const auto execution = descriptor.find("execution");
    if (execution == descriptor.end())
    {
        return true;
    }
    if (!execution->is_object())
    {
        error =
            "Capability execution metadata must be an object in "
            + manifest.generic_string();
        return false;
    }
    for (const auto& [field, ignored] : execution->items())
    {
        (void)ignored;
        if (field != "backends" && field != "preferred")
        {
            error =
                "Unsupported capability execution field '" + field
                + "' in " + manifest.generic_string();
            return false;
        }
    }
    const auto backends = execution->find("backends");
    const auto preferred = execution->find("preferred");
    if (backends == execution->end()
        || !backends->is_array()
        || backends->empty()
        || preferred == execution->end()
        || !preferred->is_string())
    {
        error =
            "Capability execution metadata requires non-empty backends and "
            "a preferred backend in " + manifest.generic_string();
        return false;
    }
    std::set<std::string> unique;
    for (const auto& backend : *backends)
    {
        if (!backend.is_string()
            || (backend != "editor"
                && backend != "localTrace"
                && backend != "localRecipe"
                && backend != "localProject"
                && backend != "localAsset"
                && backend != "localSal"
                && backend != "developmentRuntime")
            || !unique.insert(backend.get<std::string>()).second)
        {
            error =
                "Capability execution backends must contain unique supported "
                "backend names in " + manifest.generic_string();
            return false;
        }
    }
    if (!unique.contains(preferred->get<std::string>()))
    {
        error =
            "Capability preferred execution backend must be declared in "
            + manifest.generic_string();
        return false;
    }
    return true;
}

} // namespace

std::optional<CapabilityCatalog> CapabilityCatalog::Load(
    const std::filesystem::path& root,
    std::string& error)
{
    std::error_code path_error;
    auto normalized_root =
        std::filesystem::weakly_canonical(root, path_error);
    if (path_error)
    {
        normalized_root = std::filesystem::absolute(root, path_error);
    }
    if (path_error || !IsCatalogRoot(normalized_root))
    {
        error =
            "Capability root does not contain readable JSON manifests: "
            + root.generic_string();
        return std::nullopt;
    }

    std::vector<std::filesystem::path> manifests;
    for (const auto& entry :
         std::filesystem::directory_iterator(normalized_root, path_error))
    {
        if (path_error)
        {
            error = "Capability manifest directory could not be enumerated.";
            return std::nullopt;
        }
        if (entry.is_regular_file(path_error)
            && !path_error
            && entry.path().extension() == ".json")
        {
            manifests.push_back(entry.path());
        }
    }
    std::sort(manifests.begin(), manifests.end());
    return LoadFiles(normalized_root, manifests, error);
}

std::optional<CapabilityCatalog> CapabilityCatalog::LoadForCapability(
    const std::filesystem::path& root,
    const std::string& capability_id,
    std::string& error)
{
    const auto separator = capability_id.find('.');
    if (separator == std::string::npos || separator == 0)
    {
        error = "A dotted capability ID is required.";
        return std::nullopt;
    }
    const std::string domain = capability_id.substr(0, separator);
    if (!std::all_of(
            domain.begin(),
            domain.end(),
            [](const unsigned char character)
            {
                return std::islower(character)
                    || std::isdigit(character)
                    || character == '_';
            }))
    {
        error = "Capability domain is invalid: " + domain;
        return std::nullopt;
    }

    std::error_code path_error;
    auto normalized_root =
        std::filesystem::weakly_canonical(root, path_error);
    if (path_error)
    {
        normalized_root = std::filesystem::absolute(root, path_error);
    }
    if (path_error || !IsCatalogRoot(normalized_root))
    {
        error =
            "Capability root does not contain readable JSON manifests: "
            + root.generic_string();
        return std::nullopt;
    }
    const auto manifest = normalized_root / (domain + ".json");
    if (!std::filesystem::is_regular_file(manifest, path_error)
        || path_error)
    {
        CapabilityCatalog empty;
        empty.root_ = normalized_root;
        return empty;
    }
    return LoadFiles(
        normalized_root,
        { manifest },
        error);
}

std::optional<CapabilityCatalog> CapabilityCatalog::LoadFiles(
    const std::filesystem::path& root,
    const std::vector<std::filesystem::path>& manifests,
    std::string& error)
{
    CapabilityCatalog catalog;
    catalog.root_ = root;
    for (const auto& manifest : manifests)
    {
        std::ifstream stream(manifest, std::ios::binary);
        if (!stream)
        {
            error =
                "Capability manifest could not be opened: "
                + manifest.generic_string();
            return std::nullopt;
        }
        json document = json::parse(stream, nullptr, false, true);
        if (!document.is_object()
            || document.value("schemaVersion", 0) != 3
            || !document.contains("capabilities")
            || !document["capabilities"].is_array())
        {
            error =
                "Capability manifest is invalid: "
                + manifest.generic_string();
            return std::nullopt;
        }
        if (document.contains("tombstones"))
        {
            if (!document["tombstones"].is_array())
            {
                error = "Capability tombstones must be an array in "
                    + manifest.generic_string();
                return std::nullopt;
            }
            for (const auto& tombstone : document["tombstones"])
            {
                if (!tombstone.is_object()
                    || !tombstone.contains("id")
                    || !tombstone["id"].is_string()
                    || !tombstone.contains("replacement")
                    || !tombstone["replacement"].is_string())
                {
                    error = "Capability tombstone is invalid in "
                        + manifest.generic_string();
                    return std::nullopt;
                }
                const std::string id = tombstone["id"].get<std::string>();
                if (catalog.descriptors_.contains(id)
                    || !catalog.tombstones_.emplace(id, tombstone).second)
                {
                    error = "Duplicate active or removed capability id: " + id;
                    return std::nullopt;
                }
            }
        }
        for (const auto& descriptor : document["capabilities"])
        {
            if (!descriptor.is_object()
                || !descriptor.contains("id")
                || !descriptor["id"].is_string()
                || !descriptor.contains("inputSchema")
                || !descriptor["inputSchema"].is_object())
            {
                error =
                    "Capability descriptor is invalid in "
                    + manifest.generic_string();
                return std::nullopt;
            }
            if (!ValidateSearchMetadata(descriptor, manifest, error))
            {
                return std::nullopt;
            }
            if (!ValidateExecutionMetadata(descriptor, manifest, error))
            {
                return std::nullopt;
            }
            const std::string id = descriptor["id"].get<std::string>();
            if (id.empty()
                || catalog.tombstones_.contains(id)
                || !catalog.descriptors_.emplace(id, descriptor).second)
            {
                error = "Duplicate or empty capability id: " + id;
                return std::nullopt;
            }
        }
    }
    if (catalog.descriptors_.empty())
    {
        error = "Capability catalog is empty.";
        return std::nullopt;
    }
    return catalog;
}

const json* CapabilityCatalog::Find(const std::string& id) const
{
    const auto descriptor = descriptors_.find(id);
    return descriptor == descriptors_.end()
        ? nullptr
        : &descriptor->second;
}

const json* CapabilityCatalog::FindTombstone(const std::string& id) const
{
    const auto tombstone = tombstones_.find(id);
    return tombstone == tombstones_.end()
        ? nullptr
        : &tombstone->second;
}

const std::map<std::string, json>& CapabilityCatalog::Descriptors() const
{
    return descriptors_;
}

const std::filesystem::path& CapabilityCatalog::Root() const
{
    return root_;
}

std::size_t CapabilityCatalog::Size() const
{
    return descriptors_.size();
}

std::optional<std::filesystem::path> ResolveCapabilityRoot(
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
        if (const auto environment = Environment("UE_CAPABILITY_ROOT"))
        {
            candidates.emplace_back(*environment);
        }
        if (!executable.empty())
        {
            const auto bin = executable.parent_path();
            candidates.push_back(
                bin / ".." / ".." / "Resources" / "Capabilities");
            candidates.push_back(
                bin / ".." / "share" / "ue-workflow" / "Capabilities");
        }
        if (std::string_view(UE_CLI_SOURCE_ROOT).size() > 0)
        {
            candidates.emplace_back(
                std::filesystem::path(UE_CLI_SOURCE_ROOT)
                / "Resources" / "Capabilities");
        }
        candidates.push_back(
            std::filesystem::current_path()
            / "Resources" / "Capabilities");
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
        if (IsCatalogRoot(absolute))
        {
            return absolute;
        }
    }
    error = explicit_root.empty()
        ? "Could not locate packaged capability manifests. Set "
          "UE_CAPABILITY_ROOT or pass --capability-root."
        : "The explicit capability root is not readable: "
          + explicit_root.generic_string();
    return std::nullopt;
}

} // namespace ue::command
