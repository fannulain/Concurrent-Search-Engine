#ifndef DYNAMICARRAY_H
#define DYNAMICARRAY_H

#include <cstddef>
#include <initializer_list>
#include <new>
#include <stdexcept>
#include <type_traits>
#include <utility>

template <typename T> class DynamicArray
{
private:
    T *dataPtr = nullptr;
    size_t count = 0;
    size_t cap = 0;

    static T *allocateRaw(size_t n)
    {
        if (n == 0)
        {
            return nullptr;
        }
        return static_cast<T *>(::operator new(n * sizeof(T), std::align_val_t{alignof(T)}));
    }

    static void deallocateRaw(T *ptr) noexcept
    {
        if (ptr != nullptr)
        {
            ::operator delete(ptr, std::align_val_t{alignof(T)});
        }
    }

    static void destroyRange(T *first, T *last) noexcept
    {
        while (last != first)
        {
            (--last)->~T();
        }
    }

    void transferElements(T *dest, size_t n)
    {
        size_t constructed = 0;
        try
        {
            for (size_t i = 0; i < n; ++i)
            {
                if constexpr (std::is_nothrow_move_constructible_v<T> || !std::is_copy_constructible_v<T>)
                {
                    ::new (static_cast<void *>(dest + i)) T(std::move(dataPtr[i]));
                }
                else
                {
                    ::new (static_cast<void *>(dest + i)) T(dataPtr[i]);
                }
                ++constructed;
            }
        }
        catch (...)
        {
            destroyRange(dest, dest + constructed);
            throw;
        }
    }

    void reallocate(size_t newCap)
    {
        if (newCap == 0)
        {
            destroyRange(dataPtr, dataPtr + count);
            deallocateRaw(dataPtr);
            dataPtr = nullptr;
            count = 0;
            cap = 0;
            return;
        }

        T *newData = allocateRaw(newCap);
        try
        {
            transferElements(newData, count);
        }
        catch (...)
        {
            deallocateRaw(newData);
            throw;
        }

        destroyRange(dataPtr, dataPtr + count);
        deallocateRaw(dataPtr);
        dataPtr = newData;
        cap = newCap;
    }

    template <typename... Args> T &appendImpl(Args &&...args)
    {
        if (count < cap)
        {
            ::new (static_cast<void *>(dataPtr + count)) T(std::forward<Args>(args)...);
            return dataPtr[count++];
        }

        const size_t newCap = (cap == 0) ? 1 : cap * 2;
        T *newData = allocateRaw(newCap);
        bool newConstructed = false;
        try
        {
            ::new (static_cast<void *>(newData + count)) T(std::forward<Args>(args)...);
            newConstructed = true;
            transferElements(newData, count);
        }
        catch (...)
        {
            if (newConstructed)
            {
                (newData + count)->~T();
            }
            deallocateRaw(newData);
            throw;
        }

        destroyRange(dataPtr, dataPtr + count);
        deallocateRaw(dataPtr);
        dataPtr = newData;
        cap = newCap;
        return dataPtr[count++];
    }

public:
    using value_type = T;
    using size_type = size_t;
    using iterator = T *;
    using const_iterator = const T *;

    DynamicArray() noexcept = default;

    explicit DynamicArray(size_t initialSize)
    {
        if (initialSize > 0)
        {
            T *newData = allocateRaw(initialSize);
            size_t constructed = 0;
            try
            {
                for (size_t i = 0; i < initialSize; ++i)
                {
                    ::new (static_cast<void *>(newData + i)) T();
                    ++constructed;
                }
            }
            catch (...)
            {
                destroyRange(newData, newData + constructed);
                deallocateRaw(newData);
                throw;
            }
            dataPtr = newData;
            count = initialSize;
            cap = initialSize;
        }
    }

    DynamicArray(size_t initialSize, const T &value)
    {
        if (initialSize > 0)
        {
            T *newData = allocateRaw(initialSize);
            size_t constructed = 0;
            try
            {
                for (size_t i = 0; i < initialSize; ++i)
                {
                    ::new (static_cast<void *>(newData + i)) T(value);
                    ++constructed;
                }
            }
            catch (...)
            {
                destroyRange(newData, newData + constructed);
                deallocateRaw(newData);
                throw;
            }
            dataPtr = newData;
            count = initialSize;
            cap = initialSize;
        }
    }

    DynamicArray(std::initializer_list<T> init)
    {
        if (init.size() > 0)
        {
            T *newData = allocateRaw(init.size());
            size_t constructed = 0;
            try
            {
                for (const T &item : init)
                {
                    ::new (static_cast<void *>(newData + constructed)) T(item);
                    ++constructed;
                }
            }
            catch (...)
            {
                destroyRange(newData, newData + constructed);
                deallocateRaw(newData);
                throw;
            }
            dataPtr = newData;
            count = init.size();
            cap = init.size();
        }
    }

    DynamicArray(const DynamicArray &other)
    {
        if (other.count > 0)
        {
            T *newData = allocateRaw(other.count);
            size_t constructed = 0;
            try
            {
                for (size_t i = 0; i < other.count; ++i)
                {
                    ::new (static_cast<void *>(newData + i)) T(other.dataPtr[i]);
                    ++constructed;
                }
            }
            catch (...)
            {
                destroyRange(newData, newData + constructed);
                deallocateRaw(newData);
                throw;
            }
            dataPtr = newData;
            count = other.count;
            cap = other.count;
        }
    }

    DynamicArray(DynamicArray &&other) noexcept : dataPtr(other.dataPtr), count(other.count), cap(other.cap)
    {
        other.dataPtr = nullptr;
        other.count = 0;
        other.cap = 0;
    }

    DynamicArray &operator=(const DynamicArray &other)
    {
        if (this == &other)
        {
            return *this;
        }
        DynamicArray tmp(other);
        swap(tmp);
        return *this;
    }

    DynamicArray &operator=(DynamicArray &&other) noexcept
    {
        if (this == &other)
        {
            return *this;
        }
        destroyRange(dataPtr, dataPtr + count);
        deallocateRaw(dataPtr);
        dataPtr = other.dataPtr;
        count = other.count;
        cap = other.cap;
        other.dataPtr = nullptr;
        other.count = 0;
        other.cap = 0;
        return *this;
    }

    ~DynamicArray()
    {
        destroyRange(dataPtr, dataPtr + count);
        deallocateRaw(dataPtr);
    }

    void pushBack(const T &value) { appendImpl(value); }

    void pushBack(T &&value) { appendImpl(std::move(value)); }

    template <typename... Args> T &emplaceBack(Args &&...args) { return appendImpl(std::forward<Args>(args)...); }

    void popBack() noexcept
    {
        if (count > 0)
        {
            --count;
            (dataPtr + count)->~T();
        }
    }

    void clear() noexcept
    {
        destroyRange(dataPtr, dataPtr + count);
        count = 0;
    }

    void reserve(size_t newCap)
    {
        if (newCap > cap)
        {
            reallocate(newCap);
        }
    }

    void shrinkToFit()
    {
        if (count < cap)
        {
            reallocate(count);
        }
    }

    void resize(size_t newSize)
    {
        if (newSize < count)
        {
            destroyRange(dataPtr + newSize, dataPtr + count);
            count = newSize;
            return;
        }
        if (newSize > count)
        {
            reserve(newSize);
            while (count < newSize)
            {
                ::new (static_cast<void *>(dataPtr + count)) T();
                ++count;
            }
        }
    }

    void resize(size_t newSize, const T &value)
    {
        if (newSize < count)
        {
            destroyRange(dataPtr + newSize, dataPtr + count);
            count = newSize;
            return;
        }
        if (newSize > count)
        {
            reserve(newSize);
            while (count < newSize)
            {
                ::new (static_cast<void *>(dataPtr + count)) T(value);
                ++count;
            }
        }
    }

    void assign(size_t newSize, const T &value)
    {
        clear();
        resize(newSize, value);
    }

    T &operator[](size_t index) noexcept { return dataPtr[index]; }

    const T &operator[](size_t index) const noexcept { return dataPtr[index]; }

    T &at(size_t index)
    {
        if (index >= count)
        {
            throw std::out_of_range("DynamicArray index out of range");
        }
        return dataPtr[index];
    }

    const T &at(size_t index) const
    {
        if (index >= count)
        {
            throw std::out_of_range("DynamicArray index out of range");
        }
        return dataPtr[index];
    }

    T &front() noexcept { return dataPtr[0]; }

    const T &front() const noexcept { return dataPtr[0]; }

    T &back() noexcept { return dataPtr[count - 1]; }

    const T &back() const noexcept { return dataPtr[count - 1]; }

    T *data() noexcept { return dataPtr; }

    const T *data() const noexcept { return dataPtr; }

    iterator begin() noexcept { return dataPtr; }

    const_iterator begin() const noexcept { return dataPtr; }

    const_iterator cbegin() const noexcept { return dataPtr; }

    iterator end() noexcept { return dataPtr + count; }

    const_iterator end() const noexcept { return dataPtr + count; }

    const_iterator cend() const noexcept { return dataPtr + count; }

    bool empty() const noexcept { return count == 0; }

    size_t size() const noexcept { return count; }

    size_t capacity() const noexcept { return cap; }

    void swap(DynamicArray &other) noexcept
    {
        std::swap(dataPtr, other.dataPtr);
        std::swap(count, other.count);
        std::swap(cap, other.cap);
    }

    bool operator==(const DynamicArray &other) const
    {
        if (count != other.count)
        {
            return false;
        }
        for (size_t i = 0; i < count; ++i)
        {
            if (!(dataPtr[i] == other.dataPtr[i]))
            {
                return false;
            }
        }
        return true;
    }

    bool operator!=(const DynamicArray &other) const { return !(*this == other); }
};

#endif // DYNAMICARRAY_H
