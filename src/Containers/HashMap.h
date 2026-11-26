#ifndef HASHMAP_H
#define HASHMAP_H

#include <cstddef>
#include <cstdint>
#include <functional>
#include <initializer_list>
#include <iterator>
#include <new>
#include <stdexcept>
#include <type_traits>
#include <utility>

template <typename Key, typename Value, typename Hash = std::hash<Key>, typename KeyEqual = std::equal_to<Key>>
class HashMap
{
public:
    struct Entry
    {
        Key key;
        Value value;
    };

private:
    enum class SlotState : uint8_t
    {
        Empty = 0,
        Occupied = 1,
        Tombstone = 2
    };

    struct Slot
    {
        size_t hash = 0;
        alignas(Entry) unsigned char storage[sizeof(Entry)];
    };

    SlotState *states = nullptr;
    Slot *slots = nullptr;
    size_t count = 0;
    size_t tombstones = 0;
    size_t cap = 0;
    [[no_unique_address]] Hash hasher{};
    [[no_unique_address]] KeyEqual keyEqual{};

    static Entry *entryPtr(Slot &slot) noexcept { return std::launder(reinterpret_cast<Entry *>(slot.storage)); }

    static const Entry *entryPtr(const Slot &slot) noexcept
    {
        return std::launder(reinterpret_cast<const Entry *>(slot.storage));
    }

    static SlotState *allocateStates(size_t n)
    {
        if (n == 0)
        {
            return nullptr;
        }
        SlotState *ptr = nullptr;
        if constexpr (alignof(SlotState) > __STDCPP_DEFAULT_NEW_ALIGNMENT__)
        {
            ptr = static_cast<SlotState *>(::operator new(n * sizeof(SlotState), std::align_val_t{alignof(SlotState)}));
        }
        else
        {
            ptr = static_cast<SlotState *>(::operator new(n * sizeof(SlotState)));
        }
        for (size_t i = 0; i < n; ++i)
        {
            ptr[i] = SlotState::Empty;
        }
        return ptr;
    }

    static void deallocateStates(SlotState *ptr) noexcept
    {
        if (ptr != nullptr)
        {
            if constexpr (alignof(SlotState) > __STDCPP_DEFAULT_NEW_ALIGNMENT__)
            {
                ::operator delete(ptr, std::align_val_t{alignof(SlotState)});
                return;
            }
            ::operator delete(ptr);
        }
    }

    static Slot *allocateSlots(size_t n)
    {
        if (n == 0)
        {
            return nullptr;
        }
        if constexpr (alignof(Slot) > __STDCPP_DEFAULT_NEW_ALIGNMENT__)
        {
            return static_cast<Slot *>(::operator new(n * sizeof(Slot), std::align_val_t{alignof(Slot)}));
        }
        return static_cast<Slot *>(::operator new(n * sizeof(Slot)));
    }

    static void deallocateSlots(Slot *ptr) noexcept
    {
        if (ptr != nullptr)
        {
            if constexpr (alignof(Slot) > __STDCPP_DEFAULT_NEW_ALIGNMENT__)
            {
                ::operator delete(ptr, std::align_val_t{alignof(Slot)});
                return;
            }
            ::operator delete(ptr);
        }
    }

    static size_t mixHash(size_t h) noexcept
    {
        if constexpr (sizeof(size_t) >= 8)
        {
            uint64_t x = static_cast<uint64_t>(h);
            x ^= x >> 33;
            x *= 0xff51afd7ed558ccdULL;
            x ^= x >> 33;
            x *= 0xc4ceb9fe1a85ec53ULL;
            x ^= x >> 33;
            return static_cast<size_t>(x);
        }
        else
        {
            h ^= h >> 16;
            h *= 0x85ebca6bu;
            h ^= h >> 13;
            h *= 0xc2b2ae35u;
            h ^= h >> 16;
            return h;
        }
    }

    size_t computeHash(const Key &key) const { return mixHash(hasher(key)); }

    static size_t nextPowerOfTwo(size_t n) noexcept
    {
        size_t p = 16;
        while (p < n)
        {
            p <<= 1;
        }
        return p;
    }

    void destroyOccupied() noexcept
    {
        for (size_t i = 0; i < cap; ++i)
        {
            if (states[i] == SlotState::Occupied)
            {
                entryPtr(slots[i])->~Entry();
                states[i] = SlotState::Empty;
            }
        }
    }

    void releaseStorage() noexcept
    {
        destroyOccupied();
        deallocateStates(states);
        deallocateSlots(slots);
        states = nullptr;
        slots = nullptr;
        count = 0;
        tombstones = 0;
        cap = 0;
    }

    size_t findSlot(const Key &key, size_t mixedHash) const
    {
        if (cap == 0)
        {
            return 0;
        }

        const size_t mask = cap - 1;
        size_t idx = mixedHash & mask;
        for (size_t probed = 0; probed < cap; ++probed)
        {
            const SlotState st = states[idx];
            if (st == SlotState::Empty)
            {
                return cap;
            }
            if (st == SlotState::Occupied && slots[idx].hash == mixedHash && keyEqual(entryPtr(slots[idx])->key, key))
            {
                return idx;
            }
            idx = (idx + 1) & mask;
        }
        return cap;
    }

    size_t findSlotForInsert(const Key &key, size_t mixedHash, bool &foundExisting) const
    {
        const size_t mask = cap - 1;
        size_t idx = mixedHash & mask;
        size_t firstTombstone = cap;

        for (size_t probed = 0; probed < cap; ++probed)
        {
            const SlotState st = states[idx];
            if (st == SlotState::Empty)
            {
                foundExisting = false;
                return (firstTombstone != cap) ? firstTombstone : idx;
            }
            if (st == SlotState::Tombstone)
            {
                if (firstTombstone == cap)
                {
                    firstTombstone = idx;
                }
            }
            else if (slots[idx].hash == mixedHash && keyEqual(entryPtr(slots[idx])->key, key))
            {
                foundExisting = true;
                return idx;
            }
            idx = (idx + 1) & mask;
        }

        foundExisting = false;
        return firstTombstone;
    }

    void rehash(size_t newCap)
    {
        newCap = nextPowerOfTwo(newCap);
        SlotState *newStates = allocateStates(newCap);
        Slot *newSlots = nullptr;
        try
        {
            newSlots = allocateSlots(newCap);
            const size_t mask = newCap - 1;
            for (size_t i = 0; i < cap; ++i)
            {
                if (states[i] != SlotState::Occupied)
                {
                    continue;
                }
                Entry *oldEntry = entryPtr(slots[i]);
                const size_t mixed = slots[i].hash;
                size_t dest = mixed & mask;
                while (newStates[dest] == SlotState::Occupied)
                {
                    dest = (dest + 1) & mask;
                }
                ::new (static_cast<void *>(newSlots[dest].storage))
                    Entry{std::move(oldEntry->key), std::move(oldEntry->value)};
                newSlots[dest].hash = mixed;
                newStates[dest] = SlotState::Occupied;
                oldEntry->~Entry();
                states[i] = SlotState::Empty;
            }
        }
        catch (...)
        {
            if (newSlots != nullptr)
            {
                for (size_t j = 0; j < newCap; ++j)
                {
                    if (newStates[j] == SlotState::Occupied)
                    {
                        entryPtr(newSlots[j])->~Entry();
                    }
                }
                deallocateSlots(newSlots);
            }
            deallocateStates(newStates);
            throw;
        }

        deallocateStates(states);
        deallocateSlots(slots);
        states = newStates;
        slots = newSlots;
        cap = newCap;
        tombstones = 0;
    }

    void ensureCapacityForInsert()
    {
        if (cap == 0)
        {
            rehash(16);
            return;
        }
        if ((count + tombstones + 1) * 10 >= cap * 7)
        {
            const size_t nextCap = (count * 10 < cap * 4) ? cap : cap * 2;
            rehash(nextCap);
        }
    }

    template <typename KeyArg, typename... Args> std::pair<size_t, bool> tryEmplaceIndex(KeyArg &&key, Args &&...args)
    {
        ensureCapacityForInsert();
        const size_t mixed = computeHash(key);
        bool exists = false;
        const size_t idx = findSlotForInsert(key, mixed, exists);
        if (exists)
        {
            return {idx, false};
        }

        const SlotState prevState = states[idx];
        ::new (static_cast<void *>(slots[idx].storage))
            Entry{Key(std::forward<KeyArg>(key)), Value(std::forward<Args>(args)...)};
        slots[idx].hash = mixed;
        states[idx] = SlotState::Occupied;
        if (prevState == SlotState::Tombstone)
        {
            --tombstones;
        }
        ++count;
        return {idx, true};
    }

public:
    class Iterator
    {
    private:
        friend class HashMap;
        friend class ConstIterator;

        SlotState *statesPtr = nullptr;
        Slot *slotsPtr = nullptr;
        size_t index = 0;
        size_t cap = 0;

        void advanceToOccupied() noexcept
        {
            while (index < cap && statesPtr[index] != SlotState::Occupied)
            {
                ++index;
            }
        }

    public:
        using iterator_category = std::forward_iterator_tag;
        using value_type = Entry;
        using difference_type = std::ptrdiff_t;
        using pointer = Entry *;
        using reference = Entry &;

        Iterator() noexcept = default;

        Iterator(SlotState *s, Slot *sl, size_t start, size_t capacity) noexcept
            : statesPtr(s), slotsPtr(sl), index(start), cap(capacity)
        {
            advanceToOccupied();
        }

        reference operator*() const noexcept { return *entryPtr(slotsPtr[index]); }

        pointer operator->() const noexcept { return entryPtr(slotsPtr[index]); }

        Iterator &operator++() noexcept
        {
            ++index;
            advanceToOccupied();
            return *this;
        }

        Iterator operator++(int) noexcept
        {
            Iterator tmp = *this;
            ++(*this);
            return tmp;
        }

        bool operator==(const Iterator &other) const noexcept
        {
            return index == other.index && slotsPtr == other.slotsPtr;
        }

        bool operator!=(const Iterator &other) const noexcept { return !(*this == other); }
    };

    class ConstIterator
    {
    private:
        friend class HashMap;

        const SlotState *statesPtr = nullptr;
        const Slot *slotsPtr = nullptr;
        size_t index = 0;
        size_t cap = 0;

        void advanceToOccupied() noexcept
        {
            while (index < cap && statesPtr[index] != SlotState::Occupied)
            {
                ++index;
            }
        }

    public:
        using iterator_category = std::forward_iterator_tag;
        using value_type = const Entry;
        using difference_type = std::ptrdiff_t;
        using pointer = const Entry *;
        using reference = const Entry &;

        ConstIterator() noexcept = default;

        ConstIterator(const SlotState *s, const Slot *sl, size_t start, size_t capacity) noexcept
            : statesPtr(s), slotsPtr(sl), index(start), cap(capacity)
        {
            advanceToOccupied();
        }

        ConstIterator(const Iterator &it) noexcept
            : statesPtr(it.statesPtr), slotsPtr(it.slotsPtr), index(it.index), cap(it.cap)
        {
        }

        reference operator*() const noexcept { return *entryPtr(slotsPtr[index]); }

        pointer operator->() const noexcept { return entryPtr(slotsPtr[index]); }

        ConstIterator &operator++() noexcept
        {
            ++index;
            advanceToOccupied();
            return *this;
        }

        ConstIterator operator++(int) noexcept
        {
            ConstIterator tmp = *this;
            ++(*this);
            return tmp;
        }

        bool operator==(const ConstIterator &other) const noexcept
        {
            return index == other.index && slotsPtr == other.slotsPtr;
        }

        bool operator!=(const ConstIterator &other) const noexcept { return !(*this == other); }
    };

    HashMap() noexcept = default;

    explicit HashMap(size_t initialCapacity)
    {
        if (initialCapacity > 0)
        {
            rehash(initialCapacity);
        }
    }

    HashMap(std::initializer_list<std::pair<Key, Value>> init)
    {
        if (init.size() > 0)
        {
            reserve(init.size());
            for (const auto &item : init)
            {
                tryEmplaceIndex(item.first, item.second);
            }
        }
    }

    HashMap(const HashMap &other) : hasher(other.hasher), keyEqual(other.keyEqual)
    {
        if (other.count > 0)
        {
            reserve(other.count);
            for (size_t i = 0; i < other.cap; ++i)
            {
                if (other.states[i] == SlotState::Occupied)
                {
                    const Entry *src = entryPtr(other.slots[i]);
                    tryEmplaceIndex(src->key, src->value);
                }
            }
        }
    }

    HashMap(HashMap &&other) noexcept
        : states(other.states), slots(other.slots), count(other.count), tombstones(other.tombstones), cap(other.cap),
          hasher(std::move(other.hasher)), keyEqual(std::move(other.keyEqual))
    {
        other.states = nullptr;
        other.slots = nullptr;
        other.count = 0;
        other.tombstones = 0;
        other.cap = 0;
    }

    HashMap &operator=(const HashMap &other)
    {
        if (this == &other)
        {
            return *this;
        }
        HashMap tmp(other);
        swap(tmp);
        return *this;
    }

    HashMap &operator=(HashMap &&other) noexcept
    {
        if (this == &other)
        {
            return *this;
        }
        releaseStorage();
        states = other.states;
        slots = other.slots;
        count = other.count;
        tombstones = other.tombstones;
        cap = other.cap;
        hasher = std::move(other.hasher);
        keyEqual = std::move(other.keyEqual);
        other.states = nullptr;
        other.slots = nullptr;
        other.count = 0;
        other.tombstones = 0;
        other.cap = 0;
        return *this;
    }

    ~HashMap() { releaseStorage(); }

    Iterator begin() noexcept { return Iterator(states, slots, 0, cap); }

    ConstIterator begin() const noexcept { return ConstIterator(states, slots, 0, cap); }

    ConstIterator cbegin() const noexcept { return ConstIterator(states, slots, 0, cap); }

    Iterator end() noexcept { return Iterator(states, slots, cap, cap); }

    ConstIterator end() const noexcept { return ConstIterator(states, slots, cap, cap); }

    ConstIterator cend() const noexcept { return ConstIterator(states, slots, cap, cap); }

    Iterator find(const Key &key)
    {
        if (cap == 0)
        {
            return end();
        }
        const size_t idx = findSlot(key, computeHash(key));
        return Iterator(states, slots, idx, cap);
    }

    ConstIterator find(const Key &key) const
    {
        if (cap == 0)
        {
            return end();
        }
        const size_t idx = findSlot(key, computeHash(key));
        return ConstIterator(states, slots, idx, cap);
    }

    bool contains(const Key &key) const { return find(key) != end(); }

    Value &at(const Key &key)
    {
        auto it = find(key);
        if (it == end())
        {
            throw std::out_of_range("HashMap key not found");
        }
        return it->value;
    }

    const Value &at(const Key &key) const
    {
        auto it = find(key);
        if (it == end())
        {
            throw std::out_of_range("HashMap key not found");
        }
        return it->value;
    }

    Value &operator[](const Key &key)
    {
        const auto [idx, inserted] = tryEmplaceIndex(key);
        (void)inserted;
        return entryPtr(slots[idx])->value;
    }

    Value &operator[](Key &&key)
    {
        const auto [idx, inserted] = tryEmplaceIndex(std::move(key));
        (void)inserted;
        return entryPtr(slots[idx])->value;
    }

    template <typename... Args> std::pair<Iterator, bool> tryEmplace(const Key &key, Args &&...args)
    {
        const auto [idx, inserted] = tryEmplaceIndex(key, std::forward<Args>(args)...);
        return {Iterator(states, slots, idx, cap), inserted};
    }

    template <typename... Args> std::pair<Iterator, bool> tryEmplace(Key &&key, Args &&...args)
    {
        const auto [idx, inserted] = tryEmplaceIndex(std::move(key), std::forward<Args>(args)...);
        return {Iterator(states, slots, idx, cap), inserted};
    }

    template <typename ValueArg> std::pair<Iterator, bool> insertOrAssign(const Key &key, ValueArg &&value)
    {
        const auto [idx, inserted] = tryEmplaceIndex(key, std::forward<ValueArg>(value));
        if (!inserted)
        {
            entryPtr(slots[idx])->value = std::forward<ValueArg>(value);
        }
        return {Iterator(states, slots, idx, cap), inserted};
    }

    template <typename ValueArg> std::pair<Iterator, bool> insertOrAssign(Key &&key, ValueArg &&value)
    {
        const auto [idx, inserted] = tryEmplaceIndex(std::move(key), std::forward<ValueArg>(value));
        if (!inserted)
        {
            entryPtr(slots[idx])->value = std::forward<ValueArg>(value);
        }
        return {Iterator(states, slots, idx, cap), inserted};
    }

    bool erase(const Key &key)
    {
        if (cap == 0)
        {
            return false;
        }
        const size_t idx = findSlot(key, computeHash(key));
        if (idx == cap)
        {
            return false;
        }
        entryPtr(slots[idx])->~Entry();
        states[idx] = SlotState::Tombstone;
        --count;
        ++tombstones;
        return true;
    }

    void reserve(size_t expectedCount)
    {
        if (expectedCount == 0)
        {
            return;
        }
        const size_t minSlots = (expectedCount * 10) / 7 + 1;
        const size_t targetCap = nextPowerOfTwo(minSlots);
        if (targetCap > cap)
        {
            rehash(targetCap);
        }
    }

    void clear() noexcept
    {
        if (cap == 0)
        {
            return;
        }
        for (size_t i = 0; i < cap; ++i)
        {
            if (states[i] == SlotState::Occupied)
            {
                entryPtr(slots[i])->~Entry();
            }
            states[i] = SlotState::Empty;
        }
        count = 0;
        tombstones = 0;
    }

    size_t size() const noexcept { return count; }

    bool empty() const noexcept { return count == 0; }

    size_t capacity() const noexcept { return cap; }

    double loadFactor() const noexcept
    {
        return cap == 0 ? 0.0 : static_cast<double>(count) / static_cast<double>(cap);
    }

    void swap(HashMap &other) noexcept
    {
        std::swap(states, other.states);
        std::swap(slots, other.slots);
        std::swap(count, other.count);
        std::swap(tombstones, other.tombstones);
        std::swap(cap, other.cap);
        std::swap(hasher, other.hasher);
        std::swap(keyEqual, other.keyEqual);
    }
};

#endif // HASHMAP_H
