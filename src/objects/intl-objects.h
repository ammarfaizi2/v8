// Copyright 2013 the V8 project authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef V8_INTL_SUPPORT
#error Internationalization is expected to be enabled.
#endif  // V8_INTL_SUPPORT

#ifndef V8_OBJECTS_INTL_OBJECTS_H_
#define V8_OBJECTS_INTL_OBJECTS_H_

#include <map>
#include <memory>
#include <set>
#include <string>

#include "src/base/timezone-cache.h"
#include "src/objects/contexts.h"
#include "src/objects/managed.h"
#include "src/objects/objects.h"
#include "unicode/locid.h"
#include "unicode/uversion.h"

#define V8_MINIMUM_ICU_VERSION 69

namespace U_ICU_NAMESPACE {
class BreakIterator;
class Collator;
class FormattedValue;
class StringEnumeration;
class TimeZone;
class UnicodeString;
}  // namespace U_ICU_NAMESPACE

namespace v8 {
namespace internal {

template <typename T>
class Handle;
class JSCollator;

class Intl {
 public:
  enum class BoundFunctionContextSlot {
    kBoundFunction = Context::MIN_CONTEXT_SLOTS,
    kLength
  };

  // Build a set of ICU locales from a list of Locales. If there is a locale
  // with a script tag then the locales also include a locale without the
  // script; eg, pa_Guru_IN (language=Panjabi, script=Gurmukhi, country-India)
  // would include pa_IN.
  static std::set<std::string> BuildLocaleSet(
      const std::vector<std::string>& locales, const char* path,
      const char* validate_key);

  static Maybe<std::string> ToLanguageTag(const icu::Locale& locale);

  // Get the name of the numbering system from locale.
  // ICU doesn't expose numbering system in any way, so we have to assume that
  // for given locale NumberingSystem constructor produces the same digits as
  // NumberFormat/Calendar would.
  static std::string GetNumberingSystem(const icu::Locale& icu_locale);

  static V8_WARN_UNUSED_RESULT MaybeHandle<JSObject> SupportedLocalesOf(
      Isolate* isolate, const char* method_name,
      const std::set<std::string>& available_locales, Handle<Object> locales_in,
      Handle<Object> options_in);

  // https://tc39.github.io/ecma402/#sec-canonicalizelocalelist
  // {only_return_one_result} is an optimization for callers that only
  // care about the first result.
  static Maybe<std::vector<std::string>> CanonicalizeLocaleList(
      Isolate* isolate, Handle<Object> locales,
      bool only_return_one_result = false);

  // ecma-402 #sec-intl.getcanonicallocales
  V8_WARN_UNUSED_RESULT static MaybeHandle<JSArray> GetCanonicalLocales(
      Isolate* isolate, Handle<Object> locales);

  // ecma-402 #sec-intl.supportedvaluesof
  V8_WARN_UNUSED_RESULT static MaybeHandle<JSArray> SupportedValuesOf(
      Isolate* isolate, Handle<Object> key);

  // For locale sensitive functions
  V8_WARN_UNUSED_RESULT static MaybeHandle<String> StringLocaleConvertCase(
      Isolate* isolate, Handle<String> s, bool is_upper,
      Handle<Object> locales);

  V8_WARN_UNUSED_RESULT static MaybeHandle<String> ConvertToUpper(
      Isolate* isolate, Handle<String> s);

  V8_WARN_UNUSED_RESULT static MaybeHandle<String> ConvertToLower(
      Isolate* isolate, Handle<String> s);

  V8_WARN_UNUSED_RESULT static base::Optional<int> StringLocaleCompare(
      Isolate* isolate, Handle<String> s1, Handle<String> s2,
      Handle<Object> locales, Handle<Object> options, const char* method_name);

  enum class CompareStringsOptions {
    kNone,
    kTryFastPath,
  };
  template <class IsolateT>
  V8_EXPORT_PRIVATE static CompareStringsOptions CompareStringsOptionsFor(
      IsolateT* isolate, Handle<Object> locales, Handle<Object> options);
  V8_EXPORT_PRIVATE V8_WARN_UNUSED_RESULT static int CompareStrings(
      Isolate* isolate, const icu::Collator& collator, Handle<String> s1,
      Handle<String> s2,
      CompareStringsOptions compare_strings_options =
          CompareStringsOptions::kNone);

  // ecma402/#sup-properties-of-the-number-prototype-object
  V8_WARN_UNUSED_RESULT static MaybeHandle<String> NumberToLocaleString(
      Isolate* isolate, Handle<Object> num, Handle<Object> locales,
      Handle<Object> options, const char* method_name);

  // ecma402/#sec-setnfdigitoptions
  struct NumberFormatDigitOptions {
    int minimum_integer_digits;
    int minimum_fraction_digits;
    int maximum_fraction_digits;
    int minimum_significant_digits;
    int maximum_significant_digits;
  };
  V8_WARN_UNUSED_RESULT static Maybe<NumberFormatDigitOptions>
  SetNumberFormatDigitOptions(Isolate* isolate, Handle<JSReceiver> options,
                              int mnfd_default, int mxfd_default,
                              bool notation_is_compact);

  // Helper function to convert a UnicodeString to a Handle<String>
  V8_WARN_UNUSED_RESULT static MaybeHandle<String> ToString(
      Isolate* isolate, const icu::UnicodeString& string);

  // Helper function to convert a substring of UnicodeString to a Handle<String>
  V8_WARN_UNUSED_RESULT static MaybeHandle<String> ToString(
      Isolate* isolate, const icu::UnicodeString& string, int32_t begin,
      int32_t end);

  // Helper function to convert a FormattedValue to String
  V8_WARN_UNUSED_RESULT static MaybeHandle<String> FormattedToString(
      Isolate* isolate, const icu::FormattedValue& formatted);

  // Helper function to convert number field id to type string.
  static Handle<String> NumberFieldToType(Isolate* isolate,
                                          Handle<Object> numeric_obj,
                                          int32_t field_id);

  // A helper function to implement formatToParts which add element to array as
  // $array[$index] = { type: $field_type_string, value: $value }
  static void AddElement(Isolate* isolate, Handle<JSArray> array, int index,
                         Handle<String> field_type_string,
                         Handle<String> value);

  // A helper function to implement formatToParts which add element to array as
  // $array[$index] = {
  //   type: $field_type_string, value: $value,
  //   $additional_property_name: $additional_property_value
  // }
  static void AddElement(Isolate* isolate, Handle<JSArray> array, int index,
                         Handle<String> field_type_string, Handle<String> value,
                         Handle<String> additional_property_name,
                         Handle<String> additional_property_value);

  // In ECMA 402 v1, Intl constructors supported a mode of operation
  // where calling them with an existing object as a receiver would
  // transform the receiver into the relevant Intl instance with all
  // internal slots. In ECMA 402 v2, this capability was removed, to
  // avoid adding internal slots on existing objects. In ECMA 402 v3,
  // the capability was re-added as "normative optional" in a mode
  // which chains the underlying Intl instance on any object, when the
  // constructor is called
  //
  // See ecma402/#legacy-constructor.
  V8_WARN_UNUSED_RESULT static MaybeHandle<Object> LegacyUnwrapReceiver(
      Isolate* isolate, Handle<JSReceiver> receiver,
      Handle<JSFunction> constructor, bool has_initialized_slot);

  // enum for "localeMatcher" option: shared by many Intl objects.
  enum class MatcherOption { kBestFit, kLookup };

  // Shared function to read the "localeMatcher" option.
  V8_WARN_UNUSED_RESULT static Maybe<MatcherOption> GetLocaleMatcher(
      Isolate* isolate, Handle<JSReceiver> options, const char* method_name);

  // Shared function to read the "numberingSystem" option.
  V8_WARN_UNUSED_RESULT static Maybe<bool> GetNumberingSystem(
      Isolate* isolate, Handle<JSReceiver> options, const char* method_name,
      std::unique_ptr<char[]>* result);

  // Check the calendar is valid or not for that locale.
  static bool IsValidCalendar(const icu::Locale& locale,
                              const std::string& value);

  // Check the collation is valid or not for that locale.
  static bool IsValidCollation(const icu::Locale& locale,
                               const std::string& value);

  // Check the numberingSystem is valid.
  static bool IsValidNumberingSystem(const std::string& value);

  // Check the calendar is well formed.
  static bool IsWellFormedCalendar(const std::string& value);

  // Check the currency is well formed.
  static bool IsWellFormedCurrency(const std::string& value);

  struct ResolvedLocale {
    std::string locale;
    icu::Locale icu_locale;
    std::map<std::string, std::string> extensions;
  };

  static Maybe<ResolvedLocale> ResolveLocale(
      Isolate* isolate, const std::set<std::string>& available_locales,
      const std::vector<std::string>& requested_locales, MatcherOption options,
      const std::set<std::string>& relevant_extension_keys);

  // A helper template to implement the GetAvailableLocales
  // Usage in src/objects/js-XXX.cc
  // const std::set<std::string>& JSXxx::GetAvailableLocales() {
  //   static base::LazyInstance<Intl::AvailableLocales<icu::YYY>>::type
  //       available_locales = LAZY_INSTANCE_INITIALIZER;
  //   return available_locales.Pointer()->Get();
  // }

  struct SkipResourceCheck {
    static const char* key() { return nullptr; }
    static const char* path() { return nullptr; }
  };

  template <typename C = SkipResourceCheck>
  class AvailableLocales {
   public:
    AvailableLocales() {
      UErrorCode status = U_ZERO_ERROR;
      UEnumeration* uenum =
          uloc_openAvailableByType(ULOC_AVAILABLE_WITH_LEGACY_ALIASES, &status);
      DCHECK(U_SUCCESS(status));

      std::vector<std::string> all_locales;
      const char* loc;
      while ((loc = uenum_next(uenum, nullptr, &status)) != nullptr) {
        DCHECK(U_SUCCESS(status));
        std::string locstr(loc);
        std::replace(locstr.begin(), locstr.end(), '_', '-');
        // Handle special case
        if (locstr == "en-US-POSIX") locstr = "en-US-u-va-posix";
        all_locales.push_back(locstr);
      }
      uenum_close(uenum);

      set_ = Intl::BuildLocaleSet(all_locales, C::path(), C::key());
    }
    const std::set<std::string>& Get() const { return set_; }

   private:
    std::set<std::string> set_;
  };

  // Utility function to set text to BreakIterator.
  static Handle<Managed<icu::UnicodeString>> SetTextToBreakIterator(
      Isolate* isolate, Handle<String> text,
      icu::BreakIterator* break_iterator);

  // ecma262 #sec-string.prototype.normalize
  V8_WARN_UNUSED_RESULT static MaybeHandle<String> Normalize(
      Isolate* isolate, Handle<String> string, Handle<Object> form_input);
  static base::TimezoneCache* CreateTimeZoneCache();

  // Convert a Handle<String> to icu::UnicodeString
  static icu::UnicodeString ToICUUnicodeString(Isolate* isolate,
                                               Handle<String> string,
                                               int offset = 0);

  static const uint8_t* ToLatin1LowerTable();

  static const uint8_t* AsciiCollationWeightsL1();
  static const uint8_t* AsciiCollationWeightsL3();
  static const int kAsciiCollationWeightsLength;

  static String ConvertOneByteToLower(String src, String dst);

  static const std::set<std::string>& GetAvailableLocales();

  static const std::set<std::string>& GetAvailableLocalesForDateFormat();

  V8_WARN_UNUSED_RESULT static MaybeHandle<JSArray> ToJSArray(
      Isolate* isolate, const char* unicode_key,
      icu::StringEnumeration* enumeration,
      const std::function<bool(const char*)>& removes, bool sort);

  static bool RemoveCollation(const char* collation);

  static std::set<std::string> SanctionedSimpleUnits();

  V8_WARN_UNUSED_RESULT static MaybeHandle<JSArray> AvailableCalendars(
      Isolate* isolate);

  V8_WARN_UNUSED_RESULT static bool IsValidTimeZoneName(
      const icu::TimeZone& tz);
  V8_WARN_UNUSED_RESULT static bool IsValidTimeZoneName(Isolate* isolate,
                                                        const std::string& id);
  V8_WARN_UNUSED_RESULT static bool IsValidTimeZoneName(Isolate* isolate,
                                                        Handle<String> id);

  // Function to support Temporal
  V8_WARN_UNUSED_RESULT static std::string TimeZoneIdFromIndex(int32_t index);

  V8_WARN_UNUSED_RESULT static Maybe<bool> GetTimeZoneIndex(
      Isolate* isolate, Handle<String> identifier, int32_t* index);

  V8_WARN_UNUSED_RESULT static MaybeHandle<String> CanonicalizeTimeZoneName(
      Isolate* isolate, Handle<String> identifier);
};

}  // namespace internal
}  // namespace v8

#endif  // V8_OBJECTS_INTL_OBJECTS_H_
