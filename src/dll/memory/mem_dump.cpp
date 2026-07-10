// src/dll/memory/mem_dump.cpp
// Ported from CCCaster MemDump.cpp. Removed cereal/file save-load.

#include "mem_dump.hpp"
#include "util/algorithms.hpp"

#include <algorithm>
#include <cstring>
#include <list>

namespace caster::dll {

// ---- MemDumpBase constructors ----
//
// Defined out-of-line here because they need MemDumpPtr to be complete
// (the constructor body forwards to setParents which constructs
// MemDumpPtr instances). The header only forward-declares MemDumpPtr
// to break the recursive size dependency.

MemDumpBase::MemDumpBase(size_t sz) : size(sz) {}

MemDumpBase::MemDumpBase(size_t sz, const std::vector<MemDumpPtr>& p)
    : size(sz), ptrs(setParents(p)) {}

MemDumpBase::MemDumpBase(size_t sz, std::vector<std::shared_ptr<MemDumpPtr>> p)
    : size(sz), ptrs(std::move(p)) {}

void MemDumpBase::saveDump(char*& dump, const char* parentAddr) const {
    const char* a = getAddr(parentAddr);
    if (a) std::memcpy(dump, a, size);
    else std::memset(dump, 0, size);
    dump += size;
    // Forward this node's freshly-computed address as the parentAddr
    // for the children — they chase pointers relative to it.
    for (const auto& ptr : ptrs) ptr->saveDump(dump, a);
}

void MemDumpBase::loadDump(const char*& dump, char* parentAddr) const {
    char* a = getAddr(parentAddr);
    if (a) std::memcpy(a, dump, size);
    dump += size;
    for (const auto& ptr : ptrs) ptr->loadDump(dump, a);
}

size_t MemDumpBase::getTotalSize() const {
    size_t total = size;
    for (const auto& ptr : ptrs) total += ptr->getTotalSize();
    return total;
}

// ---- Protected static helpers ----
//
// setParents takes a `std::vector<MemDumpPtr>` (by value) as input —
// callers pass initializer lists or temporary vectors constructed
// from `{offset, offset, size, {children...}}` literals. It returns a
// `std::vector<std::shared_ptr<MemDumpPtr>>` suitable for storage in
// MemDumpBase::ptrs. The name is preserved for historical reasons;
// there is no longer a stored `parent` to set — each child is simply
// copied into a heap-allocated shared_ptr with the same offsets/size/
// children. Linkage to the parent is established at save/load time via
// the explicit parentAddr parameter.
//
// addOffsets and concat operate on the shared_ptr representation
// directly because they're called from internal code (the MemDump merge
// constructor) that already has shared_ptr vectors in hand.

std::vector<std::shared_ptr<MemDumpPtr>> MemDumpBase::setParents(
    const std::vector<MemDumpPtr>& p) {
    std::vector<std::shared_ptr<MemDumpPtr>> ret;
    ret.reserve(p.size());
    for (const auto& ptr : p) {
        ret.push_back(std::make_shared<MemDumpPtr>(
            ptr.ptrs, ptr.srcOffset, ptr.dstOffset, ptr.size));
    }
    return ret;
}

std::vector<std::shared_ptr<MemDumpPtr>> MemDumpBase::addOffsets(
    const std::vector<std::shared_ptr<MemDumpPtr>>& p, size_t addSrcOffset) {
    std::vector<std::shared_ptr<MemDumpPtr>> ret;
    ret.reserve(p.size());
    for (const auto& ptr : p) {
        ret.push_back(std::make_shared<MemDumpPtr>(
            ptr->ptrs,
            ptr->srcOffset + addSrcOffset, ptr->dstOffset, ptr->size));
    }
    return ret;
}

std::vector<std::shared_ptr<MemDumpPtr>> MemDumpBase::concat(
    const std::vector<std::shared_ptr<MemDumpPtr>>& a,
    const std::vector<std::shared_ptr<MemDumpPtr>>& b) {
    std::vector<std::shared_ptr<MemDumpPtr>> ret;
    ret.reserve(a.size() + b.size());
    for (const auto& p : a) ret.push_back(p);
    for (const auto& p : b) ret.push_back(p);
    return ret;
}

// ---- MemDumpList ----

void MemDumpList::update() {
    if (addrs.empty()) return;
    // Sort roots by their base address. All elements are MemDump
    // instances, so getAddr(nullptr) just returns `addr`.
    auto sortedAddrs = sorted(addrs, [](const MemDumpBase& a, const MemDumpBase& b) {
        return a.getAddr(nullptr) < b.getAddr(nullptr);
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
