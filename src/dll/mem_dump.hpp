// src/dll/mem_dump.hpp
// Ported from CCCaster MemDump.hpp. Removed cereal. Manual save/load.
// Used by RollbackManager to save/restore game state.

#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <vector>

namespace caster::dll {

class MemDumpPtr;

class MemDumpBase {
public:
    const size_t size;
    const std::vector<MemDumpPtr> ptrs;

    MemDumpBase(size_t sz) : size(sz) {}
    MemDumpBase(size_t sz, const std::vector<MemDumpPtr>& p);

    virtual char* getAddr() const = 0;

    void saveDump(char*& dump) const;
    void loadDump(const char*& dump) const;
    size_t getTotalSize() const;

protected:
    static std::vector<MemDumpPtr> setParents(const std::vector<MemDumpPtr>& ptrs, const MemDumpBase* parent);
    static std::vector<MemDumpPtr> addOffsets(const std::vector<MemDumpPtr>& ptrs, size_t addSrcOffset);
    static std::vector<MemDumpPtr> concat(const std::vector<MemDumpPtr>& a, const std::vector<MemDumpPtr>& b);
};

class MemDumpPtr : public MemDumpBase {
public:
    const MemDumpBase* const parent = nullptr;
    const size_t srcOffset;
    const size_t dstOffset;

    MemDumpPtr(size_t src, size_t dst, size_t sz)
        : MemDumpBase(sz), srcOffset(src), dstOffset(dst) {}

    MemDumpPtr(size_t src, size_t dst, size_t sz, const std::vector<MemDumpPtr>& p)
        : MemDumpBase(sz, p), srcOffset(src), dstOffset(dst) {}

    MemDumpPtr(const MemDumpBase* parent, const std::vector<MemDumpPtr>& p, size_t src, size_t dst, size_t sz)
        : MemDumpBase(sz, p), parent(parent), srcOffset(src), dstOffset(dst) {}

    char* getAddr() const override {
        if (!parent || !parent->getAddr()) return nullptr;
        char* dstAddr = *(char**)(parent->getAddr() + srcOffset);
        if (!dstAddr) return nullptr;
        return dstAddr + dstOffset;
    }
};

inline MemDumpBase::MemDumpBase(size_t sz, const std::vector<MemDumpPtr>& p)
    : size(sz), ptrs(setParents(p, this)) {}

inline size_t MemDumpBase::getTotalSize() const {
    size_t total = size;
    for (const auto& ptr : ptrs) total += ptr.getTotalSize();
    return total;
}

class MemDump : public MemDumpBase {
public:
    char* const addr;

    MemDump(void* a, size_t sz) : MemDumpBase(sz), addr((char*)a) {}
    MemDump(void* a, size_t sz, const std::vector<MemDumpPtr>& p) : MemDumpBase(sz, p), addr((char*)a) {}
    MemDump(uint32_t start, uint32_t end) : MemDumpBase(end - start), addr((char*)(uintptr_t)start) {}
    MemDump(const MemDump& a, const MemDump& b)
        : MemDumpBase(a.size + b.size, concat(a.ptrs, addOffsets(b.ptrs, a.size))), addr(a.addr) {
        // a.addr + a.size must == b.addr (contiguous ranges).
    }

    char* getAddr() const override { return addr; }
};

class MemDumpList {
public:
    size_t totalSize = 0;
    std::vector<MemDump> addrs;

    void clear() { totalSize = 0; addrs.clear(); }
    bool empty() const { return addrs.empty(); }
    void append(const MemDump& mem) { addrs.push_back(mem); }
    void update();
};

} // namespace caster::dll
