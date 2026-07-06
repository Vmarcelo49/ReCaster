// src/dll/mem_dump.cpp
// Ported from CCCaster MemDump.cpp. Removed cereal/MD5/file save-load.

#include "mem_dump.hpp"
#include "algorithms.hpp"

#include <algorithm>
#include <cstring>
#include <list>

namespace caster::dll {

void MemDumpBase::saveDump(char*& dump) const {
    const char* a = getAddr();
    if (a) std::memcpy(dump, a, size);
    else std::memset(dump, 0, size);
    dump += size;
    for (const auto& ptr : ptrs) ptr.saveDump(dump);
}

void MemDumpBase::loadDump(const char*& dump) const {
    char* a = getAddr();
    if (a) std::memcpy(a, dump, size);
    dump += size;
    for (const auto& ptr : ptrs) ptr.loadDump(dump);
}

std::vector<MemDumpPtr> MemDumpBase::setParents(const std::vector<MemDumpPtr>& p, const MemDumpBase* parent) {
    std::vector<MemDumpPtr> ret;
    ret.reserve(p.size());
    for (const auto& ptr : p)
        ret.push_back(MemDumpPtr(parent, ptr.ptrs, ptr.srcOffset, ptr.dstOffset, ptr.size));
    return ret;
}

std::vector<MemDumpPtr> MemDumpBase::addOffsets(const std::vector<MemDumpPtr>& p, size_t addSrcOffset) {
    std::vector<MemDumpPtr> ret;
    ret.reserve(p.size());
    for (const auto& ptr : p)
        ret.push_back(MemDumpPtr(ptr.parent, ptr.ptrs, ptr.srcOffset + addSrcOffset, ptr.dstOffset, ptr.size));
    return ret;
}

std::vector<MemDumpPtr> MemDumpBase::concat(const std::vector<MemDumpPtr>& a, const std::vector<MemDumpPtr>& b) {
    std::vector<MemDumpPtr> ret;
    ret.reserve(a.size() + b.size());
    for (const auto& p : a) ret.push_back(p);
    for (const auto& p : b) ret.push_back(p);
    return ret;
}

void MemDumpList::update() {
    if (addrs.empty()) return;
    auto sortedAddrs = sorted(addrs, [](const MemDumpBase& a, const MemDumpBase& b) {
        return a.getAddr() < b.getAddr();
    });
    std::list<MemDump> sortedList(sortedAddrs.begin(), sortedAddrs.end());
    addrs.clear();
    addrs.push_back(sortedList.front());
    auto it = sortedList.begin();
    auto jt = it;
    for (++jt; jt != sortedList.end(); ++jt) {
        if (addrs.back().addr + addrs.back().size == jt->addr) {
            MemDump merged(addrs.back(), *jt);
            addrs.pop_back();
            addrs.push_back(merged);
            sortedList.erase(jt);
            jt = it;
            continue;
        }
        ++it;
        addrs.push_back(*jt);
    }
    totalSize = 0;
    for (const auto& mem : addrs) totalSize += mem.getTotalSize();
}

} // namespace caster::dll
