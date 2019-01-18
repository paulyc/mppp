.. _tutorial_real128:

Quadruple-precision float tutorial
==================================

The :cpp:class:`~mppp::real128` class is a thin wrapper around the :cpp:type:`__float128` type
available in GCC and (more recently) clang. :cpp:type:`__float128` is an implementation the
`quadruple-precision IEEE 754 binary floating-point standard <https://en.wikipedia.org/wiki/Quadruple-precision_floating-point_format>`__,
which provides up to 36 decimal digits of precision.

:cpp:class:`~mppp::real128` is available in mp++ if the library is configured with the
``MPPP_WITH_QUADMATH`` option enabled (see the :ref:`installation instructions <installation>`).
Note that mp++ needs access to both the ``quadmath.h`` header file and the quadmath library
``libquadmath.so``, which may be installed in non-standard locations. While GCC is typically
able to resolve the correct paths automatically, clang might need assistance
in order to identify the location of these files.

As a thin wrapper, :cpp:class:`~mppp::real128` adds a few extra features
on top of what :cpp:type:`__float128` already provides. Specifically, :cpp:class:`~mppp::real128`:

* can interact with the other mp++ classes,
* can be constructed from string-like objects,
* supports the standard C++ ``iostream`` facilities.

Like :cpp:type:`__float128`, :cpp:class:`~mppp::real128` is a
`literal type <https://en.cppreference.com/w/cpp/named_req/LiteralType>`__, and thus it can be used
for ``constexpr`` compile-time computations. Additionally, :cpp:class:`~mppp::real128`
implements as ``constexpr`` constructs a variety of functions which are not ``constexpr``
for :cpp:type:`__float128`.

In addition to the features common to all mp++ classes, the :cpp:class:`~mppp::real128` API provides
a few additional capabilities:

* construction/conversion from/to :cpp:type:`__float128`:

  .. code-block:: c++

     real128 r{__float128(42)};                // Construction from a __float128.
     assert(r == 42);
     assert(static_cast<__float128>(r) == 42); // Conversion to __float128.

* direct access to the internal :cpp:type:`__float128` instance (via the public :cpp:member:`~mppp::real128::m_value`
  data member):

  .. code-block:: c++

     real128 r{1};
     r.m_value += 1;                 // Modify directly the internal __float128 member.
     assert(r == 2);

     r.m_value = 0;
     assert(::cosq(r.m_value) == 1); // Call a libquadmath function directly on the internal member.

* a variety of mathematical :ref:`functions <real128_functions>` wrapping the
  `libquadmath library routines <https://gcc.gnu.org/onlinedocs/libquadmath/Math-Library-Routines.html#Math-Library-Routines>`__.
  Note that the :cpp:class:`~mppp::real128` function names drop the suffix ``q`` appearing in the names of the libquadmath routines, and, as usual
  in mp++, they are supposed to be found via ADL. Member function overloads for the unary functions are also available:

  .. code-block:: c++

     real128 r{42};

     // Trigonometry.
     assert(cos(r) == ::cosq(r.m_value));
     assert(sin(r) == ::sinq(r.m_value));

     // Logarithms and exponentials.
     assert(exp(r) == ::expq(r.m_value));
     assert(log10(r) == ::log10q(r.m_value));

     // Etc.
     assert(lgamma(r) == ::lgammaq(r.m_value));
     assert(erf(r) == ::erfq(r.m_value));

     // Member function overloads.
     auto tmp = cos(r);
     assert(r.cos() == tmp); // NOTE: r.cos() will set r to its cosine.
     tmp = sin(r);
     assert(r.sin() == tmp); // NOTE: r.sin() will set r to its sine.

* NaN-friendly hashing and comparison functions, for use in standard algorithms and containers;
* a :ref:`specialisation <real128_std_specs>` of the ``std::numeric_limits`` class template;
* a selection of quadruple-precision compile-time :ref:`mathematical constants <real128_constants>`.

The :ref:`real128 reference <real128_reference>` contains the detailed description of all the features
provided by :cpp:class:`~mppp::real128`.
