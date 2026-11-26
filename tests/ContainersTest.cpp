#include "Containers/DynamicArray.h"
#include "Containers/HashMap.h"
#include <gtest/gtest.h>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>

namespace
{

struct ThrowOnCopy
{
    int value = 0;
    inline static int aliveCount = 0;
    inline static int failAfterCopies = -1;

    explicit ThrowOnCopy(int v = 0) : value(v) { ++aliveCount; }

    ThrowOnCopy(const ThrowOnCopy &other) : value(other.value)
    {
        if (failAfterCopies == 0)
        {
            throw std::runtime_error("Simulated copy failure");
        }
        if (failAfterCopies > 0)
        {
            --failAfterCopies;
        }
        ++aliveCount;
    }

    ThrowOnCopy &operator=(const ThrowOnCopy &other)
    {
        value = other.value;
        return *this;
    }

    ~ThrowOnCopy() { --aliveCount; }
};

struct CollidingHash
{
    size_t operator()(int key) const noexcept
    {
        (void)key;
        return 42;
    }
};

struct LowBitsEqualHash
{
    size_t operator()(int key) const noexcept { return static_cast<size_t>(key) << 6; }
};

} // namespace

TEST(ContainersTest, DynamicArrayConstructionBoundsAndEquality)
{
    const DynamicArray<int> emptyArr;
    EXPECT_TRUE(emptyArr.empty());
    EXPECT_EQ(emptyArr.size(), 0u);
    EXPECT_EQ(emptyArr.capacity(), 0u);
    EXPECT_THROW(emptyArr.at(0), std::out_of_range);

    DynamicArray<int> zeroFilled(4);
    EXPECT_EQ(zeroFilled.size(), 4u);
    for (size_t i = 0; i < zeroFilled.size(); ++i)
    {
        EXPECT_EQ(zeroFilled[i], 0);
    }

    DynamicArray<std::string> repeated(3, "hi");
    EXPECT_EQ(repeated.size(), 3u);
    EXPECT_EQ(repeated.front(), "hi");
    EXPECT_EQ(repeated.back(), "hi");

    DynamicArray<int> initList = {1, 2, 3, 4};
    DynamicArray<int> same = {1, 2, 3, 4};
    DynamicArray<int> diff = {1, 2, 3, 5};
    EXPECT_EQ(initList, same);
    EXPECT_NE(initList, diff);
    EXPECT_EQ(initList.at(2), 3);
    EXPECT_THROW(initList.at(4), std::out_of_range);
}

TEST(ContainersTest, DynamicArrayGrowthShrinkResizeAndSelfAliasing)
{
    DynamicArray<std::string> arr;
    arr.pushBack("first_long_string_to_bypass_sso");
    for (int i = 0; i < 8; ++i)
    {
        arr.pushBack(arr[0]);
    }
    EXPECT_EQ(arr.size(), 9u);
    for (size_t i = 0; i < arr.size(); ++i)
    {
        EXPECT_EQ(arr[i], "first_long_string_to_bypass_sso");
    }

    auto &ref = arr.emplaceBack("last");
    EXPECT_EQ(ref, "last");
    EXPECT_EQ(arr.back(), "last");

    arr.popBack();
    EXPECT_EQ(arr.size(), 9u);

    arr.reserve(32);
    EXPECT_GE(arr.capacity(), 32u);
    arr.shrinkToFit();
    EXPECT_EQ(arr.capacity(), 9u);

    arr.resize(2);
    EXPECT_EQ(arr.size(), 2u);
    arr.resize(5, "pad");
    EXPECT_EQ(arr.size(), 5u);
    EXPECT_EQ(arr[4], "pad");

    arr.assign(3, "reset");
    EXPECT_EQ(arr.size(), 3u);
    EXPECT_EQ(arr[0], "reset");
    EXPECT_EQ(arr[2], "reset");

    arr.clear();
    EXPECT_TRUE(arr.empty());
    arr.shrinkToFit();
    EXPECT_EQ(arr.capacity(), 0u);
    EXPECT_EQ(arr.data(), nullptr);
}

TEST(ContainersTest, DynamicArraySupportsMoveOnlyAndRuleOfFive)
{
    DynamicArray<std::unique_ptr<int>> ptrs;
    for (int i = 0; i < 20; ++i)
    {
        ptrs.pushBack(std::make_unique<int>(i * 10));
    }
    EXPECT_EQ(ptrs.size(), 20u);

    DynamicArray<std::unique_ptr<int>> moved(std::move(ptrs));
    EXPECT_TRUE(ptrs.empty());
    EXPECT_EQ(moved.size(), 20u);
    EXPECT_EQ(*moved[19], 190);

    ptrs = std::move(moved);
    EXPECT_EQ(ptrs.size(), 20u);
    EXPECT_EQ(*ptrs[0], 0);

    DynamicArray<std::string> a = {"alpha", "beta"};
    DynamicArray<std::string> b(a);
    b.pushBack("gamma");
    EXPECT_EQ(a.size(), 2u);
    EXPECT_EQ(b.size(), 3u);

    a = b;
    EXPECT_EQ(a, b);
    const auto &alias = a;
    a = alias;
    EXPECT_EQ(a, b);
}

TEST(ContainersTest, DynamicArrayStrongExceptionSafety)
{
    ThrowOnCopy::aliveCount = 0;
    ThrowOnCopy::failAfterCopies = -1;
    {
        DynamicArray<ThrowOnCopy> arr;
        arr.reserve(2);
        arr.emplaceBack(10);
        arr.emplaceBack(20);
        EXPECT_EQ(ThrowOnCopy::aliveCount, 2);

        ThrowOnCopy::failAfterCopies = 1;
        EXPECT_THROW(arr.emplaceBack(30), std::runtime_error);
        EXPECT_EQ(arr.size(), 2u);
        EXPECT_EQ(arr[0].value, 10);
        EXPECT_EQ(arr[1].value, 20);
        EXPECT_EQ(ThrowOnCopy::aliveCount, 2);
    }
    ThrowOnCopy::failAfterCopies = -1;
    EXPECT_EQ(ThrowOnCopy::aliveCount, 0);
}

TEST(ContainersTest, HashMapInsertLookupUpdateAndIteration)
{
    HashMap<std::string, int> map = {{"one", 1}, {"two", 2}};
    EXPECT_EQ(map.size(), 2u);
    EXPECT_FALSE(map.empty());
    EXPECT_TRUE(map.contains("one"));
    EXPECT_FALSE(map.contains("missing"));
    EXPECT_EQ(map.at("two"), 2);
    EXPECT_THROW(map.at("missing"), std::out_of_range);

    auto [itExisting, insertedExisting] = map.tryEmplace("one", 99);
    EXPECT_FALSE(insertedExisting);
    EXPECT_EQ(itExisting->value, 1);

    auto [itNew, insertedNew] = map.tryEmplace("three", 3);
    EXPECT_TRUE(insertedNew);
    EXPECT_EQ(itNew->value, 3);

    auto [itUpdated, wasInserted] = map.insertOrAssign("two", 22);
    EXPECT_FALSE(wasInserted);
    EXPECT_EQ(itUpdated->value, 22);
    EXPECT_EQ(map["four"], 0);
    map["four"] = 4;

    const HashMap<std::string, int> &constView = map;
    HashMap<std::string, int>::ConstIterator cit = map.find("three");
    ASSERT_NE(cit, constView.cend());
    EXPECT_EQ(cit->value, 3);

    int sum = 0;
    for (const auto &entry : constView)
    {
        sum += entry.value;
    }
    EXPECT_EQ(sum, 1 + 22 + 3 + 4);
}

TEST(ContainersTest, HashMapErasePreservesChainsAndPurgesTombstones)
{
    HashMap<int, int, CollidingHash> map;
    for (int i = 0; i < 6; ++i)
    {
        map.insertOrAssign(i, i * 100);
    }

    EXPECT_TRUE(map.erase(1));
    EXPECT_TRUE(map.erase(3));
    EXPECT_FALSE(map.erase(999));

    EXPECT_EQ(map.size(), 4u);
    EXPECT_EQ(map.at(0), 0);
    EXPECT_EQ(map.at(2), 200);
    EXPECT_EQ(map.at(4), 400);
    EXPECT_EQ(map.at(5), 500);

    map.insertOrAssign(1, 111);
    EXPECT_EQ(map.at(1), 111);

    HashMap<int, int> churn;
    for (int i = 0; i < 500; ++i)
    {
        churn[i] = i;
        churn.erase(i);
    }
    EXPECT_TRUE(churn.empty());
    EXPECT_LE(churn.capacity(), 32u);
}

TEST(ContainersTest, HashMapCopyMoveClearReserveAndMoveOnly)
{
    HashMap<int, std::unique_ptr<int>> uniqueMap;
    uniqueMap.reserve(32);
    const size_t reservedCap = uniqueMap.capacity();
    EXPECT_GE(reservedCap, 32u);

    for (int i = 0; i < 12; ++i)
    {
        uniqueMap.tryEmplace(i, std::make_unique<int>(i + 1));
    }

    HashMap<int, std::unique_ptr<int>> movedMap(std::move(uniqueMap));
    EXPECT_TRUE(uniqueMap.empty());
    EXPECT_EQ(movedMap.size(), 12u);
    EXPECT_EQ(*movedMap.at(11), 12);

    movedMap.clear();
    EXPECT_TRUE(movedMap.empty());
    EXPECT_EQ(movedMap.capacity(), reservedCap);
    movedMap.tryEmplace(7, std::make_unique<int>(77));
    EXPECT_EQ(*movedMap.at(7), 77);

    HashMap<std::string, int> a = {{"x", 10}, {"y", 20}};
    HashMap<std::string, int> b(a);
    b["z"] = 30;
    EXPECT_EQ(a.size(), 2u);
    a = b;
    EXPECT_EQ(a.size(), 3u);
    EXPECT_EQ(a.at("z"), 30);
    const auto &selfRef = a;
    a = selfRef;
    EXPECT_EQ(a.at("z"), 30);
}

TEST(ContainersTest, HashMapMixesLowBitsAndHandlesHighVolumeStress)
{
    HashMap<int, int, LowBitsEqualHash> map;
    constexpr int N = 10000;
    for (int i = 0; i < N; ++i)
    {
        map.insertOrAssign(i, i * 3);
    }
    EXPECT_EQ(map.size(), static_cast<size_t>(N));

    for (int i = 0; i < N; i += 2)
    {
        EXPECT_TRUE(map.erase(i));
    }
    EXPECT_EQ(map.size(), static_cast<size_t>(N / 2));

    for (int i = 0; i < N; ++i)
    {
        if (i % 2 == 0)
        {
            EXPECT_FALSE(map.contains(i));
        }
        else
        {
            EXPECT_EQ(map.at(i), i * 3);
        }
    }
}
