#include <algorithm>
#include <optional>
#include <string>
#include <string_view>

#include "nix/flake/url-name.hh"
#include "nix/util/ascii.hh"
#include "nix/util/util.hh"
#include "nix/util/url.hh"

namespace nix {

static constexpr bool isNameTokenChar(char c)
{
    return isAsciiAlpha(c) || isAsciiDigit(c) || c == '_' || c == '-';
}

static bool isNameToken(std::string_view s)
{
    // [a-zA-Z0-9_-]+
    return !s.empty() && std::ranges::all_of(s, isNameTokenChar);
}

static std::optional<std::string_view> consumeNameToken(std::string_view & s)
{
    size_t i = 0;
    while (i < s.size() && isNameTokenChar(s[i]))
        ++i;
    if (i == 0)
        return std::nullopt;
    auto res = s.substr(0, i);
    s.remove_prefix(i);
    return res;
}

static std::optional<std::string_view> lastAttributeFromFragment(std::string_view fragment)
{
    // ^((?:[a-zA-Z0-9_-]+\\.)*)([a-zA-Z0-9_-]+)(\\^.*)?$
    // - The `^...` suffix indicates output selectors (`^bin,man`, `^*`, etc.) and is ignored for naming.
    // - Reject the special `defaultPackage.*` prefix and the final attribute `default`.
    if (auto caret = fragment.find('^'); caret != std::string_view::npos)
        fragment = fragment.substr(0, caret);

    if (fragment.empty())
        return std::nullopt;

    auto remaining = fragment;

    auto first = consumeNameToken(remaining);
    if (!first)
        return std::nullopt;

    std::string_view last = *first;
    size_t tokenCount = 1;

    while (stripPrefix(remaining, ".")) {
        auto tok = consumeNameToken(remaining);
        if (!tok)
            return std::nullopt;

        last = *tok;
        ++tokenCount;
    }

    if (!remaining.empty())
        return std::nullopt;

    // Old behavior rejected exactly "defaultPackage.<attr>" (but allowed e.g. "defaultPackage.x86_64-linux.<attr>").
    if (tokenCount == 2 && *first == "defaultPackage")
        return std::nullopt;
    if (last == "default")
        return std::nullopt;

    return last;
}

static bool isGitProvider(std::string_view scheme)
{
    return scheme == "github" || scheme == "gitlab" || scheme == "sourcehut";
}

static std::optional<std::string_view> repoNameFromProviderPath(std::string_view path)
{
    // Old behavior matched: <owner>/<repo>(/.*)?
    //
    // Note: this operates on the rendered path to preserve historical behavior. In particular,
    // percent-decoded "%2F" within a path element turns into "/" in the rendered path and is
    // treated as a separator.
    auto remaining = path;

    if (!consumeNameToken(remaining))
        return std::nullopt;

    if (!stripPrefix(remaining, "/"))
        return std::nullopt;

    auto repo = consumeNameToken(remaining);
    if (!repo)
        return std::nullopt;

    if (!remaining.empty() && remaining.front() != '/')
        return std::nullopt;

    return *repo;
}

static bool isGitScheme(std::string_view scheme)
{
    // git($|\\+.*)
    return scheme == "git" || scheme.starts_with("git+");
}

static std::optional<std::string_view> lastPathSegmentFromPath(std::string_view path)
{
    // Old behavior matched: .*/(<token>)
    //
    // Note: this operates on the rendered path to preserve historical behavior. In particular,
    // percent-decoded "%2F" within a path element turns into "/" in the rendered path and is
    // treated as a separator.
    auto slash = path.rfind('/');
    if (slash == std::string_view::npos)
        return std::nullopt;
    auto last = path.substr(slash + 1);
    if (!isNameToken(last))
        return std::nullopt;
    return last;
}

std::optional<std::string> getNameFromURL(const ParsedURL & url)
{
    /* If there is a dir= argument, use its value */
    if (auto it = url.query.find("dir"); it != url.query.end())
        return it->second;

    /* If the fragment looks like an attribute path and isn't "default", use the last attribute */
    if (auto attr = lastAttributeFromFragment(url.fragment))
        return std::string{*attr};

    auto path = url.renderPath(/*encode=*/false);

    /* If this is a github/gitlab/sourcehut flake, use the repo name */
    if (isGitProvider(url.scheme)) {
        if (auto repo = repoNameFromProviderPath(path))
            return std::string{*repo};
    }

    auto last = lastPathSegmentFromPath(path);

    /* If it is a regular git flake, use the directory name */
    if (isGitScheme(url.scheme)) {
        if (last)
            return std::string{*last};
    }

    /* As a last resort, take the last element of the path */
    if (last)
        return std::string{*last};

    /* If even that didn't work, the URL does not contain enough info to determine a useful name */
    return {};
}

} // namespace nix
