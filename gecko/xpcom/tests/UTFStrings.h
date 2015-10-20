/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef utfstrings_h__
#define utfstrings_h__

struct UTFStringsStringPair
  {
    PRUnichar m16[16];
    char m8[16];
  };

static const UTFStringsStringPair ValidStrings[] =
  {
    { { 'a', 'b', 'c', 'd' },
      { 'a', 'b', 'c', 'd' } },
    { { '1', '2', '3', '4' },
      { '1', '2', '3', '4' } },
    { { 0x7F, 'A', 0x80, 'B', 0x101, 0x200 },
      { 0x7F, 'A', 0xC2, 0x80, 'B', 0xC4, 0x81, 0xC8, 0x80 } },
    { { 0x7FF, 0x800, 0x1000 },
      { 0xDF, 0xBF, 0xE0, 0xA0, 0x80, 0xE1, 0x80, 0x80 } },
    { { 0xD7FF, 0xE000, 0xF00F, 'A', 0xFFF0 },
      { 0xED, 0x9F, 0xBF, 0xEE, 0x80, 0x80, 0xEF, 0x80, 0x8F, 'A', 0xEF, 0xBF, 0xB0 } },
    { { 0xFFF7, 0xFFFC, 0xFFFD, 0xFFFD },
      { 0xEF, 0xBF, 0xB7, 0xEF, 0xBF, 0xBC, 0xEF, 0xBF, 0xBD, 0xEF, 0xBF, 0xBD } },
    { { 0xD800, 0xDC00, 0xD800, 0xDCFF },
      { 0xF0, 0x90, 0x80, 0x80, 0xF0, 0x90, 0x83, 0xBF } },
    { { 0xDBFF, 0xDFFF, 0xDBB7, 0xDCBA },
      { 0xF4, 0x8F, 0xBF, 0xBF, 0xF3, 0xBD, 0xB2, 0xBA } },
    { { 0xFFFD, 0xFFFF },
      { 0xEF, 0xBF, 0xBD, 0xEF, 0xBF, 0xBF } },
    { { 0xFFFD, 0xFFFE, 0xFFFF },
      { 0xEF, 0xBF, 0xBD, 0xEF, 0xBF, 0xBE, 0xEF, 0xBF, 0xBF } },
  };

static const UTFStringsStringPair Invalid16Strings[] =
  {
    { { 'a', 'b', 0xD800 },
      { 'a', 'b', 0xEF, 0xBF, 0xBD } },
    { { 0xD8FF, 'b' },
      { 0xEF, 0xBF, 0xBD, 'b' } },
    { { 0xD821 },
      { 0xEF, 0xBF, 0xBD } },
    { { 0xDC21 },
      { 0xEF, 0xBF, 0xBD } },
    { { 0xDC00, 0xD800, 'b' },
      { 0xEF, 0xBF, 0xBD, 0xEF, 0xBF, 0xBD, 'b' } },
    { { 'b', 0xDC00, 0xD800 },
      { 'b', 0xEF, 0xBF, 0xBD, 0xEF, 0xBF, 0xBD } },
    { { 0xDC00, 0xD800 },
      { 0xEF, 0xBF, 0xBD, 0xEF, 0xBF, 0xBD } },
    { { 0xDC00, 0xD800, 0xDC00, 0xD800 },
      { 0xEF, 0xBF, 0xBD, 0xF0, 0x90, 0x80, 0x80, 0xEF, 0xBF, 0xBD } },
    { { 0xDC00, 0xD800, 0xD800, 0xDC00 },
      { 0xEF, 0xBF, 0xBD, 0xEF, 0xBF, 0xBD, 0xF0, 0x90, 0x80, 0x80 } },
  };

static const UTFStringsStringPair Invalid8Strings[] =
  {
    { { 'a', 0xFFFD, 'b' },
      { 'a', 0xC0, 0x80, 'b' } },
    { { 0xFFFD, 0x80 },
      { 0xC1, 0xBF, 0xC2, 0x80 } },
    { { 0xFFFD },
      { 0xC1, 0xBF } },
    { { 0xFFFD, 'x', 0x0800 },
      { 0xE0, 0x80, 0x80, 'x', 0xE0, 0xA0, 0x80 } },
    { { 0xFFFD, 'x', 0xFFFD },
      { 0xF0, 0x80, 0x80, 0x80, 'x', 0xF0, 0x80, 0x8F, 0x80 } },
    { { 0xFFFD, 0xFFFD },
      { 0xF4, 0x90, 0x80, 0x80, 0xF7, 0xBF, 0xBF, 0xBF } },
    { { 0xFFFD, 'x', 0xD800, 0xDC00, 0xFFFD },
      { 0xF0, 0x8F, 0xBF, 0xBF, 'x', 0xF0, 0x90, 0x80, 0x80, 0xF0, 0x8F, 0xBF, 0xBF } },
    { { 0xFFFD, 'x', 0xFFFD },
      { 0xF8, 0x80, 0x80, 0x80, 0x80, 'x', 0xF8, 0x88, 0x80, 0x80, 0x80 } },
    { { 0xFFFD, 0xFFFD },
      { 0xFB, 0xBF, 0xBF, 0xBF, 0xBF, 0xFC, 0xA0, 0x80, 0x80, 0x80, 0x80 } },
    { { 0xFFFD, 0xFFFD },
      { 0xFC, 0x80, 0x80, 0x80, 0x80, 0x80, 0xFD, 0xBF, 0xBF, 0xBF, 0xBF, 0xBF } },
  };

// Don't use this array in debug builds as that intentionally asserts.
#ifndef DEBUG
static const char Malformed8Strings[][16] =
  {
    { 0x80 },
    { 'a', 0xC8, 'c' },
    { 'a', 0xC0 },
    { 'a', 0xE8, 'c' },
    { 'a', 0xE8, 0x80, 'c' },
    { 'a', 0xE8, 0x80 },
    { 0xE8, 0x7F, 0x80 },
    { 'a', 0xE8, 0xE8, 0x80 },
    { 'a', 0xF4 },
    { 'a', 0xF4, 0x80, 0x80, 'c', 'c' },
    { 'a', 0xF4, 0x80, 'x', 0x80 },
    { 0xF4, 0x80, 0x80, 0x80, 0x80 },
    { 'a', 0xFA, 'c' },
    { 'a', 0xFA, 0x80, 0x80, 0x7F, 0x80, 'c' },
    { 'a', 0xFA, 0x80, 0x80, 0x80, 0x80, 0x80, 'c' },
    { 'a', 0xFD },
    { 'a', 0xFD, 0x80, 0x80, 0x80, 0x80, 'c' },
    { 'a', 0xFD, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80 },
    { 'a', 0xFC, 0x80, 0x80, 0x40, 0x80, 0x80, 'c' },
  };
#endif

#endif