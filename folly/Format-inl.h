/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#ifndef FOLLY_FORMAT_H_
#error This file may only be included from Format.h.
#endif

#include <array>
#include <cinttypes>
#include <deque>
#include <map>
#include <unordered_map>
#include <vector>

#include <folly/Exception.h>
#include <folly/FormatTraits.h>
#include <folly/MapUtil.h>
#include <folly/Portability.h>
#include <folly/Traits.h>
#include <folly/lang/Exception.h>
#include <folly/lang/ToAscii.h>
#include <folly/portability/Windows.h>

// Ignore -Wformat-nonliteral and -Wconversion warnings within this file
FOLLY_PUSH_WARNING
FOLLY_GNU_DISABLE_WARNING("-Wformat-nonliteral")
FOLLY_GNU_DISABLE_WARNING("-Wconversion")

namespace folly {

namespace detail {

// Updates the end of the buffer after the comma separators have been added.
void insertThousandsGroupingUnsafe(char* start_buffer, char** end_buffer);

extern const std::array<std::array<char, 2>, 256> formatHexUpper;
extern const std::array<std::array<char, 2>, 256> formatHexLower;
extern const std::array<std::array<char, 3>, 512> formatOctal;
extern const std::array<std::array<char, 8>, 256> formatBinary;

const size_t kMaxHexLength = 2 * sizeof(uintmax_t);
const size_t kMaxOctalLength = 3 * sizeof(uintmax_t);
const size_t kMaxBinaryLength = 8 * sizeof(uintmax_t);

/**
 * Convert an unsigned to hex, using repr (which maps from each possible
 * 2-hex-bytes value to the 2-character representation).
 *
 * Just like folly::detail::uintToBuffer in Conv.h, writes at the *end* of
 * the supplied buffer and returns the offset of the beginning of the string
 * from the start of the buffer.  The formatted string will be in range
 * [buf+begin, buf+bufLen).
 */
template <class Uint>
size_t uintToHex(
    char* buffer,
    size_t bufLen,
    Uint v,
    std::array<std::array<char, 2>, 256> const& repr) {
  // 'v >>= 7, v >>= 1' is no more than a work around to get rid of shift size
  // warning when Uint = uint8_t (it's false as v >= 256 implies sizeof(v) > 1).
  for (; !less_than<unsigned, 256>(v); v >>= 7, v >>= 1) {
    auto b = v & 0xff;
    bufLen -= 2;
    buffer[bufLen] = repr[b][0];
    buffer[bufLen + 1] = repr[b][1];
  }
  buffer[--bufLen] = repr[v][1];
  if (v >= 16) {
    buffer[--bufLen] = repr[v][0];
  }
  return bufLen;
}

/**
 * Convert an unsigned to hex, using lower-case letters for the digits
 * above 9.  See the comments for uintToHex.
 */
template <class Uint>
inline size_t uintToHexLower(char* buffer, size_t bufLen, Uint v) {
  return uintToHex(buffer, bufLen, v, formatHexLower);
}

/**
 * Convert an unsigned to hex, using upper-case letters for the digits
 * above 9.  See the comments for uintToHex.
 */
template <class Uint>
inline size_t uintToHexUpper(char* buffer, size_t bufLen, Uint v) {
  return uintToHex(buffer, bufLen, v, formatHexUpper);
}

/**
 * Convert an unsigned to octal.
 *
 * Just like folly::detail::uintToBuffer in Conv.h, writes at the *end* of
 * the supplied buffer and returns the offset of the beginning of the string
 * from the start of the buffer.  The formatted string will be in range
 * [buf+begin, buf+bufLen).
 */
template <class Uint>
size_t uintToOctal(char* buffer, size_t bufLen, Uint v) {
  auto& repr = formatOctal;
  // 'v >>= 7, v >>= 2' is no more than a work around to get rid of shift size
  // warning when Uint = uint8_t (it's false as v >= 512 implies sizeof(v) > 1).
  for (; !less_than<unsigned, 512>(v); v >>= 7, v >>= 2) {
    auto b = v & 0x1ff;
    bufLen -= 3;
    buffer[bufLen] = repr[b][0];
    buffer[bufLen + 1] = repr[b][1];
    buffer[bufLen + 2] = repr[b][2];
  }
  buffer[--bufLen] = repr[v][2];
  if (v >= 8) {
    buffer[--bufLen] = repr[v][1];
  }
  if (v >= 64) {
    buffer[--bufLen] = repr[v][0];
  }
  return bufLen;
}

/**
 * Convert an unsigned to binary.
 *
 * Just like folly::detail::uintToBuffer in Conv.h, writes at the *end* of
 * the supplied buffer and returns the offset of the beginning of the string
 * from the start of the buffer.  The formatted string will be in range
 * [buf+begin, buf+bufLen).
 */
template <class Uint>
size_t uintToBinary(char* buffer, size_t bufLen, Uint v) {
  auto& repr = formatBinary;
  if (v == 0) {
    buffer[--bufLen] = '0';
    return bufLen;
  }
  for (; v; v >>= 7, v >>= 1) {
    auto b = v & 0xff;
    bufLen -= 8;
    memcpy(buffer + bufLen, &(repr[b][0]), 8);
  }
  while (buffer[bufLen] == '0') {
    ++bufLen;
  }
  return bufLen;
}

template <bool containerMode, bool RecordUsedArg, class Output>
void baseFormatterCallImpl(
    Output& out,
    size_t nargs,
    const int widths[],
    std::bool_constant<RecordUsedArg>(used)(const BaseFormatterBase&, size_t),
    BaseFormatterBase::DoFormatFn<Output>* const funs[],
    const BaseFormatterBase& base) {
  // Copy raw string (without format specifiers) to output;
  // not as simple as we'd like, as we still need to translate "}}" to "}"
  // and throw if we see any lone "}"
  auto outputString = [&out](StringPiece s) {
    auto p = s.begin();
    auto end = s.end();
    while (p != end) {
      auto q = static_cast<const char*>(memchr(p, '}', size_t(end - p)));
      if (!q) {
        out(StringPiece(p, end));
        break;
      }
      ++q;
      out(StringPiece(p, q));
      p = q;

      if (p == end || *p != '}') {
        throw_exception<BadFormatArg>(
            "folly::format: single '}' in format string");
      }
      ++p;
    }
  };

  auto str_ = base.str_;
  auto p = str_.begin();
  auto end = str_.end();

  int nextArg = 0;
  bool hasDefaultArgIndex = false;
  bool hasExplicitArgIndex = false;
  while (p != end) {
    auto q = static_cast<const char*>(memchr(p, '{', size_t(end - p)));
    if (!q) {
      outputString(StringPiece(p, end));
      break;
    }
    outputString(StringPiece(p, q));
    p = q + 1;

    if (p == end) {
      throw_exception<BadFormatArg>(
          "folly::format: '}' at end of format string");
    }

    // "{{" -> "{"
    if (*p == '{') {
      out(StringPiece(p, 1));
      ++p;
      continue;
    }

    // Format string
    q = static_cast<const char*>(memchr(p, '}', size_t(end - p)));
    if (q == nullptr) {
      throw_exception<BadFormatArg>("folly::format: missing ending '}'");
    }
    FormatArg arg(StringPiece(p, q));
    p = q + 1;

    int argIndex = 0;
    auto piece = arg.splitKey<true>(); // empty key component is okay
    if constexpr (containerMode) {
      arg.enforce(
          arg.width != FormatArg::kDynamicWidth,
          "dynamic field width not supported in vformat()");
      if (piece.empty()) {
        arg.setNextIntKey(nextArg++);
        hasDefaultArgIndex = true;
      } else {
        arg.setNextKey(piece);
        hasExplicitArgIndex = true;
      }
    } else {
      if (piece.empty()) {
        if (arg.width == FormatArg::kDynamicWidth) {
          arg.enforce(
              arg.widthIndex == FormatArg::kNoIndex,
              "cannot provide width arg index without value arg index");
          auto sizeArg = size_t(nextArg++);
          detail::formatCheckIndex(sizeArg, arg, nargs);
          if (RecordUsedArg) {
            used(base, sizeArg);
          }
          auto w = widths[sizeArg];
          arg.enforce(w >= 0, "dynamic field width argument must be integral");
          arg.width = w;
        }

        argIndex = nextArg++;
        hasDefaultArgIndex = true;
      } else {
        if (arg.width == FormatArg::kDynamicWidth) {
          arg.enforce(
              arg.widthIndex != FormatArg::kNoIndex,
              "cannot provide value arg index without width arg index");
          auto sizeArg = size_t(arg.widthIndex);
          detail::formatCheckIndex(sizeArg, arg, nargs);
          if (RecordUsedArg) {
            used(base, sizeArg);
          }
          auto w = widths[sizeArg];
          arg.enforce(w >= 0, "dynamic field width argument must be integral");
          arg.width = w;
        }

        auto result = tryTo<int>(piece);
        arg.enforce(result, "argument index must be integer");
        argIndex = *result;
        arg.enforce(argIndex >= 0, "argument index must be non-negative");
        hasExplicitArgIndex = true;
      }
    }

    if (hasDefaultArgIndex && hasExplicitArgIndex) {
      throw_exception<BadFormatArg>(
          "folly::format: may not have both default and explicit arg indexes");
    }

    if (RecordUsedArg) {
      used(base, argIndex);
    } else {
      formatCheckIndex(argIndex, arg, nargs);
      funs[argIndex](base, arg, out);
    }
  }
}

} // namespace detail

template <class Derived, bool containerMode, size_t... I, class... Args>
template <class Output>
void BaseFormatterImpl<
    Derived,
    containerMode,
    std::index_sequence<I...>,
    Args...>::operator()(Output& out) const {
  constexpr size_t nargs = sizeof...(Args);
  using RecordUsedSizeArgs = decltype(Derived::recordUsedArg(*this, 0));
  constexpr auto used = Derived::recordUsedArg;
  static constexpr auto funs = getDoFormatFnArray<Output>();
  constexpr auto in = unsafe_default_initialized;
  int widths[nargs + 1] = {conditional_t<!alignof(Args), int, int>{in}..., in};
  getSizeArg(widths);
  detail::baseFormatterCallImpl<containerMode, RecordUsedSizeArgs::value>(
      out, nargs, widths, *used, funs.data, *this);
}

namespace format_value {

template <class FormatCallback>
void formatString(StringPiece val, FormatArg& arg, FormatCallback& cb) {
  if (arg.width != FormatArg::kDefaultWidth && arg.width < 0) {
    throw_exception<BadFormatArg>("folly::format: invalid width");
  }
  if (arg.precision != FormatArg::kDefaultPrecision && arg.precision < 0) {
    throw_exception<BadFormatArg>("folly::format: invalid precision");
  }

  if (arg.precision != FormatArg::kDefaultPrecision &&
      val.size() > static_cast<size_t>(arg.precision)) {
    val.reset(val.data(), static_cast<size_t>(arg.precision));
  }

  constexpr int padBufSize = 128;
  char padBuf[padBufSize];

  // Output padding, no more than padBufSize at once
  auto pad = [&padBuf, &cb, padBufSize](int chars) {
    while (chars) {
      int n = std::min(chars, padBufSize);
      cb(StringPiece(padBuf, size_t(n)));
      chars -= n;
    }
  };

  int padRemaining = 0;
  if (arg.width != FormatArg::kDefaultWidth &&
      val.size() < static_cast<size_t>(arg.width)) {
    char fill = arg.fill == FormatArg::kDefaultFill ? ' ' : arg.fill;
    int padChars = static_cast<int>(arg.width - val.size());
    memset(padBuf, fill, size_t(std::min(padBufSize, padChars)));

    FOLLY_PUSH_WARNING
    FOLLY_CLANG_DISABLE_WARNING("-Wcovered-switch-default")
    switch (arg.align) {
      case FormatArg::Align::DEFAULT:
      case FormatArg::Align::LEFT:
        padRemaining = padChars;
        break;
      case FormatArg::Align::CENTER:
        pad(padChars / 2);
        padRemaining = padChars - padChars / 2;
        break;
      case FormatArg::Align::RIGHT:
      case FormatArg::Align::PAD_AFTER_SIGN:
        pad(padChars);
        break;
      case FormatArg::Align::INVALID:
      default:
        abort();
        break;
    }
    FOLLY_POP_WARNING
  }

  cb(val);

  if (padRemaining) {
    pad(padRemaining);
  }
}

template <class FormatCallback>
void formatNumber(
    StringPiece val, int prefixLen, FormatArg& arg, FormatCallback& cb) {
  // precision means something different for numbers
  arg.precision = FormatArg::kDefaultPrecision;
  if (arg.align == FormatArg::Align::DEFAULT) {
    arg.align = FormatArg::Align::RIGHT;
  } else if (prefixLen && arg.align == FormatArg::Align::PAD_AFTER_SIGN) {
    // Split off the prefix, then do any padding if necessary
    cb(val.subpiece(0, size_t(prefixLen)));
    val.advance(size_t(prefixLen));
    arg.width = std::max(arg.width - prefixLen, 0);
  }
  format_value::formatString(val, arg, cb);
}

template <typename FormatCallback>
struct FormatFormatterFn {
  FormatArg& arg;
  FormatCallback& cb;
  void operator()(StringPiece sp) {
    int sz = static_cast<int>(sp.size());
    if (arg.precision != FormatArg::kDefaultPrecision) {
      sz = std::min(arg.precision, sz);
      sp.reset(sp.data(), size_t(sz));
      arg.precision -= sz;
    }
    if (!sp.empty()) {
      cb(sp);
      if (arg.width != FormatArg::kDefaultWidth) {
        arg.width = std::max(arg.width - sz, 0);
      }
    }
  }
};

template <class FormatCallback, bool containerMode, class... Args>
void formatFormatter(
    const Formatter<containerMode, Args...>& formatter,
    FormatArg& arg,
    FormatCallback& cb) {
  if (arg.width == FormatArg::kDefaultWidth &&
      arg.precision == FormatArg::kDefaultPrecision) {
    // nothing to do
    formatter(cb);
  } else if (
      arg.align != FormatArg::Align::LEFT &&
      arg.align != FormatArg::Align::DEFAULT) {
    // We can only avoid creating a temporary string if we align left,
    // as we'd need to know the size beforehand otherwise
    format_value::formatString(formatter.str(), arg, cb);
  } else {
    auto fn = FormatFormatterFn<FormatCallback>{arg, cb};
    formatter(fn);
    if (arg.width != FormatArg::kDefaultWidth && arg.width != 0) {
      // Rely on formatString to do appropriate padding
      format_value::formatString(StringPiece(), arg, cb);
    }
  }
}

} // namespace format_value

// Definitions for default FormatValue classes

// Integral types (except bool)
template <class T>
class FormatValue<
    T,
    typename std::enable_if<
        std::is_integral<T>::value && !std::is_same<T, bool>::value>::type> {
 public:
  explicit FormatValue(T val) : val_(val) {}

  T getValue() const { return val_; }

  template <class FormatCallback>
  void format(FormatArg& arg, FormatCallback& cb) const {
    arg.validate(FormatArg::Type::INTEGER);
    doFormat(arg, cb);
  }

  template <class FormatCallback>
  void doFormat(FormatArg& arg, FormatCallback& cb) const {
    char presentation = arg.presentation;
    if (presentation == FormatArg::kDefaultPresentation) {
      presentation = std::is_same<T, char>::value ? 'c' : 'd';
    }

    // Do all work as unsigned, we'll add the prefix ('0' or '0x' if necessary)
    // and sign ourselves.
    typedef typename std::make_unsigned<T>::type UT;
    UT uval;
    char sign;
    if constexpr (std::is_signed<T>::value) {
      if (folly::is_negative(val_)) {
        // avoid unary negation of unsigned types, which may be warned against
        // avoid ub signed integer overflow, which ubsan checks against
        uval = UT(0 - static_cast<UT>(val_));
        sign = '-';
      } else {
        uval = static_cast<UT>(val_);
        FOLLY_PUSH_WARNING
        FOLLY_CLANG_DISABLE_WARNING("-Wcovered-switch-default")
        switch (arg.sign) {
          case FormatArg::Sign::PLUS_OR_MINUS:
            sign = '+';
            break;
          case FormatArg::Sign::SPACE_OR_MINUS:
            sign = ' ';
            break;
          case FormatArg::Sign::DEFAULT:
          case FormatArg::Sign::MINUS:
          case FormatArg::Sign::INVALID:
          default:
            sign = '\0';
            break;
        }
        FOLLY_POP_WARNING
      }
    } else {
      uval = static_cast<UT>(val_);
      sign = '\0';

      arg.enforce(
          arg.sign == FormatArg::Sign::DEFAULT,
          "sign specifications not allowed for unsigned values");
    }

    // 1 byte for sign, plus max of:
    // #x: two byte "0x" prefix + kMaxHexLength
    // #o: one byte "0" prefix + kMaxOctalLength
    // #b: two byte "0b" prefix + kMaxBinaryLength
    // n: 19 bytes + 1 NUL
    // ,d: 26 bytes (including thousands separators!)
    //
    // Binary format must take the most space, so we use that.
    //
    // Note that we produce a StringPiece rather than NUL-terminating,
    // so we don't need an extra byte for a NUL.
    constexpr size_t valBufSize = 1 + 2 + detail::kMaxBinaryLength;
    char valBuf[valBufSize];
    char* valBufBegin = nullptr;
    char* valBufEnd = nullptr;

    int prefixLen = 0;
    switch (presentation) {
      case 'n': {
        arg.enforce(
            !arg.basePrefix,
            "base prefix not allowed with '",
            presentation,
            "' specifier");

        arg.enforce(
            !arg.thousandsSeparator,
            "cannot use ',' with the '",
            presentation,
            "' specifier");

        valBufBegin = valBuf + 1; // room for sign
#if defined(__ANDROID__)
        int len = snprintf(
            valBufBegin,
            (valBuf + valBufSize) - valBufBegin,
            "%" PRIuMAX,
            static_cast<uintmax_t>(uval));
#else
        int len = snprintf(
            valBufBegin,
            size_t((valBuf + valBufSize) - valBufBegin),
            "%ju",
            static_cast<uintmax_t>(uval));
#endif
        // valBufSize should always be big enough, so this should never
        // happen.
        assert(len < valBuf + valBufSize - valBufBegin);
        valBufEnd = valBufBegin + len;
        break;
      }
      case 'd':
        arg.enforce(
            !arg.basePrefix,
            "base prefix not allowed with '",
            presentation,
            "' specifier");
        valBufBegin = valBuf + 1; // room for sign

        // Use to_ascii_decimal, faster than sprintf
        valBufEnd = valBufBegin +
            to_ascii_decimal(valBufBegin, valBuf + sizeof(valBuf), uval);
        if (arg.thousandsSeparator) {
          detail::insertThousandsGroupingUnsafe(valBufBegin, &valBufEnd);
        }
        break;
      case 'c':
        arg.enforce(
            !arg.basePrefix,
            "base prefix not allowed with '",
            presentation,
            "' specifier");
        arg.enforce(
            !arg.thousandsSeparator,
            "thousands separator (',') not allowed with '",
            presentation,
            "' specifier");
        valBufBegin = valBuf + 1; // room for sign
        *valBufBegin = static_cast<char>(uval);
        valBufEnd = valBufBegin + 1;
        break;
      case 'o':
      case 'O':
        arg.enforce(
            !arg.thousandsSeparator,
            "thousands separator (',') not allowed with '",
            presentation,
            "' specifier");
        valBufEnd = valBuf + valBufSize;
        valBufBegin = &valBuf[detail::uintToOctal(valBuf, valBufSize, uval)];
        if (arg.basePrefix) {
          *--valBufBegin = '0';
          prefixLen = 1;
        }
        break;
      case 'x':
        arg.enforce(
            !arg.thousandsSeparator,
            "thousands separator (',') not allowed with '",
            presentation,
            "' specifier");
        valBufEnd = valBuf + valBufSize;
        valBufBegin = &valBuf[detail::uintToHexLower(valBuf, valBufSize, uval)];
        if (arg.basePrefix) {
          *--valBufBegin = 'x';
          *--valBufBegin = '0';
          prefixLen = 2;
        }
        break;
      case 'X':
        arg.enforce(
            !arg.thousandsSeparator,
            "thousands separator (',') not allowed with '",
            presentation,
            "' specifier");
        valBufEnd = valBuf + valBufSize;
        valBufBegin = &valBuf[detail::uintToHexUpper(valBuf, valBufSize, uval)];
        if (arg.basePrefix) {
          *--valBufBegin = 'X';
          *--valBufBegin = '0';
          prefixLen = 2;
        }
        break;
      case 'b':
      case 'B':
        arg.enforce(
            !arg.thousandsSeparator,
            "thousands separator (',') not allowed with '",
            presentation,
            "' specifier");
        valBufEnd = valBuf + valBufSize;
        valBufBegin = &valBuf[detail::uintToBinary(valBuf, valBufSize, uval)];
        if (arg.basePrefix) {
          *--valBufBegin = presentation; // 0b or 0B
          *--valBufBegin = '0';
          prefixLen = 2;
        }
        break;
      default:
        arg.error("invalid specifier '", presentation, "'");
    }

    if (sign) {
      *--valBufBegin = sign;
      ++prefixLen;
    }

    format_value::formatNumber(
        StringPiece(valBufBegin, valBufEnd), prefixLen, arg, cb);
  }

 private:
  T val_;
};

// Bool
template <>
class FormatValue<bool> {
 public:
  explicit FormatValue(bool val) : val_(val) {}

  template <class FormatCallback>
  void format(FormatArg& arg, FormatCallback& cb) const {
    if (arg.presentation == FormatArg::kDefaultPresentation) {
      arg.validate(FormatArg::Type::OTHER);
      format_value::formatString(val_ ? "true" : "false", arg, cb);
    } else { // number
      FormatValue<int>(val_).format(arg, cb);
    }
  }

 private:
  bool val_;
};

// double
template <>
class FormatValue<double> {
 public:
  explicit FormatValue(double val) : val_(val) {}

  template <class FormatCallback>
  void format(FormatArg& arg, FormatCallback& cb) const {
    fbstring piece;
    int prefixLen;
    formatHelper(piece, prefixLen, arg);
    format_value::formatNumber(piece, prefixLen, arg, cb);
  }

 private:
  void formatHelper(fbstring& piece, int& prefixLen, FormatArg& arg) const;

  double val_;
};

// float (defer to double)
template <>
class FormatValue<float> {
 public:
  explicit FormatValue(float val) : val_(val) {}

  template <class FormatCallback>
  void format(FormatArg& arg, FormatCallback& cb) const {
    FormatValue<double>(val_).format(arg, cb);
  }

 private:
  float val_;
};

// String-y types (implicitly convertible to StringPiece, except char*)
template <class T>
class FormatValue<
    T,
    typename std::enable_if<
        (!std::is_pointer<T>::value ||
         !std::is_same<
             char,
             typename std::decay<typename std::remove_pointer<T>::type>::type>::
             value) &&
        std::is_convertible<T, StringPiece>::value>::type> {
 public:
  explicit FormatValue(StringPiece val) : val_(val) {}

  template <class FormatCallback>
  void format(FormatArg& arg, FormatCallback& cb) const {
    if (arg.keyEmpty()) {
      arg.validate(FormatArg::Type::OTHER);
      arg.enforce(
          arg.presentation == FormatArg::kDefaultPresentation ||
              arg.presentation == 's',
          "invalid specifier '",
          arg.presentation,
          "'");
      format_value::formatString(val_, arg, cb);
    } else {
      FormatValue<char>(val_.at(size_t(arg.splitIntKey()))).format(arg, cb);
    }
  }

 private:
  StringPiece val_;
};

// Null
template <>
class FormatValue<std::nullptr_t> {
 public:
  explicit FormatValue(std::nullptr_t) {}

  template <class FormatCallback>
  void format(FormatArg& arg, FormatCallback& cb) const {
    arg.validate(FormatArg::Type::OTHER);
    arg.enforce(
        arg.presentation == FormatArg::kDefaultPresentation,
        "invalid specifier '",
        arg.presentation,
        "'");
    format_value::formatString("(null)", arg, cb);
  }
};

// Partial specialization of FormatValue for char*
template <class T>
class FormatValue<
    T*,
    typename std::enable_if<
        std::is_same<char, typename std::decay<T>::type>::value>::type> {
 public:
  explicit FormatValue(T* val) : val_(val) {}

  template <class FormatCallback>
  void format(FormatArg& arg, FormatCallback& cb) const {
    if (arg.keyEmpty()) {
      if (!val_) {
        FormatValue<std::nullptr_t>(nullptr).format(arg, cb);
      } else {
        FormatValue<StringPiece>(val_).format(arg, cb);
      }
    } else {
      FormatValue<typename std::decay<T>::type>(val_[arg.splitIntKey()])
          .format(arg, cb);
    }
  }

 private:
  T* val_;
};

// Partial specialization of FormatValue for void*
template <class T>
class FormatValue<
    T*,
    typename std::enable_if<
        std::is_same<void, typename std::decay<T>::type>::value>::type> {
 public:
  explicit FormatValue(T* val) : val_(val) {}

  template <class FormatCallback>
  void format(FormatArg& arg, FormatCallback& cb) const {
    if (!val_) {
      FormatValue<std::nullptr_t>(nullptr).format(arg, cb);
    } else {
      // Print as a pointer, in hex.
      arg.validate(FormatArg::Type::OTHER);
      arg.enforce(
          arg.presentation == FormatArg::kDefaultPresentation,
          "invalid specifier '",
          arg.presentation,
          "'");
      arg.basePrefix = true;
      arg.presentation = 'x';
      if (arg.align == FormatArg::Align::DEFAULT) {
        arg.align = FormatArg::Align::LEFT;
      }
      FormatValue<uintptr_t>(reinterpret_cast<uintptr_t>(val_))
          .doFormat(arg, cb);
    }
  }

 private:
  T* val_;
};

template <class T, class = void>
class TryFormatValue {
 public:
  template <class FormatCallback>
  static void formatOrFail(
      T& /* value */, FormatArg& arg, FormatCallback& /* cb */) {
    arg.error("No formatter available for this type");
  }
};

template <class T>
class TryFormatValue<
    T,
    typename std::enable_if<
        0 < sizeof(FormatValue<typename std::decay<T>::type>)>::type> {
 public:
  template <class FormatCallback>
  static void formatOrFail(T& value, FormatArg& arg, FormatCallback& cb) {
    FormatValue<typename std::decay<T>::type>(value).format(arg, cb);
  }
};

// Partial specialization of FormatValue for other pointers
template <class T>
class FormatValue<
    T*,
    typename std::enable_if<
        !std::is_same<char, typename std::decay<T>::type>::value &&
        !std::is_same<void, typename std::decay<T>::type>::value>::type> {
 public:
  explicit FormatValue(T* val) : val_(val) {}

  template <class FormatCallback>
  void format(FormatArg& arg, FormatCallback& cb) const {
    if (arg.keyEmpty()) {
      FormatValue<void*>((void*)val_).format(arg, cb);
    } else {
      TryFormatValue<T>::formatOrFail(val_[arg.splitIntKey()], arg, cb);
    }
  }

 private:
  T* val_;
};

namespace detail {

// std::array
template <class T, size_t N>
struct IndexableTraits<std::array<T, N>>
    : public IndexableTraitsSeq<std::array<T, N>> {};

// std::vector
template <class T, class A>
struct IndexableTraits<std::vector<T, A>>
    : public IndexableTraitsSeq<std::vector<T, A>> {};

// std::deque
template <class T, class A>
struct IndexableTraits<std::deque<T, A>>
    : public IndexableTraitsSeq<std::deque<T, A>> {};

// std::map with integral keys
template <class K, class T, class C, class A>
struct IndexableTraits<
    std::map<K, T, C, A>,
    typename std::enable_if<std::is_integral<K>::value>::type>
    : public IndexableTraitsAssoc<std::map<K, T, C, A>> {};

// std::unordered_map with integral keys
template <class K, class T, class H, class E, class A>
struct IndexableTraits<
    std::unordered_map<K, T, H, E, A>,
    typename std::enable_if<std::is_integral<K>::value>::type>
    : public IndexableTraitsAssoc<std::unordered_map<K, T, H, E, A>> {};

} // namespace detail

// Partial specialization of FormatValue for integer-indexable containers
template <class T>
class FormatValue<T, typename detail::IndexableTraits<T>::enabled> {
 public:
  explicit FormatValue(const T& val) : val_(val) {}

  template <class FormatCallback>
  void format(FormatArg& arg, FormatCallback& cb) const {
    FormatValue<typename std::decay<
        typename detail::IndexableTraits<T>::value_type>::type>(
        detail::IndexableTraits<T>::at(val_, arg.splitIntKey()))
        .format(arg, cb);
  }

 private:
  const T& val_;
};

template <class Container, class Value>
class FormatValue<
    detail::DefaultValueWrapper<Container, Value>,
    typename detail::IndexableTraits<Container>::enabled> {
 public:
  explicit FormatValue(const detail::DefaultValueWrapper<Container, Value>& val)
      : val_(val) {}

  template <class FormatCallback>
  void format(FormatArg& arg, FormatCallback& cb) const {
    FormatValue<typename std::decay<
        typename detail::IndexableTraits<Container>::value_type>::type>(
        detail::IndexableTraits<Container>::at(
            val_.container, arg.splitIntKey(), val_.defaultValue))
        .format(arg, cb);
  }

 private:
  const detail::DefaultValueWrapper<Container, Value>& val_;
};

namespace detail {

// Define enabled, key_type, convert from StringPiece to the key types
// that we support
template <class T>
struct KeyFromStringPiece;

// std::string
template <>
struct KeyFromStringPiece<std::string> : public FormatTraitsBase {
  typedef std::string key_type;
  static std::string convert(StringPiece s) { return s.toString(); }
  typedef void enabled;
};

// fbstring
template <>
struct KeyFromStringPiece<fbstring> : public FormatTraitsBase {
  typedef fbstring key_type;
  static fbstring convert(StringPiece s) { return s.to<fbstring>(); }
};

// StringPiece
template <>
struct KeyFromStringPiece<StringPiece> : public FormatTraitsBase {
  typedef StringPiece key_type;
  static StringPiece convert(StringPiece s) { return s; }
};

// Base class for associative types keyed by strings
template <class T>
struct KeyableTraitsAssoc : public FormatTraitsBase {
  typedef typename T::key_type key_type;
  typedef typename T::value_type::second_type value_type;
  static const value_type& at(const T& map, StringPiece key) {
    if (auto ptr = get_ptr(map, KeyFromStringPiece<key_type>::convert(key))) {
      return *ptr;
    }
    throw_exception<FormatKeyNotFoundException>(key);
  }
  static const value_type& at(
      const T& map, StringPiece key, const value_type& dflt) {
    auto pos = map.find(KeyFromStringPiece<key_type>::convert(key));
    return pos != map.end() ? pos->second : dflt;
  }
};

// Define enabled, key_type, value_type, at() for supported string-keyed
// types
template <class T, class Enabled = void>
struct KeyableTraits;

// std::map with string key
template <class K, class T, class C, class A>
struct KeyableTraits<
    std::map<K, T, C, A>,
    typename KeyFromStringPiece<K>::enabled>
    : public KeyableTraitsAssoc<std::map<K, T, C, A>> {};

// std::unordered_map with string key
template <class K, class T, class H, class E, class A>
struct KeyableTraits<
    std::unordered_map<K, T, H, E, A>,
    typename KeyFromStringPiece<K>::enabled>
    : public KeyableTraitsAssoc<std::unordered_map<K, T, H, E, A>> {};

} // namespace detail

// Partial specialization of FormatValue for string-keyed containers
template <class T>
class FormatValue<T, typename detail::KeyableTraits<T>::enabled> {
 public:
  explicit FormatValue(const T& val) : val_(val) {}

  template <class FormatCallback>
  void format(FormatArg& arg, FormatCallback& cb) const {
    FormatValue<typename std::decay<
        typename detail::KeyableTraits<T>::value_type>::type>(
        detail::KeyableTraits<T>::at(val_, arg.splitKey()))
        .format(arg, cb);
  }

 private:
  const T& val_;
};

template <class Container, class Value>
class FormatValue<
    detail::DefaultValueWrapper<Container, Value>,
    typename detail::KeyableTraits<Container>::enabled> {
 public:
  explicit FormatValue(const detail::DefaultValueWrapper<Container, Value>& val)
      : val_(val) {}

  template <class FormatCallback>
  void format(FormatArg& arg, FormatCallback& cb) const {
    FormatValue<typename std::decay<
        typename detail::KeyableTraits<Container>::value_type>::type>(
        detail::KeyableTraits<Container>::at(
            val_.container, arg.splitKey(), val_.defaultValue))
        .format(arg, cb);
  }

 private:
  const detail::DefaultValueWrapper<Container, Value>& val_;
};

// Partial specialization of FormatValue for pairs
template <class A, class B>
class FormatValue<std::pair<A, B>> {
 public:
  explicit FormatValue(const std::pair<A, B>& val) : val_(val) {}

  template <class FormatCallback>
  void format(FormatArg& arg, FormatCallback& cb) const {
    int key = arg.splitIntKey();
    switch (key) {
      case 0:
        FormatValue<typename std::decay<A>::type>(val_.first).format(arg, cb);
        break;
      case 1:
        FormatValue<typename std::decay<B>::type>(val_.second).format(arg, cb);
        break;
      default:
        arg.error("invalid index for pair");
    }
  }

 private:
  const std::pair<A, B>& val_;
};

// Partial specialization of FormatValue for tuples
template <class... Args>
class FormatValue<std::tuple<Args...>> {
  typedef std::tuple<Args...> Tuple;

 public:
  explicit FormatValue(const Tuple& val) : val_(val) {}

  template <class FormatCallback>
  void format(FormatArg& arg, FormatCallback& cb) const {
    int key = arg.splitIntKey();
    arg.enforce(key >= 0, "tuple index must be non-negative");
    doFormat(size_t(key), arg, cb);
  }

 private:
  template <size_t K>
  using FV = FormatValue<
      typename std::decay<typename std::tuple_element<K, Tuple>::type>::type>;

  template <class Callback, size_t... I>
  void doFormat(
      size_t i, FormatArg& arg, Callback& cb, std::index_sequence<I...>) const {
    detail::formatCheckIndex(i, arg, sizeof...(Args));
    ((i == I && (FV<I>(std::get<I>(val_)).format(arg, cb), 0)), ...);
  }
  template <class Callback>
  void doFormat(size_t i, FormatArg& arg, Callback& cb) const {
    return doFormat(i, arg, cb, std::index_sequence_for<Args...>{});
  }

  const Tuple& val_;
};

// Partial specialization of FormatValue for nested Formatters
template <bool containerMode, class... Args, template <bool, class...> class F>
class FormatValue<
    F<containerMode, Args...>,
    typename std::enable_if<
        detail::IsFormatter<F<containerMode, Args...>>::value>::type> {
  typedef F<containerMode, Args...> FormatterValue;

 public:
  explicit FormatValue(const FormatterValue& f) : f_(f) {}

  template <class FormatCallback>
  void format(FormatArg& arg, FormatCallback& cb) const {
    format_value::formatFormatter(f_, arg, cb);
  }

 private:
  const FormatterValue& f_;
};

/**
 * Formatter objects can be appended to strings, and therefore they're
 * compatible with folly::toAppend and folly::to.
 */
template <class Tgt, bool containerMode, class... Args>
typename std::enable_if<IsSomeString<Tgt>::value>::type toAppend(
    const Formatter<containerMode, Args...>& value, Tgt* result) {
  value.appendTo(*result);
}

} // namespace folly

FOLLY_POP_WARNING
