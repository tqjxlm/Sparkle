#pragma once

#include <vector>

namespace sparkle
{
// remove an element from vector in constant time by swapping it with the last element
// it will return true if a swap actually happens (which is the common case)
// you may want to maintain your index if that happens
template <class T> inline bool RemoveAtSwap(std::vector<T> &v, unsigned index)
{
    bool swapped = false;
    if (index < v.size() - 1)
    {
        std::swap(v[index], v.back());
        swapped = true;
    }
    v.pop_back();

    return swapped;
}
} // namespace sparkle
