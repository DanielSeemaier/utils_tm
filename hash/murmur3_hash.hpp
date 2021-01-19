#pragma once


#include <string_view>
#include <string>

// We include the cpp to avoid generating another compile unit
#include "MurmurHash3.cpp"


namespace utils_tm {
namespace hash_tm  {


struct murmur3_hash
{
    static constexpr std::string_view name = "murmur3";
    static constexpr size_t significant_digits = 64;


    murmur3_hash(size_t s = 1203989050u) : seed(s) { }

    uint seed;

    inline uint64_t operator()(const uint64_t k) const
    {
        uint64_t local = k;
        uint64_t target[2];

        MurmurHash3_x64_128 (&local, 8, seed, target);

        return target[0];
    }

    inline uint64_t operator()(const std::string& k) const
    {
        uint64_t target[2];
        return MurmurHash3_x64_128 (k.data(), k.size(), seed, target);
    }
};

}
}
