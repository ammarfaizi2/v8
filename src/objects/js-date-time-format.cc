// Copyright 2018 the V8 project authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef V8_INTL_SUPPORT
#error Internationalization is expected to be enabled.
#endif  // V8_INTL_SUPPORT

#include "src/objects/js-date-time-format.h"

#include <algorithm>
#include <map>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "src/date/date.h"
#include "src/execution/isolate.h"
#include "src/heap/factory.h"
#include "src/objects/intl-objects.h"
#include "src/objects/js-date-time-format-inl.h"
#include "src/objects/managed-inl.h"
#include "src/objects/option-utils.h"
#include "unicode/calendar.h"
#include "unicode/dtitvfmt.h"
#include "unicode/dtptngen.h"
#include "unicode/fieldpos.h"
#include "unicode/gregocal.h"
#include "unicode/smpdtfmt.h"
#include "unicode/unistr.h"

namespace v8 {
namespace internal {

namespace {

std::string ToHourCycleString(JSDateTimeFormat::HourCycle hc) {
  switch (hc) {
    case JSDateTimeFormat::HourCycle::kH11:
      return "h11";
    case JSDateTimeFormat::HourCycle::kH12:
      return "h12";
    case JSDateTimeFormat::HourCycle::kH23:
      return "h23";
    case JSDateTimeFormat::HourCycle::kH24:
      return "h24";
    case JSDateTimeFormat::HourCycle::kUndefined:
      return "";
    default:
      UNREACHABLE();
  }
}

JSDateTimeFormat::HourCycle ToHourCycle(const std::string& hc) {
  if (hc == "h11") return JSDateTimeFormat::HourCycle::kH11;
  if (hc == "h12") return JSDateTimeFormat::HourCycle::kH12;
  if (hc == "h23") return JSDateTimeFormat::HourCycle::kH23;
  if (hc == "h24") return JSDateTimeFormat::HourCycle::kH24;
  return JSDateTimeFormat::HourCycle::kUndefined;
}

JSDateTimeFormat::HourCycle ToHourCycle(UDateFormatHourCycle hc) {
  switch (hc) {
    case UDAT_HOUR_CYCLE_11:
      return JSDateTimeFormat::HourCycle::kH11;
    case UDAT_HOUR_CYCLE_12:
      return JSDateTimeFormat::HourCycle::kH12;
    case UDAT_HOUR_CYCLE_23:
      return JSDateTimeFormat::HourCycle::kH23;
    case UDAT_HOUR_CYCLE_24:
      return JSDateTimeFormat::HourCycle::kH24;
    default:
      return JSDateTimeFormat::HourCycle::kUndefined;
  }
}

Maybe<JSDateTimeFormat::HourCycle> GetHourCycle(Isolate* isolate,
                                                Handle<JSReceiver> options,
                                                const char* method_name) {
  return GetStringOption<JSDateTimeFormat::HourCycle>(
      isolate, options, "hourCycle", method_name, {"h11", "h12", "h23", "h24"},
      {JSDateTimeFormat::HourCycle::kH11, JSDateTimeFormat::HourCycle::kH12,
       JSDateTimeFormat::HourCycle::kH23, JSDateTimeFormat::HourCycle::kH24},
      JSDateTimeFormat::HourCycle::kUndefined);
}

class PatternMap {
 public:
  PatternMap(std::string pattern, std::string value)
      : pattern(std::move(pattern)), value(std::move(value)) {}
  virtual ~PatternMap() = default;
  std::string pattern;
  std::string value;
};

class PatternItem {
 public:
  PatternItem(const std::string property, std::vector<PatternMap> pairs,
              std::vector<const char*> allowed_values)
      : property(std::move(property)),
        pairs(std::move(pairs)),
        allowed_values(allowed_values) {}
  virtual ~PatternItem() = default;

  const std::string property;
  // It is important for the pattern in the pairs from longer one to shorter one
  // if the longer one contains substring of an shorter one.
  std::vector<PatternMap> pairs;
  std::vector<const char*> allowed_values;
};

static std::vector<PatternItem> BuildPatternItems() {
  const std::vector<const char*> kLongShort = {"long", "short"};
  const std::vector<const char*> kNarrowLongShort = {"narrow", "long", "short"};
  const std::vector<const char*> k2DigitNumeric = {"2-digit", "numeric"};
  const std::vector<const char*> kNarrowLongShort2DigitNumeric = {
      "narrow", "long", "short", "2-digit", "numeric"};
  std::vector<PatternItem> items = {
      PatternItem("weekday",
                  {{"EEEEE", "narrow"},
                   {"EEEE", "long"},
                   {"EEE", "short"},
                   {"ccccc", "narrow"},
                   {"cccc", "long"},
                   {"ccc", "short"}},
                  kNarrowLongShort),
      PatternItem("era",
                  {{"GGGGG", "narrow"}, {"GGGG", "long"}, {"GGG", "short"}},
                  kNarrowLongShort),
      PatternItem("year", {{"yy", "2-digit"}, {"y", "numeric"}},
                  k2DigitNumeric)};
  // Sometimes we get L instead of M for month - standalone name.
  items.push_back(PatternItem("month",
                              {{"MMMMM", "narrow"},
                               {"MMMM", "long"},
                               {"MMM", "short"},
                               {"MM", "2-digit"},
                               {"M", "numeric"},
                               {"LLLLL", "narrow"},
                               {"LLLL", "long"},
                               {"LLL", "short"},
                               {"LL", "2-digit"},
                               {"L", "numeric"}},
                              kNarrowLongShort2DigitNumeric));
  items.push_back(PatternItem("day", {{"dd", "2-digit"}, {"d", "numeric"}},
                              k2DigitNumeric));
  items.push_back(PatternItem("dayPeriod",
                              {{"BBBBB", "narrow"},
                               {"bbbbb", "narrow"},
                               {"BBBB", "long"},
                               {"bbbb", "long"},
                               {"B", "short"},
                               {"b", "short"}},
                              kNarrowLongShort));
  items.push_back(PatternItem("hour",
                              {{"HH", "2-digit"},
                               {"H", "numeric"},
                               {"hh", "2-digit"},
                               {"h", "numeric"},
                               {"kk", "2-digit"},
                               {"k", "numeric"},
                               {"KK", "2-digit"},
                               {"K", "numeric"}},
                              k2DigitNumeric));
  items.push_back(PatternItem("minute", {{"mm", "2-digit"}, {"m", "numeric"}},
                              k2DigitNumeric));
  items.push_back(PatternItem("second", {{"ss", "2-digit"}, {"s", "numeric"}},
                              k2DigitNumeric));

    const std::vector<const char*> kTimezone = {"long",        "short",
                                                "longOffset",  "shortOffset",
                                                "longGeneric", "shortGeneric"};
    items.push_back(PatternItem("timeZoneName",
                                {{"zzzz", "long"},
                                 {"z", "short"},
                                 {"OOOO", "longOffset"},
                                 {"O", "shortOffset"},
                                 {"vvvv", "longGeneric"},
                                 {"v", "shortGeneric"}},
                                kTimezone));
    return items;
}

class PatternItems {
 public:
  PatternItems() : data(BuildPatternItems()) {}
  virtual ~PatternItems() = default;
  const std::vector<PatternItem>& Get() const { return data; }

 private:
  const std::vector<PatternItem> data;
};

static const std::vector<PatternItem>& GetPatternItems() {
  static base::LazyInstance<PatternItems>::type items =
      LAZY_INSTANCE_INITIALIZER;
  return items.Pointer()->Get();
}

class PatternData {
 public:
  PatternData(const std::string property, std::vector<PatternMap> pairs,
              std::vector<const char*> allowed_values)
      : property(std::move(property)), allowed_values(allowed_values) {
    for (const auto& pair : pairs) {
      map.insert(std::make_pair(pair.value, pair.pattern));
    }
  }
  virtual ~PatternData() = default;

  const std::string property;
  std::map<const std::string, const std::string> map;
  std::vector<const char*> allowed_values;
};

const std::vector<PatternData> CreateCommonData(const PatternData& hour_data) {
  std::vector<PatternData> build;
  for (const PatternItem& item : GetPatternItems()) {
    if (item.property == "hour") {
      build.push_back(hour_data);
    } else {
      build.push_back(
          PatternData(item.property, item.pairs, item.allowed_values));
    }
  }
  return build;
}

const std::vector<PatternData> CreateData(const char* digit2,
                                          const char* numeric) {
  return CreateCommonData(
      PatternData("hour", {{digit2, "2-digit"}, {numeric, "numeric"}},
                  {"2-digit", "numeric"}));
}

// According to "Date Field Symbol Table" in
// http://userguide.icu-project.org/formatparse/datetime
// Symbol | Meaning              | Example(s)
//   h      hour in am/pm (1~12)    h    7
//                                  hh   07
//   H      hour in day (0~23)      H    0
//                                  HH   00
//   k      hour in day (1~24)      k    24
//                                  kk   24
//   K      hour in am/pm (0~11)    K    0
//                                  KK   00

class Pattern {
 public:
  Pattern(const char* d1, const char* d2) : data(CreateData(d1, d2)) {}
  virtual ~Pattern() = default;
  virtual const std::vector<PatternData>& Get() const { return data; }

 private:
  std::vector<PatternData> data;
};

#define DEFFINE_TRAIT(name, d1, d2)              \
  struct name {                                  \
    static void Construct(void* allocated_ptr) { \
      new (allocated_ptr) Pattern(d1, d2);       \
    }                                            \
  };
DEFFINE_TRAIT(H11Trait, "KK", "K")
DEFFINE_TRAIT(H12Trait, "hh", "h")
DEFFINE_TRAIT(H23Trait, "HH", "H")
DEFFINE_TRAIT(H24Trait, "kk", "k")
DEFFINE_TRAIT(HDefaultTrait, "jj", "j")
#undef DEFFINE_TRAIT

const std::vector<PatternData>& GetPatternData(
    JSDateTimeFormat::HourCycle hour_cycle) {
  switch (hour_cycle) {
    case JSDateTimeFormat::HourCycle::kH11: {
      static base::LazyInstance<Pattern, H11Trait>::type h11 =
          LAZY_INSTANCE_INITIALIZER;
      return h11.Pointer()->Get();
    }
    case JSDateTimeFormat::HourCycle::kH12: {
      static base::LazyInstance<Pattern, H12Trait>::type h12 =
          LAZY_INSTANCE_INITIALIZER;
      return h12.Pointer()->Get();
    }
    case JSDateTimeFormat::HourCycle::kH23: {
      static base::LazyInstance<Pattern, H23Trait>::type h23 =
          LAZY_INSTANCE_INITIALIZER;
      return h23.Pointer()->Get();
    }
    case JSDateTimeFormat::HourCycle::kH24: {
      static base::LazyInstance<Pattern, H24Trait>::type h24 =
          LAZY_INSTANCE_INITIALIZER;
      return h24.Pointer()->Get();
    }
    case JSDateTimeFormat::HourCycle::kUndefined: {
      static base::LazyInstance<Pattern, HDefaultTrait>::type hDefault =
          LAZY_INSTANCE_INITIALIZER;
      return hDefault.Pointer()->Get();
    }
    default:
      UNREACHABLE();
  }
}

std::string GetGMTTzID(const std::string& input) {
  std::string ret = "Etc/GMT";
  switch (input.length()) {
    case 8:
      if (input[7] == '0') return ret + '0';
      break;
    case 9:
      if ((input[7] == '+' || input[7] == '-') &&
          base::IsInRange(input[8], '0', '9')) {
        return ret + input[7] + input[8];
      }
      break;
    case 10:
      if ((input[7] == '+' || input[7] == '-') && (input[8] == '1') &&
          base::IsInRange(input[9], '0', '4')) {
        return ret + input[7] + input[8] + input[9];
      }
      break;
  }
  return "";
}

// Locale independenty version of isalpha for ascii range. This will return
// false if the ch is alpha but not in ascii range.
bool IsAsciiAlpha(char ch) {
  return base::IsInRange(ch, 'A', 'Z') || base::IsInRange(ch, 'a', 'z');
}

// Locale independent toupper for ascii range. This will not return İ (dotted I)
// for i under Turkish locale while std::toupper may.
char LocaleIndependentAsciiToUpper(char ch) {
  return (base::IsInRange(ch, 'a', 'z')) ? (ch - 'a' + 'A') : ch;
}

// Locale independent tolower for ascii range.
char LocaleIndependentAsciiToLower(char ch) {
  return (base::IsInRange(ch, 'A', 'Z')) ? (ch - 'A' + 'a') : ch;
}

// Returns titlecased location, bueNos_airES -> Buenos_Aires
// or ho_cHi_minH -> Ho_Chi_Minh. It is locale-agnostic and only
// deals with ASCII only characters.
// 'of', 'au' and 'es' are special-cased and lowercased.
// ICU's timezone parsing is case sensitive, but ECMAScript is case insensitive
std::string ToTitleCaseTimezoneLocation(const std::string& input) {
  std::string title_cased;
  int word_length = 0;
  for (char ch : input) {
    // Convert first char to upper case, the rest to lower case
    if (IsAsciiAlpha(ch)) {
      title_cased += word_length == 0 ? LocaleIndependentAsciiToUpper(ch)
                                      : LocaleIndependentAsciiToLower(ch);
      word_length++;
    } else if (ch == '_' || ch == '-' || ch == '/') {
      // Special case Au/Es/Of to be lower case.
      if (word_length == 2) {
        size_t pos = title_cased.length() - 2;
        std::string substr = title_cased.substr(pos, 2);
        if (substr == "Of" || substr == "Es" || substr == "Au") {
          title_cased[pos] = LocaleIndependentAsciiToLower(title_cased[pos]);
        }
      }
      title_cased += ch;
      word_length = 0;
    } else {
      // Invalid input
      return std::string();
    }
  }

  return title_cased;
}

class SpecialTimeZoneMap {
 public:
  SpecialTimeZoneMap() {
    Add("America/Argentina/ComodRivadavia");
    Add("America/Knox_IN");
    Add("Antarctica/DumontDUrville");
    Add("Antarctica/McMurdo");
    Add("Australia/ACT");
    Add("Australia/LHI");
    Add("Australia/NSW");
    Add("Brazil/DeNoronha");
    Add("Chile/EasterIsland");
    Add("GB");
    Add("GB-Eire");
    Add("Mexico/BajaNorte");
    Add("Mexico/BajaSur");
    Add("NZ");
    Add("NZ-CHAT");
    Add("W-SU");
  }

  std::string Find(const std::string& id) {
    auto it = map_.find(id);
    if (it != map_.end()) {
      return it->second;
    }
    return "";
  }

 private:
  void Add(const char* id) {
    std::string upper(id);
    transform(upper.begin(), upper.end(), upper.begin(),
              LocaleIndependentAsciiToUpper);
    map_.insert({upper, id});
  }
  std::map<std::string, std::string> map_;
};

}  // namespace

// Return the time zone id which match ICU's expectation of title casing
// return empty string when error.
std::string JSDateTimeFormat::CanonicalizeTimeZoneID(const std::string& input) {
  std::string upper = input;
  transform(upper.begin(), upper.end(), upper.begin(),
            LocaleIndependentAsciiToUpper);
  if (upper.length() == 3) {
    if (upper == "GMT") return "UTC";
    // For id such as "CET", return upper case.
    return upper;
  } else if (upper.length() == 7 && '0' <= upper[3] && upper[3] <= '9') {
    // For id such as "CST6CDT", return upper case.
    return upper;
  } else if (upper.length() > 3) {
    if (memcmp(upper.c_str(), "ETC", 3) == 0) {
      if (upper == "ETC/UTC" || upper == "ETC/GMT" || upper == "ETC/UCT") {
        return "UTC";
      }
      if (strncmp(upper.c_str(), "ETC/GMT", 7) == 0) {
        return GetGMTTzID(input);
      }
    } else if (memcmp(upper.c_str(), "GMT", 3) == 0) {
      if (upper == "GMT0" || upper == "GMT+0" || upper == "GMT-0") {
        return "UTC";
      }
    } else if (memcmp(upper.c_str(), "US/", 3) == 0) {
      std::string title = ToTitleCaseTimezoneLocation(input);
      if (title.length() >= 2) {
        // Change "Us/" to "US/"
        title[1] = 'S';
      }
      return title;
    } else if (strncmp(upper.c_str(), "SYSTEMV/", 8) == 0) {
      upper.replace(0, 8, "SystemV/");
      return upper;
    }
  }
  // We expect only _, '-' and / beside ASCII letters.

  static base::LazyInstance<SpecialTimeZoneMap>::type special_time_zone_map =
      LAZY_INSTANCE_INITIALIZER;

  std::string special_case = special_time_zone_map.Pointer()->Find(upper);
  if (!special_case.empty()) {
    return special_case;
  }
  return ToTitleCaseTimezoneLocation(input);
}

namespace {
Handle<String> DateTimeStyleAsString(Isolate* isolate,
                                     JSDateTimeFormat::DateTimeStyle style) {
  switch (style) {
    case JSDateTimeFormat::DateTimeStyle::kFull:
      return ReadOnlyRoots(isolate).full_string_handle();
    case JSDateTimeFormat::DateTimeStyle::kLong:
      return ReadOnlyRoots(isolate).long_string_handle();
    case JSDateTimeFormat::DateTimeStyle::kMedium:
      return ReadOnlyRoots(isolate).medium_string_handle();
    case JSDateTimeFormat::DateTimeStyle::kShort:
      return ReadOnlyRoots(isolate).short_string_handle();
    case JSDateTimeFormat::DateTimeStyle::kUndefined:
      UNREACHABLE();
  }
}

int FractionalSecondDigitsFromPattern(const std::string& pattern) {
  int result = 0;
  for (size_t i = 0; i < pattern.length() && result < 3; i++) {
    if (pattern[i] == 'S') {
      result++;
    }
  }
  return result;
}

}  // namespace

Handle<Object> JSDateTimeFormat::TimeZoneId(Isolate* isolate,
                                            const icu::TimeZone& tz) {
  Factory* factory = isolate->factory();
  icu::UnicodeString time_zone;
  tz.getID(time_zone);
  UErrorCode status = U_ZERO_ERROR;
  icu::UnicodeString canonical_time_zone;
  icu::TimeZone::getCanonicalID(time_zone, canonical_time_zone, status);
  Handle<Object> timezone_value;
  if (U_SUCCESS(status)) {
    // In CLDR (http://unicode.org/cldr/trac/ticket/9943), Etc/UTC is made
    // a separate timezone ID from Etc/GMT even though they're still the same
    // timezone. We have Etc/UTC because 'UTC', 'Etc/Universal',
    // 'Etc/Zulu' and others are turned to 'Etc/UTC' by ICU. Etc/GMT comes
    // from Etc/GMT0, Etc/GMT+0, Etc/GMT-0, Etc/Greenwich.
    // ecma402#sec-canonicalizetimezonename step 3
    if (canonical_time_zone == UNICODE_STRING_SIMPLE("Etc/UTC") ||
        canonical_time_zone == UNICODE_STRING_SIMPLE("Etc/GMT")) {
      timezone_value = factory->UTC_string();
    } else {
      ASSIGN_RETURN_ON_EXCEPTION_VALUE(
          isolate, timezone_value, Intl::ToString(isolate, canonical_time_zone),
          Handle<Object>());
    }
  } else {
    // Somehow on Windows we will reach here.
    timezone_value = factory->undefined_value();
  }
  return timezone_value;
}

namespace {
Handle<String> GetCalendar(Isolate* isolate,
                           const icu::SimpleDateFormat& simple_date_format,
                           bool is_alt_calendar = false) {
  // getType() returns legacy calendar type name instead of LDML/BCP47 calendar
  // key values. intl.js maps them to BCP47 values for key "ca".
  // TODO(jshin): Consider doing it here, instead.
  std::string calendar_str = simple_date_format.getCalendar()->getType();

  // Maps ICU calendar names to LDML/BCP47 types for key 'ca'.
  // See typeMap section in third_party/icu/source/data/misc/keyTypeData.txt
  // and
  // http://www.unicode.org/repos/cldr/tags/latest/common/bcp47/calendar.xml
  if (calendar_str == "gregorian") {
    if (is_alt_calendar) {
      calendar_str = "iso8601";
    } else {
      calendar_str = "gregory";
    }
  } else if (calendar_str == "ethiopic-amete-alem") {
    calendar_str = "ethioaa";
  } else if (calendar_str == "islamic") {
    if (is_alt_calendar) {
      calendar_str = "islamic-rgsa";
    }
  }
  return isolate->factory()->NewStringFromAsciiChecked(calendar_str.c_str());
}

Handle<Object> GetTimeZone(Isolate* isolate,
                           const icu::SimpleDateFormat& simple_date_format) {
  return JSDateTimeFormat::TimeZoneId(
      isolate, simple_date_format.getCalendar()->getTimeZone());
}
}  // namespace

Handle<String> JSDateTimeFormat::Calendar(
    Isolate* isolate, Handle<JSDateTimeFormat> date_time_format) {
  return GetCalendar(isolate,
                     *(date_time_format->icu_simple_date_format().raw()),
                     date_time_format->alt_calendar());
}

Handle<Object> JSDateTimeFormat::TimeZone(
    Isolate* isolate, Handle<JSDateTimeFormat> date_time_format) {
  return GetTimeZone(isolate,
                     *(date_time_format->icu_simple_date_format().raw()));
}

// ecma402 #sec-intl.datetimeformat.prototype.resolvedoptions
MaybeHandle<JSObject> JSDateTimeFormat::ResolvedOptions(
    Isolate* isolate, Handle<JSDateTimeFormat> date_time_format) {
  Factory* factory = isolate->factory();
  // 4. Let options be ! ObjectCreate(%ObjectPrototype%).
  Handle<JSObject> options = factory->NewJSObject(isolate->object_function());

  Handle<Object> resolved_obj;

  Handle<String> locale = Handle<String>(date_time_format->locale(), isolate);
  DCHECK(!date_time_format->icu_locale().is_null());
  DCHECK_NOT_NULL(date_time_format->icu_locale().raw());
  icu::Locale* icu_locale = date_time_format->icu_locale().raw();

  icu::SimpleDateFormat* icu_simple_date_format =
      date_time_format->icu_simple_date_format().raw();
  Handle<Object> timezone =
      JSDateTimeFormat::TimeZone(isolate, date_time_format);

  // Ugly hack. ICU doesn't expose numbering system in any way, so we have
  // to assume that for given locale NumberingSystem constructor produces the
  // same digits as NumberFormat/Calendar would.
  // Tracked by https://unicode-org.atlassian.net/browse/ICU-13431
  std::string numbering_system = Intl::GetNumberingSystem(*icu_locale);

  icu::UnicodeString pattern_unicode;
  icu_simple_date_format->toPattern(pattern_unicode);
  std::string pattern;
  pattern_unicode.toUTF8String(pattern);

  // 5. For each row of Table 6, except the header row, in table order, do
  // Table 6: Resolved Options of DateTimeFormat Instances
  //  Internal Slot          Property
  //    [[Locale]]           "locale"
  //    [[Calendar]]         "calendar"
  //    [[NumberingSystem]]  "numberingSystem"
  //    [[TimeZone]]         "timeZone"
  //    [[HourCycle]]        "hourCycle"
  //                         "hour12"
  //    [[Weekday]]          "weekday"
  //    [[Era]]              "era"
  //    [[Year]]             "year"
  //    [[Month]]            "month"
  //    [[Day]]              "day"
  //    [[Hour]]             "hour"
  //    [[Minute]]           "minute"
  //    [[Second]]           "second"
  //    [[FractionalSecondDigits]]     "fractionalSecondDigits"
  //    [[TimeZoneName]]     "timeZoneName"
  Maybe<bool> maybe_create_locale = JSReceiver::CreateDataProperty(
      isolate, options, factory->locale_string(), locale, Just(kDontThrow));
  DCHECK(maybe_create_locale.FromJust());
  USE(maybe_create_locale);

  Handle<String> calendar =
      JSDateTimeFormat::Calendar(isolate, date_time_format);
  Maybe<bool> maybe_create_calendar = JSReceiver::CreateDataProperty(
      isolate, options, factory->calendar_string(), calendar, Just(kDontThrow));
  DCHECK(maybe_create_calendar.FromJust());
  USE(maybe_create_calendar);

  if (!numbering_system.empty()) {
    Maybe<bool> maybe_create_numbering_system = JSReceiver::CreateDataProperty(
        isolate, options, factory->numberingSystem_string(),
        factory->NewStringFromAsciiChecked(numbering_system.c_str()),
        Just(kDontThrow));
    DCHECK(maybe_create_numbering_system.FromJust());
    USE(maybe_create_numbering_system);
  }
  Maybe<bool> maybe_create_time_zone = JSReceiver::CreateDataProperty(
      isolate, options, factory->timeZone_string(), timezone, Just(kDontThrow));
  DCHECK(maybe_create_time_zone.FromJust());
  USE(maybe_create_time_zone);

  // 5.b.i. Let hc be dtf.[[HourCycle]].
  HourCycle hc = date_time_format->hour_cycle();

  if (hc != HourCycle::kUndefined) {
    Maybe<bool> maybe_create_hour_cycle = JSReceiver::CreateDataProperty(
        isolate, options, factory->hourCycle_string(),
        date_time_format->HourCycleAsString(), Just(kDontThrow));
    DCHECK(maybe_create_hour_cycle.FromJust());
    USE(maybe_create_hour_cycle);
    switch (hc) {
      //  ii. If hc is "h11" or "h12", let v be true.
      case HourCycle::kH11:
      case HourCycle::kH12: {
        Maybe<bool> maybe_create_hour12 = JSReceiver::CreateDataProperty(
            isolate, options, factory->hour12_string(), factory->true_value(),
            Just(kDontThrow));
        DCHECK(maybe_create_hour12.FromJust());
        USE(maybe_create_hour12);
      } break;
      // iii. Else if, hc is "h23" or "h24", let v be false.
      case HourCycle::kH23:
      case HourCycle::kH24: {
        Maybe<bool> maybe_create_hour12 = JSReceiver::CreateDataProperty(
            isolate, options, factory->hour12_string(), factory->false_value(),
            Just(kDontThrow));
        DCHECK(maybe_create_hour12.FromJust());
        USE(maybe_create_hour12);
      } break;
      // iv. Else, let v be undefined.
      case HourCycle::kUndefined:
        break;
    }
  }

  // If dateStyle and timeStyle are undefined, then internal slots
  // listed in "Table 1: Components of date and time formats" will be set
  // in Step 33.f.iii.1 of InitializeDateTimeFormat
  if (date_time_format->date_style() == DateTimeStyle::kUndefined &&
      date_time_format->time_style() == DateTimeStyle::kUndefined) {
    for (const auto& item : GetPatternItems()) {
      // fractionalSecondsDigits need to be added before timeZoneName
      if (item.property == "timeZoneName") {
        int fsd = FractionalSecondDigitsFromPattern(pattern);
        if (fsd > 0) {
          Maybe<bool> maybe_create_fractional_seconds_digits =
              JSReceiver::CreateDataProperty(
                  isolate, options, factory->fractionalSecondDigits_string(),
                  factory->NewNumberFromInt(fsd), Just(kDontThrow));
          DCHECK(maybe_create_fractional_seconds_digits.FromJust());
          USE(maybe_create_fractional_seconds_digits);
        }
      }
      for (const auto& pair : item.pairs) {
        if (pattern.find(pair.pattern) != std::string::npos) {
          Maybe<bool> maybe_create_property = JSReceiver::CreateDataProperty(
              isolate, options,
              factory->NewStringFromAsciiChecked(item.property.c_str()),
              factory->NewStringFromAsciiChecked(pair.value.c_str()),
              Just(kDontThrow));
          DCHECK(maybe_create_property.FromJust());
          USE(maybe_create_property);
          break;
        }
      }
    }
  }

  // dateStyle
  if (date_time_format->date_style() != DateTimeStyle::kUndefined) {
    Maybe<bool> maybe_create_date_style = JSReceiver::CreateDataProperty(
        isolate, options, factory->dateStyle_string(),
        DateTimeStyleAsString(isolate, date_time_format->date_style()),
        Just(kDontThrow));
    DCHECK(maybe_create_date_style.FromJust());
    USE(maybe_create_date_style);
  }

  // timeStyle
  if (date_time_format->time_style() != DateTimeStyle::kUndefined) {
    Maybe<bool> maybe_create_time_style = JSReceiver::CreateDataProperty(
        isolate, options, factory->timeStyle_string(),
        DateTimeStyleAsString(isolate, date_time_format->time_style()),
        Just(kDontThrow));
    DCHECK(maybe_create_time_style.FromJust());
    USE(maybe_create_time_style);
  }
  return options;
}

namespace {

// ecma402/#sec-formatdatetime
// FormatDateTime( dateTimeFormat, x )
MaybeHandle<String> FormatDateTime(Isolate* isolate,
                                   const icu::SimpleDateFormat& date_format,
                                   double x) {
  double date_value = DateCache::TimeClip(x);
  if (std::isnan(date_value)) {
    THROW_NEW_ERROR(isolate, NewRangeError(MessageTemplate::kInvalidTimeValue),
                    String);
  }

  icu::UnicodeString result;
  date_format.format(date_value, result);

  return Intl::ToString(isolate, result);
}

}  // namespace

// ecma402/#sec-datetime-format-functions
// DateTime Format Functions
MaybeHandle<String> JSDateTimeFormat::DateTimeFormat(
    Isolate* isolate, Handle<JSDateTimeFormat> date_time_format,
    Handle<Object> date) {
  // 2. Assert: Type(dtf) is Object and dtf has an [[InitializedDateTimeFormat]]
  // internal slot.

  // 3. If date is not provided or is undefined, then
  double x;
  if (date->IsUndefined()) {
    // 3.a Let x be Call(%Date_now%, undefined).
    x = JSDate::CurrentTimeValue(isolate);
  } else {
    // 4. Else,
    //    a. Let x be ? ToNumber(date).
    ASSIGN_RETURN_ON_EXCEPTION(isolate, date, Object::ToNumber(isolate, date),
                               String);
    DCHECK(date->IsNumber());
    x = date->Number();
  }
  // 5. Return FormatDateTime(dtf, x).
  icu::SimpleDateFormat* format =
      date_time_format->icu_simple_date_format().raw();
  return FormatDateTime(isolate, *format, x);
}

namespace {
Isolate::ICUObjectCacheType ConvertToCacheType(
    JSDateTimeFormat::DefaultsOption type) {
  switch (type) {
    case JSDateTimeFormat::DefaultsOption::kDate:
      return Isolate::ICUObjectCacheType::kDefaultSimpleDateFormatForDate;
    case JSDateTimeFormat::DefaultsOption::kTime:
      return Isolate::ICUObjectCacheType::kDefaultSimpleDateFormatForTime;
    case JSDateTimeFormat::DefaultsOption::kAll:
      return Isolate::ICUObjectCacheType::kDefaultSimpleDateFormat;
  }
}
}  // namespace

MaybeHandle<String> JSDateTimeFormat::ToLocaleDateTime(
    Isolate* isolate, Handle<Object> date, Handle<Object> locales,
    Handle<Object> options, RequiredOption required, DefaultsOption defaults,
    const char* method_name) {
  Isolate::ICUObjectCacheType cache_type = ConvertToCacheType(defaults);

  Factory* factory = isolate->factory();
  // 1. Let x be ? thisTimeValue(this value);
  if (!date->IsJSDate()) {
    THROW_NEW_ERROR(isolate,
                    NewTypeError(MessageTemplate::kMethodInvokedOnWrongType,
                                 factory->Date_string()),
                    String);
  }

  double const x = Handle<JSDate>::cast(date)->value().Number();
  // 2. If x is NaN, return "Invalid Date"
  if (std::isnan(x)) {
    return factory->Invalid_Date_string();
  }

  // We only cache the instance when locales is a string/undefined and
  // options is undefined, as that is the only case when the specified
  // side-effects of examining those arguments are unobservable.
  bool can_cache = (locales->IsString() || locales->IsUndefined(isolate)) &&
                   options->IsUndefined(isolate);
  if (can_cache) {
    // Both locales and options are undefined, check the cache.
    icu::SimpleDateFormat* cached_icu_simple_date_format =
        static_cast<icu::SimpleDateFormat*>(
            isolate->get_cached_icu_object(cache_type, locales));
    if (cached_icu_simple_date_format != nullptr) {
      return FormatDateTime(isolate, *cached_icu_simple_date_format, x);
    }
  }
  // 3. Let options be ? ToDateTimeOptions(options, required, defaults).
  Handle<JSObject> internal_options;
  ASSIGN_RETURN_ON_EXCEPTION(
      isolate, internal_options,
      ToDateTimeOptions(isolate, options, required, defaults), String);

  // 4. Let dateFormat be ? Construct(%DateTimeFormat%, « locales, options »).
  Handle<JSFunction> constructor = Handle<JSFunction>(
      JSFunction::cast(
          isolate->context().native_context().intl_date_time_format_function()),
      isolate);
  Handle<Map> map;
  ASSIGN_RETURN_ON_EXCEPTION(
      isolate, map,
      JSFunction::GetDerivedMap(isolate, constructor, constructor), String);
  Handle<JSDateTimeFormat> date_time_format;
  ASSIGN_RETURN_ON_EXCEPTION(
      isolate, date_time_format,
      JSDateTimeFormat::New(isolate, map, locales, internal_options,
                            method_name),
      String);

  if (can_cache) {
    isolate->set_icu_object_in_cache(
        cache_type, locales,
        std::static_pointer_cast<icu::UMemory>(
            date_time_format->icu_simple_date_format().get()));
  }
  // 5. Return FormatDateTime(dateFormat, x).
  icu::SimpleDateFormat* format =
      date_time_format->icu_simple_date_format().raw();
  return FormatDateTime(isolate, *format, x);
}

namespace {

Maybe<bool> IsPropertyUndefined(Isolate* isolate, Handle<JSObject> options,
                                Handle<String> property) {
  // i. Let prop be the property name.
  // ii. Let value be ? Get(options, prop).
  Handle<Object> value;
  ASSIGN_RETURN_ON_EXCEPTION_VALUE(
      isolate, value, Object::GetPropertyOrElement(isolate, options, property),
      Nothing<bool>());
  return Just(value->IsUndefined(isolate));
}

Maybe<bool> NeedsDefault(Isolate* isolate, Handle<JSObject> options,
                         const std::vector<Handle<String>>& props) {
  bool needs_default = true;
  for (const auto& prop : props) {
    //  i. Let prop be the property name.
    // ii. Let value be ? Get(options, prop)
    Maybe<bool> maybe_undefined = IsPropertyUndefined(isolate, options, prop);
    MAYBE_RETURN(maybe_undefined, Nothing<bool>());
    // iii. If value is not undefined, let needDefaults be false.
    if (!maybe_undefined.FromJust()) {
      needs_default = false;
    }
  }
  return Just(needs_default);
}

Maybe<bool> CreateDefault(Isolate* isolate, Handle<JSObject> options,
                          const std::vector<std::string>& props) {
  Factory* factory = isolate->factory();
  // i. Perform ? CreateDataPropertyOrThrow(options, prop, "numeric").
  for (const auto& prop : props) {
    MAYBE_RETURN(
        JSReceiver::CreateDataProperty(
            isolate, options, factory->NewStringFromAsciiChecked(prop.c_str()),
            factory->numeric_string(), Just(kThrowOnError)),
        Nothing<bool>());
  }
  return Just(true);
}

}  // namespace

// ecma-402/#sec-todatetimeoptions
MaybeHandle<JSObject> JSDateTimeFormat::ToDateTimeOptions(
    Isolate* isolate, Handle<Object> input_options, RequiredOption required,
    DefaultsOption defaults) {
  Factory* factory = isolate->factory();
  // 1. If options is undefined, let options be null; otherwise let options be ?
  //    ToObject(options).
  Handle<JSObject> options;
  if (input_options->IsUndefined(isolate)) {
    options = factory->NewJSObjectWithNullProto();
  } else {
    Handle<JSReceiver> options_obj;
    ASSIGN_RETURN_ON_EXCEPTION(isolate, options_obj,
                               Object::ToObject(isolate, input_options),
                               JSObject);
    // 2. Let options be ObjectCreate(options).
    ASSIGN_RETURN_ON_EXCEPTION(isolate, options,
                               JSObject::ObjectCreate(isolate, options_obj),
                               JSObject);
  }

  // 3. Let needDefaults be true.
  bool needs_default = true;

  // 4. If required is "date" or "any", then
  if (required == RequiredOption::kAny || required == RequiredOption::kDate) {
    // a. For each of the property names "weekday", "year", "month",
    // "day", do
    std::vector<Handle<String>> list(
        {factory->weekday_string(), factory->year_string()});
    list.push_back(factory->month_string());
    list.push_back(factory->day_string());
    Maybe<bool> maybe_needs_default = NeedsDefault(isolate, options, list);
    MAYBE_RETURN(maybe_needs_default, Handle<JSObject>());
    needs_default = maybe_needs_default.FromJust();
  }

  // 5. If required is "time" or "any", then
  if (required == RequiredOption::kAny || required == RequiredOption::kTime) {
    // a. For each of the property names "dayPeriod", "hour", "minute",
    // "second", "fractionalSecondDigits", do
    std::vector<Handle<String>> list;
    list.push_back(factory->dayPeriod_string());
    list.push_back(factory->hour_string());
    list.push_back(factory->minute_string());
    list.push_back(factory->second_string());
    list.push_back(factory->fractionalSecondDigits_string());
    Maybe<bool> maybe_needs_default = NeedsDefault(isolate, options, list);
    MAYBE_RETURN(maybe_needs_default, Handle<JSObject>());
    needs_default &= maybe_needs_default.FromJust();
  }

  // 6. Let dateStyle be ? Get(options, "dateStyle").
  Maybe<bool> maybe_datestyle_undefined =
      IsPropertyUndefined(isolate, options, factory->dateStyle_string());
  MAYBE_RETURN(maybe_datestyle_undefined, Handle<JSObject>());
  // 7. Let timeStyle be ? Get(options, "timeStyle").
  Maybe<bool> maybe_timestyle_undefined =
      IsPropertyUndefined(isolate, options, factory->timeStyle_string());
  MAYBE_RETURN(maybe_timestyle_undefined, Handle<JSObject>());
  // 8. If dateStyle is not undefined or timeStyle is not undefined, let
  // needDefaults be false.
  if (!maybe_datestyle_undefined.FromJust() ||
      !maybe_timestyle_undefined.FromJust()) {
    needs_default = false;
  }
  // 9. If required is "date" and timeStyle is not undefined,
  if (required == RequiredOption::kDate &&
      !maybe_timestyle_undefined.FromJust()) {
    //  a. Throw a TypeError exception.
    THROW_NEW_ERROR(
        isolate,
        NewTypeError(MessageTemplate::kInvalid,
                     factory->NewStringFromStaticChars("option"),
                     factory->NewStringFromStaticChars("timeStyle")),
        JSObject);
  }
  // 10. If required is "time" and dateStyle is not undefined,
  if (required == RequiredOption::kTime &&
      !maybe_datestyle_undefined.FromJust()) {
    //  a. Throw a TypeError exception.
    THROW_NEW_ERROR(
        isolate,
        NewTypeError(MessageTemplate::kInvalid,
                     factory->NewStringFromStaticChars("option"),
                     factory->NewStringFromStaticChars("dateStyle")),
        JSObject);
  }

  // 11. If needDefaults is true and defaults is either "date" or "all", then
  if (needs_default) {
    if (defaults == DefaultsOption::kAll || defaults == DefaultsOption::kDate) {
      // a. For each of the property names "year", "month", "day", do)
      const std::vector<std::string> list({"year", "month", "day"});
      MAYBE_RETURN(CreateDefault(isolate, options, list), Handle<JSObject>());
    }
    // 12. If needDefaults is true and defaults is either "time" or "all", then
    if (defaults == DefaultsOption::kAll || defaults == DefaultsOption::kTime) {
      // a. For each of the property names "hour", "minute", "second", do
      const std::vector<std::string> list({"hour", "minute", "second"});
      MAYBE_RETURN(CreateDefault(isolate, options, list), Handle<JSObject>());
    }
  }
  // 13. Return options.
  return options;
}

MaybeHandle<JSDateTimeFormat> JSDateTimeFormat::UnwrapDateTimeFormat(
    Isolate* isolate, Handle<JSReceiver> format_holder) {
  Handle<Context> native_context =
      Handle<Context>(isolate->context().native_context(), isolate);
  Handle<JSFunction> constructor = Handle<JSFunction>(
      JSFunction::cast(native_context->intl_date_time_format_function()),
      isolate);
  Handle<Object> dtf;
  ASSIGN_RETURN_ON_EXCEPTION(
      isolate, dtf,
      Intl::LegacyUnwrapReceiver(isolate, format_holder, constructor,
                                 format_holder->IsJSDateTimeFormat()),
      JSDateTimeFormat);
  // 2. If Type(dtf) is not Object or dtf does not have an
  //    [[InitializedDateTimeFormat]] internal slot, then
  if (!dtf->IsJSDateTimeFormat()) {
    // a. Throw a TypeError exception.
    THROW_NEW_ERROR(isolate,
                    NewTypeError(MessageTemplate::kIncompatibleMethodReceiver,
                                 isolate->factory()->NewStringFromAsciiChecked(
                                     "UnwrapDateTimeFormat"),
                                 format_holder),
                    JSDateTimeFormat);
  }
  // 3. Return dtf.
  return Handle<JSDateTimeFormat>::cast(dtf);
}

std::unique_ptr<icu::TimeZone> JSDateTimeFormat::CreateTimeZone(
    const char* timezone) {
  // Create time zone as specified by the user. We have to re-create time zone
  // since calendar takes ownership.
  if (timezone == nullptr) {
    // 19.a. Else / Let timeZone be DefaultTimeZone().
    return std::unique_ptr<icu::TimeZone>(icu::TimeZone::createDefault());
  }
  std::string canonicalized = CanonicalizeTimeZoneID(timezone);
  if (canonicalized.empty()) return std::unique_ptr<icu::TimeZone>();
  std::unique_ptr<icu::TimeZone> tz(
      icu::TimeZone::createTimeZone(canonicalized.c_str()));
  // 18.b If the result of IsValidTimeZoneName(timeZone) is false, then
  // i. Throw a RangeError exception.
  if (!Intl::IsValidTimeZoneName(*tz)) return std::unique_ptr<icu::TimeZone>();
  return tz;
}

namespace {

class CalendarCache {
 public:
  icu::Calendar* CreateCalendar(const icu::Locale& locale, icu::TimeZone* tz) {
    icu::UnicodeString tz_id;
    tz->getID(tz_id);
    std::string key;
    tz_id.toUTF8String<std::string>(key);
    key += ":";
    key += locale.getName();

    base::MutexGuard guard(&mutex_);
    auto it = map_.find(key);
    if (it != map_.end()) {
      delete tz;
      return it->second->clone();
    }
    // Create a calendar using locale, and apply time zone to it.
    UErrorCode status = U_ZERO_ERROR;
    std::unique_ptr<icu::Calendar> calendar(
        icu::Calendar::createInstance(tz, locale, status));
    DCHECK(U_SUCCESS(status));
    DCHECK_NOT_NULL(calendar.get());

    if (calendar->getDynamicClassID() ==
        icu::GregorianCalendar::getStaticClassID()) {
      icu::GregorianCalendar* gc =
          static_cast<icu::GregorianCalendar*>(calendar.get());
      status = U_ZERO_ERROR;
      // The beginning of ECMAScript time, namely -(2**53)
      const double start_of_time = -9007199254740992;
      gc->setGregorianChange(start_of_time, status);
      DCHECK(U_SUCCESS(status));
    }

    if (map_.size() > 8) {  // Cache at most 8 calendars.
      map_.clear();
    }
    map_[key].reset(calendar.release());
    return map_[key]->clone();
  }

 private:
  std::map<std::string, std::unique_ptr<icu::Calendar>> map_;
  base::Mutex mutex_;
};

icu::Calendar* CreateCalendar(Isolate* isolate, const icu::Locale& icu_locale,
                              icu::TimeZone* tz) {
  static base::LazyInstance<CalendarCache>::type calendar_cache =
      LAZY_INSTANCE_INITIALIZER;
  return calendar_cache.Pointer()->CreateCalendar(icu_locale, tz);
}

icu::UnicodeString ReplaceHourCycleInPattern(icu::UnicodeString pattern,
                                             JSDateTimeFormat::HourCycle hc) {
  char16_t replacement;
  switch (hc) {
    case JSDateTimeFormat::HourCycle::kUndefined:
      return pattern;
    case JSDateTimeFormat::HourCycle::kH11:
      replacement = 'K';
      break;
    case JSDateTimeFormat::HourCycle::kH12:
      replacement = 'h';
      break;
    case JSDateTimeFormat::HourCycle::kH23:
      replacement = 'H';
      break;
    case JSDateTimeFormat::HourCycle::kH24:
      replacement = 'k';
      break;
  }
  bool replace = true;
  icu::UnicodeString result;
  char16_t last = u'\0';
  for (int32_t i = 0; i < pattern.length(); i++) {
    char16_t ch = pattern.charAt(i);
    switch (ch) {
      case '\'':
        replace = !replace;
        result.append(ch);
        break;
      case 'H':
        V8_FALLTHROUGH;
      case 'h':
        V8_FALLTHROUGH;
      case 'K':
        V8_FALLTHROUGH;
      case 'k':
        // If the previous field is a day, add a space before the hour.
        if (replace && last == u'd') {
          result.append(' ');
        }
        result.append(replace ? replacement : ch);
        break;
      default:
        result.append(ch);
        break;
    }
    last = ch;
  }
  return result;
}

std::unique_ptr<icu::SimpleDateFormat> CreateICUDateFormat(
    const icu::Locale& icu_locale, const icu::UnicodeString& skeleton,
    icu::DateTimePatternGenerator* generator, JSDateTimeFormat::HourCycle hc) {
  // See https://github.com/tc39/ecma402/issues/225 . The best pattern
  // generation needs to be done in the base locale according to the
  // current spec however odd it may be. See also crbug.com/826549 .
  // This is a temporary work-around to get v8's external behavior to match
  // the current spec, but does not follow the spec provisions mentioned
  // in the above Ecma 402 issue.
  // TODO(jshin): The spec may need to be revised because using the base
  // locale for the pattern match is not quite right. Moreover, what to
  // do with 'related year' part when 'chinese/dangi' calendar is specified
  // has to be discussed. Revisit once the spec is clarified/revised.
  icu::UnicodeString pattern;
  UErrorCode status = U_ZERO_ERROR;
  pattern = generator->getBestPattern(skeleton, UDATPG_MATCH_HOUR_FIELD_LENGTH,
                                      status);
  pattern = ReplaceHourCycleInPattern(pattern, hc);
  DCHECK(U_SUCCESS(status));

  // Make formatter from skeleton. Calendar and numbering system are added
  // to the locale as Unicode extension (if they were specified at all).
  status = U_ZERO_ERROR;
  std::unique_ptr<icu::SimpleDateFormat> date_format(
      new icu::SimpleDateFormat(pattern, icu_locale, status));
  if (U_FAILURE(status)) return std::unique_ptr<icu::SimpleDateFormat>();

  DCHECK_NOT_NULL(date_format.get());
  return date_format;
}

class DateFormatCache {
 public:
  icu::SimpleDateFormat* Create(const icu::Locale& icu_locale,
                                const icu::UnicodeString& skeleton,
                                icu::DateTimePatternGenerator* generator,
                                JSDateTimeFormat::HourCycle hc) {
    std::string key;
    skeleton.toUTF8String<std::string>(key);
    key += ":";
    key += icu_locale.getName();

    base::MutexGuard guard(&mutex_);
    auto it = map_.find(key);
    if (it != map_.end()) {
      return static_cast<icu::SimpleDateFormat*>(it->second->clone());
    }

    if (map_.size() > 8) {  // Cache at most 8 DateFormats.
      map_.clear();
    }
    std::unique_ptr<icu::SimpleDateFormat> instance(
        CreateICUDateFormat(icu_locale, skeleton, generator, hc));
    if (instance.get() == nullptr) return nullptr;
    map_[key] = std::move(instance);
    return static_cast<icu::SimpleDateFormat*>(map_[key]->clone());
  }

 private:
  std::map<std::string, std::unique_ptr<icu::SimpleDateFormat>> map_;
  base::Mutex mutex_;
};

std::unique_ptr<icu::SimpleDateFormat> CreateICUDateFormatFromCache(
    const icu::Locale& icu_locale, const icu::UnicodeString& skeleton,
    icu::DateTimePatternGenerator* generator, JSDateTimeFormat::HourCycle hc) {
  static base::LazyInstance<DateFormatCache>::type cache =
      LAZY_INSTANCE_INITIALIZER;
  return std::unique_ptr<icu::SimpleDateFormat>(
      cache.Pointer()->Create(icu_locale, skeleton, generator, hc));
}

icu::UnicodeString SkeletonFromDateFormat(
    const icu::SimpleDateFormat& icu_date_format) {
  icu::UnicodeString pattern;
  pattern = icu_date_format.toPattern(pattern);

  UErrorCode status = U_ZERO_ERROR;
  icu::UnicodeString skeleton =
      icu::DateTimePatternGenerator::staticGetSkeleton(pattern, status);
  DCHECK(U_SUCCESS(status));
  return skeleton;
}

icu::DateIntervalFormat* LazyCreateDateIntervalFormat(
    Isolate* isolate, Handle<JSDateTimeFormat> date_time_format) {
  Managed<icu::DateIntervalFormat> managed_format =
      date_time_format->icu_date_interval_format();
  if (managed_format.get()) {
    return managed_format.raw();
  }
  icu::SimpleDateFormat* icu_simple_date_format =
      date_time_format->icu_simple_date_format().raw();
  UErrorCode status = U_ZERO_ERROR;

  icu::Locale loc = *(date_time_format->icu_locale().raw());
  // We need to pass in the hc to DateIntervalFormat by using Unicode 'hc'
  // extension.
  std::string hcString = ToHourCycleString(date_time_format->hour_cycle());
  if (!hcString.empty()) {
    loc.setUnicodeKeywordValue("hc", hcString, status);
  }

  std::unique_ptr<icu::DateIntervalFormat> date_interval_format(
      icu::DateIntervalFormat::createInstance(
          SkeletonFromDateFormat(*icu_simple_date_format), loc, status));
  if (U_FAILURE(status)) {
    return nullptr;
  }
  date_interval_format->setTimeZone(icu_simple_date_format->getTimeZone());
  Handle<Managed<icu::DateIntervalFormat>> managed_interval_format =
      Managed<icu::DateIntervalFormat>::FromUniquePtr(
          isolate, 0, std::move(date_interval_format));
  date_time_format->set_icu_date_interval_format(*managed_interval_format);
  return (*managed_interval_format).raw();
}

JSDateTimeFormat::HourCycle HourCycleFromPattern(
    const icu::UnicodeString pattern) {
  bool in_quote = false;
  for (int32_t i = 0; i < pattern.length(); i++) {
    char16_t ch = pattern[i];
    switch (ch) {
      case '\'':
        in_quote = !in_quote;
        break;
      case 'K':
        if (!in_quote) return JSDateTimeFormat::HourCycle::kH11;
        break;
      case 'h':
        if (!in_quote) return JSDateTimeFormat::HourCycle::kH12;
        break;
      case 'H':
        if (!in_quote) return JSDateTimeFormat::HourCycle::kH23;
        break;
      case 'k':
        if (!in_quote) return JSDateTimeFormat::HourCycle::kH24;
        break;
    }
  }
  return JSDateTimeFormat::HourCycle::kUndefined;
}

icu::DateFormat::EStyle DateTimeStyleToEStyle(
    JSDateTimeFormat::DateTimeStyle style) {
  switch (style) {
    case JSDateTimeFormat::DateTimeStyle::kFull:
      return icu::DateFormat::EStyle::kFull;
    case JSDateTimeFormat::DateTimeStyle::kLong:
      return icu::DateFormat::EStyle::kLong;
    case JSDateTimeFormat::DateTimeStyle::kMedium:
      return icu::DateFormat::EStyle::kMedium;
    case JSDateTimeFormat::DateTimeStyle::kShort:
      return icu::DateFormat::EStyle::kShort;
    case JSDateTimeFormat::DateTimeStyle::kUndefined:
      UNREACHABLE();
  }
}

icu::UnicodeString ReplaceSkeleton(const icu::UnicodeString input,
                                   JSDateTimeFormat::HourCycle hc) {
  icu::UnicodeString result;
  char16_t to;
  switch (hc) {
    case JSDateTimeFormat::HourCycle::kH11:
      to = 'K';
      break;
    case JSDateTimeFormat::HourCycle::kH12:
      to = 'h';
      break;
    case JSDateTimeFormat::HourCycle::kH23:
      to = 'H';
      break;
    case JSDateTimeFormat::HourCycle::kH24:
      to = 'k';
      break;
    case JSDateTimeFormat::HourCycle::kUndefined:
      UNREACHABLE();
  }
  for (int32_t i = 0; i < input.length(); i++) {
    switch (input[i]) {
      // We need to skip 'a', 'b', 'B' here due to
      // https://unicode-org.atlassian.net/browse/ICU-20437
      case 'a':
        V8_FALLTHROUGH;
      case 'b':
        V8_FALLTHROUGH;
      case 'B':
        // ignore
        break;
      case 'h':
        V8_FALLTHROUGH;
      case 'H':
        V8_FALLTHROUGH;
      case 'K':
        V8_FALLTHROUGH;
      case 'k':
        result += to;
        break;
      default:
        result += input[i];
        break;
    }
  }
  return result;
}

std::unique_ptr<icu::SimpleDateFormat> DateTimeStylePattern(
    JSDateTimeFormat::DateTimeStyle date_style,
    JSDateTimeFormat::DateTimeStyle time_style, icu::Locale& icu_locale,
    JSDateTimeFormat::HourCycle hc, icu::DateTimePatternGenerator* generator) {
  std::unique_ptr<icu::SimpleDateFormat> result;
  if (date_style != JSDateTimeFormat::DateTimeStyle::kUndefined) {
    if (time_style != JSDateTimeFormat::DateTimeStyle::kUndefined) {
      result.reset(reinterpret_cast<icu::SimpleDateFormat*>(
          icu::DateFormat::createDateTimeInstance(
              DateTimeStyleToEStyle(date_style),
              DateTimeStyleToEStyle(time_style), icu_locale)));
    } else {
      result.reset(reinterpret_cast<icu::SimpleDateFormat*>(
          icu::DateFormat::createDateInstance(DateTimeStyleToEStyle(date_style),
                                              icu_locale)));
      // For instance without time, we do not need to worry about the hour cycle
      // impact so we can return directly.
      if (result.get() != nullptr) {
        return result;
      }
    }
  } else {
    if (time_style != JSDateTimeFormat::DateTimeStyle::kUndefined) {
      result.reset(reinterpret_cast<icu::SimpleDateFormat*>(
          icu::DateFormat::createTimeInstance(DateTimeStyleToEStyle(time_style),
                                              icu_locale)));
    } else {
      UNREACHABLE();
    }
  }

  UErrorCode status = U_ZERO_ERROR;
  // Somehow we fail to create the instance.
  if (result.get() == nullptr) {
    // Fallback to the locale without "nu".
    if (!icu_locale.getUnicodeKeywordValue<std::string>("nu", status).empty()) {
      status = U_ZERO_ERROR;
      icu_locale.setUnicodeKeywordValue("nu", nullptr, status);
      return DateTimeStylePattern(date_style, time_style, icu_locale, hc,
                                  generator);
    }
    status = U_ZERO_ERROR;
    // Fallback to the locale without "hc".
    if (!icu_locale.getUnicodeKeywordValue<std::string>("hc", status).empty()) {
      status = U_ZERO_ERROR;
      icu_locale.setUnicodeKeywordValue("hc", nullptr, status);
      return DateTimeStylePattern(date_style, time_style, icu_locale, hc,
                                  generator);
    }
    status = U_ZERO_ERROR;
    // Fallback to the locale without "ca".
    if (!icu_locale.getUnicodeKeywordValue<std::string>("ca", status).empty()) {
      status = U_ZERO_ERROR;
      icu_locale.setUnicodeKeywordValue("ca", nullptr, status);
      return DateTimeStylePattern(date_style, time_style, icu_locale, hc,
                                  generator);
    }
    return nullptr;
  }
  icu::UnicodeString pattern;
  pattern = result->toPattern(pattern);

  status = U_ZERO_ERROR;
  icu::UnicodeString skeleton =
      icu::DateTimePatternGenerator::staticGetSkeleton(pattern, status);
  DCHECK(U_SUCCESS(status));

  // If the skeleton match the HourCycle, we just return it.
  if (hc == HourCycleFromPattern(pattern)) {
    return result;
  }

  return CreateICUDateFormatFromCache(icu_locale, ReplaceSkeleton(skeleton, hc),
                                      generator, hc);
}

class DateTimePatternGeneratorCache {
 public:
  // Return a clone copy that the caller have to free.
  icu::DateTimePatternGenerator* CreateGenerator(const icu::Locale& locale) {
    std::string key(locale.getName());
    base::MutexGuard guard(&mutex_);
    auto it = map_.find(key);
    if (it != map_.end()) {
      return it->second->clone();
    }
    UErrorCode status = U_ZERO_ERROR;
    map_[key].reset(
        icu::DateTimePatternGenerator::createInstance(locale, status));
    // Fallback to use "root".
    if (U_FAILURE(status)) {
      status = U_ZERO_ERROR;
      map_[key].reset(
          icu::DateTimePatternGenerator::createInstance("root", status));
    }
    DCHECK(U_SUCCESS(status));
    return map_[key]->clone();
  }

 private:
  std::map<std::string, std::unique_ptr<icu::DateTimePatternGenerator>> map_;
  base::Mutex mutex_;
};

}  // namespace

enum FormatMatcherOption { kBestFit, kBasic };

// ecma402/#sec-initializedatetimeformat
MaybeHandle<JSDateTimeFormat> JSDateTimeFormat::New(
    Isolate* isolate, Handle<Map> map, Handle<Object> locales,
    Handle<Object> input_options, const char* service) {
  Factory* factory = isolate->factory();
  // 1. Let requestedLocales be ? CanonicalizeLocaleList(locales).
  Maybe<std::vector<std::string>> maybe_requested_locales =
      Intl::CanonicalizeLocaleList(isolate, locales);
  MAYBE_RETURN(maybe_requested_locales, Handle<JSDateTimeFormat>());
  std::vector<std::string> requested_locales =
      maybe_requested_locales.FromJust();
  // 2. Let options be ? ToDateTimeOptions(options, "any", "date").
  Handle<JSObject> options;
  ASSIGN_RETURN_ON_EXCEPTION(
      isolate, options,
      JSDateTimeFormat::ToDateTimeOptions(
          isolate, input_options, RequiredOption::kAny, DefaultsOption::kDate),
      JSDateTimeFormat);

  // 4. Let matcher be ? GetOption(options, "localeMatcher", "string",
  // « "lookup", "best fit" », "best fit").
  // 5. Set opt.[[localeMatcher]] to matcher.
  Maybe<Intl::MatcherOption> maybe_locale_matcher =
      Intl::GetLocaleMatcher(isolate, options, service);
  MAYBE_RETURN(maybe_locale_matcher, MaybeHandle<JSDateTimeFormat>());
  Intl::MatcherOption locale_matcher = maybe_locale_matcher.FromJust();

  std::unique_ptr<char[]> calendar_str = nullptr;
  std::unique_ptr<char[]> numbering_system_str = nullptr;
  const std::vector<const char*> empty_values = {};
  // 6. Let calendar be ? GetOption(options, "calendar",
  //    "string", undefined, undefined).
  Maybe<bool> maybe_calendar = GetStringOption(
      isolate, options, "calendar", empty_values, service, &calendar_str);
  MAYBE_RETURN(maybe_calendar, MaybeHandle<JSDateTimeFormat>());
  if (maybe_calendar.FromJust() && calendar_str != nullptr) {
    icu::Locale default_locale;
    if (!Intl::IsWellFormedCalendar(calendar_str.get())) {
      THROW_NEW_ERROR(
          isolate,
          NewRangeError(MessageTemplate::kInvalid, factory->calendar_string(),
                        factory->NewStringFromAsciiChecked(calendar_str.get())),
          JSDateTimeFormat);
    }
  }

  // 8. Let numberingSystem be ? GetOption(options, "numberingSystem",
  //    "string", undefined, undefined).
  Maybe<bool> maybe_numberingSystem = Intl::GetNumberingSystem(
      isolate, options, service, &numbering_system_str);
  MAYBE_RETURN(maybe_numberingSystem, MaybeHandle<JSDateTimeFormat>());

  // 6. Let hour12 be ? GetOption(options, "hour12", "boolean", undefined,
  // undefined).
  bool hour12;
  Maybe<bool> maybe_get_hour12 =
      GetBoolOption(isolate, options, "hour12", service, &hour12);
  MAYBE_RETURN(maybe_get_hour12, Handle<JSDateTimeFormat>());

  // 7. Let hourCycle be ? GetOption(options, "hourCycle", "string", « "h11",
  // "h12", "h23", "h24" », undefined).
  Maybe<HourCycle> maybe_hour_cycle = GetHourCycle(isolate, options, service);
  MAYBE_RETURN(maybe_hour_cycle, MaybeHandle<JSDateTimeFormat>());
  HourCycle hour_cycle = maybe_hour_cycle.FromJust();

  // 8. If hour12 is not undefined, then
  if (maybe_get_hour12.FromJust()) {
    // a. Let hourCycle be null.
    hour_cycle = HourCycle::kUndefined;
  }
  // 9. Set opt.[[hc]] to hourCycle.

  // ecma402/#sec-intl.datetimeformat-internal-slots
  // The value of the [[RelevantExtensionKeys]] internal slot is
  // « "ca", "nu", "hc" ».
  std::set<std::string> relevant_extension_keys = {"nu", "ca", "hc"};

  // 10. Let localeData be %DateTimeFormat%.[[LocaleData]].
  // 11. Let r be ResolveLocale( %DateTimeFormat%.[[AvailableLocales]],
  //     requestedLocales, opt, %DateTimeFormat%.[[RelevantExtensionKeys]],
  //     localeData).
  //
  Maybe<Intl::ResolvedLocale> maybe_resolve_locale = Intl::ResolveLocale(
      isolate, JSDateTimeFormat::GetAvailableLocales(), requested_locales,
      locale_matcher, relevant_extension_keys);
  if (maybe_resolve_locale.IsNothing()) {
    THROW_NEW_ERROR(isolate, NewRangeError(MessageTemplate::kIcuError),
                    JSDateTimeFormat);
  }
  Intl::ResolvedLocale r = maybe_resolve_locale.FromJust();

  icu::Locale icu_locale = r.icu_locale;
  DCHECK(!icu_locale.isBogus());

  UErrorCode status = U_ZERO_ERROR;
  if (calendar_str != nullptr) {
    auto ca_extension_it = r.extensions.find("ca");
    if (ca_extension_it != r.extensions.end() &&
        ca_extension_it->second != calendar_str.get()) {
      icu_locale.setUnicodeKeywordValue("ca", nullptr, status);
      DCHECK(U_SUCCESS(status));
    }
  }
  if (numbering_system_str != nullptr) {
    auto nu_extension_it = r.extensions.find("nu");
    if (nu_extension_it != r.extensions.end() &&
        nu_extension_it->second != numbering_system_str.get()) {
      icu_locale.setUnicodeKeywordValue("nu", nullptr, status);
      DCHECK(U_SUCCESS(status));
    }
  }

  // Need to keep a copy of icu_locale which not changing "ca", "nu", "hc"
  // by option.
  icu::Locale resolved_locale(icu_locale);

  if (calendar_str != nullptr &&
      Intl::IsValidCalendar(icu_locale, calendar_str.get())) {
    icu_locale.setUnicodeKeywordValue("ca", calendar_str.get(), status);
    DCHECK(U_SUCCESS(status));
  }
  bool alt_calendar =
      strstr(icu_locale.getName(), "calendar=iso8601") != nullptr ||
      strstr(icu_locale.getName(), "calendar=islamic-rgsa") != nullptr;

  if (numbering_system_str != nullptr &&
      Intl::IsValidNumberingSystem(numbering_system_str.get())) {
    icu_locale.setUnicodeKeywordValue("nu", numbering_system_str.get(), status);
    DCHECK(U_SUCCESS(status));
  }

  static base::LazyInstance<DateTimePatternGeneratorCache>::type
      generator_cache = LAZY_INSTANCE_INITIALIZER;

  std::unique_ptr<icu::DateTimePatternGenerator> generator(
      generator_cache.Pointer()->CreateGenerator(icu_locale));

  // 15.Let hcDefault be dataLocaleData.[[hourCycle]].
  HourCycle hc_default = ToHourCycle(generator->getDefaultHourCycle(status));
  DCHECK(U_SUCCESS(status));

  // 16.Let hc be r.[[hc]].
  HourCycle hc = HourCycle::kUndefined;
  if (hour_cycle == HourCycle::kUndefined) {
    auto hc_extension_it = r.extensions.find("hc");
    if (hc_extension_it != r.extensions.end()) {
      hc = ToHourCycle(hc_extension_it->second.c_str());
    }
  } else {
    hc = hour_cycle;
  }
  // 17. If hc is null, then
  if (hc == HourCycle::kUndefined) {
    // a. Set hc to hcDefault.
    hc = hc_default;
  }

  // 18. If hour12 is not undefined, then
  if (maybe_get_hour12.FromJust()) {
    // a. If hour12 is true, then
    if (hour12) {
      // i. If hcDefault is "h11" or "h23", then
      if (hc_default == HourCycle::kH11 || hc_default == HourCycle::kH23) {
        // 1. Set hc to "h11".
        hc = HourCycle::kH11;
        // ii. Else,
      } else {
        // 1. Set hc to "h12".
        hc = HourCycle::kH12;
      }
      // b. Else,
    } else {
      // ii. If hcDefault is "h11" or "h23", then
      if (hc_default == HourCycle::kH11 || hc_default == HourCycle::kH23) {
        // 1. Set hc to "h23".
        hc = HourCycle::kH23;
        // iii. Else,
      } else {
        // 1. Set hc to "h24".
        hc = HourCycle::kH24;
      }
    }
  }

  // 17. Let timeZone be ? Get(options, "timeZone").
  std::unique_ptr<char[]> timezone = nullptr;
  Maybe<bool> maybe_timezone = GetStringOption(
      isolate, options, "timeZone", empty_values, service, &timezone);
  MAYBE_RETURN(maybe_timezone, Handle<JSDateTimeFormat>());

  std::unique_ptr<icu::TimeZone> tz =
      JSDateTimeFormat::CreateTimeZone(timezone.get());
  if (tz.get() == nullptr) {
    THROW_NEW_ERROR(
        isolate,
        NewRangeError(MessageTemplate::kInvalidTimeZone,
                      factory->NewStringFromAsciiChecked(timezone.get())),
        JSDateTimeFormat);
  }

  std::unique_ptr<icu::Calendar> calendar(
      CreateCalendar(isolate, icu_locale, tz.release()));

  // 18.b If the result of IsValidTimeZoneName(timeZone) is false, then
  // i. Throw a RangeError exception.
  if (calendar.get() == nullptr) {
    THROW_NEW_ERROR(
        isolate,
        NewRangeError(MessageTemplate::kInvalidTimeZone,
                      factory->NewStringFromAsciiChecked(timezone.get())),
        JSDateTimeFormat);
  }

  DateTimeStyle date_style = DateTimeStyle::kUndefined;
  DateTimeStyle time_style = DateTimeStyle::kUndefined;
  std::unique_ptr<icu::SimpleDateFormat> icu_date_format;

  // 28. For each row of Table 1, except the header row, do
  bool has_hour_option = false;
  std::string skeleton;
  for (const PatternData& item : GetPatternData(hc)) {
    // Need to read fractionalSecondDigits before reading the timeZoneName
    if (item.property == "timeZoneName") {
      // Let _value_ be ? GetNumberOption(options, "fractionalSecondDigits", 1,
      // 3, *undefined*). The *undefined* is represented by value 0 here.
      Maybe<int> maybe_fsd = GetNumberOption(
          isolate, options, factory->fractionalSecondDigits_string(), 1, 3, 0);
      MAYBE_RETURN(maybe_fsd, MaybeHandle<JSDateTimeFormat>());
      // Convert fractionalSecondDigits to skeleton.
      int fsd = maybe_fsd.FromJust();
      for (int i = 0; i < fsd; i++) {
        skeleton += "S";
      }
    }
    std::unique_ptr<char[]> input;
    // i. Let prop be the name given in the Property column of the row.
    // ii. Let value be ? GetOption(options, prop, "string", « the strings
    // given in the Values column of the row », undefined).
    Maybe<bool> maybe_get_option =
        GetStringOption(isolate, options, item.property.c_str(),
                        item.allowed_values, service, &input);
    MAYBE_RETURN(maybe_get_option, Handle<JSDateTimeFormat>());
    if (maybe_get_option.FromJust()) {
      if (item.property == "hour") {
        has_hour_option = true;
      }
      DCHECK_NOT_NULL(input.get());
      // iii. Set opt.[[<prop>]] to value.
      skeleton += item.map.find(input.get())->second;
    }
  }

  // 29. Let matcher be ? GetOption(options, "formatMatcher", "string", «
  // "basic", "best fit" », "best fit").
  // We implement only best fit algorithm, but still need to check
  // if the formatMatcher values are in range.
  // c. Let matcher be ? GetOption(options, "formatMatcher", "string",
  //     «  "basic", "best fit" », "best fit").
  Maybe<FormatMatcherOption> maybe_format_matcher =
      GetStringOption<FormatMatcherOption>(
          isolate, options, "formatMatcher", service, {"best fit", "basic"},
          {FormatMatcherOption::kBestFit, FormatMatcherOption::kBasic},
          FormatMatcherOption::kBestFit);
  MAYBE_RETURN(maybe_format_matcher, MaybeHandle<JSDateTimeFormat>());
  // TODO(ftang): uncomment the following line and handle format_matcher.
  // FormatMatcherOption format_matcher = maybe_format_matcher.FromJust();

  // 32. Let dateStyle be ? GetOption(options, "dateStyle", "string", «
  // "full", "long", "medium", "short" », undefined).
  Maybe<DateTimeStyle> maybe_date_style = GetStringOption<DateTimeStyle>(
      isolate, options, "dateStyle", service,
      {"full", "long", "medium", "short"},
      {DateTimeStyle::kFull, DateTimeStyle::kLong, DateTimeStyle::kMedium,
       DateTimeStyle::kShort},
      DateTimeStyle::kUndefined);
  MAYBE_RETURN(maybe_date_style, MaybeHandle<JSDateTimeFormat>());
  // 33. Set dateTimeFormat.[[DateStyle]] to dateStyle.
  date_style = maybe_date_style.FromJust();

  // 34. Let timeStyle be ? GetOption(options, "timeStyle", "string", «
  // "full", "long", "medium", "short" »).
  Maybe<DateTimeStyle> maybe_time_style = GetStringOption<DateTimeStyle>(
      isolate, options, "timeStyle", service,
      {"full", "long", "medium", "short"},
      {DateTimeStyle::kFull, DateTimeStyle::kLong, DateTimeStyle::kMedium,
       DateTimeStyle::kShort},
      DateTimeStyle::kUndefined);
  MAYBE_RETURN(maybe_time_style, MaybeHandle<JSDateTimeFormat>());

  // 35. Set dateTimeFormat.[[TimeStyle]] to timeStyle.
  time_style = maybe_time_style.FromJust();

  // 36. If timeStyle is not undefined, then
  HourCycle dateTimeFormatHourCycle = HourCycle::kUndefined;
  if (time_style != DateTimeStyle::kUndefined) {
    // a. Set dateTimeFormat.[[HourCycle]] to hc.
    dateTimeFormatHourCycle = hc;
  }

  // 37. If dateStyle or timeStyle are not undefined, then
  if (date_style != DateTimeStyle::kUndefined ||
      time_style != DateTimeStyle::kUndefined) {
    // a. For each row in Table 1, except the header row, do
    //    i. Let prop be the name given in the Property column of the row.
    //   ii. Let p be opt.[[<prop>]].
    //  iii. If p is not undefined, then
    //      1. Throw a TypeError exception.
    if (skeleton.length() > 0) {
      std::string prop;
      for (const auto& item : GetPatternItems()) {
        for (const auto& pair : item.pairs) {
          if (skeleton.find(pair.pattern) != std::string::npos) {
            prop.assign(item.property);
            break;
          }
        }
        if (!prop.empty()) {
          break;
        }
      }
      if (prop.empty() && skeleton.find("S") != std::string::npos) {
        prop.assign("fractionalSecondDigits");
      }
      if (!prop.empty()) {
        THROW_NEW_ERROR(
            isolate,
            NewTypeError(MessageTemplate::kCantSetOptionXWhenYIsUsed,
                         factory->NewStringFromAsciiChecked(prop.c_str()),
                         date_style != DateTimeStyle::kUndefined
                             ? factory->dateStyle_string()
                             : factory->timeStyle_string()),
            JSDateTimeFormat);
      }
      UNREACHABLE();
    }
    // b. Let pattern be DateTimeStylePattern(dateStyle, timeStyle,
    // dataLocaleData, hc).
    isolate->CountUsage(
        v8::Isolate::UseCounterFeature::kDateTimeFormatDateTimeStyle);

    icu_date_format =
        DateTimeStylePattern(date_style, time_style, icu_locale,
                             dateTimeFormatHourCycle, generator.get());
    if (icu_date_format.get() == nullptr) {
      THROW_NEW_ERROR(isolate, NewRangeError(MessageTemplate::kIcuError),
                      JSDateTimeFormat);
    }
  } else {
    // e. If dateTimeFormat.[[Hour]] is not undefined, then
    if (has_hour_option) {
      // v. Set dateTimeFormat.[[HourCycle]] to hc.
      dateTimeFormatHourCycle = hc;
    } else {
      // f. Else,
      // Set dateTimeFormat.[[HourCycle]] to undefined.
      dateTimeFormatHourCycle = HourCycle::kUndefined;
    }
    icu::UnicodeString skeleton_ustr(skeleton.c_str());
    icu_date_format = CreateICUDateFormatFromCache(
        icu_locale, skeleton_ustr, generator.get(), dateTimeFormatHourCycle);
    if (icu_date_format.get() == nullptr) {
      // Remove extensions and try again.
      icu_locale = icu::Locale(icu_locale.getBaseName());
      icu_date_format = CreateICUDateFormatFromCache(
          icu_locale, skeleton_ustr, generator.get(), dateTimeFormatHourCycle);
      if (icu_date_format.get() == nullptr) {
        THROW_NEW_ERROR(isolate, NewRangeError(MessageTemplate::kIcuError),
                        JSDateTimeFormat);
      }
    }
  }

  // The creation of Calendar depends on timeZone so we have to put 13 after 17.
  // Also icu_date_format is not created until here.
  // 13. Set dateTimeFormat.[[Calendar]] to r.[[ca]].
  icu_date_format->adoptCalendar(calendar.release());

  // 12.1.1 InitializeDateTimeFormat ( dateTimeFormat, locales, options )
  //
  // Steps 8-9 set opt.[[hc]] to value *other than undefined*
  // if "hour12" is set or "hourCycle" is set in the option.
  //
  // 9.2.6 ResolveLocale (... )
  // Step 8.h / 8.i and 8.k
  //
  // An hour12 option always overrides an hourCycle option.
  // Additionally hour12 and hourCycle both clear out any existing Unicode
  // extension key in the input locale.
  //
  // See details in https://github.com/tc39/test262/pull/2035
  if (maybe_get_hour12.FromJust() ||
      maybe_hour_cycle.FromJust() != HourCycle::kUndefined) {
    auto hc_extension_it = r.extensions.find("hc");
    if (hc_extension_it != r.extensions.end()) {
      if (dateTimeFormatHourCycle !=
          ToHourCycle(hc_extension_it->second.c_str())) {
        // Remove -hc- if it does not agree with what we used.
        status = U_ZERO_ERROR;
        resolved_locale.setUnicodeKeywordValue("hc", nullptr, status);
        DCHECK(U_SUCCESS(status));
      }
    }
  }

  Maybe<std::string> maybe_locale_str = Intl::ToLanguageTag(resolved_locale);
  MAYBE_RETURN(maybe_locale_str, MaybeHandle<JSDateTimeFormat>());
  Handle<String> locale_str = isolate->factory()->NewStringFromAsciiChecked(
      maybe_locale_str.FromJust().c_str());

  Handle<Managed<icu::Locale>> managed_locale =
      Managed<icu::Locale>::FromRawPtr(isolate, 0, icu_locale.clone());

  Handle<Managed<icu::SimpleDateFormat>> managed_format =
      Managed<icu::SimpleDateFormat>::FromUniquePtr(isolate, 0,
                                                    std::move(icu_date_format));

  Handle<Managed<icu::DateIntervalFormat>> managed_interval_format =
      Managed<icu::DateIntervalFormat>::FromRawPtr(isolate, 0, nullptr);

  // Now all properties are ready, so we can allocate the result object.
  Handle<JSDateTimeFormat> date_time_format = Handle<JSDateTimeFormat>::cast(
      isolate->factory()->NewFastOrSlowJSObjectFromMap(map));
  DisallowGarbageCollection no_gc;
  date_time_format->set_flags(0);
  if (date_style != DateTimeStyle::kUndefined) {
    date_time_format->set_date_style(date_style);
  }
  if (time_style != DateTimeStyle::kUndefined) {
    date_time_format->set_time_style(time_style);
  }
  date_time_format->set_hour_cycle(dateTimeFormatHourCycle);
  date_time_format->set_alt_calendar(alt_calendar);
  date_time_format->set_locale(*locale_str);
  date_time_format->set_icu_locale(*managed_locale);
  date_time_format->set_icu_simple_date_format(*managed_format);
  date_time_format->set_icu_date_interval_format(*managed_interval_format);
  return date_time_format;
}

namespace {

// The list comes from third_party/icu/source/i18n/unicode/udat.h.
// They're mapped to DateTimeFormat components listed at
// https://tc39.github.io/ecma402/#sec-datetimeformat-abstracts .
Handle<String> IcuDateFieldIdToDateType(int32_t field_id, Isolate* isolate) {
  switch (field_id) {
    case -1:
      return isolate->factory()->literal_string();
    case UDAT_YEAR_FIELD:
    case UDAT_EXTENDED_YEAR_FIELD:
      return isolate->factory()->year_string();
    case UDAT_YEAR_NAME_FIELD:
      return isolate->factory()->yearName_string();
    case UDAT_MONTH_FIELD:
    case UDAT_STANDALONE_MONTH_FIELD:
      return isolate->factory()->month_string();
    case UDAT_DATE_FIELD:
      return isolate->factory()->day_string();
    case UDAT_HOUR_OF_DAY1_FIELD:
    case UDAT_HOUR_OF_DAY0_FIELD:
    case UDAT_HOUR1_FIELD:
    case UDAT_HOUR0_FIELD:
      return isolate->factory()->hour_string();
    case UDAT_MINUTE_FIELD:
      return isolate->factory()->minute_string();
    case UDAT_SECOND_FIELD:
      return isolate->factory()->second_string();
    case UDAT_DAY_OF_WEEK_FIELD:
    case UDAT_DOW_LOCAL_FIELD:
    case UDAT_STANDALONE_DAY_FIELD:
      return isolate->factory()->weekday_string();
    case UDAT_AM_PM_FIELD:
    case UDAT_AM_PM_MIDNIGHT_NOON_FIELD:
    case UDAT_FLEXIBLE_DAY_PERIOD_FIELD:
      return isolate->factory()->dayPeriod_string();
    case UDAT_TIMEZONE_FIELD:
    case UDAT_TIMEZONE_RFC_FIELD:
    case UDAT_TIMEZONE_GENERIC_FIELD:
    case UDAT_TIMEZONE_SPECIAL_FIELD:
    case UDAT_TIMEZONE_LOCALIZED_GMT_OFFSET_FIELD:
    case UDAT_TIMEZONE_ISO_FIELD:
    case UDAT_TIMEZONE_ISO_LOCAL_FIELD:
      return isolate->factory()->timeZoneName_string();
    case UDAT_ERA_FIELD:
      return isolate->factory()->era_string();
    case UDAT_FRACTIONAL_SECOND_FIELD:
      return isolate->factory()->fractionalSecond_string();
    case UDAT_RELATED_YEAR_FIELD:
      return isolate->factory()->relatedYear_string();

    case UDAT_QUARTER_FIELD:
    case UDAT_STANDALONE_QUARTER_FIELD:
    default:
      // Other UDAT_*_FIELD's cannot show up because there is no way to specify
      // them via options of Intl.DateTimeFormat.
      UNREACHABLE();
  }
}

}  // namespace

MaybeHandle<JSArray> JSDateTimeFormat::FormatToParts(
    Isolate* isolate, Handle<JSDateTimeFormat> date_time_format,
    double date_value, bool output_source) {
  Factory* factory = isolate->factory();
  icu::SimpleDateFormat* format =
      date_time_format->icu_simple_date_format().raw();
  DCHECK_NOT_NULL(format);

  icu::UnicodeString formatted;
  icu::FieldPositionIterator fp_iter;
  icu::FieldPosition fp;
  UErrorCode status = U_ZERO_ERROR;
  format->format(date_value, formatted, &fp_iter, status);
  if (U_FAILURE(status)) {
    THROW_NEW_ERROR(isolate, NewTypeError(MessageTemplate::kIcuError), JSArray);
  }

  Handle<JSArray> result = factory->NewJSArray(0);
  int32_t length = formatted.length();
  if (length == 0) return result;

  int index = 0;
  int32_t previous_end_pos = 0;
  Handle<String> substring;
  while (fp_iter.next(fp)) {
    int32_t begin_pos = fp.getBeginIndex();
    int32_t end_pos = fp.getEndIndex();

    if (previous_end_pos < begin_pos) {
      ASSIGN_RETURN_ON_EXCEPTION(
          isolate, substring,
          Intl::ToString(isolate, formatted, previous_end_pos, begin_pos),
          JSArray);
      if (output_source) {
        Intl::AddElement(isolate, result, index,
                         IcuDateFieldIdToDateType(-1, isolate), substring,
                         isolate->factory()->source_string(),
                         isolate->factory()->shared_string());
      } else {
        Intl::AddElement(isolate, result, index,
                         IcuDateFieldIdToDateType(-1, isolate), substring);
      }
      ++index;
    }
    ASSIGN_RETURN_ON_EXCEPTION(
        isolate, substring,
        Intl::ToString(isolate, formatted, begin_pos, end_pos), JSArray);
    if (output_source) {
      Intl::AddElement(isolate, result, index,
                       IcuDateFieldIdToDateType(fp.getField(), isolate),
                       substring, isolate->factory()->source_string(),
                       isolate->factory()->shared_string());
    } else {
      Intl::AddElement(isolate, result, index,
                       IcuDateFieldIdToDateType(fp.getField(), isolate),
                       substring);
    }
    previous_end_pos = end_pos;
    ++index;
  }
  if (previous_end_pos < length) {
    ASSIGN_RETURN_ON_EXCEPTION(
        isolate, substring,
        Intl::ToString(isolate, formatted, previous_end_pos, length), JSArray);
    if (output_source) {
      Intl::AddElement(isolate, result, index,
                       IcuDateFieldIdToDateType(-1, isolate), substring,
                       isolate->factory()->source_string(),
                       isolate->factory()->shared_string());
    } else {
      Intl::AddElement(isolate, result, index,
                       IcuDateFieldIdToDateType(-1, isolate), substring);
    }
  }
  JSObject::ValidateElements(*result);
  return result;
}

const std::set<std::string>& JSDateTimeFormat::GetAvailableLocales() {
  return Intl::GetAvailableLocalesForDateFormat();
}

Handle<String> JSDateTimeFormat::HourCycleAsString() const {
  switch (hour_cycle()) {
    case HourCycle::kUndefined:
      return GetReadOnlyRoots().undefined_string_handle();
    case HourCycle::kH11:
      return GetReadOnlyRoots().h11_string_handle();
    case HourCycle::kH12:
      return GetReadOnlyRoots().h12_string_handle();
    case HourCycle::kH23:
      return GetReadOnlyRoots().h23_string_handle();
    case HourCycle::kH24:
      return GetReadOnlyRoots().h24_string_handle();
    default:
      UNREACHABLE();
  }
}

enum Source { kShared, kStartRange, kEndRange };

namespace {

class SourceTracker {
 public:
  SourceTracker() { start_[0] = start_[1] = limit_[0] = limit_[1] = 0; }
  void Add(int32_t field, int32_t start, int32_t limit) {
    DCHECK_LT(field, 2);
    start_[field] = start;
    limit_[field] = limit;
  }

  Source GetSource(int32_t start, int32_t limit) const {
    Source source = Source::kShared;
    if (FieldContains(0, start, limit)) {
      source = Source::kStartRange;
    } else if (FieldContains(1, start, limit)) {
      source = Source::kEndRange;
    }
    return source;
  }

 private:
  int32_t start_[2];
  int32_t limit_[2];

  bool FieldContains(int32_t field, int32_t start, int32_t limit) const {
    DCHECK_LT(field, 2);
    return (start_[field] <= start) && (start <= limit_[field]) &&
           (start_[field] <= limit) && (limit <= limit_[field]);
  }
};

Handle<String> SourceString(Isolate* isolate, Source source) {
  switch (source) {
    case Source::kShared:
      return ReadOnlyRoots(isolate).shared_string_handle();
    case Source::kStartRange:
      return ReadOnlyRoots(isolate).startRange_string_handle();
    case Source::kEndRange:
      return ReadOnlyRoots(isolate).endRange_string_handle();
      UNREACHABLE();
  }
}

Maybe<bool> AddPartForFormatRange(Isolate* isolate, Handle<JSArray> array,
                                  const icu::UnicodeString& string,
                                  int32_t index, int32_t field, int32_t start,
                                  int32_t end, const SourceTracker& tracker) {
  Handle<String> substring;
  ASSIGN_RETURN_ON_EXCEPTION_VALUE(isolate, substring,
                                   Intl::ToString(isolate, string, start, end),
                                   Nothing<bool>());
  Intl::AddElement(isolate, array, index,
                   IcuDateFieldIdToDateType(field, isolate), substring,
                   isolate->factory()->source_string(),
                   SourceString(isolate, tracker.GetSource(start, end)));
  return Just(true);
}

MaybeHandle<String> FormattedToString(Isolate* isolate,
                                      const icu::FormattedValue& formatted,
                                      bool* outputRange) {
  UErrorCode status = U_ZERO_ERROR;
  icu::UnicodeString result = formatted.toString(status);
  if (U_FAILURE(status)) {
    THROW_NEW_ERROR(isolate, NewTypeError(MessageTemplate::kIcuError), String);
  }
  *outputRange = false;
  icu::ConstrainedFieldPosition cfpos;
  while (formatted.nextPosition(cfpos, status)) {
    if (cfpos.getCategory() == UFIELD_CATEGORY_DATE_INTERVAL_SPAN) {
      *outputRange = true;
      break;
    }
  }
  return Intl::ToString(isolate, result);
}

// A helper function to convert the FormattedDateInterval to a
// MaybeHandle<JSArray> for the implementation of formatRangeToParts.
MaybeHandle<JSArray> FormattedDateIntervalToJSArray(
    Isolate* isolate, const icu::FormattedValue& formatted, bool* outputRange) {
  UErrorCode status = U_ZERO_ERROR;
  icu::UnicodeString result = formatted.toString(status);

  Factory* factory = isolate->factory();
  Handle<JSArray> array = factory->NewJSArray(0);
  icu::ConstrainedFieldPosition cfpos;
  int index = 0;
  int32_t previous_end_pos = 0;
  SourceTracker tracker;
  *outputRange = false;
  while (formatted.nextPosition(cfpos, status)) {
    int32_t category = cfpos.getCategory();
    int32_t field = cfpos.getField();
    int32_t start = cfpos.getStart();
    int32_t limit = cfpos.getLimit();

    if (category == UFIELD_CATEGORY_DATE_INTERVAL_SPAN) {
      DCHECK_LE(field, 2);
      *outputRange = true;
      tracker.Add(field, start, limit);
    } else {
      DCHECK(category == UFIELD_CATEGORY_DATE);
      if (start > previous_end_pos) {
        // Add "literal" from the previous end position to the start if
        // necessary.
        Maybe<bool> maybe_added =
            AddPartForFormatRange(isolate, array, result, index, -1,
                                  previous_end_pos, start, tracker);
        MAYBE_RETURN(maybe_added, Handle<JSArray>());
        previous_end_pos = start;
        index++;
      }
      Maybe<bool> maybe_added = AddPartForFormatRange(
          isolate, array, result, index, field, start, limit, tracker);
      MAYBE_RETURN(maybe_added, Handle<JSArray>());
      previous_end_pos = limit;
      ++index;
    }
  }
  int32_t end = result.length();
  // Add "literal" in the end if necessary.
  if (end > previous_end_pos) {
    Maybe<bool> maybe_added = AddPartForFormatRange(
        isolate, array, result, index, -1, previous_end_pos, end, tracker);
    MAYBE_RETURN(maybe_added, Handle<JSArray>());
  }

  if (U_FAILURE(status)) {
    THROW_NEW_ERROR(isolate, NewTypeError(MessageTemplate::kIcuError), JSArray);
  }

  JSObject::ValidateElements(*array);
  return array;
}

// The shared code between formatRange and formatRangeToParts
template <typename T>
MaybeHandle<T> FormatRangeCommon(
    Isolate* isolate, Handle<JSDateTimeFormat> date_time_format, double x,
    double y,
    const std::function<MaybeHandle<T>(Isolate*, const icu::FormattedValue&,
                                       bool*)>& formatToResult,
    bool* outputRange) {
  // Track newer feature formateRange and formatRangeToParts
  isolate->CountUsage(v8::Isolate::UseCounterFeature::kDateTimeFormatRange);

  // #sec-partitiondatetimerangepattern
  // 1. Let x be TimeClip(x).
  x = DateCache::TimeClip(x);
  // 2. If x is NaN, throw a RangeError exception.
  if (std::isnan(x)) {
    THROW_NEW_ERROR(isolate, NewRangeError(MessageTemplate::kInvalidTimeValue),
                    T);
  }
  // 3. Let y be TimeClip(y).
  y = DateCache::TimeClip(y);
  // 4. If y is NaN, throw a RangeError exception.
  if (std::isnan(y)) {
    THROW_NEW_ERROR(isolate, NewRangeError(MessageTemplate::kInvalidTimeValue),
                    T);
  }

  icu::DateIntervalFormat* format =
      LazyCreateDateIntervalFormat(isolate, date_time_format);
  if (format == nullptr) {
    THROW_NEW_ERROR(isolate, NewTypeError(MessageTemplate::kIcuError), T);
  }

  UErrorCode status = U_ZERO_ERROR;

  icu::SimpleDateFormat* date_format =
      date_time_format->icu_simple_date_format().raw();
  const icu::Calendar* calendar = date_format->getCalendar();
  std::unique_ptr<icu::Calendar> c1(calendar->clone());
  std::unique_ptr<icu::Calendar> c2(calendar->clone());
  c1->setTime(x, status);
  c2->setTime(y, status);
  // We need to format by Calendar because we need the Gregorian change
  // adjustment already in the SimpleDateFormat to set the correct value of date
  // older than Oct 15, 1582.
  icu::FormattedDateInterval formatted =
      format->formatToValue(*c1, *c2, status);
  if (U_FAILURE(status)) {
    THROW_NEW_ERROR(isolate, NewTypeError(MessageTemplate::kIcuError), T);
  }
  return formatToResult(isolate, formatted, outputRange);
}

}  // namespace

MaybeHandle<String> JSDateTimeFormat::FormatRange(
    Isolate* isolate, Handle<JSDateTimeFormat> date_time_format, double x,
    double y) {
  bool outputRange = true;
  MaybeHandle<String> ret = FormatRangeCommon<String>(
      isolate, date_time_format, x, y, FormattedToString, &outputRange);
  if (outputRange) {
    return ret;
  }
  return FormatDateTime(isolate,
                        *(date_time_format->icu_simple_date_format().raw()), x);
}

MaybeHandle<JSArray> JSDateTimeFormat::FormatRangeToParts(
    Isolate* isolate, Handle<JSDateTimeFormat> date_time_format, double x,
    double y) {
  bool outputRange = true;
  MaybeHandle<JSArray> ret =
      FormatRangeCommon<JSArray>(isolate, date_time_format, x, y,
                                 FormattedDateIntervalToJSArray, &outputRange);
  if (outputRange) {
    return ret;
  }
  return JSDateTimeFormat::FormatToParts(isolate, date_time_format, x, true);
}

}  // namespace internal
}  // namespace v8
