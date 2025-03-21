/* Copyright 2022 Advanced Micro Devices, Inc.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE COPYRIGHT HOLDER(S) OR AUTHOR(S) BE LIABLE FOR ANY CLAIM, DAMAGES OR
 * OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE,
 * ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR
 * OTHER DEALINGS IN THE SOFTWARE.
 *
 * Authors: AMD
 *
 */

#include "fixed31_32.h"
#include "calc_u64.h"

static inline unsigned long long abs_i64(long long arg)
{
    if (arg > 0)
        return (unsigned long long)arg;
    else
        return (unsigned long long)(-arg);
}

/*
 * @brief
 * result = dividend / divisor
 * *remainder = dividend % divisor
 */
static inline unsigned long long complete_integer_division_u64(
    unsigned long long dividend, unsigned long long divisor, unsigned long long *remainder)
{
    unsigned long long result;

    VPE_ASSERT(divisor);

    result = div64_u64_rem(dividend, divisor, (uint64_t *)remainder);

    return result;
}

#define FRACTIONAL_PART_MASK ((1ULL << FIXED31_32_BITS_PER_FRACTIONAL_PART) - 1)

#define GET_INTEGER_PART(x) ((x) >> FIXED31_32_BITS_PER_FRACTIONAL_PART)

#define GET_FRACTIONAL_PART(x) (FRACTIONAL_PART_MASK & (x))

struct fixed31_32 vpe_fixpt_from_fraction(long long numerator, long long denominator)
{
    struct fixed31_32 res;

    bool arg1_negative = numerator < 0;
    bool arg2_negative = denominator < 0;

    unsigned long long arg1_value = (unsigned long long)(arg1_negative ? -numerator : numerator);
    unsigned long long arg2_value =
        (unsigned long long)(arg2_negative ? -denominator : denominator);

    unsigned long long remainder;

    /* determine integer part */

    unsigned long long res_value =
        complete_integer_division_u64(arg1_value, arg2_value, &remainder);

    VPE_ASSERT(res_value <= LONG_MAX);

    /* determine fractional part */
    {
        unsigned int i = FIXED31_32_BITS_PER_FRACTIONAL_PART;

        do {
            remainder <<= 1;

            res_value <<= 1;

            if (remainder >= arg2_value) {
                res_value |= 1;
                remainder -= arg2_value;
            }
        } while (--i != 0);
    }

    /* round up LSB */
    {
        unsigned long long summand = (remainder << 1) >= arg2_value;

        VPE_ASSERT(res_value <= LLONG_MAX - summand);

        res_value += summand;
    }

    res.value = (long long)res_value;

    if (arg1_negative ^ arg2_negative)
        res.value = -res.value;

    return res;
}

struct fixed31_32 vpe_fixpt_mul(struct fixed31_32 arg1, struct fixed31_32 arg2)
{
    struct fixed31_32 res;

    bool arg1_negative = arg1.value < 0;
    bool arg2_negative = arg2.value < 0;

    unsigned long long arg1_value = (unsigned long long)(arg1_negative ? -arg1.value : arg1.value);
    unsigned long long arg2_value = (unsigned long long)(arg2_negative ? -arg2.value : arg2.value);

    unsigned long long arg1_int = GET_INTEGER_PART(arg1_value);
    unsigned long long arg2_int = GET_INTEGER_PART(arg2_value);

    unsigned long long arg1_fra = GET_FRACTIONAL_PART(arg1_value);
    unsigned long long arg2_fra = GET_FRACTIONAL_PART(arg2_value);

    unsigned long long tmp;

    res.value = (long long)(arg1_int * arg2_int);

    VPE_ASSERT(res.value <= LONG_MAX);

    res.value <<= FIXED31_32_BITS_PER_FRACTIONAL_PART;

    tmp = arg1_int * arg2_fra;

    VPE_ASSERT(tmp <= (unsigned long long)(LLONG_MAX - res.value));

    res.value += tmp;

    tmp = arg2_int * arg1_fra;

    VPE_ASSERT(tmp <= (unsigned long long)(LLONG_MAX - res.value));

    res.value += tmp;

    tmp = arg1_fra * arg2_fra;

    tmp = (tmp >> FIXED31_32_BITS_PER_FRACTIONAL_PART) +
          (tmp >= (unsigned long long)vpe_fixpt_half.value);

    VPE_ASSERT(tmp <= (unsigned long long)(LLONG_MAX - res.value));

    res.value += tmp;

    if (arg1_negative ^ arg2_negative)
        res.value = -res.value;

    return res;
}

struct fixed31_32 vpe_fixpt_sqr(struct fixed31_32 arg)
{
    struct fixed31_32 res;

    unsigned long long arg_value = abs_i64(arg.value);

    unsigned long long arg_int = GET_INTEGER_PART(arg_value);

    unsigned long long arg_fra = GET_FRACTIONAL_PART(arg_value);

    unsigned long long tmp;

    res.value = (long long)(arg_int * arg_int);

    VPE_ASSERT(res.value <= LONG_MAX);

    res.value <<= FIXED31_32_BITS_PER_FRACTIONAL_PART;

    tmp = arg_int * arg_fra;

    VPE_ASSERT(tmp <= (unsigned long long)(LLONG_MAX - res.value));

    res.value += tmp;

    VPE_ASSERT(tmp <= (unsigned long long)(LLONG_MAX - res.value));

    res.value += tmp;

    tmp = arg_fra * arg_fra;

    tmp = (tmp >> FIXED31_32_BITS_PER_FRACTIONAL_PART) +
          (tmp >= (unsigned long long)vpe_fixpt_half.value);

    VPE_ASSERT(tmp <= (unsigned long long)(LLONG_MAX - res.value));

    res.value += tmp;

    return res;
}

struct fixed31_32 vpe_fixpt_recip(struct fixed31_32 arg)
{
    /*
     * @note
     * Good idea to use Newton's method
     */

    VPE_ASSERT(arg.value);

    return vpe_fixpt_from_fraction(vpe_fixpt_one.value, arg.value);
}

struct fixed31_32 vpe_fixpt_sinc(struct fixed31_32 arg)
{
    struct fixed31_32 square;

    struct fixed31_32 res = vpe_fixpt_one;

    int n = 27;

    struct fixed31_32 arg_norm = arg;

    if (vpe_fixpt_le(vpe_fixpt_two_pi, vpe_fixpt_abs(arg))) {
        arg_norm =
            vpe_fixpt_sub(arg_norm, vpe_fixpt_mul_int(vpe_fixpt_two_pi,
                                        (int)div64_s64(arg_norm.value, vpe_fixpt_two_pi.value)));
    }

    square = vpe_fixpt_sqr(arg_norm);

    do {
        res = vpe_fixpt_sub(
            vpe_fixpt_one, vpe_fixpt_div_int(vpe_fixpt_mul(square, res), n * (n - 1)));

        n -= 2;
    } while (n > 2);

    if (arg.value != arg_norm.value)
        res = vpe_fixpt_div(vpe_fixpt_mul(res, arg_norm), arg);

    return res;
}

struct fixed31_32 vpe_fixpt_sin(struct fixed31_32 arg)
{
    return vpe_fixpt_mul(arg, vpe_fixpt_sinc(arg));
}

struct fixed31_32 vpe_fixpt_cos(struct fixed31_32 arg)
{

    const struct fixed31_32 square = vpe_fixpt_sqr(arg);

    struct fixed31_32 res = vpe_fixpt_one;

    int n = 26;

    do {
        res = vpe_fixpt_sub(
            vpe_fixpt_one, vpe_fixpt_div_int(vpe_fixpt_mul(square, res), n * (n - 1)));

        n -= 2;
    } while (n != 0);

    return res;
}

/*
 * @brief
 * result = exp(arg),
 * where abs(arg) < 1
 *
 * Calculated as Taylor series.
 */
static struct fixed31_32 fixed31_32_exp_from_taylor_series(struct fixed31_32 arg)
{
    unsigned int n = 9;

    struct fixed31_32 res = vpe_fixpt_from_fraction(n + 2, n + 1);

    VPE_ASSERT(vpe_fixpt_lt(arg, vpe_fixpt_one));

    do
        res = vpe_fixpt_add(vpe_fixpt_one, vpe_fixpt_div_int(vpe_fixpt_mul(arg, res), n));
    while (--n != 1);

    return vpe_fixpt_add(vpe_fixpt_one, vpe_fixpt_mul(arg, res));
}

struct fixed31_32 vpe_fixpt_exp(struct fixed31_32 arg)
{
    /*
     * @brief
     * Main equation is:
     * exp(x) = exp(r + m * ln(2)) = (1 << m) * exp(r),
     * where m = round(x / ln(2)), r = x - m * ln(2)
     */

    if (vpe_fixpt_le(vpe_fixpt_ln2_div_2, vpe_fixpt_abs(arg))) {
        int m = vpe_fixpt_round(vpe_fixpt_div(arg, vpe_fixpt_ln2));

        struct fixed31_32 r = vpe_fixpt_sub(arg, vpe_fixpt_mul_int(vpe_fixpt_ln2, m));

        VPE_ASSERT(m != 0);

        VPE_ASSERT(vpe_fixpt_lt(vpe_fixpt_abs(r), vpe_fixpt_one));

        if (m > 0)
            return vpe_fixpt_shl(fixed31_32_exp_from_taylor_series(r), (unsigned char)m);
        else
            return vpe_fixpt_div_int(fixed31_32_exp_from_taylor_series(r), 1LL << -m);
    } else if (arg.value != 0)
        return fixed31_32_exp_from_taylor_series(arg);
    else
        return vpe_fixpt_one;
}

struct fixed31_32 vpe_fixpt_log(struct fixed31_32 arg)
{
    struct fixed31_32 res = vpe_fixpt_neg(vpe_fixpt_one);

    struct fixed31_32 error;

    VPE_ASSERT(arg.value > 0); /*log is defined only for positive numbers*/

    do {
        struct fixed31_32 res1 = vpe_fixpt_add(
            vpe_fixpt_sub(res, vpe_fixpt_one), vpe_fixpt_div(arg, vpe_fixpt_exp(res)));

        error = vpe_fixpt_sub(res, res1);

        res = res1;
    } while (abs_i64(error.value) > 100ULL);

    return res;
}

/* this function is a generic helper to translate fixed point value to
 * specified integer format that will consist of integer_bits integer part and
 * fractional_bits fractional part. For example it is used in
 *  vpe_fixpt_u2d19 to receive 2 bits integer part and 19 bits fractional
 * part in 32 bits. It is used in hw programming (scaler)
 */

static inline unsigned int ux_dy(
    long long value, unsigned int integer_bits, unsigned int fractional_bits)
{
    /* 1. create mask of integer part */
    unsigned int result = (1 << integer_bits) - 1;
    /* 2. mask out fractional part */
    unsigned int fractional_part = FRACTIONAL_PART_MASK & (unsigned long long)value;
    /* 3. shrink fixed point integer part to be of integer_bits width*/
    result &= GET_INTEGER_PART(value);
    /* 4. make space for fractional part to be filled in after integer */
    result <<= fractional_bits;
    /* 5. shrink fixed point fractional part to of fractional_bits width*/
    fractional_part >>= FIXED31_32_BITS_PER_FRACTIONAL_PART - fractional_bits;
    /* 6. merge the result */
    return result | fractional_part;
}

static inline unsigned int clamp_ux_dy(long long value, unsigned int integer_bits,
    unsigned int fractional_bits, unsigned int min_clamp)
{
    unsigned int truncated_val = ux_dy(value, integer_bits, fractional_bits);

    if (value >= (1LL << (integer_bits + FIXED31_32_BITS_PER_FRACTIONAL_PART)))
        return (1 << (integer_bits + fractional_bits)) - 1;
    else if (truncated_val > min_clamp)
        return truncated_val;
    else
        return min_clamp;
}

unsigned int vpe_fixpt_u4d19(struct fixed31_32 arg)
{
    return ux_dy(arg.value, 4, 19);
}

unsigned int vpe_fixpt_u3d19(struct fixed31_32 arg)
{
    return ux_dy(arg.value, 3, 19);
}

unsigned int vpe_fixpt_u2d19(struct fixed31_32 arg)
{
    return ux_dy(arg.value, 2, 19);
}

unsigned int vpe_fixpt_u0d19(struct fixed31_32 arg)
{
    return ux_dy(arg.value, 0, 19);
}

unsigned int vpe_fixpt_clamp_u0d14(struct fixed31_32 arg)
{
    return clamp_ux_dy(arg.value, 0, 14, 1);
}

unsigned int vpe_fixpt_clamp_u0d10(struct fixed31_32 arg)
{
    return clamp_ux_dy(arg.value, 0, 10, 1);
}

int vpe_fixpt_s4d19(struct fixed31_32 arg)
{
    if (arg.value < 0)
        return -(int)ux_dy(vpe_fixpt_abs(arg).value, 4, 19);
    else
        return (int)ux_dy(arg.value, 4, 19);
}

unsigned int vpe_to_fixed_point(
    unsigned int decimalBits, double value, unsigned int mask, double d_pix)
{
    unsigned int d_i;

    d_i = (int)((value * d_pix) + 0.5);
    d_i = d_i & mask;
    return d_i;
}

/* This function is a generic way to convert a double into fixpt format AdBu, where
 * A is the decimal bits and B is the fractional bits. If clamp is set, it will
 * clamp the max value, otherwise there is risk of overflow.
 */
unsigned long long vpe_double_to_fixed_point(
    double x, unsigned long long decimal_bits, unsigned long long fractional_bits, bool clamp)
{
    unsigned long long shift   = 1;
    double             norm    = (double)(shift << fractional_bits);
    unsigned long long x_fixpt = (long long)(x * norm);
    unsigned long long mask    = ((shift << (decimal_bits + fractional_bits)) - 1);

    if ((clamp == true) && (x_fixpt > mask)) {
        x_fixpt = mask;
    } else {
        x_fixpt = x_fixpt & mask;
    }

    return x_fixpt;
}
