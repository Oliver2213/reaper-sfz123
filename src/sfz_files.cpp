#include "sfz_files.h"

#include "reaper.h"

#include <algorithm>
#include <fstream>
#include <map>
#include <set>
#include <sstream>

namespace sfp {

namespace {

std::string parentDir(const std::string& path)
{
    auto slash = path.find_last_of('/');
    return slash == std::string::npos ? std::string() : path.substr(0, slash);
}

std::string trim(const std::string& s)
{
    size_t a = s.find_first_not_of(" \t\r\n");
    size_t b = s.find_last_not_of(" \t\r\n");
    return a == std::string::npos ? std::string() : s.substr(a, b - a + 1);
}

std::string stripComments(const std::string& text)
{
    std::string out;
    out.reserve(text.size());
    for (size_t i = 0; i < text.size(); ++i) {
        if (text.compare(i, 2, "//") == 0) {
            while (i < text.size() && text[i] != '\n')
                ++i;
            if (i < text.size())
                out += '\n';
        } else if (text.compare(i, 2, "/*") == 0) {
            size_t end = text.find("*/", i + 2);
            i = end == std::string::npos ? text.size() : end + 1;
        } else {
            out += text[i];
        }
    }
    return out;
}

bool isOpcodeChar(char c)
{
    return std::isalnum((unsigned char)c) || c == '_' || c == '$';
}

struct Collector {
    std::string rootDir;
    std::string defaultPath;
    std::map<std::string, std::string> defines;
    std::set<std::string> files;
    int depth = 0;

    std::string expand(std::string line) const
    {
        // longest names first, so $DIR2 is not replaced as $DIR + "2"
        std::vector<std::pair<std::string, std::string>> sorted(defines.begin(), defines.end());
        std::sort(sorted.begin(), sorted.end(), [](auto& a, auto& b) { return a.first.size() > b.first.size(); });
        for (const auto& [name, value] : sorted)
            for (size_t p = line.find(name); p != std::string::npos; p = line.find(name, p + value.size()))
                line.replace(p, name.size(), value);
        return line;
    }

    void addFile(std::string rel)
    {
        std::replace(rel.begin(), rel.end(), '\\', '/');
        std::string path = joinPath(rootDir, rel);
        if (fileExists(path))
            files.insert(path);
    }

    void parse(const std::string& path)
    {
        std::ifstream in(path, std::ios::binary);
        if (!in || ++depth > 32)
            return;
        files.insert(path);
        std::stringstream ss;
        ss << in.rdbuf();

        std::istringstream lines(stripComments(ss.str()));
        std::string raw;
        while (std::getline(lines, raw)) {
            std::string line = trim(raw);
            if (line.rfind("#define", 0) == 0) {
                std::istringstream d(line.substr(7));
                std::string name, value;
                d >> name;
                std::getline(d, value);
                if (!name.empty())
                    defines[name] = trim(value);
                continue;
            }
            line = expand(line);
            if (line.rfind("#include", 0) == 0) {
                size_t a = line.find('"'), b = line.rfind('"');
                if (a != std::string::npos && b > a) {
                    std::string inc = line.substr(a + 1, b - a - 1);
                    std::replace(inc.begin(), inc.end(), '\\', '/');
                    parse(joinPath(rootDir, inc)); // includes are relative to the root .sfz
                }
                continue;
            }
            scanOpcodes(line);
        }
        --depth;
    }

    // opcode values run to the next "name=" or header; sample paths may contain spaces
    void scanOpcodes(const std::string& line)
    {
        std::vector<std::pair<size_t, size_t>> ops; // name start, '=' position
        for (size_t eq = line.find('='); eq != std::string::npos; eq = line.find('=', eq + 1)) {
            size_t start = eq;
            while (start > 0 && isOpcodeChar(line[start - 1]))
                --start;
            if (start < eq && (start == 0 || std::isspace((unsigned char)line[start - 1]) || line[start - 1] == '>'))
                ops.push_back({ start, eq });
        }
        for (size_t i = 0; i < ops.size(); ++i) {
            std::string name = line.substr(ops[i].first, ops[i].second - ops[i].first);
            size_t end = i + 1 < ops.size() ? ops[i + 1].first : line.size();
            size_t header = line.find('<', ops[i].second);
            if (header != std::string::npos && header < end)
                end = header;
            std::string value = trim(line.substr(ops[i].second + 1, end - ops[i].second - 1));
            if (name == "default_path") {
                defaultPath = value;
                std::replace(defaultPath.begin(), defaultPath.end(), '\\', '/');
            } else if (name == "sample" && !value.empty() && value[0] != '*') {
                addFile(defaultPath + value);
            }
        }
    }
};

} // namespace

std::vector<std::string> collectSfzFiles(const std::string& sfzPath)
{
    Collector c;
    c.rootDir = parentDir(sfzPath);
    c.parse(sfzPath);
    return { c.files.begin(), c.files.end() };
}

std::string commonDirectory(const std::vector<std::string>& files)
{
    if (files.empty())
        return {};
    std::string common = parentDir(files[0]);
    for (const auto& f : files)
        while (!common.empty() && f.compare(0, common.size() + 1, common + "/") != 0)
            common = parentDir(common);
    return common;
}

} // namespace sfp
