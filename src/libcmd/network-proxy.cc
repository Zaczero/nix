#include "nix/cmd/network-proxy.hh"

#include <algorithm>
#include <cctype>
#include <iterator>
#include <ranges>
#include <string>

#include "nix/util/environment-variables.hh"
#include "nix/util/types.hh"

namespace nix {

static const StringSet lowercaseVariables{"http_proxy", "https_proxy", "ftp_proxy", "all_proxy", "no_proxy"};

static StringSet getAllVariables()
{
    StringSet variables = lowercaseVariables;
    for (const auto & variable : lowercaseVariables) {
        std::string upperVariable(variable);
        std::ranges::transform(
            upperVariable, upperVariable.begin(), [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
        variables.insert(std::move(upperVariable));
    }
    return variables;
}

const StringSet networkProxyVariables = getAllVariables();

static StringSet getExcludingNoProxyVariables()
{
    static const StringSet excludeVariables{"no_proxy", "NO_PROXY"};
    StringSet variables;
    std::ranges::set_difference(networkProxyVariables, excludeVariables, std::inserter(variables, variables.begin()));
    return variables;
}

static const StringSet excludingNoProxyVariables = getExcludingNoProxyVariables();

bool haveNetworkProxyConnection()
{
    for (const auto & variable : excludingNoProxyVariables) {
        if (getEnv(variable).has_value()) {
            return true;
        }
    }
    return false;
}

} // namespace nix
