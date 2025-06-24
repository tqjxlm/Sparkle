#pragma once

#include <crc32.h>

namespace sparkle
{
template <class T> void HashCombine(uint32_t &seed, const T &v)
{
    CRC32 hasher;
    hasher.add(&v, sizeof(T));

    uint32_t this_hash;
    hasher.getHash(reinterpret_cast<unsigned char *>(&this_hash));

    seed ^= this_hash + 0x9e3779b9 + (seed << 6) + (seed >> 2);
}

template <class T> void HashCombine(CRC32 &hasher, const T &v)
{
    hasher.add(&v, sizeof(T));
}

template <class T> void HashCombine(size_t &seed, const T &v)
{
    std::hash<T> hasher;
    seed ^= hasher(v) + 0x9e3779b9 + (seed << 6) + (seed >> 2);
}
} // namespace sparkle

namespace std
{
template <class T1, class T2> struct PairHash
{
    size_t operator()(const pair<T1, T2> &x) const
    {
        hash<T1> hasher1;
        hash<T2> hasher2;

        size_t seed = 0;

        sparkle::HashCombine(seed, hasher1(x.first));
        sparkle::HashCombine(seed, hasher2(x.second));

        return seed;
    }
};
} // namespace std
