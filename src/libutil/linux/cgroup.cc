#include "nix/util/cgroup.hh"
#include "nix/util/signals.hh"
#include "nix/util/split.hh"
#include "nix/util/util.hh"
#include "nix/util/file-system.hh"
#include "nix/util/finally.hh"

#include <algorithm>
#include <boost/unordered/unordered_flat_set.hpp>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <thread>

#include <dirent.h>
#include <mntent.h>

namespace nix {

std::optional<std::filesystem::path> getCgroupFS()
{
    static auto res = [&]() -> std::optional<std::filesystem::path> {
        auto fp = fopen("/proc/mounts", "r");
        if (!fp)
            return std::nullopt;
        Finally delFP = [&]() { fclose(fp); };
        while (auto ent = getmntent(fp))
            if (std::string_view(ent->mnt_type) == "cgroup2")
                return ent->mnt_dir;

        return std::nullopt;
    }();
    return res;
}

// FIXME: obsolete, check for cgroup2
StringMap getCgroups(const std::filesystem::path & cgroupFile)
{
    StringMap cgroups;

    for (auto line : tokenizeString(readFile(cgroupFile), "\n")) {
        auto lineView = std::string_view(line);

        // ([0-9]+):([^:]*):(.*)
        auto firstSplit = splitOnce(lineView, ':');
        if (!firstSplit)
            throw Error("invalid line '%s' in '%s'", line, cgroupFile);

        auto secondSplit = splitOnce(firstSplit->second, ':');
        if (!secondSplit)
            throw Error("invalid line '%s' in '%s'", line, cgroupFile);

        auto hierarchyId = firstSplit->first;
        auto controller = secondSplit->first;
        auto path = secondSplit->second;

        if (hierarchyId.empty() || !std::ranges::all_of(hierarchyId, [](char c) { return isAsciiDigit(c); }))
            throw Error("invalid line '%s' in '%s'", line, cgroupFile);

        std::string name =
            controller.starts_with("name=") ? std::string(controller.substr(5)) : std::string(controller);
        cgroups.insert_or_assign(std::move(name), std::string(path));
    }

    return cgroups;
}

CgroupStats getCgroupStats(const std::filesystem::path & cgroup)
{
    CgroupStats stats;

    auto cpustatPath = cgroup / "cpu.stat";

    if (pathExists(cpustatPath)) {
        for (auto line : tokenizeString(readFile(cpustatPath), "\n")) {
            std::string_view userPrefix = "user_usec ";
            if (line.starts_with(userPrefix)) {
                auto n = string2Int<uint64_t>(line.substr(userPrefix.size()));
                if (n)
                    stats.cpuUser = std::chrono::microseconds(*n);
            }

            std::string_view systemPrefix = "system_usec ";
            if (line.starts_with(systemPrefix)) {
                auto n = string2Int<uint64_t>(line.substr(systemPrefix.size()));
                if (n)
                    stats.cpuSystem = std::chrono::microseconds(*n);
            }
        }
    }

    return stats;
}

static CgroupStats destroyCgroup(const std::filesystem::path & cgroup, bool returnStats)
{
    if (!pathExists(cgroup))
        return {};

    auto procsFile = cgroup / "cgroup.procs";

    if (!pathExists(procsFile))
        throw Error("'%s' is not a cgroup", cgroup);

    /* Use the fast way to kill every process in a cgroup, if
       available. */
    auto killFile = cgroup / "cgroup.kill";
    if (pathExists(killFile))
        writeFile(killFile, "1");

    /* Otherwise, manually kill every process in the subcgroups and
       this cgroup. */
    for (auto & entry : DirectoryIterator{cgroup}) {
        checkInterrupt();
        if (entry.symlink_status().type() != std::filesystem::file_type::directory)
            continue;
        destroyCgroup(cgroup / entry.path().filename(), false);
    }

    int round = 1;

    boost::unordered_flat_set<pid_t> pidsShown;

    while (true) {
        auto pids = tokenizeString(readFile(procsFile));
        auto it = pids.begin();
        if (it == pids.end())
            break;

        if (round > 20)
            throw Error("cannot kill cgroup '%s'", cgroup);

        for (; it != pids.end(); ++it) {
            pid_t pid;
            if (auto o = string2Int<pid_t>(*it))
                pid = *o;
            else
                throw Error("invalid pid '%s'", pid);
            if (pidsShown.insert(pid).second) {
                try {
                    auto cmdline = readFile(fmt("/proc/%d/cmdline", pid));
                    using namespace std::string_literals;
                    warn("killing stray builder process %d (%s)...", pid, trim(replaceStrings(cmdline, "\0"s, " ")));
                } catch (SystemError &) {
                }
            }
            // FIXME: pid wraparound
            if (kill(pid, SIGKILL) == -1 && errno != ESRCH)
                throw SysError("killing member %d of cgroup '%s'", pid, cgroup);
        }

        auto sleep = std::chrono::milliseconds((int) std::pow(2.0, std::min(round, 10)));
        if (sleep.count() > 100)
            printError("waiting for %d ms for cgroup '%s' to become empty", sleep.count(), cgroup);
        std::this_thread::sleep_for(sleep);
        round++;
    }

    CgroupStats stats;
    if (returnStats)
        stats = getCgroupStats(cgroup);

    if (rmdir(cgroup.c_str()) == -1)
        throw SysError("deleting cgroup %s", cgroup);

    return stats;
}

CgroupStats destroyCgroup(const std::filesystem::path & cgroup)
{
    return destroyCgroup(cgroup, true);
}

CanonPath getCurrentCgroup()
{
    auto cgroupFS = getCgroupFS();
    if (!cgroupFS)
        throw Error("cannot determine the cgroups file system");

    auto ourCgroups = getCgroups("/proc/self/cgroup");
    auto ourCgroup = ourCgroups[""];
    if (ourCgroup == "")
        throw Error("cannot determine cgroup name from /proc/self/cgroup");
    return CanonPath{ourCgroup};
}

CanonPath getRootCgroup()
{
    static auto rootCgroup = getCurrentCgroup();
    return rootCgroup;
}

} // namespace nix
