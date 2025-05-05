/*
 * Copyright © Microsoft Corporation
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice (including the next
 * paragraph) shall be included in all copies or substantial portions of the
 * Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
 * IN THE SOFTWARE.
 */

#pragma once

#include <initializer_list>
#include <type_traits>

template <typename T>
concept EnumType = std::is_enum_v<T>;

template <EnumType Enum>
class EnumMask
{
   using UnderlyingType = std::underlying_type_t<Enum>;

 public:
   explicit constexpr EnumMask( std::initializer_list<Enum> values )
   {
      for( auto v : values )
      {
         m_mask |= MakeValue( v );
      }
   }

   constexpr bool HasAll( Enum v )
   {
      return m_mask & MakeValue( v );
   }

   template <EnumType... Enums>
   constexpr bool HasAll( Enum v, Enums... values )
   {
      return HasAll( v ) && HasAll( values... );
   }

 private:
   constexpr UnderlyingType MakeValue( Enum v )
   {
      return 1 << (UnderlyingType) v;
   }

   UnderlyingType m_mask {};
};