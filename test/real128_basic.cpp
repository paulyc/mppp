// Copyright 2016-2017 Francesco Biscani (bluescarni@gmail.com)
//
// This file is part of the mp++ library.
//
// This Source Code Form is subject to the terms of the Mozilla
// Public License v. 2.0. If a copy of the MPL was not distributed
// with this file, You can obtain one at http://mozilla.org/MPL/2.0/.

#include <cstdint>
#include <gmp.h>
#include <limits>
#include <quadmath.h>
#include <random>
#include <stdexcept>
#include <string>
#if __cplusplus >= 201703L
#include <string_view>
#endif
#include <type_traits>
#include <utility>
#include <vector>

#include <mp++/mp++.hpp>

#include "test_utils.hpp"

#define CATCH_CONFIG_MAIN
#include "catch.hpp"

using namespace mppp;

using int_t = integer<1>;
using rat_t = rational<1>;

static int ntries = 1000;

static std::mt19937 rng;

TEST_CASE("real128 constructors")
{
    real128 r;
    REQUIRE((r.m_value == 0));
    r.m_value = 12;
    real128 r2{r};
    REQUIRE((r2.m_value == 12));
    real128 r3{std::move(r)};
    REQUIRE((r3.m_value == 12));
    REQUIRE((r.m_value == 12));
    real128 r4{::__float128(-56)};
    REQUIRE((r4.m_value == -56));
    real128 r5{-123};
    REQUIRE((r5.m_value == -123));
    real128 r6{124ull};
    REQUIRE((r6.m_value == 124));
    real128 r7{-0.5};
    REQUIRE((r7.m_value == -0.5));
    real128 r8{1.5f};
    REQUIRE((r8.m_value == 1.5f));
#if defined(MPPP_WITH_MPFR)
    real128 r8a{1.5l};
    REQUIRE((r8a.m_value == 1.5l));
#endif
    REQUIRE((!std::is_constructible<real128, std::vector<int>>::value));
    // Construction from integer.
    REQUIRE((real128{int_t{0}}.m_value == 0));
    int_t n{123};
    REQUIRE((real128{n}.m_value == 123));
    n = -123;
    n.promote();
    REQUIRE((real128{n}.m_value == -123));
    // Use a couple of limbs, nbits does not divide GMP_NUMB_BITS exactly.
    n = -1;
    n <<= GMP_NUMB_BITS + 1;
    REQUIRE((real128{n}.m_value == ::scalbnq(::__float128(-1), GMP_NUMB_BITS + 1)));
    n.promote();
    n.neg();
    REQUIRE((real128{n}.m_value == ::scalbnq(::__float128(1), GMP_NUMB_BITS + 1)));
    // Use two limbs, nbits dividing exactly.
    n = -2;
    n <<= 2 * GMP_NUMB_BITS - 1;
    REQUIRE((real128{n}.m_value == ::scalbnq(::__float128(-2), 2 * GMP_NUMB_BITS - 1)));
    n.promote();
    n.neg();
    REQUIRE((real128{n}.m_value == ::scalbnq(::__float128(2), 2 * GMP_NUMB_BITS - 1)));
    // Random testing.
    constexpr auto delta64 = std::numeric_limits<std::uint_least64_t>::digits - 64;
    constexpr auto delta49 = std::numeric_limits<std::uint_least64_t>::digits - 49;
    std::uniform_int_distribution<std::uint_least64_t> dist64(0u, (std::uint_least64_t(-1) << delta64) >> delta64);
    std::uniform_int_distribution<std::uint_least64_t> dist49(0u, (std::uint_least64_t(-1) << delta49) >> delta49);
    std::uniform_int_distribution<int> sdist(0, 1);
    std::uniform_int_distribution<int> extra_bits(0, 8);
    for (int i = 0; i < ntries; ++i) {
        const auto hi = dist49(rng);
        const auto lo = dist64(rng);
        const auto sign = sdist(rng) ? 1 : -1;
        const auto ebits = extra_bits(rng);
        auto tmp_r = real128{((int_t{hi} << 64) * sign + lo) << ebits};
        auto cmp_r = ::scalbnq(::scalbnq(::__float128(hi) * sign, 64) + lo, ebits);
        REQUIRE((tmp_r.m_value == cmp_r));
        REQUIRE(static_cast<int_t>(tmp_r) == ((int_t{hi} << 64) * sign + lo) << ebits);
        tmp_r = real128{(int_t{hi} << (64 - ebits)) * sign + (lo >> ebits)};
        cmp_r = ::scalbnq(::__float128(hi) * sign, 64 - ebits) + (lo >> ebits);
        REQUIRE((tmp_r.m_value == cmp_r));
        REQUIRE(static_cast<int_t>(tmp_r) == (int_t{hi} << (64 - ebits)) * sign + (lo >> ebits));
    }
    // Constructor from rational.
    // A few simple cases.
    REQUIRE((real128{rat_t{0}}.m_value == 0));
    REQUIRE((real128{rat_t{1, 2}}.m_value == real128{"0.5"}.m_value));
    REQUIRE((real128{rat_t{3, -2}}.m_value == real128{"-1.5"}.m_value));
    // Num's bit size > 113, den not.
    // These have been checked with mpmath.
    REQUIRE((::fabsq(real128{rat_t{int_t{"-38534035372951953445309927667133500127"},
                                   int_t{"276437038692051021425869207346"}}}
                         .m_value
                     - real128{"-139395341.359732211699141193741051607"}.m_value)
             < 1E-34 / 139395341.));
    // Opposite of above.
    REQUIRE((::fabsq(real128{rat_t{int_t{"861618639356201333739137018526"},
                                   int_t{"-30541779607702874593949544341902312610"}}}
                         .m_value
                     - real128{"-0.0000000282111471703140181436825504811494878"}.m_value)
             < 1E-34 / 0.000000028211147170));
    // Both num and den large.
    REQUIRE((::fabsq(real128{rat_t{int_t{"-32304709999587426335154241885499878925"},
                                   int_t{"41881836637791190397532909138415249190"}}}
                         .m_value
                     - real128{"-0.77132983156803476500525887410811607"}.m_value)
             < 1E-34));
    REQUIRE((::fabsq(real128{rat_t{int_t{"41881836637791190397532909138415249190"} / 2,
                                   int_t{"-32304709999587426335154241885499878925"}}}
                         .m_value
                     - real128{"-0.648231119213360475524695260458732616"}.m_value)
             < 1E-34));
    // Subnormal numbers.
    REQUIRE((real128{rat_t{1, int_t{1} << 16493}}.m_value
             == real128{"1.295035023887605022184887791645529310e-4965"}.m_value));
    REQUIRE((real128{rat_t{-1, int_t{1} << 16494}}.m_value
             == real128{"-6.47517511943802511092443895822764655e-4966"}.m_value));
    // String ctors.
    REQUIRE((real128{"0"}.m_value == 0));
    REQUIRE((real128{"-0"}.m_value == 0));
    REQUIRE((real128{"+0"}.m_value == 0));
    REQUIRE((real128{"123"}.m_value == 123));
    REQUIRE((real128{"-123"}.m_value == -123));
    REQUIRE((real128{".123E3"}.m_value == 123));
    REQUIRE((real128{"-.123e3"}.m_value == -123));
    REQUIRE((real128{"12300E-2"}.m_value == 123));
    REQUIRE((real128{"-12300e-2"}.m_value == -123));
    REQUIRE((real128{std::string{"12300E-2"}}.m_value == 123));
    REQUIRE((real128{std::string{"-12300e-2"}}.m_value == -123));
    const char tmp_char[] = "foobar-1234 baz";
    REQUIRE((real128{tmp_char + 6, tmp_char + 11}.m_value == -1234));
    REQUIRE_THROWS_PREDICATE(
        (real128{tmp_char + 6, tmp_char + 12}), std::invalid_argument, [](const std::invalid_argument &ex) {
            return std::string(ex.what())
                   == "The string '-1234 ' does not represent a valid quadruple-precision floating-point value";
        });
#if __cplusplus >= 201703L
    REQUIRE((real128{std::string_view{tmp_char + 6, 5}}.m_value == -1234));
    REQUIRE_THROWS_PREDICATE(
        (real128{std::string_view{tmp_char + 6, 6}}), std::invalid_argument, [](const std::invalid_argument &ex) {
            return std::string(ex.what())
                   == "The string '-1234 ' does not represent a valid quadruple-precision floating-point value";
        });
#endif
    REQUIRE((real128{"  -12300e-2"}.m_value == -123));
    REQUIRE_THROWS_PREDICATE(real128{""}, std::invalid_argument, [](const std::invalid_argument &ex) {
        return std::string(ex.what())
               == "The string '' does not represent a valid quadruple-precision floating-point value";
    });
    REQUIRE_THROWS_PREDICATE(real128{"foobar"}, std::invalid_argument, [](const std::invalid_argument &ex) {
        return std::string(ex.what())
               == "The string 'foobar' does not represent a valid quadruple-precision floating-point value";
    });
    REQUIRE_THROWS_PREDICATE(real128{"12 "}, std::invalid_argument, [](const std::invalid_argument &ex) {
        return std::string(ex.what())
               == "The string '12 ' does not represent a valid quadruple-precision floating-point value";
    });
    REQUIRE(::isnanq(real128{"nan"}.m_value));
    REQUIRE(::isnanq(real128{"-nan"}.m_value));
    REQUIRE(::isinfq(real128{"inf"}.m_value));
    REQUIRE(::isinfq(real128{"-inf"}.m_value));
    // Assignment operators.
    real128 ra{1}, rb{2};
    ra = rb;
    REQUIRE((ra.m_value == 2));
    ra = real128{123};
    REQUIRE((ra.m_value == 123));
    ra = ::__float128(-345);
    REQUIRE((ra.m_value == -345));
    ra = 456.;
    REQUIRE((ra.m_value == 456));
    ra = -23ll;
    REQUIRE((ra.m_value == -23));
    REQUIRE((!std::is_assignable<real128 &, std::vector<int>>::value));
    ra = int_t{-128};
    REQUIRE((ra.m_value == -128));
    ra = rat_t{-6, -3};
    REQUIRE((ra.m_value == 2));
    ra = "-1.23E5";
    REQUIRE((ra.m_value == -123000));
    ra = std::string("1234");
    REQUIRE((ra.m_value == 1234));
#if __cplusplus >= 201703L
    ra = std::string_view{tmp_char + 6, 5};
    REQUIRE((ra.m_value == -1234));
#endif
}
