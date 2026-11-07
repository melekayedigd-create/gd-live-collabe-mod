#pragma once
// Pure helpers for comparing editor states. No Geode dependency.
//
// Every object in a GD level has a "save string" such as  1,8,2,105,3,15
// (key,value pairs: 1 = object id, 2 = x, 3 = y, ...).
// We compare the multiset of save strings every few tenths of a second:
//   strings that appeared   -> objects added / changed (new version)
//   strings that disappeared -> objects removed / changed (old version)
// Moving or editing an object is simply "remove old version + add new version".

#include <string>
#include <vector>
#include <unordered_map>
#include <cmath>
#include <cstdio>
#include <cstdlib>

namespace collab {

using Counts = std::unordered_map<std::string, int>;

struct Diff {
    std::vector<std::string> added;
    std::vector<std::string> removed;
    bool empty() const { return added.empty() && removed.empty(); }
};

inline Diff computeDiff(Counts const& before, Counts const& after) {
    Diff d;
    for (auto const& [str, n] : after) {
        auto it = before.find(str);
        int had = it == before.end() ? 0 : it->second;
        for (int i = had; i < n; i++) d.added.push_back(str);
    }
    for (auto const& [str, n] : before) {
        auto it = after.find(str);
        int has = it == after.end() ? 0 : it->second;
        for (int i = has; i < n; i++) d.removed.push_back(str);
    }
    return d;
}

// "-old\n-old2\n+new\n"  (removals first, so an edited object is replaced in place)
inline std::string encodeDiff(Diff const& d) {
    std::string out;
    for (auto const& s : d.removed) { out += '-'; out += s; out += '\n'; }
    for (auto const& s : d.added)   { out += '+'; out += s; out += '\n'; }
    return out;
}

inline Diff decodeDiff(std::string const& body) {
    Diff d;
    size_t start = 0;
    while (start < body.size()) {
        size_t end = body.find('\n', start);
        if (end == std::string::npos) end = body.size();
        if (end > start + 1) {
            std::string line = body.substr(start + 1, end - start - 1);
            if (body[start] == '-') d.removed.push_back(std::move(line));
            else if (body[start] == '+') d.added.push_back(std::move(line));
        }
        start = end + 1;
    }
    return d;
}

// A looser identity (object id + position rounded to 0.1) used when the
// exact save string differs slightly between two devices.
inline std::string signatureOf(std::string const& objStr) {
    std::string id, x, y;
    size_t pos = 0;
    while (pos < objStr.size()) {
        size_t c1 = objStr.find(',', pos);
        if (c1 == std::string::npos) break;
        size_t c2 = objStr.find(',', c1 + 1);
        if (c2 == std::string::npos) c2 = objStr.size();
        std::string key = objStr.substr(pos, c1 - pos);
        std::string val = objStr.substr(c1 + 1, c2 - c1 - 1);
        if (key == "1") id = val;
        else if (key == "2") x = val;
        else if (key == "3") y = val;
        if (!id.empty() && !x.empty() && !y.empty()) break;
        pos = c2 + 1;
    }
    char buf[96];
    std::snprintf(buf, sizeof(buf), "%s|%.1f|%.1f", id.c_str(),
        std::round(std::atof(x.c_str()) * 10.0) / 10.0,
        std::round(std::atof(y.c_str()) * 10.0) / 10.0);
    return buf;
}

} // namespace collab
