// Command implementations for the nuttool CLI.
#pragma once

#include "common.hpp"

#include <functional>
#include <map>
#include <set>
#include <vector>

namespace nut::cli {

struct Args {
    std::vector<std::string> pos;
    std::map<std::string, std::string> opts;
    std::set<std::string> flags;

    const std::string& at(size_t i, const char* what) const;
    std::string opt(const std::string& k, const std::string& def = "") const;
    bool flag(const std::string& f) const { return flags.count(f) != 0; }
};

struct Command {
    const char* name;
    const char* args;
    const char* help;
    std::function<int(const Args&)> fn;
};
const std::vector<Command>& commands();
void usage();
// Runs one command line (args[0] is the command); errors are printed, the exit code returned.
int run(const std::vector<std::string>& args);

// Command output: to stdout, or to the innermost SinkScope's function on this thread (scripts).
void print(const char* fmt, ...);
using Sink = std::function<void(const std::string&)>;
class SinkScope {
public:
    explicit SinkScope(const Sink& s);
    ~SinkScope();
    SinkScope(const SinkScope&) = delete;
    SinkScope& operator=(const SinkScope&) = delete;

private:
    const Sink* prev_;
};

int info(const Args& a);
int hash(const Args& a);
int index(const Args& a);
int list(const Args& a);
int decompress(const Args& a);
int compress(const Args& a);
int extract(const Args& a);
int replace(const Args& a);
int caffUnpack(const Args& a);
int caffPack(const Args& a);
int sbUnpack(const Args& a);
int sbPack(const Args& a);
int texExport(const Args& a);
int texImport(const Args& a);
int modelExport(const Args& a);
int modelImport(const Args& a);
int modelSwap(const Args& a);
int modelFbx(const Args& a);
int animImport(const Args& a);
int textExport(const Args& a);
int textImport(const Args& a);
int gamescriptExport(const Args& a);
int gamescriptImport(const Args& a);
int objparamsExport(const Args& a);
int objparamsImport(const Args& a);
int fontExport(const Args& a);
int fontImport(const Args& a);
int xmvInfo(const Args& a);
int waveExport(const Args& a);
int iniExport(const Args& a);
int iniImport(const Args& a);
int extractAll(const Args& a);
int verify(const Args& a);
int makeNbpatch(const Args& a);

} // namespace nut::cli
