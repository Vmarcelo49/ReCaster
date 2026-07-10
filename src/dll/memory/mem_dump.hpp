// src/dll/memory/mem_dump.hpp
// Ported from CCCaster MemDump.hpp. Removed cereal. Manual save/load.
// Used by RollbackManager to save/restore game state.
//
// Refactored to break a recursive type dependency that exists in the
// original CCCaster code:
//
//   MemDumpBase has `std::vector<MemDumpPtr> ptrs` (by value)
//   MemDumpPtr    inherits from MemDumpBase
//
// GCC tolerates this as an extension (it computes sizes lazily), but
// Clang (LLVM-MinGW) correctly rejects it: "arithmetic on a pointer to
// an incomplete type 'MemDumpPtr'" because std::vector<MemDumpPtr>
// requires MemDumpPtr to be complete at the point of instantiation,
// which is impossible while MemDumpBase is still being defined.
//
// Fix: store children as std::shared_ptr<MemDumpPtr> instead of by
// value. A vector of pointers only requires a forward-declared type.
// The public API (constructor signatures taking
// `const std::vector<MemDumpPtr>&`) is preserved, so callers in
// rollback_addresses.hpp and elsewhere are unaffected.

#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <vector>

namespace caster::dll {

class MemDumpPtr;

class MemDumpBase {
public:
    const size_t size;
    // Children stored as shared_ptr to break the recursive size
    // dependency between MemDumpBase and MemDumpPtr (which inherits
    // from MemDumpBase). See file header comment for details.
    const std::vector<std::shared_ptr<MemDumpPtr>> ptrs;

    MemDumpBase(size_t sz);
    MemDumpBase(size_t sz, const std::vector<MemDumpPtr>& p);
    // Construct from already-shared children (used by the MemDump merge
    // constructor, which composes children from two existing MemDump
    // instances via addOffsets/concat).
    MemDumpBase(size_t sz, std::vector<std::shared_ptr<MemDumpPtr>> p);

    virtual char* getAddr() const = 0;

    void saveDump(char*& dump) const;
    void loadDump(const char*& dump) const;
    size_t getTotalSize() const;

protected:
    static std::vector<std::shared_ptr<MemDumpPtr>> setParents(
        const std::vector<MemDumpPtr>& ptrs, const MemDumpBase* parent);
    static std::vector<std::shared_ptr<MemDumpPtr>> addOffsets(
        const std::vector<std::shared_ptr<MemDumpPtr>>& ptrs, size_t addSrcOffset);
    static std::vector<std::shared_ptr<MemDumpPtr>> concat(
        const std::vector<std::shared_ptr<MemDumpPtr>>& a,
        const std::vector<std::shared_ptr<MemDumpPtr>>& b);
};

class MemDumpPtr : public MemDumpBase {
public:
    // The parent memory dump (set by MemDumpBase::setParents at
    // construction time; raw pointer because the parent owns this child
    // via its `ptrs` vector).
    const MemDumpBase* parent = nullptr;

    const size_t srcOffset;
    const size_t dstOffset;

    MemDumpPtr(size_t src, size_t dst, size_t sz)
        : MemDumpBase(sz), srcOffset(src), dstOffset(dst) {}

    MemDumpPtr(size_t src, size_t dst, size_t sz, const std::vector<MemDumpPtr>& p)
        : MemDumpBase(sz, p), srcOffset(src), dstOffset(dst) {}

    // Re-parenting constructor: creates a copy with a new parent and
    // (optionally) adjusted srcOffset. Used by setParents / addOffsets.
    MemDumpPtr(const MemDumpBase* newParent,
               const std::vector<std::shared_ptr<MemDumpPtr>>& childPtrs,
               size_t src, size_t dst, size_t sz)
        : MemDumpBase(sz, childPtrs), parent(newParent),
          srcOffset(src), dstOffset(dst) {}

    char* getAddr() const override {
        if (!parent) return nullptr;
        char* parentAddr = parent->getAddr();
        if (!parentAddr) return nullptr;
        char* dstAddr = *(char**)(parentAddr + srcOffset);
        if (!dstAddr) return nullptr;
        return dstAddr + dstOffset;
    }
};

class MemDump : public MemDumpBase {
public:
    char* const addr;

    MemDump(void* a, size_t sz) : MemDumpBase(sz), addr((char*)a) {}
    MemDump(void* a, size_t sz, const std::vector<MemDumpPtr>& p)
        : MemDumpBase(sz, p), addr((char*)a) {}
    MemDump(uint32_t start, uint32_t end)
        : MemDumpBase(end - start), addr((char*)(uintptr_t)start) {}
    MemDump(const MemDump& a, const MemDump& b)
        : MemDumpBase(a.size + b.size,
                      concat(a.ptrs, addOffsets(b.ptrs, a.size))),
          addr(a.addr) {
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
