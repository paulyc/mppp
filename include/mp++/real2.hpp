// Copyright 2016-2017 Francesco Biscani (bluescarni@gmail.com)
//
// This file is part of the mp++ library.
//
// This Source Code Form is subject to the terms of the Mozilla
// Public License v. 2.0. If a copy of the MPL was not distributed
// with this file, You can obtain one at http://mozilla.org/MPL/2.0/.

#ifndef MPPP_REAL2_HPP
#define MPPP_REAL2_HPP

#include <mp++/config.hpp>

#if defined(MPPP_WITH_MPFR)

#include <algorithm>
#include <array>
#include <cassert>
#include <cstddef>
#include <iostream>
#include <limits>
#include <mp++/detail/mpfr.hpp>
#include <mp++/integer.hpp>
#include <stdexcept>
#include <string>
#include <type_traits>

namespace mppp
{

inline namespace detail
{

// constexpr max/min implementations.
template <typename T>
constexpr T c_max(T a, T b)
{
    return a > b ? a : b;
}

template <typename T>
constexpr T c_min(T a, T b)
{
    return a < b ? a : b;
}

// Compute a power of 2 value of the signed integer type T that can be safely negated.
template <typename T>
constexpr T safe_abs(T n = T(1))
{
    static_assert(std::is_integral<T>::value && std::is_signed<T>::value, "Invalid type.");
    return (n <= std::numeric_limits<T>::max() / T(2) && n >= std::numeric_limits<T>::min() / T(2))
               ? safe_abs(T(n * T(2)))
               : n;
}

// Min prec that will be allowed for reals. It should be MPFR_PREC_MIN, as that value is currently 2.
// We just put an extra constraint for paranoia, so that we ensure 100% that the min prec is not zero
// (we rely on that for distinguishing static vs dynamic storage).
constexpr ::mpfr_prec_t real_prec_min()
{
    return c_max(::mpfr_prec_t(1), ::mpfr_prec_t(MPFR_PREC_MIN));
}

// For the max precision, we first remove 7 bits from the MPFR_PREC_MAX value (as the MPFR docs warn
// to never set the precision "close" to the max value). Then we make sure that we can negate safely.
constexpr ::mpfr_prec_t real_prec_max()
{
    return c_min(::mpfr_prec_t(MPFR_PREC_MAX / 128), safe_abs<::mpfr_prec_t>());
}

// Sanity check.
static_assert(real_prec_min() <= real_prec_max(),
              "The minimum precision for real is larger than the maximum precision.");

// TODO mpfr struct checks (see integer)

// Helper function to print an mpfr to stream in a specified base.
inline void mpfr_to_stream(const ::mpfr_t r, std::ostream &os, int base)
{
    // Check the base.
    if (mppp_unlikely(base < 2 || base > 62)) {
        throw std::invalid_argument("In the conversion of a real to string, a base of " + std::to_string(base)
                                    + " was specified, but the only valid values are in the [2,62] range");
    }

    // Get the string fractional representation via the MPFR function,
    // and wrap it into a smart pointer.
    ::mpfr_exp_t exp(0);
    smart_mpfr_str str(::mpfr_get_str(nullptr, &exp, 10, 0, r, MPFR_RNDN), ::mpfr_free_str);
    if (mppp_unlikely(!str)) {
        throw std::runtime_error("Error in the conversion of a real to string: the call to mpfr_get_str() failed");
    }

    // Print the string, inserting a decimal point after the first digit.
    bool dot_added = false;
    for (auto cptr = str.get(); *cptr != '\0'; ++cptr) {
        os << (*cptr);
        // NOTE: check this answer:
        // http://stackoverflow.com/questions/13827180/char-ascii-relation
        // """
        // The mapping of integer values for characters does have one guarantee given
        // by the Standard: the values of the decimal digits are continguous.
        // (i.e., '1' - '0' == 1, ... '9' - '0' == 9)
        // """
        if (!dot_added && *cptr >= '0' && *cptr <= '9') {
            os << '.';
            dot_added = true;
        }
    }
    assert(dot_added);

    // Adjust the exponent. Do it in multiprec in order to avoid potential overflow.
    integer<1> z_exp{exp};
    --z_exp;
    if (z_exp.sgn() && !mpfr_zero_p(r)) {
        // Add the exponent at the end of the string, if both the value and the exponent
        // are nonzero.
        os << "e" << z_exp;
    }
}

template <std::size_t SSize>
struct static_real {
    // Let's put a hard cap and sanity check on the static size.
    static_assert(SSize > 0u && SSize <= 64u, "Invalid static size for real2.");
    mpfr_struct_t get_mpfr()
    {
        // NOTE: here we will assume the precision is set to a value which can
        // always be negated safely.
        return mpfr_struct_t{-_mpfr_prec, _mpfr_sign, _mpfr_exp, m_limbs.data()};
    }
    mpfr_struct_t get_mpfr_c() const
    {
        // NOTE: as usual, we are using const_cast here to cast away the constness of the pointer.
        // We need to make extra-sure the returned value is used only where const mpfr_t is expected.
        return mpfr_struct_t{-_mpfr_prec, _mpfr_sign, _mpfr_exp, const_cast<::mp_limb_t *>(m_limbs.data())};
    }
    // Maximum precision possible for a static real.
    static constexpr ::mpfr_prec_t max_prec_impl(::mpfr_prec_t n)
    {
        return n == std::numeric_limits<::mpfr_prec_t>::max()
                   // Overflow check first.
                   ? (throw std::overflow_error(
                         "Overflow in the determination of the maximum precision for a static real."))
                   : (mpfr_custom_get_size(n) <= SSize * sizeof(::mp_limb_t)
                          // For the current precision n, we have enough storage. Try 1 bit more of precision.
                          ? max_prec_impl(static_cast<::mpfr_prec_t>(n + 1))
                          // We don't have enough storage for the current precision. Removing one bit should
                          // give us a suitable precision.
                          // NOTE: the min n possible here is real_prec_min(), and that is > 0. Hence, n - 1 is a safe
                          // computation.
                          : (mpfr_custom_get_size(static_cast<::mpfr_prec_t>(n - 1)) <= SSize * sizeof(::mp_limb_t)
                                 ? static_cast<::mpfr_prec_t>(n - 1)
                                 // This should never happen, as it would mean that 1 extra bit of precision
                                 // requires 2 bytes of storage. Let's leave it for sanity check.
                                 : (throw std::logic_error(
                                       "Could not determine the maximum precision for a static real."))));
    }
    static constexpr ::mpfr_prec_t max_prec()
    {
        // NOTE: the impl() function assumes there's enough storage for minimum precision.
        static_assert(mpfr_custom_get_size(real_prec_min()) <= SSize * sizeof(::mp_limb_t),
                      "Not enough storage in static_real to represent a real with minimum precision.");
        // Make sure the output is never greater than real_prec_max().
        return c_min(real_prec_max(), max_prec_impl(real_prec_min()));
    }
    ::mpfr_prec_t _mpfr_prec;
    ::mpfr_sign_t _mpfr_sign;
    ::mpfr_exp_t _mpfr_exp;
    std::array<::mp_limb_t, SSize> m_limbs;
};

template <std::size_t SSize>
union real_union {
    using s_storage = static_real<SSize>;
    using d_storage = mpfr_struct_t;
    // Implementation of default ctor. This assumes that no member is currently active,
    // and it will construct the static storage member with zero value and minimum precision.
    void default_init()
    {
        // Activate the static member.
        ::new (static_cast<void *>(&m_st)) s_storage;

        // Static member is active. Init it with a minimum precision zero.
        // Start by zero filling the limbs array. The reason we do this
        // is that we are not sure the mpfr api will init the whole array,
        // so we could be left with uninited values in the array, thus
        // ending up reading uninited values during copy/move. It's not
        // clear 100% if this is UB, but better safe than sorry.
        std::fill(m_st.m_limbs.begin(), m_st.m_limbs.end(), ::mp_limb_t(0));

        // A temporary mpfr struct for use with the mpfr custom interface.
        mpfr_struct_t tmp;
        // Init the limbs first, as indicated by the mpfr docs. But first assert the static
        // storage is enough to store a real with minimum precision.
        static_assert(mpfr_custom_get_size(real_prec_min()) <= SSize * sizeof(::mp_limb_t),
                      "Not enough storage in static_real to represent a real with minimum precision.");
        mpfr_custom_init(m_st.m_limbs.data(), real_prec_min());
        // Do the custom init with a zero value, exponent 0 (unused), minimum precision (must match
        // the previous mpfr_custom_init() call), and the limbs array pointer.
        mpfr_custom_init_set(&tmp, MPFR_ZERO_KIND, 0, real_prec_min(), m_st.m_limbs.data());

        // Copy over from tmp. The precision is set to the negative of the actual precision to signal static storage.
        assert(tmp._mpfr_prec == real_prec_min());
        m_st._mpfr_prec = -real_prec_min();
        m_st._mpfr_sign = tmp._mpfr_sign;
        m_st._mpfr_exp = tmp._mpfr_exp;
    }
    real_union()
    {
        default_init();
    }
    real_union(const real_union &r)
    {
        if (r.is_static()) {
            // Activate and init the static member with a copy.
            ::new (static_cast<void *>(&m_st)) s_storage(r.g_st());
        } else {
            // Activate dynamic storage.
            ::new (static_cast<void *>(&m_dy)) d_storage;
            // Init with same precision as r, then set.
            ::mpfr_init2(&m_dy, mpfr_get_prec(&r.g_dy()));
            ::mpfr_set(&m_dy, &r.g_dy(), MPFR_RNDN);
            // NOTE: use the function (rather than the macro) in order
            // to avoid a warning about identical branches in recent GCC versions.
            assert((mpfr_get_prec)(&m_dy) > 0);
        }
    }
    real_union(real_union &&r) noexcept
    {
        if (r.is_static()) {
            // Activate and init the static member with a copy.
            ::new (static_cast<void *>(&m_st)) s_storage(r.g_st());
        } else {
            // Activate dynamic storage, and shallow copy r.
            ::new (static_cast<void *>(&m_dy)) d_storage(r.g_dy());

            // Deactivate the static storage in r, and default-init to static.
            r.g_dy().~d_storage();
            r.default_init();
        }
    }
    ~real_union()
    {
        assert(m_st._mpfr_prec);
        if (is_static()) {
            g_st().~s_storage();
        } else {
            ::mpfr_clear(&g_dy());
            g_dy().~d_storage();
        }
    }
    // Check storage type.
    bool is_static() const
    {
        return m_st._mpfr_prec < 0;
    }
    bool is_dynamic() const
    {
        return m_st._mpfr_prec > 0;
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
    // Data members.
    s_storage m_st;
    d_storage m_dy;
};
}

template <std::size_t SSize>
class real2
{
public:
    real2() = default;
    real2(const real2 &) = default;
    bool is_static() const
    {
        return m_real.is_static();
    }
    bool is_dynamic() const
    {
        return m_real.is_dynamic();
    }
    ::mpfr_prec_t get_prec() const
    {
        return static_cast<::mpfr_prec_t>(m_real.m_st._mpfr_prec > 0 ? m_real.m_st._mpfr_prec
                                                                     : -m_real.m_st._mpfr_prec);
    }
    const real_union<SSize> &_get_union() const
    {
        return m_real;
    }
    real_union<SSize> &_get_union()
    {
        return m_real;
    }

private:
    real_union<SSize> m_real;
};

template <std::size_t SSize>
inline std::ostream &operator<<(std::ostream &os, const real2<SSize> &r)
{
    if (r.is_static()) {
        const auto m = r._get_union().g_st().get_mpfr_c();
        mpfr_to_stream(&m, os, 10);
    } else {
        mpfr_to_stream(&r._get_union().g_dy(), os, 10);
    }
    return os;
}
}

#else

#error The real2.hpp header was included but mp++ was not configured with the MPPP_WITH_MPFR option.

#endif

#endif