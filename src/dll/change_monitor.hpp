// src/dll/change_monitor.hpp
// Ported from CCCaster ChangeMonitor.hpp. Header-only singleton.

#pragma once

#include <memory>
#include <vector>

namespace caster::dll {

class ChangeMonitor {
public:
    struct Interface {
        virtual bool check() = 0;
        virtual ~Interface() = default;
    };

    template<typename O, typename K, typename T>
    Interface* addRef(O* owner, K key, const T& ref);

    bool remove(Interface* monitor) {
        for (auto it = _monitors.begin(); it != _monitors.end(); ++it) {
            if (it->get() == monitor) { _monitors.erase(it); return true; }
        }
        return false;
    }

    void check() {
        for (size_t i = 0; i < _monitors.size(); ++i)
            _monitors[i]->check();
    }

    void clear() { _monitors.clear(); }
    static ChangeMonitor& get() { static ChangeMonitor inst; return inst; }

private:
    ChangeMonitor() = default;
    std::vector<std::shared_ptr<Interface>> _monitors;
};

template<typename K, typename T>
class RefChangeMonitor : public ChangeMonitor::Interface {
public:
    struct Owner {
        virtual void changedValue(K key, T prev, T curr) = 0;
    };
    Owner* owner;
    RefChangeMonitor(Owner* o, K key, const T& ref)
        : owner(o), _key(key), _current(ref), _previous(ref) {}
    bool check() override {
        if (_current == _previous) return false;
        if (owner) owner->changedValue(_key, _previous, _current);
        _previous = _current;
        return true;
    }
private:
    K _key;
    const T& _current;
    T _previous;
};

template<typename O, typename K, typename T>
ChangeMonitor::Interface* ChangeMonitor::addRef(O* owner, K key, const T& ref) {
    using Owner = typename RefChangeMonitor<K, T>::Owner;
    auto monitor = std::make_shared<RefChangeMonitor<K, T>>(static_cast<Owner*>(owner), key, ref);
    _monitors.push_back(monitor);
    return monitor.get();
}

} // namespace caster::dll
