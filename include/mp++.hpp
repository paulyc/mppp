/* Copyright 2009-2016 Francesco Biscani (bluescarni@gmail.com)

This file is part of the mp++ library.

The mp++ library is free software; you can redistribute it and/or modify
it under the terms of either:

  * the GNU Lesser General Public License as published by the Free
    Software Foundation; either version 3 of the License, or (at your
    option) any later version.

or

  * the GNU General Public License as published by the Free Software
    Foundation; either version 3 of the License, or (at your option) any
    later version.

or both in parallel, as here.

The mp++ library is distributed in the hope that it will be useful, but
WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY
or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License
for more details.

You should have received copies of the GNU General Public License and the
GNU Lesser General Public License along with the mp++ library.  If not,
see https://www.gnu.org/licenses/. */

#ifndef MPPP_MPPP_HPP
#define MPPP_MPPP_HPP

#include <algorithm>
#include <array>
#include <cassert>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <functional>
#include <gmp.h>
#include <iostream>
#include <limits>
#include <new>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

#if defined(MPPP_WITH_LONG_DOUBLE)

#include <mpfr.h>

#endif

// Compiler configuration.
#if defined(__clang__) || defined(__GNUC__) || defined(__INTEL_COMPILER)

#define mppp_likely(x) __builtin_expect(!!(x), 1)
#define mppp_unlikely(x) __builtin_expect(!!(x), 0)
#define MPPP_RESTRICT __restrict

// NOTE: we can check int128 on GCC/clang with __SIZEOF_INT128__ apparently:
// http://stackoverflow.com/questions/21886985/what-gcc-versions-support-the-int128-intrinsic-type
#if defined(__SIZEOF_INT128__)
#define MPPP_UINT128 __uint128_t
#endif

#elif defined(_MSC_VER)

// Disable clang-format here, as the include order seems to matter.
// clang-format off
#include <windows.h>
#include <Winnt.h>
// clang-format on

#define mppp_likely(x) (x)
#define mppp_unlikely(x) (x)
#define MPPP_RESTRICT __restrict

// Disable some warnings for MSVC.
#pragma warning(push)
#pragma warning(disable : 4127)
#pragma warning(disable : 4804)

#else

#define mppp_likely(x) (x)
#define mppp_unlikely(x) (x)
#define MPPP_RESTRICT

#endif

// Configuration of the thread_local keyword.
#if defined(__apple_build_version__) || defined(__MINGW32__) || defined(__INTEL_COMPILER)

// - Apple clang does not support the thread_local keyword until very recent versions.
// - Testing shows that at least some MinGW versions have buggy thread_local implementations.
// - Also on Intel the thread_local keyword looks buggy.
#define MPPP_MAYBE_TLS

#else

// For the rest, we assume thread_local is available.
#define MPPP_HAVE_THREAD_LOCAL
#define MPPP_MAYBE_TLS static thread_local

#endif

namespace mppp
{

inline namespace detail
{

// Just a small helper, like C++14.
template <bool B, typename T = void>
using enable_if_t = typename std::enable_if<B, T>::type;

// TODO mpz struct checks -> TODO check about _mp_size as well.

// mpz_t is an array of some struct.
using mpz_struct_t = std::remove_extent<::mpz_t>::type;
// Integral types used for allocation size and number of limbs.
using mpz_alloc_t = decltype(std::declval<mpz_struct_t>()._mp_alloc);
using mpz_size_t = decltype(std::declval<mpz_struct_t>()._mp_size);

// Helper function to init an mpz to zero with nlimbs preallocated limbs.
inline void mpz_init_nlimbs(mpz_struct_t &rop, std::size_t nlimbs)
{
    // A bit of horrid overflow checking.
    using ump_bitcnt_t = std::make_unsigned<::mp_bitcnt_t>::type;
    if (mppp_unlikely(nlimbs > std::numeric_limits<std::size_t>::max() / unsigned(GMP_NUMB_BITS))) {
        // NOTE: here we are doing what GMP does in case of memory allocation errors. It does not make much sense
        // to do anything else, as long as GMP does not provide error recovery.
        std::abort();
    }
    const auto nbits = static_cast<std::size_t>(unsigned(GMP_NUMB_BITS) * nlimbs);
    if (mppp_unlikely(nbits > ump_bitcnt_t(std::numeric_limits<::mp_bitcnt_t>::max()))) {
        std::abort();
    }
    // NOTE: nbits == 0 is allowed.
    ::mpz_init2(&rop, static_cast<::mp_bitcnt_t>(nbits));
    assert(std::make_unsigned<mpz_size_t>::type(rop._mp_alloc) >= nlimbs);
}

// This is a cache that is used by functions below in order to keep a pool of allocated mpzs, with the goal
// of avoiding continuous (de)allocation when frequently creating/destroying mpzs.
struct mpz_cache {
    // Max mpz alloc size.
    static const unsigned max_size = 10u;
    // Max number of cache entries per size.
    static const unsigned max_entries = 100u;
    // Type definitions.
    using v_type = std::vector<std::vector<mpz_struct_t>>;
    using v_size_t = v_type::size_type;
    mpz_cache() : m_vec(v_size_t(max_size))
    {
    }
    void push(mpz_struct_t &m)
    {
        // NOTE: here we are assuming always nonegative mp_alloc.
        assert(m._mp_alloc >= 0);
        const auto size = static_cast<std::make_unsigned<mpz_size_t>::type>(m._mp_alloc);
        if (!size || size > max_size || m_vec[static_cast<v_size_t>(size - 1u)].size() >= max_entries) {
            // If the allocated size is zero, too large, or we have reached the max number of entries,
            // don't cache it. Just clear it.
            ::mpz_clear(&m);
        } else {
            // Add it to the cache.
            m_vec[static_cast<v_size_t>(size - 1u)].emplace_back(m);
        }
    }
    void pop(mpz_struct_t &retval, std::size_t size)
    {
        if (!size || size > max_size || !m_vec[static_cast<v_size_t>(size - 1u)].size()) {
            // If the size is zero, too large, or we don't have entries for the specified
            // size, init a new mpz.
            mpz_init_nlimbs(retval, size);
        } else {
            // If we have cache entries, use them: pop the last one
            // for the current size.
            retval = m_vec[static_cast<v_size_t>(size - 1u)].back();
            m_vec[static_cast<v_size_t>(size - 1u)].pop_back();
        }
    }
    ~mpz_cache()
    {
        // On destruction, deallocate all the cached mpzs.
        for (auto &v : m_vec) {
            for (auto &m : v) {
                ::mpz_clear(&m);
            }
        }
    }

    v_type m_vec;
};

// NOTE: the cache will be used only if we have the thread_local keyword available.
// Otherwise, we provide alternative implementations of the mpz_init_cache() and mpz_clear_cache()
// that do not use any cache internally.
#if defined(MPPP_HAVE_THREAD_LOCAL)

inline mpz_cache &get_mpz_cache()
{
    static thread_local mpz_cache cache;
    return cache;
}

// NOTE: contrary to the GMP functions, this one does *not* init to zero:
// after init, m will have whatever value was contained in the cached mpz.
inline void mpz_init_cache(mpz_struct_t &m, std::size_t nlimbs)
{
    get_mpz_cache().pop(m, nlimbs);
}

// Clear m (might move its value into the cache).
inline void mpz_clear_cache(mpz_struct_t &m)
{
    get_mpz_cache().push(m);
}

#else

inline void mpz_init_cache(mpz_struct_t &m, std::size_t nlimbs)
{
    mpz_init_nlimbs(m, nlimbs);
}

inline void mpz_clear_cache(mpz_struct_t &m)
{
    ::mpz_clear(&m);
}

#endif

// Combined init+set.
inline void mpz_init_set_cache(mpz_struct_t &m0, const mpz_struct_t &m1)
{
    mpz_init_cache(m0, ::mpz_size(&m1));
    ::mpz_set(&m0, &m1);
}

// Simple RAII holder for GMP integers.
struct mpz_raii {
    mpz_raii() : fail_flag(false)
    {
        ::mpz_init(&m_mpz);
        assert(m_mpz._mp_alloc >= 0);
    }
    mpz_raii(const mpz_raii &) = delete;
    mpz_raii(mpz_raii &&) = delete;
    mpz_raii &operator=(const mpz_raii &) = delete;
    mpz_raii &operator=(mpz_raii &&) = delete;
    ~mpz_raii()
    {
        // NOTE: even in recent GMP versions, with lazy allocation,
        // it seems like the pointer always points to something:
        // https://gmplib.org/repo/gmp/file/835f8974ff6e/mpz/init.c
        assert(m_mpz._mp_d != nullptr);
        ::mpz_clear(&m_mpz);
    }
    mpz_struct_t m_mpz;
    // This is a failure flag that is used in the static integer ctors (it can be ignored
    // for other uses of mpz_raii)
    bool fail_flag;
};

#if defined(MPPP_WITH_LONG_DOUBLE)

// mpfr_t is an array of some struct.
using mpfr_struct_t = std::remove_extent<::mpfr_t>::type;

// Simple RAII holder for MPFR floats.
struct mpfr_raii {
    mpfr_raii()
    {
        ::mpfr_init2(&m_mpfr, 53);
    }
    ~mpfr_raii()
    {
        ::mpfr_clear(&m_mpfr);
    }
    mpfr_struct_t m_mpfr;
};

// A couple of sanity checks when constructing temporary mpfrs from long double.
static_assert(std::numeric_limits<long double>::digits10 < std::numeric_limits<int>::max() / 4, "Overflow error.");
static_assert(std::numeric_limits<long double>::digits10 * 4 < std::numeric_limits<::mpfr_prec_t>::max(),
              "Overflow error.");

#endif

// Convert an mpz to a string in a specific base, to be written into out.
inline void mpz_to_str(std::vector<char> &out, const mpz_struct_t *mpz, int base = 10)
{
    assert(base >= 2 && base <= 62);
    const auto size_base = ::mpz_sizeinbase(mpz, base);
    if (mppp_unlikely(size_base > std::numeric_limits<std::size_t>::max() - 2u)) {
        throw std::overflow_error("Too many digits in the conversion of mpz_t to string.");
    }
    // Total max size is the size in base plus an optional sign and the null terminator.
    const auto total_size = size_base + 2u;
    // NOTE: possible improvement: use a null allocator to avoid initing the chars each time
    // we resize up.
    out.resize(static_cast<std::vector<char>::size_type>(total_size));
    // Overflow check.
    if (mppp_unlikely(out.size() != total_size)) {
        throw std::overflow_error("Too many digits in the conversion of mpz_t to string.");
    }
    ::mpz_get_str(out.data(), base, mpz);
}

// Convenience overload for the above.
inline std::string mpz_to_str(const mpz_struct_t *mpz, int base = 10)
{
    MPPP_MAYBE_TLS std::vector<char> tmp;
    mpz_to_str(tmp, mpz, base);
    return tmp.data();
}

// Type trait to check if T is a supported integral type.
template <typename T>
using is_supported_integral
    = std::integral_constant<bool, std::is_same<T, bool>::value || std::is_same<T, char>::value
                                       || std::is_same<T, signed char>::value || std::is_same<T, unsigned char>::value
                                       || std::is_same<T, short>::value || std::is_same<T, unsigned short>::value
                                       || std::is_same<T, int>::value || std::is_same<T, unsigned>::value
                                       || std::is_same<T, long>::value || std::is_same<T, unsigned long>::value
                                       || std::is_same<T, long long>::value
                                       || std::is_same<T, unsigned long long>::value>;

// Type trait to check if T is a supported floating-point type.
template <typename T>
using is_supported_float = std::integral_constant<bool, std::is_same<T, float>::value || std::is_same<T, double>::value
#if defined(MPPP_WITH_LONG_DOUBLE)
                                                            || std::is_same<T, long double>::value
#endif
                                                  >;

template <typename T>
using is_supported_interop
    = std::integral_constant<bool, is_supported_integral<T>::value || is_supported_float<T>::value>;

// Small wrapper to copy limbs.
inline void copy_limbs(const ::mp_limb_t *begin, const ::mp_limb_t *end, ::mp_limb_t *out)
{
    for (; begin != end; ++begin, ++out) {
        *out = *begin;
    }
}

// The version with non-overlapping ranges.
inline void copy_limbs_no(const ::mp_limb_t *begin, const ::mp_limb_t *end, ::mp_limb_t *MPPP_RESTRICT out)
{
    assert(begin != out);
    for (; begin != end; ++begin, ++out) {
        *out = *begin;
    }
}

// Add a and b, store the result in res, and return 1 if there's unsigned overflow, 0 othersize.
// NOTE: recent GCC versions have builtins for this, but they don't seem to make much of a difference:
// https://gcc.gnu.org/onlinedocs/gcc/Integer-Overflow-Builtins.html
inline ::mp_limb_t limb_add_overflow(::mp_limb_t a, ::mp_limb_t b, ::mp_limb_t *res)
{
    *res = a + b;
    return *res < a;
}

// The static integer class.
template <std::size_t SSize>
struct static_int {
    // Let's put a hard cap and sanity check on the static size.
    static_assert(SSize > 0u && SSize <= 64u, "Invalid static size.");
    using limbs_type = std::array<::mp_limb_t, SSize>;
    // Cast it to mpz_size_t for convenience.
    static const mpz_size_t s_size = SSize;
    // Special alloc value to signal static storage in the union.
    static const mpz_alloc_t s_alloc = -1;
    // The largest number of limbs for which special optimisations are activated.
    // This is important because the arithmetic optimisations rely on the unused limbs
    // being zero, thus whenever we use mpn functions on a static int we need to
    // take care of ensuring that this invariant is respected (see dtor_checks() and
    // zero_unused_limbs(), for instance).
    static const std::size_t opt_size = 2;
    // NOTE: init limbs to zero: in some few-limbs optimisations we operate on the whole limb
    // array regardless of the integer size, for performance reasons. If we didn't init to zero,
    // we would read from uninited storage and we would have wrong results as well.
    static_int() : _mp_alloc(s_alloc), _mp_size(0), m_limbs()
    {
    }
    // The defaults here are good.
    static_int(const static_int &) = default;
    static_int(static_int &&) = default;
    static_int &operator=(const static_int &) = default;
    static_int &operator=(static_int &&) = default;
    bool dtor_checks() const
    {
        const auto asize = abs_size();
        // Check the value of the alloc member.
        if (_mp_alloc != s_alloc) {
            return false;
        }
        // Check the asize is not too large.
        if (asize > s_size) {
            return false;
        }
        // Check that all limbs which do not participate in the value are zero, iff the SSize is small enough.
        // For small SSize, we might be using optimisations that require unused limbs to be zero.
        if (SSize <= opt_size) {
            for (auto i = static_cast<std::size_t>(asize); i < SSize; ++i) {
                if (m_limbs[i]) {
                    return false;
                }
            }
        }
        // Check that the highest limb is not zero.
        if (asize > 0 && (m_limbs[static_cast<std::size_t>(asize - 1)] & GMP_NUMB_MASK) == 0u) {
            return false;
        }
        return true;
    }
    ~static_int()
    {
        assert(dtor_checks());
    }
    // Zero the limbs that are not used for representing the value.
    // This is normally not needed, but it is useful when using the GMP mpn api on a static int:
    // the GMP api does not clear unused limbs, but we rely on unused limbs being zero when optimizing operations
    // for few static limbs.
    void zero_unused_limbs()
    {
        // Don't do anything if the static size is larger that the maximum size for which the
        // few-limbs optimisations are activated.
        if (SSize <= opt_size) {
            for (auto i = static_cast<std::size_t>(abs_size()); i < SSize; ++i) {
                m_limbs[i] = 0u;
            }
        }
    }
    // Size in limbs (absolute value of the _mp_size member).
    mpz_size_t abs_size() const
    {
        return _mp_size >= 0 ? _mp_size : -_mp_size;
    }
    // Construct from mpz. If the size in limbs is too large, it will set a failure flag
    // into m, to signal that the operation failed.
    void ctor_from_mpz(mpz_raii &m)
    {
        if (m.m_mpz._mp_size > s_size || m.m_mpz._mp_size < -s_size) {
            _mp_size = 0;
            m.fail_flag = true;
        } else {
            // All this is noexcept.
            _mp_size = m.m_mpz._mp_size;
            copy_limbs_no(m.m_mpz._mp_d, m.m_mpz._mp_d + abs_size(), m_limbs.data());
        }
    }
    template <typename Int, enable_if_t<is_supported_integral<Int>::value && std::is_unsigned<Int>::value, int> = 0>
    bool attempt_1limb_ctor(Int n)
    {
        if (!n) {
            _mp_size = 0;
            return true;
        }
        // This contraption is to avoid a compiler warning when Int is bool: in that case cast it to unsigned,
        // otherwise use the original value.
        if ((std::is_same<bool, Int>::value ? unsigned(n) : n) <= GMP_NUMB_MAX) {
            _mp_size = 1;
            m_limbs[0] = static_cast<::mp_limb_t>(n);
            return true;
        }
        return false;
    }
    template <typename Int, enable_if_t<is_supported_integral<Int>::value && std::is_signed<Int>::value, int> = 0>
    bool attempt_1limb_ctor(Int n)
    {
        using uint_t = typename std::make_unsigned<Int>::type;
        if (!n) {
            _mp_size = 0;
            return true;
        }
        if (n > 0 && uint_t(n) <= GMP_NUMB_MAX) {
            // If n is positive and fits in a limb, just cast it.
            _mp_size = 1;
            m_limbs[0] = static_cast<::mp_limb_t>(n);
            return true;
        }
        // For the negative case, we cast to long long and we check against
        // guaranteed limits for taking the absolute value of n.
        const long long lln = n;
        if (lln < 0 && lln >= -9223372036854775807ll && (unsigned long long)(-lln) <= GMP_NUMB_MAX) {
            _mp_size = -1;
            m_limbs[0] = static_cast<::mp_limb_t>(-lln);
            return true;
        }
        return false;
    }
    // NOTE: this wrapper around std::numeric_limits is to work around an MSVC bug (it is seemingly
    // unable to deal with constexpr functions in a SFINAE context).
    template <typename T, typename = void>
    struct limits {
    };
    template <typename T>
    struct limits<T, enable_if_t<std::is_integral<T>::value>> {
        static const T max = std::numeric_limits<T>::max();
        static const T min = std::numeric_limits<T>::min();
    };
    // Ctor from unsigned integral types that are wider than unsigned long.
    // This requires special handling as the GMP api does not support unsigned long long natively.
    template <typename Uint, enable_if_t<is_supported_integral<Uint>::value && std::is_unsigned<Uint>::value
                                             && (limits<Uint>::max > limits<unsigned long>::max),
                                         int> = 0>
    explicit static_int(Uint n, mpz_raii &mpz) : _mp_alloc(s_alloc), m_limbs()
    {
        if (attempt_1limb_ctor(n)) {
            return;
        }
        constexpr auto ulmax = std::numeric_limits<unsigned long>::max();
        if (n <= ulmax) {
            // The value fits unsigned long, just cast it.
            ::mpz_set_ui(&mpz.m_mpz, static_cast<unsigned long>(n));
        } else {
            // Init the shifter.
            MPPP_MAYBE_TLS mpz_raii shifter;
            ::mpz_set_ui(&shifter.m_mpz, 1u);
            // Set output to the lowest UL limb of n.
            ::mpz_set_ui(&mpz.m_mpz, static_cast<unsigned long>(n & ulmax));
            // Move the limbs of n to the right.
            // NOTE: this is ok because we tested above that n is wider than unsigned long, so its bit
            // width must be larger than unsigned long's.
            n >>= std::numeric_limits<unsigned long>::digits;
            while (n) {
                // Increase the shifter.
                ::mpz_mul_2exp(&shifter.m_mpz, &shifter.m_mpz, std::numeric_limits<unsigned long>::digits);
                // Add the current lowest UL limb of n to the output, after having multiplied it
                // by the shitfer.
                ::mpz_addmul_ui(&mpz.m_mpz, &shifter.m_mpz, static_cast<unsigned long>(n & ulmax));
                n >>= std::numeric_limits<unsigned long>::digits;
            }
        }
        ctor_from_mpz(mpz);
    }
    // Ctor from unsigned integral types that are not wider than unsigned long.
    template <typename Uint, enable_if_t<is_supported_integral<Uint>::value && std::is_unsigned<Uint>::value
                                             && (limits<Uint>::max <= limits<unsigned long>::max),
                                         int> = 0>
    explicit static_int(Uint n, mpz_raii &mpz) : _mp_alloc(s_alloc), m_limbs()
    {
        if (attempt_1limb_ctor(n)) {
            return;
        }
        ::mpz_set_ui(&mpz.m_mpz, static_cast<unsigned long>(n));
        ctor_from_mpz(mpz);
    }
    // Ctor from signed integral types that are wider than long.
    template <typename Int,
              enable_if_t<std::is_signed<Int>::value && is_supported_integral<Int>::value
                              && (limits<Int>::max > limits<long>::max || limits<Int>::min < limits<long>::min),
                          int> = 0>
    explicit static_int(Int n, mpz_raii &mpz) : _mp_alloc(s_alloc), m_limbs()
    {
        if (attempt_1limb_ctor(n)) {
            return;
        }
        constexpr auto lmax = std::numeric_limits<long>::max(), lmin = std::numeric_limits<long>::min();
        if (n <= lmax && n >= lmin) {
            // The value fits long, just cast it.
            ::mpz_set_si(&mpz.m_mpz, static_cast<long>(n));
        } else {
            // A temporary variable for the accumulation of the result in the loop below.
            // Needed because GMP does not have mpz_addmul_si().
            MPPP_MAYBE_TLS mpz_raii tmp;
            // The rest is as above, with the following differences:
            // - use % instead of bit masking and division instead of bit shift,
            // - proceed by chunks of 30 bits, as that's the highest power of 2 portably
            //   representable by long.
            MPPP_MAYBE_TLS mpz_raii shifter;
            ::mpz_set_ui(&shifter.m_mpz, 1u);
            ::mpz_set_si(&mpz.m_mpz, static_cast<long>(n % (1l << 30)));
            n /= (1l << 30);
            while (n) {
                ::mpz_mul_2exp(&shifter.m_mpz, &shifter.m_mpz, 30);
                ::mpz_set_si(&tmp.m_mpz, static_cast<long>(n % (1l << 30)));
                ::mpz_addmul(&mpz.m_mpz, &shifter.m_mpz, &tmp.m_mpz);
                n /= (1l << 30);
            }
        }
        ctor_from_mpz(mpz);
    }
    // Ctor from signed integral types that are not wider than long.
    template <typename Int,
              enable_if_t<std::is_signed<Int>::value && is_supported_integral<Int>::value
                              && (limits<Int>::max <= limits<long>::max && limits<Int>::min >= limits<long>::min),
                          int> = 0>
    explicit static_int(Int n, mpz_raii &mpz) : _mp_alloc(s_alloc), m_limbs()
    {
        if (attempt_1limb_ctor(n)) {
            return;
        }
        ::mpz_set_si(&mpz.m_mpz, static_cast<long>(n));
        ctor_from_mpz(mpz);
    }
    // Ctor from float or double.
    template <typename Float,
              enable_if_t<std::is_same<Float, float>::value || std::is_same<Float, double>::value, int> = 0>
    explicit static_int(Float f, mpz_raii &mpz) : _mp_alloc(s_alloc), m_limbs()
    {
        if (mppp_unlikely(!std::isfinite(f))) {
            throw std::invalid_argument("Cannot init integer from non-finite floating-point value.");
        }
        ::mpz_set_d(&mpz.m_mpz, static_cast<double>(f));
        ctor_from_mpz(mpz);
    }
#if defined(MPPP_WITH_LONG_DOUBLE)
    // Ctor from long double.
    explicit static_int(long double x, mpz_raii &mpz) : _mp_alloc(s_alloc), m_limbs()
    {
        if (!std::isfinite(x)) {
            throw std::invalid_argument("Cannot init integer from non-finite floating-point value.");
        }
        MPPP_MAYBE_TLS mpfr_raii mpfr;
        // NOTE: static checks for overflows are done above.
        constexpr int d2 = std::numeric_limits<long double>::digits10 * 4;
        ::mpfr_set_prec(&mpfr.m_mpfr, static_cast<::mpfr_prec_t>(d2));
        ::mpfr_set_ld(&mpfr.m_mpfr, x, MPFR_RNDN);
        ::mpfr_get_z(&mpz.m_mpz, &mpfr.m_mpfr, MPFR_RNDZ);
        ctor_from_mpz(mpz);
    }
#endif

    class static_mpz_view
    {
    public:
        // NOTE: this is needed when we have the variant view in the integer class: if the active view
        // is the dynamic one, we need to def construct a static view that we will never use.
        // NOTE: m_mpz needs to be zero inited because otherwise when using the move ctor we will
        // be reading from uninited memory.
        static_mpz_view() : m_mpz{}
        {
        }
        // NOTE: we use the const_cast to cast away the constness from the pointer to the limbs
        // in n. This is valid as we are never going to use this pointer for writing.
        // NOTE: in recent GMP versions there are functions to accomplish this type of read-only init
        // of an mpz:
        // https://gmplib.org/manual/Integer-Special-Functions.html#Integer-Special-Functions
        explicit static_mpz_view(const static_int &n)
            : m_mpz{s_size, n._mp_size, const_cast<::mp_limb_t *>(n.m_limbs.data())}
        {
        }
        static_mpz_view(const static_mpz_view &) = delete;
        static_mpz_view(static_mpz_view &&) = default;
        static_mpz_view &operator=(const static_mpz_view &) = delete;
        static_mpz_view &operator=(static_mpz_view &&) = delete;
        operator const mpz_struct_t *() const
        {
            return &m_mpz;
        }

    private:
        mpz_struct_t m_mpz;
    };
    static_mpz_view get_mpz_view() const
    {
        return static_mpz_view{*this};
    }
    mpz_alloc_t _mp_alloc;
    mpz_size_t _mp_size;
    limbs_type m_limbs;
};

// {static_int,mpz} union.
template <std::size_t SSize>
union integer_union {
public:
    using s_storage = static_int<SSize>;
    using d_storage = mpz_struct_t;
    // Def ctor, will init to static.
    integer_union() : m_st()
    {
    }
    // Copy constructor, does a deep copy maintaining the storage class of other.
    integer_union(const integer_union &other)
    {
        if (other.is_static()) {
            ::new (static_cast<void *>(&m_st)) s_storage(other.g_st());
        } else {
            ::new (static_cast<void *>(&m_dy)) d_storage;
            mpz_init_set_cache(m_dy, other.g_dy());
            assert(m_dy._mp_alloc >= 0);
        }
    }
    // Move constructor. Will downgrade other to a static zero integer if other is dynamic.
    integer_union(integer_union &&other) noexcept
    {
        if (other.is_static()) {
            ::new (static_cast<void *>(&m_st)) s_storage(std::move(other.g_st()));
        } else {
            ::new (static_cast<void *>(&m_dy)) d_storage;
            // NOTE: this copies the mpz struct members (shallow copy).
            m_dy = other.g_dy();
            // Downgrade the other to an empty static.
            other.g_dy().~d_storage();
            ::new (static_cast<void *>(&other.m_st)) s_storage();
        }
    }
    // Generic constructor from the interoperable basic C++ types. It will first try to construct
    // a static, if too many limbs are needed it will construct a dynamic instead.
    template <typename T, enable_if_t<is_supported_interop<T>::value, int> = 0>
    explicit integer_union(T x)
    {
        MPPP_MAYBE_TLS mpz_raii mpz;
        assert(!mpz.fail_flag);
        // Attempt static storage construction.
        ::new (static_cast<void *>(&m_st)) s_storage(x, mpz);
        // Check if too many limbs were generated.
        if (mpz.fail_flag) {
            // Reset the fail_flag.
            mpz.fail_flag = false;
            // Destroy static.
            g_st().~s_storage();
            // Init dynamic.
            ::new (static_cast<void *>(&m_dy)) d_storage;
            mpz_init_set_cache(m_dy, mpz.m_mpz);
        }
    }
    explicit integer_union(const char *s, int base) : m_st()
    {
        MPPP_MAYBE_TLS mpz_raii mpz;
        if (::mpz_set_str(&mpz.m_mpz, s, base)) {
            throw std::invalid_argument(std::string("The string '") + s + "' is not a valid integer in base "
                                        + std::to_string(base) + ".");
        }
        assert(!mpz.fail_flag);
        g_st().ctor_from_mpz(mpz);
        // Check if the construction succeeded.
        if (mpz.fail_flag) {
            // Reset the fail_flag.
            mpz.fail_flag = false;
            // Destroy static.
            g_st().~s_storage();
            // Init dynamic.
            ::new (static_cast<void *>(&m_dy)) d_storage;
            mpz_init_set_cache(m_dy, mpz.m_mpz);
        }
    }
    // Copy assignment operator, performs a deep copy maintaining the storage class.
    integer_union &operator=(const integer_union &other)
    {
        if (mppp_unlikely(this == &other)) {
            return *this;
        }
        const bool s1 = is_static(), s2 = other.is_static();
        if (s1 && s2) {
            g_st() = other.g_st();
        } else if (s1 && !s2) {
            // Destroy static.
            g_st().~s_storage();
            // Construct the dynamic struct.
            ::new (static_cast<void *>(&m_dy)) d_storage;
            // Init + assign the mpz.
            mpz_init_set_cache(m_dy, other.g_dy());
            assert(m_dy._mp_alloc >= 0);
        } else if (!s1 && s2) {
            // Destroy the dynamic this.
            destroy_dynamic();
            // Init-copy the static from other.
            ::new (static_cast<void *>(&m_st)) s_storage(other.g_st());
        } else {
            ::mpz_set(&g_dy(), &other.g_dy());
        }
        return *this;
    }
    // Move assignment, same as above plus possibly steals resources. If this is static
    // and other is dynamic, other is downgraded to a zero static.
    integer_union &operator=(integer_union &&other) noexcept
    {
        if (mppp_unlikely(this == &other)) {
            return *this;
        }
        const bool s1 = is_static(), s2 = other.is_static();
        if (s1 && s2) {
            g_st() = std::move(other.g_st());
        } else if (s1 && !s2) {
            // Destroy static.
            g_st().~s_storage();
            // Construct the dynamic struct.
            ::new (static_cast<void *>(&m_dy)) d_storage;
            m_dy = other.g_dy();
            // Downgrade the other to an empty static.
            other.g_dy().~d_storage();
            ::new (static_cast<void *>(&other.m_st)) s_storage();
        } else if (!s1 && s2) {
            // Same as copy assignment: destroy and copy-construct.
            destroy_dynamic();
            ::new (static_cast<void *>(&m_st)) s_storage(other.g_st());
        } else {
            // Swap with other.
            ::mpz_swap(&g_dy(), &other.g_dy());
        }
        return *this;
    }
    ~integer_union()
    {
        if (is_static()) {
            g_st().~s_storage();
        } else {
            destroy_dynamic();
        }
    }
    void destroy_dynamic()
    {
        assert(!is_static());
        assert(g_dy()._mp_alloc >= 0);
        assert(g_dy()._mp_d != nullptr);
        mpz_clear_cache(g_dy());
        m_dy.~d_storage();
    }
    // Check storage type.
    bool is_static() const
    {
        return m_st._mp_alloc == s_storage::s_alloc;
    }
    bool is_dynamic() const
    {
        return m_st._mp_alloc != s_storage::s_alloc;
    }
    // Getters for st and dy.
    const s_storage &g_st() const
    {
        assert(is_static());
        return m_st;
    }
    s_storage &g_st()
    {
        assert(is_static());
        return m_st;
    }
    const d_storage &g_dy() const
    {
        assert(is_dynamic());
        return m_dy;
    }
    d_storage &g_dy()
    {
        assert(is_dynamic());
        return m_dy;
    }
    // Promotion from static to dynamic. If nlimbs != 0u, allocate nlimbs limbs, otherwise
    // allocate exactly the nlimbs necessary to represent this.
    void promote(std::size_t nlimbs = 0u)
    {
        assert(is_static());
        mpz_struct_t tmp_mpz;
        auto v = g_st().get_mpz_view();
        if (nlimbs == 0u) {
            // If nlimbs is zero, we will allocate exactly the needed
            // number of limbs to represent this.
            mpz_init_set_cache(tmp_mpz, *v);
        } else {
            // Otherwise, we preallocate nlimbs and then set tmp_mpz
            // to the value of this.
            mpz_init_cache(tmp_mpz, nlimbs);
            ::mpz_set(&tmp_mpz, v);
        }
        // Destroy static.
        g_st().~s_storage();
        // Construct the dynamic struct.
        ::new (static_cast<void *>(&m_dy)) d_storage;
        m_dy = tmp_mpz;
    }
    // Demotion from dynamic to static.
    bool demote()
    {
        assert(is_dynamic());
        const auto dyn_size = ::mpz_size(&g_dy());
        // If the dynamic size is greater than the static size, we cannot demote.
        if (dyn_size > SSize) {
            return false;
        }
        // Copy over the limbs to temporary storage.
        std::array<::mp_limb_t, SSize> tmp;
        copy_limbs_no(g_dy()._mp_d, g_dy()._mp_d + dyn_size, tmp.data());
        const auto signed_size = g_dy()._mp_size;
        // Destroy the dynamic storage.
        destroy_dynamic();
        // Init the static storage and copy over the data.
        // NOTE: here the ctor makes sure the static limbs are zeroed
        // out and we don't get stray limbs when copying below.
        ::new (static_cast<void *>(&m_st)) s_storage();
        g_st()._mp_size = signed_size;
        copy_limbs_no(tmp.data(), tmp.data() + dyn_size, g_st().m_limbs.data());
        return true;
    }
    // NOTE: keep these public as we need them below.
    s_storage m_st;
    d_storage m_dy;
};
}

struct zero_division_error final : std::domain_error {
    using std::domain_error::domain_error;
};

template <std::size_t SSize>
class mp_integer
{
    // The underlying static int.
    using s_int = static_int<SSize>;
    // mpz view class.
    class mpz_view
    {
        using static_mpz_view = typename s_int::static_mpz_view;

    public:
        explicit mpz_view(const mp_integer &n)
            : m_static_view(n.is_static() ? n.m_int.g_st().get_mpz_view() : static_mpz_view{}),
              m_ptr(n.is_static() ? m_static_view : &(n.m_int.g_dy()))
        {
        }
        mpz_view(const mpz_view &) = delete;
        mpz_view(mpz_view &&) = default;
        mpz_view &operator=(const mpz_view &) = delete;
        mpz_view &operator=(mpz_view &&) = delete;
        operator const mpz_struct_t *() const
        {
            return get();
        }
        const mpz_struct_t *get() const
        {
            return m_ptr;
        }

    private:
        static_mpz_view m_static_view;
        const mpz_struct_t *m_ptr;
    };

public:
    mp_integer() = default;
    mp_integer(const mp_integer &other) = default;
    mp_integer(mp_integer &&other) = default;

private:
    // Enabler for generic ctor.
    template <typename T>
    using generic_ctor_enabler = enable_if_t<is_supported_interop<T>::value, int>;

public:
    template <typename T, generic_ctor_enabler<T> = 0>
    explicit mp_integer(T x) : m_int(x)
    {
    }
    explicit mp_integer(const char *s, int base = 10) : m_int(s, base)
    {
    }
    explicit mp_integer(const std::string &s, int base = 10) : mp_integer(s.c_str(), base)
    {
    }
    mp_integer &operator=(const mp_integer &other) = default;
    mp_integer &operator=(mp_integer &&other) = default;
    bool is_static() const
    {
        return m_int.is_static();
    }
    bool is_dynamic() const
    {
        return m_int.is_dynamic();
    }
    friend std::ostream &operator<<(std::ostream &os, const mp_integer &n)
    {
        return os << n.to_string();
    }
    std::string to_string(int base = 10) const
    {
        if (mppp_unlikely(base < 2 || base > 62)) {
            throw std::invalid_argument("Invalid base for string conversion: the base must be between "
                                        "2 and 62, but a value of "
                                        + std::to_string(base) + " was provided instead.");
        }
        if (is_static()) {
            return mpz_to_str(m_int.g_st().get_mpz_view());
        }
        return mpz_to_str(&m_int.g_dy());
    }
    // NOTE: maybe provide a method to access the lower-level str conversion that writes to
    // std::vector<char>?

private:
    // Conversion operator.
    template <typename T>
    using generic_conversion_enabler = generic_ctor_enabler<T>;
    template <typename T>
    using uint_conversion_enabler
        = enable_if_t<is_supported_integral<T>::value && std::is_unsigned<T>::value && !std::is_same<bool, T>::value,
                      int>;
    template <typename T>
    using int_conversion_enabler = enable_if_t<is_supported_integral<T>::value && std::is_signed<T>::value, int>;
    // Static conversion to bool.
    template <typename T, enable_if_t<std::is_same<T, bool>::value, int> = 0>
    static T conversion_impl(const s_int &n)
    {
        return n._mp_size != 0;
    }
    // Static conversion to unsigned ints.
    template <typename T, uint_conversion_enabler<T> = 0>
    static T conversion_impl(const s_int &n)
    {
        // Handle zero.
        if (!n._mp_size) {
            return T(0);
        }
        if (n._mp_size == 1) {
            // Single-limb, positive value case.
            if ((n.m_limbs[0] & GMP_NUMB_MASK) > std::numeric_limits<T>::max()) {
                // TODO error message.
                throw std::overflow_error("");
            }
            return static_cast<T>(n.m_limbs[0] & GMP_NUMB_MASK);
        }
        if (n._mp_size < 0) {
            // Negative values cannot be converted to unsigned ints.
            // TODO error message.
            throw std::overflow_error("");
        }
        // In multilimb case, forward to mpz.
        return conversion_impl<T>(*static_cast<const mpz_struct_t *>(n.get_mpz_view()));
    }
    // Static conversion to signed ints.
    template <typename T, int_conversion_enabler<T> = 0>
    static T conversion_impl(const s_int &n)
    {
        using uint_t = typename std::make_unsigned<T>::type;
        // Handle zero.
        if (!n._mp_size) {
            return T(0);
        }
        if (n._mp_size == 1) {
            // Single-limb, positive value case.
            if ((n.m_limbs[0] & GMP_NUMB_MASK) > uint_t(std::numeric_limits<T>::max())) {
                // TODO error message.
                throw std::overflow_error("");
            }
            return static_cast<T>(n.m_limbs[0] & GMP_NUMB_MASK);
        }
        if (n._mp_size == -1 && (n.m_limbs[0] & GMP_NUMB_MASK) <= 9223372036854775807ull) {
            // Handle negative single limb only if we are in the safe range of long long.
            const auto candidate = -static_cast<long long>(n.m_limbs[0] & GMP_NUMB_MASK);
            if (candidate < std::numeric_limits<T>::min()) {
                // TODO error message.
                throw std::overflow_error("");
            }
            return static_cast<T>(candidate);
        }
        // Forward to mpz.
        return conversion_impl<T>(*static_cast<const mpz_struct_t *>(n.get_mpz_view()));
    }
    // Static conversion to floating-point.
    template <typename T, enable_if_t<is_supported_float<T>::value, int> = 0>
    static T conversion_impl(const s_int &n)
    {
        // Handle zero.
        if (!n._mp_size) {
            return T(0);
        }
        if (std::numeric_limits<T>::is_iec559) {
            // Optimization for single-limb integers.
            // NOTE: the reasoning here is as follows. If the floating-point type has infinity,
            // then its "range" is the whole real line. The C++ standard guarantees that:
            // """
            // A prvalue of an integer type or of an unscoped enumeration type can be converted to a prvalue of a
            // floating point type. The result is exact if possible. If the value being converted is in the range
            // of values that can be represented but the value cannot be represented exactly,
            // it is an implementation-defined choice of either the next lower or higher representable value. If the
            // value being converted is outside the range of values that can be represented, the behavior is undefined.
            // """
            // This seems to indicate that if the limb value "overflows" the finite range of the floating-point type, we
            // will get either the max/min finite value or +-inf. Additionally, the IEEE standard seems to indicate
            // that an overflowing conversion will produce infinity:
            // http://stackoverflow.com/questions/40694384/integer-to-float-conversions-with-ieee-fp
            if (n._mp_size == 1) {
                return static_cast<T>(n.m_limbs[0] & GMP_NUMB_MASK);
            }
            if (n._mp_size == -1) {
                return -static_cast<T>(n.m_limbs[0] & GMP_NUMB_MASK);
            }
        }
        // For all the other cases, just delegate to the GMP/MPFR routines.
        return conversion_impl<T>(*static_cast<const mpz_struct_t *>(n.get_mpz_view()));
    }
    // Dynamic conversion to bool.
    template <typename T, enable_if_t<std::is_same<T, bool>::value, int> = 0>
    static T conversion_impl(const mpz_struct_t &m)
    {
        return mpz_sgn(&m) != 0;
    }
    // Dynamic conversion to unsigned ints.
    template <typename T, uint_conversion_enabler<T> = 0>
    static T conversion_impl(const mpz_struct_t &m)
    {
        if (mpz_sgn(&m) < 0) {
            // Cannot convert negative values into unsigned ints.
            // TODO error message.
            throw std::overflow_error("");
        }
        if (::mpz_fits_ulong_p(&m)) {
            // Go through GMP if possible.
            const auto ul = ::mpz_get_ui(&m);
            if (ul <= std::numeric_limits<T>::max()) {
                return static_cast<T>(ul);
            }
            // TODO error message.
            throw std::overflow_error("");
        }
        // We are now in a situation in which m does not fit in ulong. The only hope is that it does
        // fit in ulonglong. We will try to build one operating in 32-bit chunks.
        unsigned long long retval = 0u;
        // q will be a copy of m that will be right-shifted down in chunks.
        MPPP_MAYBE_TLS mpz_raii q;
        ::mpz_set(&q.m_mpz, &m);
        // Init the multiplier for use in the loop below.
        unsigned long long multiplier = 1u;
        // Handy shortcut.
        constexpr auto ull_max = std::numeric_limits<unsigned long long>::max();
        while (true) {
            // NOTE: mpz_get_ui() already gives the lower bits of q, select the first 32 bits.
            unsigned long long ull = ::mpz_get_ui(&q.m_mpz) & ((1ull << 32) - 1ull);
            // Overflow check.
            if (ull > ull_max / multiplier) {
                // TODO message.
                throw std::overflow_error("");
            }
            // Shift up the current 32 bits being considered.
            ull *= multiplier;
            // Overflow check.
            if (retval > ull_max - ull) {
                // TODO message.
                throw std::overflow_error("");
            }
            // Add the current 32 bits to the result.
            retval += ull;
            // Shift down q.
            ::mpz_tdiv_q_2exp(&q.m_mpz, &q.m_mpz, 32);
            // The iteration will stop when q becomes zero.
            if (!mpz_sgn(&q.m_mpz)) {
                break;
            }
            // Overflow check.
            if (multiplier > ull_max / (1ull << 32)) {
                // TODO message.
                throw std::overflow_error("");
            }
            // Update the multiplier.
            multiplier *= 1ull << 32;
        }
        if (retval > std::numeric_limits<T>::max()) {
            // TODO error message.
            throw std::overflow_error("");
        }
        return static_cast<T>(retval);
    }
    // Dynamic conversion to signed ints.
    template <typename T, int_conversion_enabler<T> = 0>
    static T conversion_impl(const mpz_struct_t &m)
    {
        if (::mpz_fits_slong_p(&m)) {
            const auto sl = ::mpz_get_si(&m);
            if (sl >= std::numeric_limits<T>::min() && sl <= std::numeric_limits<T>::max()) {
                return static_cast<T>(sl);
            }
            // TODO error message.
            throw std::overflow_error("");
        }
        // The same approach as for the unsigned case, just slightly more complicated because
        // of the presence of the sign.
        long long retval = 0;
        MPPP_MAYBE_TLS mpz_raii q;
        ::mpz_set(&q.m_mpz, &m);
        const bool sign = mpz_sgn(&q.m_mpz) > 0;
        long long multiplier = sign ? 1 : -1;
        // Shortcuts.
        constexpr auto ll_max = std::numeric_limits<long long>::max();
        constexpr auto ll_min = std::numeric_limits<long long>::min();
        while (true) {
            auto ll = static_cast<long long>(::mpz_get_ui(&q.m_mpz) & ((1ull << 32) - 1ull));
            if ((sign && ll && multiplier > ll_max / ll) || (!sign && ll && multiplier < ll_min / ll)) {
                // TODO error message.
                throw std::overflow_error("1");
            }
            ll *= multiplier;
            if ((sign && retval > ll_max - ll) || (!sign && retval < ll_min - ll)) {
                // TODO error message.
                throw std::overflow_error("2");
            }
            retval += ll;
            ::mpz_tdiv_q_2exp(&q.m_mpz, &q.m_mpz, 32);
            if (!mpz_sgn(&q.m_mpz)) {
                break;
            }
            if ((sign && multiplier > ll_max / (1ll << 32)) || (!sign && multiplier < ll_min / (1ll << 32))) {
                // TODO error message.
                throw std::overflow_error("3");
            }
            multiplier *= 1ll << 32;
        }
        if (retval > std::numeric_limits<T>::max() || retval < std::numeric_limits<T>::min()) {
            // TODO error message.
            throw std::overflow_error("4");
        }
        return static_cast<T>(retval);
    }
    // Dynamic conversion to float/double.
    template <typename T, enable_if_t<std::is_same<T, float>::value || std::is_same<T, double>::value, int> = 0>
    static T conversion_impl(const mpz_struct_t &m)
    {
        return static_cast<T>(::mpz_get_d(&m));
    }
#if defined(MPPP_WITH_LONG_DOUBLE)
    // Dynamic conversion to long double.
    template <typename T, enable_if_t<std::is_same<T, long double>::value, int> = 0>
    static T conversion_impl(const mpz_struct_t &m)
    {
        MPPP_MAYBE_TLS mpfr_raii mpfr;
        constexpr int d2 = std::numeric_limits<long double>::digits10 * 4;
        ::mpfr_set_prec(&mpfr.m_mpfr, static_cast<::mpfr_prec_t>(d2));
        ::mpfr_set_z(&mpfr.m_mpfr, &m, MPFR_RNDN);
        return ::mpfr_get_ld(&mpfr.m_mpfr, MPFR_RNDN);
    }
#endif
public:
    template <typename T, generic_conversion_enabler<T> = 0>
    explicit operator T() const
    {
        if (is_static()) {
            return conversion_impl<T>(m_int.g_st());
        }
        return conversion_impl<T>(m_int.g_dy());
    }
    void promote()
    {
        if (is_dynamic()) {
            // TODO throw.
            throw std::invalid_argument("");
        }
        m_int.promote();
    }
    std::size_t nbits() const
    {
        return ::mpz_sizeinbase(get_mpz_view(), 2);
    }
    std::size_t size() const
    {
        if (is_static()) {
            return std::size_t(m_int.g_st().abs_size());
        }
        return ::mpz_size(&m_int.g_dy());
    }
    mpz_view get_mpz_view() const
    {
        return mpz_view(*this);
    }
    void negate()
    {
        if (is_static()) {
            m_int.g_st()._mp_size = -m_int.g_st()._mp_size;
        } else {
            ::mpz_neg(&m_int.g_dy(), &m_int.g_dy());
        }
    }

private:
    // Metaprogramming for selecting the algorithm for static addition. The selection happens via
    // an std::integral_constant with 3 possible values:
    // - 0 (default case): use the GMP mpn functions,
    // - 1: selected when there are no nail bits and the static size is 1,
    // - 2: selected when there are no nail bits and the static size is 2.
    template <typename SInt>
    using static_add_algo = std::integral_constant<int, (!GMP_NAIL_BITS && SInt::s_size == 1)
                                                            ? 1
                                                            : ((!GMP_NAIL_BITS && SInt::s_size == 2) ? 2 : 0)>;
    // General implementation via mpn.
    // Small helper to compute the size after subtraction via mpn. s is a strictly positive size.
    static mpz_size_t sub_compute_size(const ::mp_limb_t *rdata, mpz_size_t s)
    {
        assert(s > 0);
        mpz_size_t cur_idx = s - 1;
        for (; cur_idx >= 0; --cur_idx) {
            if (rdata[cur_idx] & GMP_NUMB_MASK) {
                break;
            }
        }
        return cur_idx + 1;
    }
    // NOTE: this function (and its other overloads) will return true in case of success, false in case of failure
    // (i.e., the addition overflows and the static size is not enough).
    static bool static_add_impl(s_int &rop, const s_int &op1, const s_int &op2, mpz_size_t asize1, mpz_size_t asize2,
                                int sign1, int sign2, const std::integral_constant<int, 0> &)
    {
        auto rdata = &rop.m_limbs[0];
        auto data1 = &op1.m_limbs[0], data2 = &op2.m_limbs[0];
        const auto size1 = op1._mp_size, size2 = op2._mp_size;
        // mpn functions require nonzero arguments.
        if (mppp_unlikely(!sign2)) {
            rop._mp_size = size1;
            copy_limbs(data1, data1 + asize1, &rop.m_limbs[0]);
            return true;
        }
        if (mppp_unlikely(!sign1)) {
            rop._mp_size = size2;
            copy_limbs(data2, data2 + asize2, &rop.m_limbs[0]);
            return true;
        }
        // Check, for op1 and op2, whether:
        // - the asize is the max static size, and, if yes,
        // - the highest bit in the highest limb is set.
        // If this condition holds for op1 and op2, we return failure, as the computation might require
        // an extra limb.
        // NOTE: the reason we check this is that we do not want to run into the situation in which we have written
        // something into rop (via the mpn functions below), only to realize later that the computation overflows.
        // This would be bad because, in case of overlapping arguments, it would destroy one or two operands without
        // possibility of recovering. The alternative would be to do the computation in some local buffer and then copy
        // it out, but that is rather costly. Note that this means that in principle a computation that could fit in
        // static storage ends up triggering a promotion.
        const bool c1 = std::size_t(asize1) == SSize && ((data1[asize1 - 1] & GMP_NUMB_MASK) >> (GMP_NUMB_BITS - 1));
        const bool c2 = std::size_t(asize2) == SSize && ((data2[asize2 - 1] & GMP_NUMB_MASK) >> (GMP_NUMB_BITS - 1));
        if (mppp_unlikely(c1 || c2)) {
            return false;
        }
        if (sign1 == sign2) {
            // Same sign.
            if (asize1 >= asize2) {
                // The number of limbs of op1 >= op2.
                ::mp_limb_t cy;
                if (asize2 == 1) {
                    // NOTE: we are not masking data2[0] with GMP_NUMB_MASK, I am assuming the mpn function
                    // is able to deal with a limb with a nail.
                    cy = ::mpn_add_1(rdata, data1, static_cast<::mp_size_t>(asize1), data2[0]);
                } else if (asize1 == asize2) {
                    cy = ::mpn_add_n(rdata, data1, data2, static_cast<::mp_size_t>(asize1));
                } else {
                    cy = ::mpn_add(rdata, data1, static_cast<::mp_size_t>(asize1), data2,
                                   static_cast<::mp_size_t>(asize2));
                }
                if (cy) {
                    assert(asize1 < s_int::s_size);
                    rop._mp_size = size1 + sign1;
                    // NOTE: there should be no need to use GMP_NUMB_MASK here.
                    rdata[asize1] = 1;
                } else {
                    // Without carry, the size is unchanged.
                    rop._mp_size = size1;
                }
            } else {
                // The number of limbs of op2 > op1.
                ::mp_limb_t cy;
                if (asize1 == 1) {
                    cy = ::mpn_add_1(rdata, data2, static_cast<::mp_size_t>(asize2), data1[0]);
                } else {
                    cy = ::mpn_add(rdata, data2, static_cast<::mp_size_t>(asize2), data1,
                                   static_cast<::mp_size_t>(asize1));
                }
                if (cy) {
                    assert(asize2 < s_int::s_size);
                    rop._mp_size = size2 + sign2;
                    rdata[asize2] = 1;
                } else {
                    rop._mp_size = size2;
                }
            }
        } else {
            if (asize1 > asize2
                || (asize1 == asize2 && ::mpn_cmp(data1, data2, static_cast<::mp_size_t>(asize1)) >= 0)) {
                // abs(op1) >= abs(op2).
                ::mp_limb_t br;
                if (asize2 == 1) {
                    br = ::mpn_sub_1(rdata, data1, static_cast<::mp_size_t>(asize1), data2[0]);
                } else if (asize1 == asize2) {
                    br = ::mpn_sub_n(rdata, data1, data2, static_cast<::mp_size_t>(asize1));
                } else {
                    br = ::mpn_sub(rdata, data1, static_cast<::mp_size_t>(asize1), data2,
                                   static_cast<::mp_size_t>(asize2));
                }
                assert(!br);
                rop._mp_size = sub_compute_size(rdata, asize1);
                if (sign1 != 1) {
                    rop._mp_size = -rop._mp_size;
                }
            } else {
                // abs(op2) > abs(op1).
                ::mp_limb_t br;
                if (asize1 == 1) {
                    br = ::mpn_sub_1(rdata, data2, static_cast<::mp_size_t>(asize2), data1[0]);
                } else {
                    br = ::mpn_sub(rdata, data2, static_cast<::mp_size_t>(asize2), data1,
                                   static_cast<::mp_size_t>(asize1));
                }
                assert(!br);
                rop._mp_size = sub_compute_size(rdata, asize2);
                if (sign2 != 1) {
                    rop._mp_size = -rop._mp_size;
                }
            }
        }
        return true;
    }
    // Optimization for single-limb statics with no nails.
    static bool static_add_impl(s_int &rop, const s_int &op1, const s_int &op2, mpz_size_t asize1, mpz_size_t asize2,
                                int sign1, int sign2, const std::integral_constant<int, 1> &)
    {
        auto rdata = &rop.m_limbs[0];
        auto data1 = &op1.m_limbs[0], data2 = &op2.m_limbs[0];
        // NOTE: both asizes have to be 0 or 1 here.
        assert((asize1 == 1 && data1[0] != 0u) || (asize1 == 0 && data1[0] == 0u));
        assert((asize2 == 1 && data2[0] != 0u) || (asize2 == 0 && data2[0] == 0u));
        ::mp_limb_t tmp;
        if (sign1 == sign2) {
            // When the signs are identical, we can implement addition as a true addition.
            if (mppp_unlikely(limb_add_overflow(data1[0], data2[0], &tmp))) {
                return false;
            }
            // Assign the output. asize can be zero (sign1 == sign2 == 0) or 1.
            rop._mp_size = sign1;
            rdata[0] = tmp;
        } else {
            // When the signs differ, we need to implement addition as a subtraction.
            if (data1[0] >= data2[0]) {
                // op1 is not smaller than op2.
                tmp = data1[0] - data2[0];
                // asize is either 1 or 0 (0 iff abs(op1) == abs(op2)).
                rop._mp_size = sign1;
                if (mppp_unlikely(!tmp)) {
                    rop._mp_size = 0;
                }
                rdata[0] = tmp;
            } else {
                // NOTE: this has to be one, as data2[0] and data1[0] cannot be equal.
                rop._mp_size = sign2;
                rdata[0] = data2[0] - data1[0];
            }
        }
        return true;
    }
    // Optimization for two-limbs statics with no nails.
    // Small helper to compare two statics of equal asize 1 or 2.
    static int compare_limbs_2(const ::mp_limb_t *data1, const ::mp_limb_t *data2, mpz_size_t asize)
    {
        // NOTE: this requires no nail bits.
        assert(!GMP_NAIL_BITS);
        assert(asize == 1 || asize == 2);
        // Start comparing from the top.
        auto cmp_idx = asize - 1;
        if (data1[cmp_idx] != data2[cmp_idx]) {
            return data1[cmp_idx] > data2[cmp_idx] ? 1 : -1;
        }
        // The top limbs are equal, move down one limb.
        // If we are already at the bottom limb, it means the two numbers are equal.
        if (!cmp_idx) {
            return 0;
        }
        --cmp_idx;
        if (data1[cmp_idx] != data2[cmp_idx]) {
            return data1[cmp_idx] > data2[cmp_idx] ? 1 : -1;
        }
        return 0;
    }
    static bool static_add_impl(s_int &rop, const s_int &op1, const s_int &op2, mpz_size_t asize1, mpz_size_t asize2,
                                int sign1, int sign2, const std::integral_constant<int, 2> &)
    {
        auto rdata = &rop.m_limbs[0];
        auto data1 = &op1.m_limbs[0], data2 = &op2.m_limbs[0];
        if (sign1 == sign2) {
            // NOTE: this handles the case in which the numbers have the same sign, including 0 + 0.
            //
            // NOTE: this is the implementation for 2 limbs, even if potentially the operands have 1 limb.
            // The idea here is that it's better to do a few computations more rather than paying the branching
            // cost. 1-limb operands will have the upper limb set to zero from the zero-initialization of
            // the limbs of static ints.
            //
            // NOTE: the rop hi limb might spill over either from the addition of the hi limbs
            // of op1 and op2, or by the addition of carry coming over from the addition of
            // the lo limbs of op1 and op2.
            //
            // Add the hi and lo limbs.
            const ::mp_limb_t a = data1[0], b = data2[0], c = data1[1], d = data2[1];
            ::mp_limb_t lo, hi1, hi2;
            const ::mp_limb_t cy_lo = limb_add_overflow(a, b, &lo), cy_hi1 = limb_add_overflow(c, d, &hi1),
                              cy_hi2 = limb_add_overflow(hi1, cy_lo, &hi2);
            // The result will overflow if we have any carry relating to the high limb.
            if (mppp_unlikely(cy_hi1 || cy_hi2)) {
                return false;
            }
            // For the size compuation:
            // - if sign1 == 0, size is zero,
            // - if sign1 == +-1, then the size is either +-1 or +-2: asize is 2 if the result
            //   has a nonzero 2nd limb, otherwise asize is 1.
            rop._mp_size = sign1;
            if (hi2) {
                rop._mp_size = sign1 + sign1;
            }
            rdata[0] = lo;
            rdata[1] = hi2;
        } else {
            // When the signs differ, we need to implement addition as a subtraction.
            if (asize1 > asize2 || (asize1 == asize2 && compare_limbs_2(data1, data2, asize1) >= 0)) {
                // op1 is >= op2 in absolute value.
                const auto lo = data1[0] - data2[0];
                // If there's a borrow, then hi1 > hi2, otherwise we would have a negative result.
                assert(data1[0] >= data2[0] || data1[1] > data2[1]);
                // This can never wrap around, at most it goes to zero.
                const auto hi = data1[1] - data2[1] - static_cast<::mp_limb_t>(data1[0] < data2[0]);
                // asize can be 0, 1 or 2.
                rop._mp_size = 0;
                if (hi) {
                    rop._mp_size = sign1 + sign1;
                } else if (lo) {
                    rop._mp_size = sign1;
                }
                rdata[0] = lo;
                rdata[1] = hi;
            } else {
                // op2 is > op1 in absolute value.
                const auto lo = data2[0] - data1[0];
                assert(data2[0] >= data1[0] || data2[1] > data1[1]);
                const auto hi = data2[1] - data1[1] - static_cast<::mp_limb_t>(data2[0] < data1[0]);
                // asize can be 1 or 2, but not zero as we know abs(op1) != abs(op2).
                rop._mp_size = sign2;
                if (hi) {
                    rop._mp_size = sign2 + sign2;
                }
                rdata[0] = lo;
                rdata[1] = hi;
            }
        }
        return true;
    }
    template <bool AddOrSub>
    static bool static_addsub(s_int &rop, const s_int &op1, const s_int &op2)
    {
        // NOTE: effectively negate op2 if we are subtracting.
        mpz_size_t asize1 = op1._mp_size, asize2 = AddOrSub ? op2._mp_size : -op2._mp_size;
        int sign1 = asize1 != 0, sign2 = asize2 != 0;
        if (asize1 < 0) {
            asize1 = -asize1;
            sign1 = -1;
        }
        if (asize2 < 0) {
            asize2 = -asize2;
            sign2 = -1;
        }
        const bool retval = static_add_impl(rop, op1, op2, asize1, asize2, sign1, sign2, static_add_algo<s_int>{});
        if (static_add_algo<s_int>::value == 0 && retval) {
            // If we used the mpn functions and we actually wrote into rop, then
            // make sure we zero out the unused limbs.
            rop.zero_unused_limbs();
        }
        return retval;
    }

public:
    friend void add(mp_integer &rop, const mp_integer &op1, const mp_integer &op2)
    {
        const bool sr = rop.is_static(), s1 = op1.is_static(), s2 = op2.is_static();
        if (mppp_likely(sr && s1 && s2)) {
            // Optimise the case of all statics.
            if (mppp_likely(static_addsub<true>(rop.m_int.g_st(), op1.m_int.g_st(), op2.m_int.g_st()))) {
                return;
            }
        }
        if (sr) {
            rop.m_int.promote(SSize + 1u);
        }
        ::mpz_add(&rop.m_int.g_dy(), op1.get_mpz_view(), op2.get_mpz_view());
    }
    friend void sub(mp_integer &rop, const mp_integer &op1, const mp_integer &op2)
    {
        const bool sr = rop.is_static(), s1 = op1.is_static(), s2 = op2.is_static();
        if (mppp_likely(sr && s1 && s2)) {
            // Optimise the case of all statics.
            if (mppp_likely(static_addsub<false>(rop.m_int.g_st(), op1.m_int.g_st(), op2.m_int.g_st()))) {
                return;
            }
        }
        if (sr) {
            rop.m_int.promote(SSize + 1u);
        }
        ::mpz_sub(&rop.m_int.g_dy(), op1.get_mpz_view(), op2.get_mpz_view());
    }
    mp_integer &operator+=(const mp_integer &other)
    {
        add(*this, *this, other);
        return *this;
    }
    friend mp_integer operator+(const mp_integer &a, const mp_integer &b)
    {
        mp_integer retval;
        add(retval, a, b);
        return retval;
    }
    friend mp_integer operator+(const mp_integer &a, int n)
    {
        mp_integer retval;
        const mp_integer b{n};
        add(retval, a, b);
        return retval;
    }
    mp_integer &operator-=(const mp_integer &other)
    {
        sub(*this, *this, other);
        return *this;
    }
    friend mp_integer operator-(const mp_integer &a, const mp_integer &b)
    {
        mp_integer retval;
        sub(retval, a, b);
        return retval;
    }

private:
    // The double limb multiplication optimization is available in the following cases:
    // - no nails, we are on a 64bit MSVC build, the limb type has exactly 64 bits and GMP_NUMB_BITS is 64,
    // - no nails, we have a 128bit unsigned available, the limb type has exactly 64 bits and GMP_NUMB_BITS is 64,
    // - no nails, the smallest 64 bit unsigned type has exactly 64 bits, the limb type has exactly 32 bits and
    //   GMP_NUMB_BITS is 32.
    // NOTE: here we are checking that GMP_NUMB_BITS is the same as the limits ::digits property, which is probably
    // rather redundant on any conceivable architecture.
    using have_dlimb_mul = std::integral_constant<bool,
#if (defined(_MSC_VER) && defined(_WIN64) && (GMP_NUMB_BITS == 64)) || (defined(MPPP_UINT128) && (GMP_NUMB_BITS == 64))
                                                  !GMP_NAIL_BITS && std::numeric_limits<::mp_limb_t>::digits == 64
#elif GMP_NUMB_BITS == 32
                                                  !GMP_NAIL_BITS
                                                      && std::numeric_limits<std::uint_least64_t>::digits == 64
                                                      && std::numeric_limits<::mp_limb_t>::digits == 32
#else
                                                  false
#endif
                                                  >;
    template <typename SInt>
    using static_mul_algo = std::integral_constant<int, (SInt::s_size == 1 && have_dlimb_mul::value)
                                                            ? 1
                                                            : ((SInt::s_size == 2 && have_dlimb_mul::value) ? 2 : 0)>;
    // mpn implementation.
    // NOTE: this function (and the other overloads) returns 0 in case of success, otherwise it returns a hint
    // about the size in limbs of the result.
    static std::size_t static_mul_impl(s_int &rop, const s_int &op1, const s_int &op2, mpz_size_t asize1,
                                       mpz_size_t asize2, int sign1, int sign2, const std::integral_constant<int, 0> &)
    {
        // Handle zeroes.
        if (mppp_unlikely(!sign1 || !sign2)) {
            rop._mp_size = 0;
            return 0u;
        }
        auto rdata = &rop.m_limbs[0];
        auto data1 = &op1.m_limbs[0], data2 = &op2.m_limbs[0];
        const auto max_asize = std::size_t(asize1 + asize2);
        // Temporary storage, to be used if we cannot write into rop.
        std::array<::mp_limb_t, SSize * 2u> res;
        // We can write directly into rop if these conditions hold:
        // - rop does not overlap with op1 and op2,
        // - SSize is large enough to hold the max size of the result.
        ::mp_limb_t *MPPP_RESTRICT res_data
            = (rdata != data1 && rdata != data2 && max_asize <= SSize) ? rdata : &res[0];
        // Proceed to the multiplication.
        ::mp_limb_t hi;
        if (asize2 == 1) {
            // NOTE: the 1-limb versions do not write the hi limb, we have to write it ourselves.
            hi = ::mpn_mul_1(res_data, data1, asize1, data2[0]);
            res_data[asize1] = hi;
        } else if (asize1 == 1) {
            hi = ::mpn_mul_1(res_data, data2, asize2, data1[0]);
            res_data[asize2] = hi;
        } else if (asize1 == asize2) {
            ::mpn_mul_n(res_data, data1, data2, asize1);
            hi = res_data[2 * asize1 - 1];
        } else if (asize1 >= asize2) {
            hi = ::mpn_mul(res_data, data1, asize1, data2, asize2);
        } else {
            hi = ::mpn_mul(res_data, data2, asize2, data1, asize1);
        }
        // The actual size.
        const std::size_t asize = max_asize - unsigned(hi == 0u);
        if (res_data == rdata) {
            // If we wrote directly into rop, it means that we had enough storage in it to begin with.
            rop._mp_size = mpz_size_t(asize);
            if (sign1 != sign2) {
                rop._mp_size = -rop._mp_size;
            }
            return 0u;
        }
        // If we used the temporary storage, we need to check if we can write into rop.
        if (asize > SSize) {
            // Not enough space, return the size.
            return asize;
        }
        // Enough space, set size and copy limbs.
        rop._mp_size = mpz_size_t(asize);
        if (sign1 != sign2) {
            rop._mp_size = -rop._mp_size;
        }
        copy_limbs_no(res_data, res_data + asize, rdata);
        return 0u;
    }
#if defined(_MSC_VER) && defined(_WIN64) && (GMP_NUMB_BITS == 64) && !GMP_NAIL_BITS
    static ::mp_limb_t dlimb_mul(::mp_limb_t op1, ::mp_limb_t op2, ::mp_limb_t *hi)
    {
        return ::UnsignedMultiply128(op1, op2, hi);
    }
#elif defined(MPPP_UINT128) && (GMP_NUMB_BITS == 64) && !GMP_NAIL_BITS
    static ::mp_limb_t dlimb_mul(::mp_limb_t op1, ::mp_limb_t op2, ::mp_limb_t *hi)
    {
        using dlimb_t = MPPP_UINT128;
        const dlimb_t res = dlimb_t(op1) * op2;
        *hi = static_cast<::mp_limb_t>(res >> 64);
        return static_cast<::mp_limb_t>(res);
    }
#elif GMP_NUMB_BITS == 32 && !GMP_NAIL_BITS
    static ::mp_limb_t dlimb_mul(::mp_limb_t op1, ::mp_limb_t op2, ::mp_limb_t *hi)
    {
        using dlimb_t = std::uint_least64_t;
        const dlimb_t res = dlimb_t(op1) * op2;
        *hi = static_cast<::mp_limb_t>(res >> 32);
        return static_cast<::mp_limb_t>(res);
    }
#endif
    // 1-limb optimization via dlimb.
    static std::size_t static_mul_impl(s_int &rop, const s_int &op1, const s_int &op2, mpz_size_t, mpz_size_t,
                                       int sign1, int sign2, const std::integral_constant<int, 1> &)
    {
        ::mp_limb_t hi;
        const ::mp_limb_t lo = dlimb_mul(op1.m_limbs[0], op2.m_limbs[0], &hi);
        if (mppp_unlikely(hi)) {
            return 2u;
        }
        const mpz_size_t asize = (lo != 0u);
        rop._mp_size = asize;
        if (sign1 != sign2) {
            rop._mp_size = -rop._mp_size;
        }
        rop.m_limbs[0] = lo;
        return 0u;
    }
    // 2-limb optimization via dlimb.
    static std::size_t static_mul_impl(s_int &rop, const s_int &op1, const s_int &op2, mpz_size_t asize1,
                                       mpz_size_t asize2, int sign1, int sign2, const std::integral_constant<int, 2> &)
    {
        if (mppp_unlikely(!asize1 || !asize2)) {
            // Handle zeroes.
            rop._mp_size = 0;
            rop.m_limbs[0] = 0u;
            rop.m_limbs[1] = 0u;
            return 0u;
        }
        if (asize1 == 1 && asize2 == 1) {
            rop.m_limbs[0] = dlimb_mul(op1.m_limbs[0], op2.m_limbs[0], &rop.m_limbs[1]);
            rop._mp_size = static_cast<mpz_size_t>((asize1 + asize2) - mpz_size_t(rop.m_limbs[1] == 0u));
            if (sign1 != sign2) {
                rop._mp_size = -rop._mp_size;
            }
            return 0u;
        }
        if (asize1 != asize2) {
            // The only possibility of success is 2limbs x 1limb.
            //
            //             b      a X
            //                    c
            // --------------------
            // tmp[2] tmp[1] tmp[0]
            //
            ::mp_limb_t a = op1.m_limbs[0], b = op1.m_limbs[1], c = op2.m_limbs[0];
            if (asize1 < asize2) {
                // Switch them around if needed.
                a = op2.m_limbs[0], b = op2.m_limbs[1], c = op1.m_limbs[0];
            }
            ::mp_limb_t ca_lo, ca_hi, cb_lo, cb_hi, tmp0, tmp1, tmp2;
            ca_lo = dlimb_mul(c, a, &ca_hi);
            cb_lo = dlimb_mul(c, b, &cb_hi);
            tmp0 = ca_lo;
            const auto cy = limb_add_overflow(cb_lo, ca_hi, &tmp1);
            tmp2 = cb_hi + cy;
            // Now determine the size. asize must be at least 2.
            const mpz_size_t asize = 2 + mpz_size_t(tmp2 != 0u);
            if (asize == 2) {
                // Size is good, write out the result.
                rop._mp_size = asize;
                if (sign1 != sign2) {
                    rop._mp_size = -rop._mp_size;
                }
                rop.m_limbs[0] = tmp0;
                rop.m_limbs[1] = tmp1;
                return 0u;
            }
        }
        // Return 4 as a size hint. Real size could be 3, but GMP will require 4 limbs
        // of storage to perform the operation anyway.
        return 4u;
    }
    static std::size_t static_mul(s_int &rop, const s_int &op1, const s_int &op2)
    {
        // Cache a few quantities, detect signs.
        mpz_size_t asize1 = op1._mp_size, asize2 = op2._mp_size;
        int sign1 = asize1 != 0, sign2 = asize2 != 0;
        if (asize1 < 0) {
            asize1 = -asize1;
            sign1 = -1;
        }
        if (asize2 < 0) {
            asize2 = -asize2;
            sign2 = -1;
        }
        const std::size_t retval
            = static_mul_impl(rop, op1, op2, asize1, asize2, sign1, sign2, static_mul_algo<s_int>{});
        if (static_mul_algo<s_int>::value == 0 && retval == 0u) {
            rop.zero_unused_limbs();
        }
        return retval;
    }

public:
    friend void mul(mp_integer &rop, const mp_integer &op1, const mp_integer &op2)
    {
        const bool sr = rop.is_static(), s1 = op1.is_static(), s2 = op2.is_static();
        std::size_t size_hint = 0u;
        if (mppp_likely(sr && s1 && s2)) {
            size_hint = static_mul(rop.m_int.g_st(), op1.m_int.g_st(), op2.m_int.g_st());
            if (mppp_likely(size_hint == 0u)) {
                return;
            }
        }
        if (sr) {
            // We use the size hint from the static_mul if available, otherwise a normal promotion will take place.
            // NOTE: here the best way of proceeding would be to calculate the max size of the result based on
            // the op1/op2 sizes, but for whatever reason this computation has disastrous performance consequences
            // on micro-benchmarks. We need to understand if that's the case in real-world scenarios as well, and
            // revisit this.
            rop.m_int.promote(size_hint);
        }
        ::mpz_mul(&rop.m_int.g_dy(), op1.get_mpz_view(), op2.get_mpz_view());
    }
    mp_integer &operator*=(const mp_integer &other)
    {
        mul(*this, *this, other);
        return *this;
    }
    friend mp_integer operator*(const mp_integer &a, const mp_integer &b)
    {
        mp_integer retval;
        mul(retval, a, b);
        return retval;
    }
    friend mp_integer operator*(const mp_integer &a, int n)
    {
        mp_integer retval;
        const mp_integer b{n};
        mul(retval, a, b);
        return retval;
    }
    int sign() const
    {
        if (m_int.m_st._mp_size != 0) {
            return m_int.m_st._mp_size > 0 ? 1 : -1;
        } else {
            return 0;
        }
    }
    friend bool operator==(const mp_integer &a, const mp_integer &b)
    {
        const bool sa = a.is_static(), sb = b.is_static();
        if (sa && sb) {
            return a.m_int.g_st()._mp_size == b.m_int.g_st()._mp_size
                   && std::equal(a.m_int.g_st().m_limbs.begin(),
                                 a.m_int.g_st().m_limbs.begin() + a.m_int.g_st().abs_size(),
                                 b.m_int.g_st().m_limbs.begin(), [](const ::mp_limb_t &l1, const ::mp_limb_t &l2) {
                                     return (l1 & GMP_NUMB_BITS) == (l2 & GMP_NUMB_BITS);
                                 });
        } else {
            return ::mpz_cmp(a.get_mpz_view(), b.get_mpz_view()) == 0;
        }
    }
    friend bool operator!=(const mp_integer &a, const mp_integer &b)
    {
        return !(a == b);
    }

private:
    // Selection of the algorithm for addmul: if optimised algorithms exist for both add and mul, then use the optimised
    // addmul algos. Otherwise, use the mpn one.
    template <typename SInt>
    using static_addmul_algo
        = std::integral_constant<int,
                                 (static_add_algo<SInt>::value == 2 && static_mul_algo<SInt>::value == 2)
                                     ? 2
                                     : ((static_add_algo<SInt>::value == 1 && static_mul_algo<SInt>::value == 1) ? 1
                                                                                                                 : 0)>;
    // NOTE: same return value as mul: 0 for success, otherwise a hint for the size of the result.
    static std::size_t static_addmul_impl(s_int &rop, const s_int &op1, const s_int &op2, mpz_size_t asizer,
                                          mpz_size_t asize1, mpz_size_t asize2, int signr, int sign1, int sign2,
                                          const std::integral_constant<int, 0> &)
    {
        // First try to do the static prod, if it does not work it's a failure.
        s_int prod;
        if (mppp_unlikely(
                static_mul_impl(prod, op1, op2, asize1, asize2, sign1, sign2, std::integral_constant<int, 0>{}))) {
            // This is the maximum size a static addmul can have.
            return SSize * 2u + 1u;
        }
        // Determine sign and asize of the product.
        mpz_size_t asize_prod = prod._mp_size;
        int sign_prod = (asize_prod != 0);
        if (asize_prod < 0) {
            asize_prod = -asize_prod;
            sign_prod = -1;
        }
        // Try to do the add.
        if (mppp_unlikely(!static_add_impl(rop, rop, prod, asizer, asize_prod, signr, sign_prod,
                                           std::integral_constant<int, 0>{}))) {
            return SSize + 1u;
        }
        return 0u;
    }
    static std::size_t static_addmul_impl(s_int &rop, const s_int &op1, const s_int &op2, mpz_size_t, mpz_size_t,
                                          mpz_size_t, int signr, int sign1, int sign2,
                                          const std::integral_constant<int, 1> &)
    {
        // First we do op1 * op2.
        ::mp_limb_t tmp;
        const ::mp_limb_t prod = dlimb_mul(op1.m_limbs[0], op2.m_limbs[0], &tmp);
        if (mppp_unlikely(tmp)) {
            return 3u;
        }
        // Determine the sign of the product: 0, 1 or -1.
        int sign_prod = prod != 0u;
        if (sign1 != sign2) {
            sign_prod = -sign_prod;
        }
        // Now add/sub.
        if (signr == sign_prod) {
            // Same sign, do addition with overflow check.
            if (mppp_unlikely(limb_add_overflow(rop.m_limbs[0], prod, &tmp))) {
                return 2u;
            }
            // Assign the output.
            rop._mp_size = signr;
            rop.m_limbs[0] = tmp;
        } else {
            // When the signs differ, we need to implement addition as a subtraction.
            if (rop.m_limbs[0] >= prod) {
                // abs(rop) >= abs(prod).
                tmp = rop.m_limbs[0] - prod;
                // asize is either 1 or 0 (0 iff rop == prod).
                rop._mp_size = signr;
                if (mppp_unlikely(!tmp)) {
                    rop._mp_size = 0;
                }
                rop.m_limbs[0] = tmp;
            } else {
                // NOTE: this cannot be zero, as rop and prod cannot be equal.
                rop._mp_size = sign_prod;
                rop.m_limbs[0] = prod - rop.m_limbs[0];
            }
        }
        return 0u;
    }
    static std::size_t static_addmul_impl(s_int &rop, const s_int &op1, const s_int &op2, mpz_size_t asizer,
                                          mpz_size_t asize1, mpz_size_t asize2, int signr, int sign1, int sign2,
                                          const std::integral_constant<int, 2> &)
    {
        if (mppp_unlikely(!asize1 || !asize2)) {
            // If op1 or op2 are zero, rop will be unchanged.
            return 0u;
        }
        // Handle op1 * op2.
        std::array<::mp_limb_t, 2> prod;
        int sign_prod = 1;
        if (sign1 != sign2) {
            sign_prod = -1;
        }
        mpz_size_t asize_prod;
        if (asize1 == 1 && asize2 == 1) {
            prod[0] = dlimb_mul(op1.m_limbs[0], op2.m_limbs[0], &prod[1]);
            asize_prod = (asize1 + asize2) - mpz_size_t(prod[1] == 0u);
        } else {
            // The only possibility of success is 2limbs x 1limb.
            //
            //       b       a X
            //               c
            // ---------------
            // prod[1] prod[0]
            //
            if (mppp_unlikely(asize1 == asize2)) {
                // This means that both sizes are 2.
                return 5u;
            }
            ::mp_limb_t a = op1.m_limbs[0], b = op1.m_limbs[1], c = op2.m_limbs[0];
            if (asize1 < asize2) {
                // Switch them around if needed.
                a = op2.m_limbs[0], b = op2.m_limbs[1], c = op1.m_limbs[0];
            }
            ::mp_limb_t ca_hi, cb_hi;
            // These are the three operations that build the result, two mults, one add.
            prod[0] = dlimb_mul(c, a, &ca_hi);
            prod[1] = dlimb_mul(c, b, &cb_hi);
            // NOTE: we can use this with overlapping arguments because the first two
            // are passed as copies.
            const auto cy = limb_add_overflow(prod[1], ca_hi, &prod[1]);
            // Check if the third limb exists, if so we return failure.
            if (mppp_unlikely(cb_hi || cy)) {
                return 4u;
            }
            asize_prod = 2;
        }
        // Proceed to the addition.
        if (signr == sign_prod) {
            // Add the hi and lo limbs.
            ::mp_limb_t lo, hi1, hi2;
            const ::mp_limb_t cy_lo = limb_add_overflow(rop.m_limbs[0], prod[0], &lo),
                              cy_hi1 = limb_add_overflow(rop.m_limbs[1], prod[1], &hi1),
                              cy_hi2 = limb_add_overflow(hi1, cy_lo, &hi2);
            // The result will overflow if we have any carry relating to the high limb.
            if (mppp_unlikely(cy_hi1 || cy_hi2)) {
                return 3u;
            }
            // For the size compuation:
            // - cannot be zero, as prod is not zero,
            // - if signr == +-1, then the size is either +-1 or +-2: asize is 2 if the result
            //   has a nonzero 2nd limb, otherwise asize is 1.
            rop._mp_size = signr;
            if (hi2) {
                rop._mp_size = signr + signr;
            }
            rop.m_limbs[0] = lo;
            rop.m_limbs[1] = hi2;
        } else {
            // When the signs differ, we need to implement addition as a subtraction.
            if (asizer > asize_prod
                || (asizer == asize_prod && compare_limbs_2(&rop.m_limbs[0], &prod[0], asizer) >= 0)) {
                // rop >= prod in absolute value.
                const auto lo = rop.m_limbs[0] - prod[0];
                // If there's a borrow, then hi1 > hi2, otherwise we would have a negative result.
                assert(rop.m_limbs[0] >= prod[0] || rop.m_limbs[1] > prod[1]);
                // This can never wrap around, at most it goes to zero.
                const auto hi = rop.m_limbs[1] - prod[1] - static_cast<::mp_limb_t>(rop.m_limbs[0] < prod[0]);
                // asize can be 0, 1 or 2.
                rop._mp_size = 0;
                if (hi) {
                    rop._mp_size = signr + signr;
                } else if (lo) {
                    rop._mp_size = signr;
                }
                rop.m_limbs[0] = lo;
                rop.m_limbs[1] = hi;
            } else {
                // prod > rop in absolute value.
                const auto lo = prod[0] - rop.m_limbs[0];
                assert(prod[0] >= rop.m_limbs[0] || prod[1] > rop.m_limbs[1]);
                const auto hi = prod[1] - rop.m_limbs[1] - static_cast<::mp_limb_t>(prod[0] < rop.m_limbs[0]);
                // asize can be 1 or 2, but not zero as we know abs(prod) != abs(rop).
                rop._mp_size = sign_prod;
                if (hi) {
                    rop._mp_size = sign_prod + sign_prod;
                }
                rop.m_limbs[0] = lo;
                rop.m_limbs[1] = hi;
            }
        }
        return 0u;
    }
    static std::size_t static_addmul(s_int &rop, const s_int &op1, const s_int &op2)
    {
        mpz_size_t asizer = rop._mp_size, asize1 = op1._mp_size, asize2 = op2._mp_size;
        int signr = asizer != 0, sign1 = asize1 != 0, sign2 = asize2 != 0;
        if (asizer < 0) {
            asizer = -asizer;
            signr = -1;
        }
        if (asize1 < 0) {
            asize1 = -asize1;
            sign1 = -1;
        }
        if (asize2 < 0) {
            asize2 = -asize2;
            sign2 = -1;
        }
        const std::size_t retval = static_addmul_impl(rop, op1, op2, asizer, asize1, asize2, signr, sign1, sign2,
                                                      static_addmul_algo<s_int>{});
        if (static_addmul_algo<s_int>::value == 0 && retval == 0u) {
            rop.zero_unused_limbs();
        }
        return retval;
    }

public:
    friend void addmul(mp_integer &rop, const mp_integer &op1, const mp_integer &op2)
    {
        const bool sr = rop.is_static(), s1 = op1.is_static(), s2 = op2.is_static();
        std::size_t size_hint = 0u;
        if (mppp_likely(sr && s1 && s2)) {
            size_hint = static_addmul(rop.m_int.g_st(), op1.m_int.g_st(), op2.m_int.g_st());
            if (mppp_likely(size_hint == 0u)) {
                return;
            }
        }
        if (sr) {
            rop.m_int.promote(size_hint);
        }
        ::mpz_addmul(&rop.m_int.g_dy(), op1.get_mpz_view(), op2.get_mpz_view());
    }

private:
    // Detect the presence of dual-limb division. This is currently possible only if:
    // - we are on a 32bit build (with the usual constraints that the types have exactly 32/64 bits and no nails),
    // - we are on a 64bit build and we have the 128bit int type available (plus usual constraints).
    // MSVC currently does not provide any primitive for 128bit division.
    using have_dlimb_div = std::integral_constant<bool,
#if defined(MPPP_UINT128) && (GMP_NUMB_BITS == 64)
                                                  !GMP_NAIL_BITS && std::numeric_limits<::mp_limb_t>::digits == 64
#elif GMP_NUMB_BITS == 32
                                                  !GMP_NAIL_BITS
                                                      && std::numeric_limits<std::uint_least64_t>::digits == 64
                                                      && std::numeric_limits<::mp_limb_t>::digits == 32
#else
                                                  false
#endif
                                                  >;
    // Selection of algorithm for static division:
    // - for 1 limb, we can always do static division,
    // - for 2 limbs, we need the dual limb division if avaiable,
    // - otherwise we just use the mpn functions.
    template <typename SInt>
    using static_div_algo
        = std::integral_constant<int, SInt::s_size == 1 ? 1 : ((SInt::s_size == 2 && have_dlimb_div::value) ? 2 : 0)>;
    // mpn implementation.
    static void static_div_impl(s_int &q, s_int &r, const s_int &op1, const s_int &op2, mpz_size_t asize1,
                                mpz_size_t asize2, int sign1, int sign2, const std::integral_constant<int, 0> &)
    {
        // First we check if the divisor is larger than the dividend (in abs limb size), as the mpn function requires
        // asize1 >= asize2.
        if (asize2 > asize1) {
            // Copy op1 into the remainder.
            r = op1;
            // Zero out q.
            q._mp_size = 0;
            return;
        }
        // We need to take care of potentially overlapping arguments. We know that q and r are distinct, but op1
        // could overlap with q or r, and op2 could overlap with op1, q or r.
        std::array<::mp_limb_t, SSize> op1_alt, op2_alt;
        const ::mp_limb_t *data1 = op1.m_limbs.data();
        const ::mp_limb_t *data2 = op2.m_limbs.data();
        if (&op1 == &q || &op1 == &r) {
            copy_limbs_no(data1, data1 + asize1, op1_alt.data());
            data1 = op1_alt.data();
        }
        if (&op2 == &q || &op2 == &r || &op1 == &op2) {
            copy_limbs_no(data2, data2 + asize2, op2_alt.data());
            data2 = op2_alt.data();
        }
        // Small helper function to verify that all pointers are distinct. Used exclusively for debugging purposes.
        auto distinct_op = [&q, &r, data1, data2]() -> bool {
            const ::mp_limb_t *ptrs[] = {q.m_limbs.data(), r.m_limbs.data(), data1, data2};
            std::sort(ptrs, ptrs + 4, std::less<const ::mp_limb_t *>());
            return std::unique(ptrs, ptrs + 4) == (ptrs + 4);
        };
        (void)distinct_op;
        assert(distinct_op());
        // Proceed to the division.
        if (asize2 == 1) {
            // Optimization when the divisor has 1 limb.
            r.m_limbs[0]
                = ::mpn_divrem_1(q.m_limbs.data(), ::mp_size_t(0), data1, static_cast<::mp_size_t>(asize1), data2[0]);
        } else {
            // General implementation.
            ::mpn_tdiv_qr(q.m_limbs.data(), r.m_limbs.data(), ::mp_size_t(0), data1, static_cast<::mp_size_t>(asize1),
                          data2, static_cast<::mp_size_t>(asize2));
        }
        // Complete the quotient: compute size and sign.
        q._mp_size = asize1 - asize2 + 1;
        while (q._mp_size && !(q.m_limbs[static_cast<std::size_t>(q._mp_size - 1)] & GMP_NUMB_MASK)) {
            --q._mp_size;
        }
        if (sign1 != sign2) {
            q._mp_size = -q._mp_size;
        }
        // Complete the remainder.
        r._mp_size = asize2;
        while (r._mp_size && !(r.m_limbs[static_cast<std::size_t>(r._mp_size - 1)] & GMP_NUMB_MASK)) {
            --r._mp_size;
        }
        if (sign1 == -1) {
            r._mp_size = -r._mp_size;
        }
    }
    // 1-limb optimisation.
    static void static_div_impl(s_int &q, s_int &r, const s_int &op1, const s_int &op2, mpz_size_t, mpz_size_t,
                                int sign1, int sign2, const std::integral_constant<int, 1> &)
    {
        // NOTE: here we have to use GMP_NUMB_MASK because if s_size is 1 this implementation is *always*
        // called, even if we have nail bits (whereas the optimisation for other operations currently kicks in
        // only without nail bits). Thus, we need to discard from m_limbs[0] the nail bits before doing the division.
        const ::mp_limb_t q_ = (op1.m_limbs[0] & GMP_NUMB_MASK) / (op2.m_limbs[0] & GMP_NUMB_MASK),
                          r_ = (op1.m_limbs[0] & GMP_NUMB_MASK) % (op2.m_limbs[0] & GMP_NUMB_MASK);
        // Write q.
        q._mp_size = (q_ != 0u);
        if (sign1 != sign2) {
            q._mp_size = -q._mp_size;
        }
        // NOTE: there should be no need here to mask.
        q.m_limbs[0] = q_;
        // Write r.
        r._mp_size = (r_ != 0u);
        // Following C++11, the sign of r is the sign of op1:
        // http://stackoverflow.com/questions/13100711/operator-modulo-change-in-c-11
        if (sign1 == -1) {
            r._mp_size = -r._mp_size;
        }
        r.m_limbs[0] = r_;
    }
#if defined(MPPP_UINT128) && (GMP_NUMB_BITS == 64) && !GMP_NAIL_BITS
    static void dlimb_div(::mp_limb_t op11, ::mp_limb_t op12, ::mp_limb_t op21, ::mp_limb_t op22,
                          ::mp_limb_t *MPPP_RESTRICT q1, ::mp_limb_t *MPPP_RESTRICT q2, ::mp_limb_t *MPPP_RESTRICT r1,
                          ::mp_limb_t *MPPP_RESTRICT r2)
    {
        using dlimb_t = MPPP_UINT128;
        const auto op1 = op11 + (dlimb_t(op12) << 64);
        const auto op2 = op21 + (dlimb_t(op22) << 64);
        const auto q = op1 / op2, r = op1 % op2;
        *q1 = static_cast<::mp_limb_t>(q & ::mp_limb_t(-1));
        *q2 = static_cast<::mp_limb_t>(q >> 64);
        *r1 = static_cast<::mp_limb_t>(r & ::mp_limb_t(-1));
        *r2 = static_cast<::mp_limb_t>(r >> 64);
    }
#elif GMP_NUMB_BITS == 32 && !GMP_NAIL_BITS
    static void dlimb_div(::mp_limb_t op11, ::mp_limb_t op12, ::mp_limb_t op21, ::mp_limb_t op22,
                          ::mp_limb_t *MPPP_RESTRICT q1, ::mp_limb_t *MPPP_RESTRICT q2, ::mp_limb_t *MPPP_RESTRICT r1,
                          ::mp_limb_t *MPPP_RESTRICT r2)
    {
        using dlimb_t = std::uint_least64_t;
        const auto op1 = op11 + (dlimb_t(op12) << 32);
        const auto op2 = op21 + (dlimb_t(op22) << 32);
        const auto q = op1 / op2, r = op1 % op2;
        *q1 = static_cast<::mp_limb_t>(q & ::mp_limb_t(-1));
        *q2 = static_cast<::mp_limb_t>(q >> 32);
        *r1 = static_cast<::mp_limb_t>(r & ::mp_limb_t(-1));
        *r2 = static_cast<::mp_limb_t>(r >> 32);
    }
#endif
    // 2-limbs optimisation.
    static void static_div_impl(s_int &q, s_int &r, const s_int &op1, const s_int &op2, mpz_size_t asize1,
                                mpz_size_t asize2, int sign1, int sign2, const std::integral_constant<int, 2> &)
    {
        if (asize1 < 2 && asize2 < 2) {
            // NOTE: testing indicates that it pays off to optimize the case in which the operands have
            // fewer than 2 limbs. This a slightly modified version of the 1-limb division from above,
            // without the need to mask as this function is called only if there are no nail bits.
            const ::mp_limb_t q_ = op1.m_limbs[0] / op2.m_limbs[0], r_ = op1.m_limbs[0] % op2.m_limbs[0];
            q._mp_size = (q_ != 0u);
            if (sign1 != sign2) {
                q._mp_size = -q._mp_size;
            }
            q.m_limbs[0] = q_;
            q.m_limbs[1] = 0u;
            r._mp_size = (r_ != 0u);
            if (sign1 == -1) {
                r._mp_size = -r._mp_size;
            }
            r.m_limbs[0] = r_;
            r.m_limbs[1] = 0u;
            return;
        }
        // Perform the division.
        ::mp_limb_t q1, q2, r1, r2;
        dlimb_div(op1.m_limbs[0], op1.m_limbs[1], op2.m_limbs[0], op2.m_limbs[1], &q1, &q2, &r1, &r2);
        // Write out.
        q._mp_size = q2 ? 2 : (q1 ? 1 : 0);
        if (sign1 != sign2) {
            q._mp_size = -q._mp_size;
        }
        q.m_limbs[0] = q1;
        q.m_limbs[1] = q2;
        r._mp_size = r2 ? 2 : (r1 ? 1 : 0);
        if (sign1 == -1) {
            r._mp_size = -r._mp_size;
        }
        r.m_limbs[0] = r1;
        r.m_limbs[1] = r2;
    }
    static void static_div(s_int &q, s_int &r, const s_int &op1, const s_int &op2)
    {
        mpz_size_t asize1 = op1._mp_size, asize2 = op2._mp_size;
        int sign1 = asize1 != 0, sign2 = asize2 != 0;
        if (asize1 < 0) {
            asize1 = -asize1;
            sign1 = -1;
        }
        if (asize2 < 0) {
            asize2 = -asize2;
            sign2 = -1;
        }
        static_div_impl(q, r, op1, op2, asize1, asize2, sign1, sign2, static_div_algo<s_int>{});
        if (static_div_algo<s_int>::value == 0) {
            q.zero_unused_limbs();
            r.zero_unused_limbs();
        }
    }

public:
    friend void div(mp_integer &q, mp_integer &r, const mp_integer &op1, const mp_integer &op2)
    {
        if (mppp_unlikely(&q == &r)) {
            throw std::invalid_argument("When performing a division with remainder, the quotient 'q' and the "
                                        "remainder 'r' must be distinct objects");
        }
        if (mppp_unlikely(op2.sign() == 0)) {
            throw zero_division_error("Integer division by zero");
        }
        const bool sq = q.is_static(), sr = r.is_static(), s1 = op1.is_static(), s2 = op2.is_static();
        if (mppp_likely(sq && sr && s1 && s2)) {
            static_div(q.m_int.g_st(), r.m_int.g_st(), op1.m_int.g_st(), op2.m_int.g_st());
            // Division can never fail.
            return;
        }
        if (sq) {
            q.m_int.promote();
        }
        if (sr) {
            r.m_int.promote();
        }
        ::mpz_tdiv_qr(&q.m_int.g_dy(), &r.m_int.g_dy(), op1.get_mpz_view(), op2.get_mpz_view());
    }

private:
    // Selection of the algorithm for static mul_2exp.
    template <typename SInt>
    using static_mul_2exp_algo = std::integral_constant<int, SInt::s_size == 1 ? 1 : (SInt::s_size == 2 ? 2 : 0)>;
    // mpn implementation.
    static bool static_mul_2exp_impl(s_int &rop, const s_int &n, ::mp_bitcnt_t s,
                                     const std::integral_constant<int, 0> &)
    {
        mpz_size_t asize = n._mp_size;
        if (s == 0u || asize == 0) {
            // If shift is zero, or the operand is zero, nothing to do and just return success.
            return true;
        }
        // Finish setting up asize and sign.
        int sign = asize != 0;
        if (asize < 0) {
            asize = -asize;
            sign = -1;
        }
        // ls: number of entire limbs shifted.
        // rs: effective shift that will be passed to the mpn function.
        const auto ls = s / GMP_NUMB_BITS, rs = s % GMP_NUMB_BITS;
        // At the very minimum, the new asize will be the old asize
        // plus ls.
        const mpz_size_t new_asize = asize + static_cast<mpz_size_t>(ls);
        if (std::size_t(new_asize) < SSize) {
            // In this case the operation will always succeed, and we can write directly into rop.
            ::mp_limb_t ret = 0u;
            if (rs) {
                // Perform the shift via the mpn function, if we are effectively shifting at least 1 bit.
                // Overlapping is fine, as it is guaranteed that rop.m_limbs.data() + ls >= n.m_limbs.data().
                ret = ::mpn_lshift(rop.m_limbs.data() + ls, n.m_limbs.data(), asize, unsigned(rs));
                // Write bits spilling out.
                rop.m_limbs[std::size_t(new_asize)] = ret;
            } else {
                // Otherwise, just copy over (the mpn function requires the shift to be at least 1).
                // These ranges are potentially overlapping.
                copy_limbs(n.m_limbs.data(), n.m_limbs.data() + asize, rop.m_limbs.data() + ls);
            }
            // Zero the lower limbs vacated by the shift.
            std::fill(rop.m_limbs.data(), rop.m_limbs.data() + ls, ::mp_limb_t(0));
            // Set the size.
            rop._mp_size = new_asize + (ret != 0u);
            if (sign == -1) {
                rop._mp_size = -rop._mp_size;
            }
            return true;
        }
        if (std::size_t(new_asize) == SSize) {
            if (rs) {
                // In this case the operation may fail, so we need to write to temporary storage.
                std::array<::mp_limb_t, SSize> tmp;
                if (::mpn_lshift(tmp.data(), n.m_limbs.data(), asize, unsigned(rs))) {
                    return false;
                }
                // The shift was successful without spill over, copy the content from the tmp
                // storage into rop.
                copy_limbs_no(tmp.data(), tmp.data() + asize, rop.m_limbs.data() + ls);
            } else {
                // If we shifted by a multiple of the limb size, then we can write directly to rop.
                copy_limbs(n.m_limbs.data(), n.m_limbs.data() + asize, rop.m_limbs.data() + ls);
            }
            // Zero the lower limbs vacated by the shift.
            std::fill(rop.m_limbs.data(), rop.m_limbs.data() + ls, ::mp_limb_t(0));
            // Set the size.
            rop._mp_size = new_asize;
            if (sign == -1) {
                rop._mp_size = -rop._mp_size;
            }
            return true;
        }
        // Shift is too much, the size will overflow the static limit.
        return false;
    }
    // 1-limb optimisation.
    static bool static_mul_2exp_impl(s_int &rop, const s_int &n, ::mp_bitcnt_t s,
                                     const std::integral_constant<int, 1> &)
    {
        const ::mp_limb_t l = n.m_limbs[0] & GMP_NUMB_MASK;
        if (s == 0u || l == 0u) {
            // If shift is zero, or the operand is zero, nothing to do and just return success.
            return true;
        }
        if (mppp_unlikely(s >= GMP_NUMB_BITS || (l >> (GMP_NUMB_BITS - s)))) {
            // The two conditions:
            // - if the shift is >= number of data bits, the operation will certainly
            //   fail (as the operand is nonzero);
            // - shifting by s would overflow  the single limb.
            // NOTE: s is at least 1, so in the right shift above we never risk UB due to too much shift.
            return false;
        }
        // Write out.
        rop.m_limbs[0] = l << s;
        // Size is the same as the input value.
        rop._mp_size = n._mp_size;
        return true;
    }
    // 2-limb optimisation.
    static bool static_mul_2exp_impl(s_int &rop, const s_int &n, ::mp_bitcnt_t s,
                                     const std::integral_constant<int, 2> &)
    {
        mpz_size_t asize = n._mp_size;
        if (s == 0u || asize == 0) {
            return true;
        }
        int sign = asize != 0;
        if (asize < 0) {
            asize = -asize;
            sign = -1;
        }
        // Too much shift, this can never work on a nonzero value.
        static_assert(GMP_NUMB_BITS
                          < std::numeric_limits<typename std::decay<decltype(GMP_NUMB_BITS)>::type>::max() / 2u,
                      "Overflow error.");
        if (mppp_unlikely(s >= 2u * GMP_NUMB_BITS)) {
            return false;
        }
        if (s == GMP_NUMB_BITS) {
            // This case can be dealt with moving lo into hi, but only if asize is 1.
            if (mppp_unlikely(asize == 2)) {
                // asize is 2, shift is too much.
                return false;
            }
            rop.m_limbs[1u] = rop.m_limbs[0u];
            rop.m_limbs[0u] = 0u;
            // The size has to be 2.
            rop._mp_size = 2;
            if (sign == -1) {
                rop._mp_size = -2;
            }
            return true;
        }
        // Temp hi lo limbs to store the result that will eventually go into rop.
        ::mp_limb_t lo = n.m_limbs[0u], hi = n.m_limbs[1u];
        if (s > GMP_NUMB_BITS) {
            if (mppp_unlikely(asize == 2)) {
                return false;
            }
            // Move lo to hi and set lo to zero.
            hi = n.m_limbs[0u];
            lo = 0u;
            // Update the shift.
            s = static_cast<::mp_bitcnt_t>(s - GMP_NUMB_BITS);
        }
        // Check that hi will not be shifted too much. Note that
        // here and below s can never be zero, so we never shift too much.
        assert(s > 0u && s < GMP_NUMB_BITS);
        if (mppp_unlikely((hi & GMP_NUMB_MASK) >> (GMP_NUMB_BITS - s))) {
            return false;
        }
        // Shift hi and lo. hi gets the carry over from lo.
        hi = ((hi & GMP_NUMB_MASK) << s) + ((lo & GMP_NUMB_MASK) >> (GMP_NUMB_BITS - s));
        // NOTE: here the result needs to be masked as well as the shift could
        // end up writing in nail bits.
        lo = ((lo & GMP_NUMB_MASK) << s) & GMP_NUMB_MASK;
        // Write rop.
        rop.m_limbs[0u] = lo;
        rop.m_limbs[1u] = hi;
        // asize is at least 1.
        rop._mp_size = 1 + (hi != 0u);
        if (sign == -1) {
            rop._mp_size = -rop._mp_size;
        }
        return true;
    }
    static void static_mul_2exp(s_int &rop, const s_int &n, ::mp_bitcnt_t s)
    {
        static_mul_2exp_impl(rop, n, s, static_mul_2exp_algo<s_int>{});
        if (static_mul_2exp_algo<s_int>::value == 0) {
            rop.zero_unused_limbs();
        }
    }

public:
    friend void mul_2exp(mp_integer &rop, const mp_integer &n, ::mp_bitcnt_t s)
    {
    }

private:
    integer_union<SSize> m_int;
};
}

#if defined(_MSC_VER)

#pragma warning(pop)

#endif

#endif
