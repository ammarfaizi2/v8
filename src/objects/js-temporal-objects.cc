// Copyright 2021 the V8 project authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "src/objects/js-temporal-objects.h"

#include <set>

#include "src/common/globals.h"
#include "src/date/date.h"
#include "src/execution/isolate.h"
#include "src/heap/factory.h"
#include "src/numbers/conversions-inl.h"
#ifdef V8_INTL_SUPPORT
#include "src/objects/intl-objects.h"
#include "src/objects/js-date-time-format.h"
#endif  // V8_INTL_SUPPORT
#include "src/objects/js-objects-inl.h"
#include "src/objects/js-objects.h"
#include "src/objects/js-temporal-objects-inl.h"
#ifdef V8_INTL_SUPPORT
#include "src/objects/managed-inl.h"
#endif  // V8_INTL_SUPPORT
#include "src/objects/objects-inl.h"
#include "src/objects/option-utils.h"
#include "src/objects/property-descriptor.h"
#include "src/strings/string-builder-inl.h"
#include "src/temporal/temporal-parser.h"
#ifdef V8_INTL_SUPPORT
#include "unicode/calendar.h"
#include "unicode/unistr.h"
#endif  // V8_INTL_SUPPORT

namespace v8 {
namespace internal {

namespace {

enum class Unit {
  kNotPresent,
  kAuto,
  kYear,
  kMonth,
  kWeek,
  kDay,
  kHour,
  kMinute,
  kSecond,
  kMillisecond,
  kMicrosecond,
  kNanosecond
};

/**
 * This header declare the Abstract Operations defined in the
 * Temporal spec with the enum and struct for them.
 */

// Struct
struct DateTimeRecordCommon {
  int32_t year;
  int32_t month;
  int32_t day;
  int32_t hour;
  int32_t minute;
  int32_t second;
  int32_t millisecond;
  int32_t microsecond;
  int32_t nanosecond;
};

struct DurationRecord {
  int64_t years;
  int64_t months;
  int64_t weeks;
  int64_t days;
  int64_t hours;
  int64_t minutes;
  int64_t seconds;
  int64_t milliseconds;
  int64_t microseconds;
  int64_t nanoseconds;
};

struct TimeZoneRecord {
  bool z;
  Handle<String> offset_string;
  Handle<String> name;
};

// Options

V8_WARN_UNUSED_RESULT Handle<String> UnitToString(Isolate* isolate, Unit unit);

// #sec-temporal-totemporaldisambiguation
enum class Disambiguation { kCompatible, kEarlier, kLater, kReject };

// ISO8601 String Parsing

// #sec-temporal-parsetemporalcalendarstring
V8_WARN_UNUSED_RESULT MaybeHandle<String> ParseTemporalCalendarString(
    Isolate* isolate, Handle<String> iso_string);

// #sec-temporal-parsetemporaltimezone
V8_WARN_UNUSED_RESULT MaybeHandle<String> ParseTemporalTimeZone(
    Isolate* isolate, Handle<String> string);

V8_WARN_UNUSED_RESULT Maybe<int64_t> ParseTimeZoneOffsetString(
    Isolate* isolate, Handle<String> offset_string,
    bool throwIfNotSatisfy = true);

void BalanceISODate(Isolate* isolate, int32_t* year, int32_t* month,
                    int32_t* day);

// Math and Misc

V8_WARN_UNUSED_RESULT MaybeHandle<BigInt> AddInstant(
    Isolate* isolate, Handle<BigInt> epoch_nanoseconds, int64_t hours,
    int64_t minutes, int64_t seconds, int64_t milliseconds,
    int64_t microseconds, int64_t nanoseconds);

// #sec-temporal-balanceduration
V8_WARN_UNUSED_RESULT Maybe<bool> BalanceDuration(
    Isolate* isolate, int64_t* days, int64_t* hours, int64_t* minutes,
    int64_t* seconds, int64_t* milliseconds, int64_t* microseconds,
    int64_t* nanoseconds, Unit largest_unit, Handle<Object> relative_to,
    const char* method);

V8_WARN_UNUSED_RESULT Maybe<DurationRecord> DifferenceISODateTime(
    Isolate* isolate, int32_t y1, int32_t mon1, int32_t d1, int32_t h1,
    int32_t min1, int32_t s1, int32_t ms1, int32_t mus1, int32_t ns1,
    int32_t y2, int32_t mon2, int32_t d2, int32_t h2, int32_t min2, int32_t s2,
    int32_t ms2, int32_t mus2, int32_t ns2, Handle<JSReceiver> calendar,
    Unit largest_unit, Handle<Object> relative_to, const char* method);

// #sec-temporal-adddatetime
V8_WARN_UNUSED_RESULT Maybe<DateTimeRecordCommon> AddDateTime(
    Isolate* isolate, int32_t year, int32_t month, int32_t day, int32_t hour,
    int32_t minute, int32_t second, int32_t millisecond, int32_t microsecond,
    int32_t nanosecond, Handle<JSReceiver> calendar, const DurationRecord& dur,
    Handle<Object> options);

// #sec-temporal-addzoneddatetime
V8_WARN_UNUSED_RESULT MaybeHandle<BigInt> AddZonedDateTime(
    Isolate* isolate, Handle<BigInt> eopch_nanoseconds,
    Handle<JSReceiver> time_zone, Handle<JSReceiver> calendar,
    const DurationRecord& duration, const char* method);

V8_WARN_UNUSED_RESULT MaybeHandle<BigInt> AddZonedDateTime(
    Isolate* isolate, Handle<BigInt> eopch_nanoseconds,
    Handle<JSReceiver> time_zone, Handle<JSReceiver> calendar,
    const DurationRecord& duration, Handle<JSReceiver> options,
    const char* method);

// #sec-temporal-isvalidepochnanoseconds
bool IsValidEpochNanoseconds(Isolate* isolate,
                             Handle<BigInt> epoch_nanoseconds);

// #sec-temporal-isvalidduration
bool IsValidDuration(Isolate* isolate, const DurationRecord& dur);

// #sec-temporal-nanosecondstodays
V8_WARN_UNUSED_RESULT Maybe<bool> NanosecondsToDays(
    Isolate* isolate, Handle<BigInt> nanoseconds,
    Handle<Object> relative_to_obj, int64_t* result_days,
    int64_t* result_nanoseconds, int64_t* result_day_length,
    const char* method);

V8_WARN_UNUSED_RESULT Maybe<bool> NanosecondsToDays(
    Isolate* isolate, int64_t nanoseconds, Handle<Object> relative_to_obj,
    int64_t* result_days, int64_t* resultj_nanoseconds,
    int64_t* result_day_length, const char* method);

// #sec-temporal-interpretisodatetimeoffset
enum class OffsetBehaviour { kOption, kExact, kWall };

V8_WARN_UNUSED_RESULT
MaybeHandle<BigInt> GetEpochFromISOParts(Isolate* isolate, int32_t year,
                                         int32_t month, int32_t day,
                                         int32_t hour, int32_t minute,
                                         int32_t second, int32_t millisecond,
                                         int32_t microsecond,
                                         int32_t nanosecond);

int32_t DurationSign(Isolate* isolaet, const DurationRecord& dur);

// #sec-temporal-isodaysinmonth
int32_t ISODaysInMonth(Isolate* isolate, int32_t year, int32_t month);

// #sec-temporal-isodaysinyear
int32_t ISODaysInYear(Isolate* isolate, int32_t year);

bool IsValidTime(Isolate* isolate, int32_t hour, int32_t minute, int32_t second,
                 int32_t millisecond, int32_t microsecond, int32_t nanosecond);

// #sec-temporal-isvalidisodate
bool IsValidISODate(Isolate* isolate, int32_t year, int32_t month, int32_t day);

// #sec-temporal-compareisodate
int32_t CompareISODate(Isolate* isolate, int32_t y1, int32_t m1, int32_t d1,
                       int32_t y2, int32_t m2, int32_t d2);

// #sec-temporal-balanceisoyearmonth
void BalanceISOYearMonth(Isolate* isolate, int32_t* year, int32_t* month);

// #sec-temporal-balancetime
V8_WARN_UNUSED_RESULT DateTimeRecordCommon
BalanceTime(Isolate* isolate, int64_t hour, int64_t minute, int64_t second,
            int64_t millisecond, int64_t microsecond, int64_t nanosecond);

// #sec-temporal-differencetime
V8_WARN_UNUSED_RESULT DurationRecord
DifferenceTime(Isolate* isolate, int32_t h1, int32_t min1, int32_t s1,
               int32_t ms1, int32_t mus1, int32_t ns1, int32_t h2, int32_t min2,
               int32_t s2, int32_t ms2, int32_t mus2, int32_t ns2);

// #sec-temporal-addtime
V8_WARN_UNUSED_RESULT DateTimeRecordCommon
AddTime(Isolate* isolate, int64_t hour, int64_t minute, int64_t second,
        int64_t millisecond, int64_t microsecond, int64_t nanosecond,
        int64_t hours, int64_t minutes, int64_t seconds, int64_t milliseconds,
        int64_t microseconds, int64_t nanoseconds);

// #sec-temporal-totaldurationnanoseconds
int64_t TotalDurationNanoseconds(Isolate* isolate, int64_t days, int64_t hours,
                                 int64_t minutes, int64_t seconds,
                                 int64_t milliseconds, int64_t microseconds,
                                 int64_t nanoseconds, int64_t offset_shift);

// Calendar Operations

V8_WARN_UNUSED_RESULT MaybeHandle<JSTemporalPlainDate> CalendarDateAdd(
    Isolate* isolate, Handle<JSReceiver> calendar, Handle<Object> date,
    Handle<Object> durations, Handle<Object> options, Handle<Object> date_add);

V8_WARN_UNUSED_RESULT MaybeHandle<JSTemporalDuration> CalendarDateUntil(
    Isolate* isolate, Handle<JSReceiver> calendar, Handle<Object> one,
    Handle<Object> two, Handle<Object> options, Handle<Object> date_until);

// #sec-temporal-getoffsetnanosecondsfor
V8_WARN_UNUSED_RESULT Maybe<int64_t> GetOffsetNanosecondsFor(
    Isolate* isolate, Handle<JSReceiver> time_zone, Handle<Object> instant,
    const char* method);

bool IsBuiltinCalendar(Isolate* isolate, Handle<String> id);

// Internal Helper Function
int32_t CalendarIndex(Isolate* isolate, Handle<String> id);

// #sec-isvalidtimezonename
bool IsValidTimeZoneName(Isolate* isolate, Handle<String> time_zone);

// #sec-canonicalizetimezonename
V8_WARN_UNUSED_RESULT MaybeHandle<String> CanonicalizeTimeZoneName(
    Isolate* isolate, Handle<String> identifier);

inline int64_t floor_divide(int64_t a, int64_t b) {
  return (((a) / (b)) + ((((a) < 0) && (((a) % (b)) != 0)) ? -1 : 0));
}
inline int64_t modulo(int64_t a, int64_t b) {
  return ((((a) % (b)) + (b)) % (b));
}

#define STRINGIFY(x) #x
#define TOSTRING(x) STRINGIFY(x)
#define AT __FILE__ ":" TOSTRING(__LINE__)

#ifdef DEBUG
#define TEMPORAL_DEBUG_INFO AT
#define TEMPORAL_ENTER_FUNC()
// #define TEMPORAL_ENTER_FUNC()  do { PrintF("Start: %s\n", __func__); } while
// (false)
#else
// #define TEMPORAL_DEBUG_INFO ""
#define TEMPORAL_DEBUG_INFO AT
#define TEMPORAL_ENTER_FUNC()
// #define TEMPORAL_ENTER_FUNC()  do { PrintF("Start: %s\n", __func__); } while
// (false)
#endif  // DEBUG

#define NEW_TEMPORAL_INVALD_ARG_TYPE_ERROR()        \
  NewTypeError(                                     \
      MessageTemplate::kInvalidArgumentForTemporal, \
      isolate->factory()->NewStringFromStaticChars(TEMPORAL_DEBUG_INFO))

#define NEW_TEMPORAL_INVALD_ARG_RANGE_ERROR()        \
  NewRangeError(                                     \
      MessageTemplate::kInvalidTimeValueForTemporal, \
      isolate->factory()->NewStringFromStaticChars(TEMPORAL_DEBUG_INFO))

// #sec-temporal-isodatetimewithinlimits
bool ISODateTimeWithinLimits(Isolate* isolate, int32_t year, int32_t month,
                             int32_t day, int32_t hour, int32_t minute,
                             int32_t second, int32_t millisecond,
                             int32_t microsecond, int32_t nanosecond) {
  TEMPORAL_ENTER_FUNC();
  /**
   * Note: It is really overkill to decide within the limit by following the
   * specified algorithm literally, which require the conversion to BigInt.
   * Take a short cut and use pre-calculated year/month/day boundary instead.
   *
   * Math:
   * (-8.64 x 10^21- 8.64 x 10^16,  8.64 x 10^21 + 8.64 x 10^16) ns
   * = (-8.64 x 9999 x 10^16,  8.64 x 9999 x 10^16) ns
   * = (-8.64 x 9999 x 10^10,  8.64 x 9999 x 10^10) millisecond
   * = (-8.64 x 9999 x 10^7,  8.64 x 9999 x 10^7) second
   * = (-86400 x 9999 x 10^3,  86400 x 9999 x 10^3) second
   * = (-9999 x 10^3,  9999 x 10^3) days => Because 60*60*24 = 86400
   * 9999000 days is about 27376 years, 4 months and 7 days.
   * Therefore 9999000 days before Jan 1 1970 is around Auguest 23, -25407 and
   * 9999000 days after Jan 1 1970 is around April 9, 29346.
   */
  if (year > -25407 && year < 29346) return true;
  if (year < -25407 || year > 29346) return false;
  if (year == -25407) {
    if (month > 8) return true;
    if (month < 8) return false;
    return (day > 23);
  } else {
    DCHECK_EQ(year, 29346);
    if (month > 4) return false;
    if (month < 4) return true;
    return (day > 23);
  }
  // 1. Assert: year, month, day, hour, minute, second, millisecond,
  // microsecond, and nanosecond are integers.
  // 2. Let ns be ! GetEpochFromISOParts(year, month, day, hour, minute,
  // second, millisecond, microsecond, nanosecond).
  // 3. If ns ≤ -8.64 × 10^21 - 8.64 × 10^16, then
  // 4. If ns ≥ 8.64 × 10^21 + 8.64 × 10^16, then
  // 5. Return true.
}

// #sec-temporal-isoyearmonthwithinlimits
bool ISOYearMonthWithinLimits(int32_t year, int32_t month) {
  TEMPORAL_ENTER_FUNC();
  // 1. Assert: year and month are integers.
  // 2. If year < −271821 or year > 275760, then
  // a. Return false.
  if (year < -271821 || year > 275760) return false;
  // 3. If year is −271821 and month < 4, then
  // a. Return false.
  if (year == -271821 && month < 4) return false;
  // 4. If year is 275760 and month > 9, then
  // a. Return false.
  if (year == 275760 && month > 9) return false;
  // 5. Return true.
  return true;
}

#define ORDINARY_CREATE_FROM_CONSTRUCTOR(obj, target, new_target, T)       \
  Handle<JSReceiver> new_target_receiver =                                 \
      Handle<JSReceiver>::cast(new_target);                                \
  Handle<Map> map;                                                         \
  ASSIGN_RETURN_ON_EXCEPTION(                                              \
      isolate, map,                                                        \
      JSFunction::GetDerivedMap(isolate, target, new_target_receiver), T); \
  Handle<T> object =                                                       \
      Handle<T>::cast(isolate->factory()->NewFastOrSlowJSObjectFromMap(map));

#define THROW_INVALID_RANGE(T) \
  THROW_NEW_ERROR(isolate, NEW_TEMPORAL_INVALD_ARG_RANGE_ERROR(), T);

#define CONSTRUCTOR(name)                                                    \
  Handle<JSFunction>(                                                        \
      JSFunction::cast(                                                      \
          isolate->context().native_context().temporal_##name##_function()), \
      isolate)

// #sec-temporal-createtemporalcalendar
MaybeHandle<JSTemporalCalendar> CreateTemporalCalendar(
    Isolate* isolate, Handle<JSFunction> target, Handle<HeapObject> new_target,
    Handle<String> identifier) {
  TEMPORAL_ENTER_FUNC();
  // 1. Assert: ! IsBuiltinCalendar(identifier) is true.
  // 2. If newTarget is not provided, set newTarget to %Temporal.Calendar%.
  // 3. Let object be ? OrdinaryCreateFromConstructor(newTarget,
  // "%Temporal.Calendar.prototype%", « [[InitializedTemporalCalendar]],
  // [[Identifier]] »).
  int32_t index = CalendarIndex(isolate, identifier);

  ORDINARY_CREATE_FROM_CONSTRUCTOR(object, target, new_target,
                                   JSTemporalCalendar)

  object->set_flags(0);
  // 4. Set object.[[Identifier]] to identifier.
  object->set_calendar_index(index);
  // 5. Return object.
  return object;
}

MaybeHandle<JSTemporalCalendar> CreateTemporalCalendar(
    Isolate* isolate, Handle<String> identifier) {
  TEMPORAL_ENTER_FUNC();
  return CreateTemporalCalendar(isolate, CONSTRUCTOR(calendar),
                                CONSTRUCTOR(calendar), identifier);
}

// #sec-temporal-createtemporaldate
MaybeHandle<JSTemporalPlainDate> CreateTemporalDate(
    Isolate* isolate, Handle<JSFunction> target, Handle<HeapObject> new_target,
    int32_t iso_year, int32_t iso_month, int32_t iso_day,
    Handle<JSReceiver> calendar) {
  TEMPORAL_ENTER_FUNC();
  // 1. Assert: isoYear is an integer.
  // 2. Assert: isoMonth is an integer.
  // 3. Assert: isoDay is an integer.
  // 4. Assert: Type(calendar) is Object.
  // 5. If ! IsValidISODate(isoYear, isoMonth, isoDay) is false, throw a
  // RangeError exception.
  if (!IsValidISODate(isolate, iso_year, iso_month, iso_day)) {
    THROW_INVALID_RANGE(JSTemporalPlainDate);
  }
  // 6. If ! ISODateTimeWithinLimits(isoYear, isoMonth, isoDay, 12, 0, 0, 0, 0,
  // 0) is false, throw a RangeError exception.
  if (!ISODateTimeWithinLimits(isolate, iso_year, iso_month, iso_day, 12, 0, 0,
                               0, 0, 0)) {
    THROW_INVALID_RANGE(JSTemporalPlainDate);
  }
  // 7. If newTarget is not present, set it to %Temporal.PlainDate%.

  // 8. Let object be ? OrdinaryCreateFromConstructor(newTarget,
  // "%Temporal.PlainDate.prototype%", « [[InitializedTemporalDate]],
  // [[ISOYear]], [[ISOMonth]], [[ISODay]], [[Calendar]] »).
  ORDINARY_CREATE_FROM_CONSTRUCTOR(object, target, new_target,
                                   JSTemporalPlainDate)
  object->set_year_month_day(0);
  // 9. Set object.[[ISOYear]] to isoYear.
  object->set_iso_year(iso_year);
  // 10. Set object.[[ISOMonth]] to isoMonth.
  object->set_iso_month(iso_month);
  // 11. Set object.[[ISODay]] to isoDay.
  object->set_iso_day(iso_day);
  // 12. Set object.[[Calendar]] to calendar.
  object->set_calendar(*calendar);
  // 13. Return object.
  return object;
}

MaybeHandle<JSTemporalPlainDate> CreateTemporalDate(
    Isolate* isolate, int32_t iso_year, int32_t iso_month, int32_t iso_day,
    Handle<JSReceiver> calendar) {
  TEMPORAL_ENTER_FUNC();
  return CreateTemporalDate(isolate, CONSTRUCTOR(plain_date),
                            CONSTRUCTOR(plain_date), iso_year, iso_month,
                            iso_day, calendar);
}

// #sec-temporal-createtemporaldatetime
MaybeHandle<JSTemporalPlainDateTime> CreateTemporalDateTime(
    Isolate* isolate, Handle<JSFunction> target, Handle<HeapObject> new_target,
    int32_t iso_year, int32_t iso_month, int32_t iso_day, int32_t hour,
    int32_t minute, int32_t second, int32_t millisecond, int32_t microsecond,
    int32_t nanosecond, Handle<JSReceiver> calendar) {
  TEMPORAL_ENTER_FUNC();
  // 1. Assert: isoYear, isoMonth, isoDay, hour, minute, second, millisecond,
  // microsecond, and nanosecond are integers.
  // 2. Assert: Type(calendar) is Object.
  // 3. If ! IsValidISODate(isoYear, isoMonth, isoDay) is false, throw a
  // RangeError exception.
  if (!IsValidISODate(isolate, iso_year, iso_month, iso_day)) {
    THROW_INVALID_RANGE(JSTemporalPlainDateTime);
  }
  // 4. If ! IsValidTime(hour, minute, second, millisecond, microsecond,
  // nanosecond) is false, throw a RangeError exception.
  if (!IsValidTime(isolate, hour, minute, second, millisecond, microsecond,
                   nanosecond)) {
    THROW_INVALID_RANGE(JSTemporalPlainDateTime);
  }
  // 5. If ! ISODateTimeWithinLimits(isoYear, isoMonth, isoDay, hour, minute,
  // second, millisecond, microsecond, nanosecond) is false, then
  if (!ISODateTimeWithinLimits(isolate, iso_year, iso_month, iso_day, hour,
                               minute, second, millisecond, microsecond,
                               nanosecond)) {
    // a. Throw a RangeError exception.
    THROW_INVALID_RANGE(JSTemporalPlainDateTime);
  }
  // 6. If newTarget is not present, set it to %Temporal.PlainDateTime%.
  // 7. Let object be ? OrdinaryCreateFromConstructor(newTarget,
  // "%Temporal.PlainDateTime.prototype%", « [[InitializedTemporalDateTime]],
  // [[ISOYear]], [[ISOMonth]], [[ISODay]], [[ISOHour]], [[ISOMinute]],
  // [[ISOSecond]], [[ISOMillisecond]], [[ISOMicrosecond]], [[ISONanosecond]],
  // [[Calendar]] »).
  ORDINARY_CREATE_FROM_CONSTRUCTOR(object, target, new_target,
                                   JSTemporalPlainDateTime)

  object->set_year_month_day(0);
  object->set_hour_minute_second(0);
  object->set_second_parts(0);
  // 8. Set object.[[ISOYear]] to isoYear.
  object->set_iso_year(iso_year);
  // 9. Set object.[[ISOMonth]] to isoMonth.
  object->set_iso_month(iso_month);
  // 10. Set object.[[ISODay]] to isoDay.
  object->set_iso_day(iso_day);
  // 11. Set object.[[ISOHour]] to hour.
  object->set_iso_hour(hour);
  // 12. Set object.[[ISOMinute]] to minute.
  object->set_iso_minute(minute);
  // 13. Set object.[[ISOSecond]] to second.
  object->set_iso_second(second);
  // 14. Set object.[[ISOMillisecond]] to millisecond.
  object->set_iso_millisecond(millisecond);
  // 15. Set object.[[ISOMicrosecond]] to microsecond.
  object->set_iso_microsecond(microsecond);
  // 16. Set object.[[ISONanosecond]] to nanosecond.
  object->set_iso_nanosecond(nanosecond);
  // 17. Set object.[[Calendar]] to calendar.
  object->set_calendar(*calendar);
  // 18. Return object.
  return object;
}

MaybeHandle<JSTemporalPlainDateTime> CreateTemporalDateTimeDefaultTarget(
    Isolate* isolate, int32_t iso_year, int32_t iso_month, int32_t iso_day,
    int32_t hour, int32_t minute, int32_t second, int32_t millisecond,
    int32_t microsecond, int32_t nanosecond, Handle<JSReceiver> calendar) {
  TEMPORAL_ENTER_FUNC();
  return CreateTemporalDateTime(isolate, CONSTRUCTOR(plain_date_time),
                                CONSTRUCTOR(plain_date_time), iso_year,
                                iso_month, iso_day, hour, minute, second,
                                millisecond, microsecond, nanosecond, calendar);
}

}  // namespace

namespace temporal {

MaybeHandle<JSTemporalPlainDateTime> CreateTemporalDateTime(
    Isolate* isolate, int32_t iso_year, int32_t iso_month, int32_t iso_day,
    int32_t hour, int32_t minute, int32_t second, int32_t millisecond,
    int32_t microsecond, int32_t nanosecond, Handle<JSReceiver> calendar) {
  return CreateTemporalDateTimeDefaultTarget(
      isolate, iso_year, iso_month, iso_day, hour, minute, second, millisecond,
      microsecond, nanosecond, calendar);
}

}  // namespace temporal

namespace {
// #sec-temporal-createtemporaltime
MaybeHandle<JSTemporalPlainTime> CreateTemporalTime(
    Isolate* isolate, Handle<JSFunction> target, Handle<HeapObject> new_target,
    int32_t hour, int32_t minute, int32_t second, int32_t millisecond,
    int32_t microsecond, int32_t nanosecond) {
  TEMPORAL_ENTER_FUNC();
  // 2. If ! IsValidTime(hour, minute, second, millisecond, microsecond,
  // nanosecond) is false, throw a RangeError exception.
  if (!IsValidTime(isolate, hour, minute, second, millisecond, microsecond,
                   nanosecond)) {
    THROW_INVALID_RANGE(JSTemporalPlainTime);
  }

  // 4. Let object be ? OrdinaryCreateFromConstructor(newTarget,
  // "%Temporal.PlainTime.prototype%", « [[InitializedTemporalTime]],
  // [[ISOHour]], [[ISOMinute]], [[ISOSecond]], [[ISOMillisecond]],
  // [[ISOMicrosecond]], [[ISONanosecond]], [[Calendar]] »).
  ORDINARY_CREATE_FROM_CONSTRUCTOR(object, target, new_target,
                                   JSTemporalPlainTime)
  Handle<JSTemporalCalendar> calendar;
  ASSIGN_RETURN_ON_EXCEPTION(isolate, calendar,
                             temporal::GetISO8601Calendar(isolate),
                             JSTemporalPlainTime);
  object->set_hour_minute_second(0);
  object->set_second_parts(0);
  // 5. Set object.[[ISOHour]] to hour.
  object->set_iso_hour(hour);
  // 6. Set object.[[ISOMinute]] to minute.
  object->set_iso_minute(minute);
  // 7. Set object.[[ISOSecond]] to second.
  object->set_iso_second(second);
  // 8. Set object.[[ISOMillisecond]] to millisecond.
  object->set_iso_millisecond(millisecond);
  // 9. Set object.[[ISOMicrosecond]] to microsecond.
  object->set_iso_microsecond(microsecond);
  // 10. Set object.[[ISONanosecond]] to nanosecond.
  object->set_iso_nanosecond(nanosecond);
  // 11. Set object.[[Calendar]] to ? GetISO8601Calendar().
  object->set_calendar(*calendar);

  // 12. Return object.
  return object;
}

MaybeHandle<JSTemporalPlainMonthDay> CreateTemporalMonthDay(
    Isolate* isolate, Handle<JSFunction> target, Handle<HeapObject> new_target,
    int32_t iso_month, int32_t iso_day, Handle<JSReceiver> calendar,
    int32_t reference_iso_year) {
  TEMPORAL_ENTER_FUNC();
  // 1. Assert: isoMonth, isoDay, and referenceISOYear are integers.
  // 2. Assert: Type(calendar) is Object.
  // 3. If ! IsValidISODate(referenceISOYear, isoMonth, isoDay) is false, throw
  // a RangeError exception.
  if (!IsValidISODate(isolate, reference_iso_year, iso_month, iso_day)) {
    THROW_INVALID_RANGE(JSTemporalPlainMonthDay);
  }
  // 4. If newTarget is not present, set it to %Temporal.PlainMonthDay%.
  // 5. Let object be ? OrdinaryCreateFromConstructor(newTarget,
  // "%Temporal.PlainMonthDay.prototype%", « [[InitializedTemporalMonthDay]],
  // [[ISOMonth]], [[ISODay]], [[ISOYear]], [[Calendar]] »).
  ORDINARY_CREATE_FROM_CONSTRUCTOR(object, target, new_target,
                                   JSTemporalPlainMonthDay)
  object->set_year_month_day(0);
  // 6. Set object.[[ISOMonth]] to isoMonth.
  object->set_iso_month(iso_month);
  // 7. Set object.[[ISODay]] to isoDay.
  object->set_iso_day(iso_day);
  // 8. Set object.[[Calendar]] to calendar.
  object->set_calendar(*calendar);
  // 9. Set object.[[ISOYear]] to referenceISOYear.
  object->set_iso_year(reference_iso_year);
  // 10. Return object.
  return object;
}

// #sec-temporal-createtemporalyearmonth
MaybeHandle<JSTemporalPlainYearMonth> CreateTemporalYearMonth(
    Isolate* isolate, Handle<JSFunction> target, Handle<HeapObject> new_target,
    int32_t iso_year, int32_t iso_month, Handle<JSReceiver> calendar,
    int32_t reference_iso_day) {
  TEMPORAL_ENTER_FUNC();
  // 1. Assert: isoYear, isoMonth, and referenceISODay are integers.
  // 2. Assert: Type(calendar) is Object.
  // 3. If ! IsValidISODate(isoYear, isoMonth, referenceISODay) is false, throw
  // a RangeError exception.
  if (!IsValidISODate(isolate, iso_year, iso_month, reference_iso_day)) {
    THROW_INVALID_RANGE(JSTemporalPlainYearMonth);
  }
  // 4. If ! ISOYearMonthWithinLimits(isoYear, isoMonth) is false, throw a
  // RangeError exception.
  if (!ISOYearMonthWithinLimits(iso_year, iso_month)) {
    THROW_INVALID_RANGE(JSTemporalPlainYearMonth);
  }
  // 5. If newTarget is not present, set it to %Temporal.PlainYearMonth%.
  // 6. Let object be ? OrdinaryCreateFromConstructor(newTarget,
  // "%Temporal.PlainYearMonth.prototype%", « [[InitializedTemporalYearMonth]],
  // [[ISOYear]], [[ISOMonth]], [[ISODay]], [[Calendar]] »).
  ORDINARY_CREATE_FROM_CONSTRUCTOR(object, target, new_target,
                                   JSTemporalPlainYearMonth)
  object->set_year_month_day(0);
  // 7. Set object.[[ISOYear]] to isoYear.
  object->set_iso_year(iso_year);
  // 8. Set object.[[ISOMonth]] to isoMonth.
  object->set_iso_month(iso_month);
  // 9. Set object.[[Calendar]] to calendar.
  object->set_calendar(*calendar);
  // 10. Set object.[[ISODay]] to referenceISODay.
  object->set_iso_day(reference_iso_day);
  // 11. Return object.
  return object;
}

// #sec-temporal-createtemporalzoneddatetime
MaybeHandle<JSTemporalZonedDateTime> CreateTemporalZonedDateTime(
    Isolate* isolate, Handle<JSFunction> target, Handle<HeapObject> new_target,
    Handle<BigInt> epoch_nanoseconds, Handle<JSReceiver> time_zone,
    Handle<JSReceiver> calendar) {
  TEMPORAL_ENTER_FUNC();
  // 1. Assert: Type(epochNanoseconds) is BigInt.
  // 2. Assert: ! IsValidEpochNanoseconds(epochNanoseconds) is true.
  DCHECK(IsValidEpochNanoseconds(isolate, epoch_nanoseconds));
  // 3. Assert: Type(timeZone) is Object.
  // 4. Assert: Type(calendar) is Object.
  // 5. If newTarget is not present, set it to %Temporal.ZonedDateTime%.
  // 6. Let object be ? OrdinaryCreateFromConstructor(newTarget,
  // "%Temporal.ZonedDateTime.prototype%", «
  // [[InitializedTemporalZonedDateTime]], [[Nanoseconds]], [[TimeZone]],
  // [[Calendar]] »).
  ORDINARY_CREATE_FROM_CONSTRUCTOR(object, target, new_target,
                                   JSTemporalZonedDateTime)
  // 7. Set object.[[Nanoseconds]] to epochNanoseconds.
  object->set_nanoseconds(*epoch_nanoseconds);
  // 8. Set object.[[TimeZone]] to timeZone.
  object->set_time_zone(*time_zone);
  // 9. Set object.[[Calendar]] to calendar.
  object->set_calendar(*calendar);
  // 10. Return object.
  return object;
}

// #sec-temporal-createtemporalduration
MaybeHandle<JSTemporalDuration> CreateTemporalDuration(
    Isolate* isolate, Handle<JSFunction> target, Handle<HeapObject> new_target,
    int64_t years, int64_t months, int64_t weeks, int64_t days, int64_t hours,
    int64_t minutes, int64_t seconds, int64_t milliseconds,
    int64_t microseconds, int64_t nanoseconds) {
  TEMPORAL_ENTER_FUNC();
  Factory* factory = isolate->factory();
  // 1. If ! IsValidDuration(years, months, weeks, days, hours, minutes,
  // seconds, milliseconds, microseconds, nanoseconds) is false, throw a
  // RangeError exception.
  if (!IsValidDuration(isolate,
                       {years, months, weeks, days, hours, minutes, seconds,
                        milliseconds, microseconds, nanoseconds})) {
    THROW_INVALID_RANGE(JSTemporalDuration);
  }

  // 2. If newTarget is not present, set it to %Temporal.Duration%.
  // 3. Let object be ? OrdinaryCreateFromConstructor(newTarget,
  // "%Temporal.Duration.prototype%", « [[InitializedTemporalDuration]],
  // [[Years]], [[Months]], [[Weeks]], [[Days]], [[Hours]], [[Minutes]],
  // [[Seconds]], [[Milliseconds]], [[Microseconds]], [[Nanoseconds]] »).
  ORDINARY_CREATE_FROM_CONSTRUCTOR(object, target, new_target,
                                   JSTemporalDuration)
#define SET_FROM_INT64(obj, p)                            \
  do {                                                    \
    Handle<Object> item = factory->NewNumberFromInt64(p); \
    object->set_##p(*item);                               \
  } while (false)
  // 4. Set object.[[Years]] to years.
  SET_FROM_INT64(object, years);
  // 5. Set object.[[Months]] to months.
  SET_FROM_INT64(object, months);
  // 6. Set object.[[Weeks]] to weeks.
  SET_FROM_INT64(object, weeks);
  // 7. Set object.[[Days]] to days.
  SET_FROM_INT64(object, days);
  // 8. Set object.[[Hours]] to hours.
  SET_FROM_INT64(object, hours);
  // 9. Set object.[[Minutes]] to minutes.
  SET_FROM_INT64(object, minutes);
  // 10. Set object.[[Seconds]] to seconds.
  SET_FROM_INT64(object, seconds);
  // 11. Set object.[[Milliseconds]] to milliseconds.
  SET_FROM_INT64(object, milliseconds);
  // 12. Set object.[[Microseconds]] to microseconds.
  SET_FROM_INT64(object, microseconds);
  // 13. Set object.[[Nanoseconds]] to nanoseconds.
  SET_FROM_INT64(object, nanoseconds);
#undef SET_FROM_INT64
  // 14. Return object.
  return object;
}

MaybeHandle<JSTemporalDuration> CreateTemporalDuration(
    Isolate* isolate, int64_t years, int64_t months, int64_t weeks,
    int64_t days, int64_t hours, int64_t minutes, int64_t seconds,
    int64_t milliseconds, int64_t microseconds, int64_t nanoseconds) {
  TEMPORAL_ENTER_FUNC();
  return CreateTemporalDuration(isolate, CONSTRUCTOR(duration),
                                CONSTRUCTOR(duration), years, months, weeks,
                                days, hours, minutes, seconds, milliseconds,
                                microseconds, nanoseconds);
}

// #sec-temporal-createtemporalinstant
MaybeHandle<JSTemporalInstant> CreateTemporalInstant(
    Isolate* isolate, Handle<JSFunction> target, Handle<HeapObject> new_target,
    Handle<BigInt> epoch_nanoseconds) {
  TEMPORAL_ENTER_FUNC();
  // 1. Assert: Type(epochNanoseconds) is BigInt.
  // 2. Assert: ! IsValidEpochNanoseconds(epochNanoseconds) is true.
  DCHECK(IsValidEpochNanoseconds(isolate, epoch_nanoseconds));

  // 4. Let object be ? OrdinaryCreateFromConstructor(newTarget,
  // "%Temporal.Instant.prototype%", « [[InitializedTemporalInstant]],
  // [[Nanoseconds]] »).
  ORDINARY_CREATE_FROM_CONSTRUCTOR(object, target, new_target,
                                   JSTemporalInstant)
  // 5. Set object.[[Nanoseconds]] to ns.
  object->set_nanoseconds(*epoch_nanoseconds);
  return object;
}

MaybeHandle<JSTemporalInstant> CreateTemporalInstant(
    Isolate* isolate, Handle<BigInt> epoch_nanoseconds) {
  TEMPORAL_ENTER_FUNC();
  return CreateTemporalInstant(isolate, CONSTRUCTOR(instant),
                               CONSTRUCTOR(instant), epoch_nanoseconds);
}

MaybeHandle<JSTemporalTimeZone> CreateTemporalTimeZoneFromIndex(
    Isolate* isolate, Handle<JSFunction> target, Handle<HeapObject> new_target,
    int32_t index) {
  TEMPORAL_ENTER_FUNC();
  ORDINARY_CREATE_FROM_CONSTRUCTOR(object, target, new_target,
                                   JSTemporalTimeZone)
  object->set_flags(0);
  object->set_details(0);

  object->set_is_offset(false);
  object->set_offset_milliseconds_or_time_zone_index(index);
  return object;
}

MaybeHandle<JSTemporalTimeZone> CreateTemporalTimeZoneUTC(
    Isolate* isolate, Handle<JSFunction> target,
    Handle<HeapObject> new_target) {
  TEMPORAL_ENTER_FUNC();
  return CreateTemporalTimeZoneFromIndex(isolate, target, new_target, 0);
}

bool IsUTC(Isolate* isolate, Handle<String> time_zone);

// #sec-temporal-createtemporaltimezone
MaybeHandle<JSTemporalTimeZone> CreateTemporalTimeZone(
    Isolate* isolate, Handle<JSFunction> target, Handle<HeapObject> new_target,
    Handle<String> identifier) {
  TEMPORAL_ENTER_FUNC();
  // 1. If newTarget is not present, set it to %Temporal.TimeZone%.
  // 2. Let object be ? OrdinaryCreateFromConstructor(newTarget,
  // "%Temporal.TimeZone.prototype%", « [[InitializedTemporalTimeZone]],
  // [[Identifier]], [[OffsetNanoseconds]] »).
  // 3. Set object.[[Identifier]] to identifier.
  if (IsUTC(isolate, identifier)) {
    return CreateTemporalTimeZoneUTC(isolate, target, new_target);
  }
#ifdef V8_INTL_SUPPORT
  int32_t time_zone_index;
  Maybe<bool> maybe_time_zone_index =
      Intl::GetTimeZoneIndex(isolate, identifier, &time_zone_index);
  MAYBE_RETURN(maybe_time_zone_index, Handle<JSTemporalTimeZone>());
  if (maybe_time_zone_index.FromJust()) {
    return CreateTemporalTimeZoneFromIndex(isolate, target, new_target,
                                           time_zone_index);
  }
#endif  // V8_INTL_SUPPORT

  // 4. If identifier satisfies the syntax of a TimeZoneNumericUTCOffset
  // (see 13.33), then a. Set object.[[OffsetNanoseconds]] to !
  // ParseTimeZoneOffsetString(identifier).
  // 5. Else,
  // a. Assert: ! CanonicalizeTimeZoneName(identifier) is identifier.
  // b. Set object.[[OffsetNanoseconds]] to undefined.
  // 6. Return object.
  Maybe<int64_t> maybe_offset_nanoseconds =
      ParseTimeZoneOffsetString(isolate, identifier, false);
  MAYBE_RETURN(maybe_offset_nanoseconds, Handle<JSTemporalTimeZone>());
  int64_t offset_nanoseconds = maybe_offset_nanoseconds.FromJust();

  ORDINARY_CREATE_FROM_CONSTRUCTOR(object, target, new_target,
                                   JSTemporalTimeZone)
  object->set_flags(0);
  object->set_details(0);

  object->set_is_offset(true);
  object->set_offset_nanoseconds(offset_nanoseconds);
  return object;
}

MaybeHandle<JSTemporalTimeZone> CreateTemporalTimeZoneDefaultTarget(
    Isolate* isolate, Handle<String> identifier) {
  TEMPORAL_ENTER_FUNC();
  return CreateTemporalTimeZone(isolate, CONSTRUCTOR(time_zone),
                                CONSTRUCTOR(time_zone), identifier);
}

}  // namespace

namespace temporal {
MaybeHandle<JSTemporalTimeZone> CreateTemporalTimeZone(
    Isolate* isolate, Handle<String> identifier) {
  return CreateTemporalTimeZoneDefaultTarget(isolate, identifier);
}
}  // namespace temporal

namespace {

Maybe<DateTimeRecordCommon> GetISOPartsFromEpoch(
    Isolate* isolate, Handle<BigInt> epoch_nanoseconds) {
  TEMPORAL_ENTER_FUNC();
  DateTimeRecordCommon result;
  // 1. Let remainderNs be epochNanoseconds modulo 10^6.
  Handle<BigInt> million = BigInt::FromInt64(isolate, 1000000);
  Handle<BigInt> remainder_ns;
  ASSIGN_RETURN_ON_EXCEPTION_VALUE(
      isolate, remainder_ns,
      BigInt::Remainder(isolate, epoch_nanoseconds, million),
      Nothing<DateTimeRecordCommon>());
  // Need to do some remainder magic to negative remainder.
  if (remainder_ns->IsNegative()) {
    ASSIGN_RETURN_ON_EXCEPTION_VALUE(
        isolate, remainder_ns, BigInt::Add(isolate, remainder_ns, million),
        Nothing<DateTimeRecordCommon>());
  }

  // 2. Let epochMilliseconds be (epochNanoseconds − remainderNs) / 10^6.
  Handle<BigInt> bigint;
  ASSIGN_RETURN_ON_EXCEPTION_VALUE(
      isolate, bigint,
      BigInt::Subtract(isolate, epoch_nanoseconds, remainder_ns),
      Nothing<DateTimeRecordCommon>());
  ASSIGN_RETURN_ON_EXCEPTION_VALUE(isolate, bigint,
                                   BigInt::Divide(isolate, bigint, million),
                                   Nothing<DateTimeRecordCommon>());
  int64_t epoch_milliseconds = bigint->AsInt64();
  int year = 0;
  int month = 0;
  int day = 0;
  int wday = 0;
  int hour = 0;
  int min = 0;
  int sec = 0;
  int ms = 0;
  isolate->date_cache()->BreakDownTime(epoch_milliseconds, &year, &month, &day,
                                       &wday, &hour, &min, &sec, &ms);

  // 3. Let year be ! YearFromTime(epochMilliseconds).
  result.year = year;
  // 4. Let month be ! MonthFromTime(epochMilliseconds) + 1.
  result.month = month + 1;
  DCHECK_GE(result.month, 1);
  DCHECK_LE(result.month, 12);
  // 5. Let day be ! DateFromTime(epochMilliseconds).
  result.day = day;
  DCHECK_GE(result.day, 1);
  DCHECK_LE(result.day, 31);
  // 6. Let hour be ! HourFromTime(epochMilliseconds).
  result.hour = hour;
  DCHECK_GE(result.hour, 0);
  DCHECK_LE(result.hour, 23);
  // 7. Let minute be ! MinFromTime(epochMilliseconds).
  result.minute = min;
  DCHECK_GE(result.minute, 0);
  DCHECK_LE(result.minute, 59);
  // 8. Let second be ! SecFromTime(epochMilliseconds).
  result.second = sec;
  DCHECK_GE(result.second, 0);
  DCHECK_LE(result.second, 59);
  // 9. Let millisecond be ! msFromTime(epochMilliseconds).
  result.millisecond = ms;
  DCHECK_GE(result.millisecond, 0);
  DCHECK_LE(result.millisecond, 999);
  // 10. Let microsecond be floor(remainderNs / 1000) modulo 1000.
  int64_t remainder = remainder_ns->AsInt64();
  result.microsecond = (remainder / 1000) % 1000;
  DCHECK_GE(result.microsecond, 0);
  DCHECK_LE(result.microsecond, 999);
  // 11. Let nanosecond be remainderNs modulo 1000.
  result.nanosecond = remainder % 1000;
  DCHECK_GE(result.nanosecond, 0);
  DCHECK_LE(result.nanosecond, 999);
  return Just(result);
}

// #sec-temporal-balanceisodatetime
DateTimeRecordCommon BalanceISODateTime(Isolate* isolate, int32_t year,
                                        int32_t month, int32_t day,
                                        int32_t hour, int32_t minute,
                                        int32_t second, int32_t millisecond,
                                        int32_t microsecond,
                                        int64_t nanosecond) {
  TEMPORAL_ENTER_FUNC();
  // 1. Assert: year, month, day, hour, minute, second, millisecond,
  // microsecond, and nanosecond are integers.
  // 2. Let balancedTime be ! BalanceTime(hour, minute, second, millisecond,
  // microsecond, nanosecond).
  DateTimeRecordCommon balanced_time = BalanceTime(
      isolate, hour, minute, second, millisecond, microsecond, nanosecond);
  // 3. Let balancedDate be ! BalanceISODate(year, month, day +
  // balancedTime.[[Days]]).
  day += balanced_time.day;
  BalanceISODate(isolate, &year, &month, &day);
  // 4. Return the Record { [[Year]]: balancedDate.[[Year]], [[Month]]:
  // balancedDate.[[Month]], [[Day]]: balancedDate.[[Day]], [[Hour]]:
  // balancedTime.[[Hour]], [[Minute]]: balancedTime.[[Minute]], [[Second]]:
  // balancedTime.[[Second]], [[Millisecond]]: balancedTime.[[Millisecond]],
  // [[Microsecond]]: balancedTime.[[Microsecond]], [[Nanosecond]]:
  // balancedTime.[[Nanosecond]] }.
  return {year,
          month,
          day,
          balanced_time.hour,
          balanced_time.minute,
          balanced_time.second,
          balanced_time.millisecond,
          balanced_time.microsecond,
          balanced_time.nanosecond};
}

MaybeHandle<JSTemporalPlainDateTime> BuiltinTimeZoneGetPlainDateTimeFor(
    Isolate* isolate, Handle<JSReceiver> time_zone,
    Handle<JSTemporalInstant> instant, Handle<JSReceiver> calendar,
    const char* method) {
  TEMPORAL_ENTER_FUNC();
  // 1. Let offsetNanoseconds be ? GetOffsetNanosecondsFor(timeZone, instant).
  Maybe<int64_t> maybe_offset_nanoseconds =
      GetOffsetNanosecondsFor(isolate, time_zone, instant, method);
  MAYBE_RETURN(maybe_offset_nanoseconds, Handle<JSTemporalPlainDateTime>());
  // 2. Let result be ! GetISOPartsFromEpoch(instant.[[Nanoseconds]]).
  Maybe<DateTimeRecordCommon> maybe_result = GetISOPartsFromEpoch(
      isolate, Handle<BigInt>(instant->nanoseconds(), isolate));
  MAYBE_RETURN(maybe_result, Handle<JSTemporalPlainDateTime>());
  int64_t offset_nanoseconds = maybe_offset_nanoseconds.FromJust();

  // 3. Set result to ! BalanceISODateTime(result.[[Year]], result.[[Month]],
  // result.[[Day]], result.[[Hour]], result.[[Minute]], result.[[Second]],
  // result.[[Millisecond]], result.[[Microsecond]], result.[[Nanosecond]] +
  // offsetNanoseconds).
  DateTimeRecordCommon result = maybe_result.FromJust();
  result = BalanceISODateTime(isolate, result.year, result.month, result.day,
                              result.hour, result.minute, result.second,
                              result.millisecond, result.microsecond,
                              offset_nanoseconds + result.nanosecond);
  // 4. Return ? CreateTemporalDateTime(result.[[Year]], result.[[Month]],
  // result.[[Day]], result.[[Hour]], result.[[Minute]], result.[[Second]],
  // result.[[Millisecond]], result.[[Microsecond]], result.[[Nanosecond]],
  // calendar).
  return temporal::CreateTemporalDateTime(
      isolate, result.year, result.month, result.day, result.hour,
      result.minute, result.second, result.millisecond, result.microsecond,
      result.nanosecond, calendar);
}

// #sec-temporal-getpossibleinstantsfor
MaybeHandle<FixedArray> GetPossibleInstantsFor(Isolate* isolate,
                                               Handle<JSReceiver> time_zone,
                                               Handle<Object> date_time) {
  TEMPORAL_ENTER_FUNC();
  // 1. Let possibleInstants be ? Invoke(timeZone, "getPossibleInstantsFor", «
  // dateTime »).
  Handle<Object> function;
  ASSIGN_RETURN_ON_EXCEPTION(
      isolate, function,
      Object::GetProperty(isolate, time_zone,
                          isolate->factory()->getPossibleInstantsFor_string()),
      FixedArray);
  if (!function->IsCallable()) {
    THROW_NEW_ERROR(
        isolate,
        NewTypeError(MessageTemplate::kCalledNonCallable,
                     isolate->factory()->getPossibleInstantsFor_string()),
        FixedArray);
  }
  Handle<Object> possible_instants;
  {
    Handle<Object> argv[] = {date_time};
    ASSIGN_RETURN_ON_EXCEPTION(
        isolate, possible_instants,
        Execution::Call(isolate, function, time_zone, 1, argv), FixedArray);
  }

  // Step 4-6 of GetPossibleInstantsFor is implemented inside
  // temporal_instant_fixed_array_from_iterable.
  {
    Handle<Object> argv[] = {possible_instants};
    ASSIGN_RETURN_ON_EXCEPTION(
        isolate, possible_instants,
        Execution::CallBuiltin(
            isolate, isolate->temporal_instant_fixed_array_from_iterable(),
            possible_instants, 1, argv),
        FixedArray);
  }
  DCHECK(possible_instants->IsFixedArray());
  // 7. Return list.
  return Handle<FixedArray>::cast(possible_instants);
}

// #sec-temporal-disambiguatepossibleinstants
MaybeHandle<JSTemporalInstant> DisambiguatePossibleInstants(
    Isolate* isolate, Handle<FixedArray> possible_instants,
    Handle<JSReceiver> time_zone, Handle<Object> date_time_obj,
    Disambiguation disambiguation, const char* method) {
  TEMPORAL_ENTER_FUNC();
  // 1. Assert: dateTime has an [[InitializedTemporalDateTime]] internal slot.
  DCHECK(date_time_obj->IsJSTemporalPlainDateTime());
  Handle<JSTemporalPlainDateTime> date_time =
      Handle<JSTemporalPlainDateTime>::cast(date_time_obj);

  // 2. Let n be possibleInstants's length.
  int32_t n = possible_instants->length();

  // 3. If n = 1, then
  if (n == 1) {
    // a. Return possibleInstants[0].
    Handle<Object> ret_obj = FixedArray::get(*possible_instants, 0, isolate);
    DCHECK(ret_obj->IsJSTemporalInstant());
    return Handle<JSTemporalInstant>::cast(ret_obj);
  }
  // 4. If n ≠ 0, then
  if (n != 0) {
    // a. If disambiguation is "earlier" or "compatible", then
    if (disambiguation == Disambiguation::kEarlier ||
        disambiguation == Disambiguation::kCompatible) {
      // i. Return possibleInstants[0].
      Handle<Object> ret_obj = FixedArray::get(*possible_instants, 0, isolate);
      DCHECK(ret_obj->IsJSTemporalInstant());
      return Handle<JSTemporalInstant>::cast(ret_obj);
    }
    // b. If disambiguation is "later", then
    if (disambiguation == Disambiguation::kLater) {
      // i. Return possibleInstants[n − 1].
      Handle<Object> ret_obj =
          FixedArray::get(*possible_instants, n - 1, isolate);
      DCHECK(ret_obj->IsJSTemporalInstant());
      return Handle<JSTemporalInstant>::cast(ret_obj);
    }
    // c. Assert: disambiguation is "reject".
    DCHECK_EQ(disambiguation, Disambiguation::kReject);
    // d. Throw a RangeError exception.
    THROW_NEW_ERROR(isolate, NEW_TEMPORAL_INVALD_ARG_RANGE_ERROR(),
                    JSTemporalInstant);
  }
  // 5. Assert: n = 0.
  DCHECK_EQ(n, 0);
  // 6. If disambiguation is "reject", then
  if (disambiguation == Disambiguation::kReject) {
    // a. Throw a RangeError exception.
    THROW_NEW_ERROR(isolate, NEW_TEMPORAL_INVALD_ARG_RANGE_ERROR(),
                    JSTemporalInstant);
  }
  // 7. Let epochNanoseconds be ! GetEpochFromISOParts(dateTime.[[ISOYear]],
  // dateTime.[[ISOMonth]], dateTime.[[ISODay]], dateTime.[[ISOHour]],
  // dateTime.[[ISOMinute]], dateTime.[[ISOSecond]],
  // dateTime.[[ISOMillisecond]], dateTime.[[ISOMicrosecond]],
  // dateTime.[[ISONanosecond]]).
  Handle<BigInt> epoch_nanoseconds;
  ASSIGN_RETURN_ON_EXCEPTION(
      isolate, epoch_nanoseconds,
      GetEpochFromISOParts(
          isolate, date_time->iso_year(), date_time->iso_month(),
          date_time->iso_day(), date_time->iso_hour(), date_time->iso_minute(),
          date_time->iso_second(), date_time->iso_millisecond(),
          date_time->iso_microsecond(), date_time->iso_nanosecond()),
      JSTemporalInstant);

  // 8. Let dayBefore be ! CreateTemporalInstant(epochNanoseconds − 8.64 ×
  // 10^13).
  Handle<BigInt> one_day_in_ns = BigInt::FromUint64(isolate, 86400000000000ULL);
  Handle<BigInt> day_before_ns;
  ASSIGN_RETURN_ON_EXCEPTION(
      isolate, day_before_ns,
      BigInt::Subtract(isolate, epoch_nanoseconds, one_day_in_ns),
      JSTemporalInstant);
  Handle<JSTemporalInstant> day_before;
  ASSIGN_RETURN_ON_EXCEPTION(isolate, day_before,
                             CreateTemporalInstant(isolate, day_before_ns),
                             JSTemporalInstant);
  // 9. Let dayAfter be ! CreateTemporalInstant(epochNanoseconds + 8.64 ×
  // 10^13).
  Handle<BigInt> day_after_ns;
  ASSIGN_RETURN_ON_EXCEPTION(
      isolate, day_after_ns,
      BigInt::Add(isolate, epoch_nanoseconds, one_day_in_ns),
      JSTemporalInstant);
  Handle<JSTemporalInstant> day_after;
  ASSIGN_RETURN_ON_EXCEPTION(isolate, day_after,
                             CreateTemporalInstant(isolate, day_after_ns),
                             JSTemporalInstant);
  // 10. Let offsetBefore be ? GetOffsetNanosecondsFor(timeZone, dayBefore).
  Maybe<int64_t> maybe_offset_before =
      GetOffsetNanosecondsFor(isolate, time_zone, day_before, method);
  MAYBE_RETURN(maybe_offset_before, Handle<JSTemporalInstant>());
  // 11. Let offsetAfter be ? GetOffsetNanosecondsFor(timeZone, dayAfter).
  Maybe<int64_t> maybe_offset_after =
      GetOffsetNanosecondsFor(isolate, time_zone, day_after, method);
  MAYBE_RETURN(maybe_offset_after, Handle<JSTemporalInstant>());

  // 12. Let nanoseconds be offsetAfter − offsetBefore.
  int64_t nanoseconds =
      maybe_offset_after.FromJust() - maybe_offset_before.FromJust();

  // 13. If disambiguation is "earlier", then
  if (disambiguation == Disambiguation::kEarlier) {
    // a. Let earlier be ? AddDateTime(dateTime.[[ISOYear]],
    // dateTime.[[ISOMonth]], dateTime.[[ISODay]], dateTime.[[ISOHour]],
    // dateTime.[[ISOMinute]], dateTime.[[ISOSecond]],
    // dateTime.[[ISOMillisecond]],
    // dateTime.[[ISOMicrosecond]], dateTime.[[ISONanosecond]],
    // dateTime.[[Calendar]], 0, 0, 0, 0, 0, 0, 0, 0, 0, −nanoseconds,
    // undefined).
    Maybe<DateTimeRecordCommon> maybe_earlier = AddDateTime(
        isolate, date_time->iso_year(), date_time->iso_month(),
        date_time->iso_day(), date_time->iso_hour(), date_time->iso_minute(),
        date_time->iso_second(), date_time->iso_millisecond(),
        date_time->iso_microsecond(), date_time->iso_nanosecond(),
        Handle<JSReceiver>(date_time->calendar(), isolate),
        {0, 0, 0, 0, 0, 0, 0, 0, 0, -nanoseconds},
        isolate->factory()->undefined_value());
    MAYBE_RETURN(maybe_earlier, Handle<JSTemporalInstant>());
    DateTimeRecordCommon earlier = maybe_earlier.FromJust();

    // See https://github.com/tc39/proposal-temporal/issues/1816
    // b. Let earlierDateTime be ? CreateTemporalDateTime(earlier.[[Year]],
    // earlier.[[Month]], earlier.[[Day]], earlier.[[Hour]], earlier.[[Minute]],
    // earlier.[[Second]], earlier.[[Millisecond]], earlier.[[Microsecond]],
    // earlier.[[Nanosecond]], dateTime.[[Calendar]]).
    Handle<JSTemporalPlainDateTime> earlier_date_time;
    ASSIGN_RETURN_ON_EXCEPTION(
        isolate, earlier_date_time,
        temporal::CreateTemporalDateTime(
            isolate, earlier.year, earlier.month, earlier.day, earlier.hour,
            earlier.minute, earlier.second, earlier.millisecond,
            earlier.microsecond, earlier.nanosecond,
            Handle<JSReceiver>(date_time->calendar(), isolate)),
        JSTemporalInstant);

    // c. Set possibleInstants to ? GetPossibleInstantsFor(timeZone,
    // earlierDateTime).
    ASSIGN_RETURN_ON_EXCEPTION(
        isolate, possible_instants,
        GetPossibleInstantsFor(isolate, time_zone, earlier_date_time),
        JSTemporalInstant);

    // d. If possibleInstants is empty, throw a RangeError exception.
    if (possible_instants->length() == 0) {
      THROW_NEW_ERROR(isolate, NEW_TEMPORAL_INVALD_ARG_RANGE_ERROR(),
                      JSTemporalInstant);
    }
    // e. Return possibleInstants[0].
    Handle<Object> ret_obj = FixedArray::get(*possible_instants, 0, isolate);
    DCHECK(ret_obj->IsJSTemporalInstant());
    return Handle<JSTemporalInstant>::cast(ret_obj);
  }
  // 14. Assert: disambiguation is "compatible" or "later".
  DCHECK(disambiguation == Disambiguation::kCompatible ||
         disambiguation == Disambiguation::kLater);
  // 15. Let later be ? AddDateTime(dateTime.[[ISOYear]], dateTime.[[ISOMonth]],
  // dateTime.[[ISODay]], dateTime.[[ISOHour]], dateTime.[[ISOMinute]],
  // dateTime.[[ISOSecond]], dateTime.[[ISOMillisecond]],
  // dateTime.[[ISOMicrosecond]], dateTime.[[ISONanosecond]],
  // dateTime.[[Calendar]], 0, 0, 0, 0, 0, 0, 0, 0, 0, nanoseconds, undefined).
  Maybe<DateTimeRecordCommon> maybe_later = AddDateTime(
      isolate, date_time->iso_year(), date_time->iso_month(),
      date_time->iso_day(), date_time->iso_hour(), date_time->iso_minute(),
      date_time->iso_second(), date_time->iso_millisecond(),
      date_time->iso_microsecond(), date_time->iso_nanosecond(),
      Handle<JSReceiver>(date_time->calendar(), isolate),
      {0, 0, 0, 0, 0, 0, 0, 0, 0, nanoseconds},
      isolate->factory()->undefined_value());
  MAYBE_RETURN(maybe_later, Handle<JSTemporalInstant>());
  DateTimeRecordCommon later = maybe_later.FromJust();

  // See https://github.com/tc39/proposal-temporal/issues/1816
  // 16. Let laterDateTime be ? CreateTemporalDateTime(later.[[Year]],
  // later.[[Month]], later.[[Day]], later.[[Hour]], later.[[Minute]],
  // later.[[Second]], later.[[Millisecond]], later.[[Microsecond]],
  // later.[[Nanosecond]], dateTime.[[Calendar]]).

  Handle<JSTemporalPlainDateTime> later_date_time;
  ASSIGN_RETURN_ON_EXCEPTION(
      isolate, later_date_time,
      temporal::CreateTemporalDateTime(
          isolate, later.year, later.month, later.day, later.hour, later.minute,
          later.second, later.millisecond, later.microsecond, later.nanosecond,
          Handle<JSReceiver>(date_time->calendar(), isolate)),
      JSTemporalInstant);
  // 17. Set possibleInstants to ? GetPossibleInstantsFor(timeZone,
  // laterDateTime).
  ASSIGN_RETURN_ON_EXCEPTION(
      isolate, possible_instants,
      GetPossibleInstantsFor(isolate, time_zone, later_date_time),
      JSTemporalInstant);
  // 18. Set n to possibleInstants's length.
  n = possible_instants->length();
  // 19. If n = 0, throw a RangeError exception.
  if (n == 0) {
    THROW_NEW_ERROR(isolate, NEW_TEMPORAL_INVALD_ARG_RANGE_ERROR(),
                    JSTemporalInstant);
  }
  // 20. Return possibleInstants[n − 1].
  Handle<Object> ret_obj = FixedArray::get(*possible_instants, n - 1, isolate);
  DCHECK(ret_obj->IsJSTemporalInstant());
  return Handle<JSTemporalInstant>::cast(ret_obj);
}

// #sec-temporal-builtintimezonegetinstantfor
MaybeHandle<JSTemporalInstant> BuiltinTimeZoneGetInstantFor(
    Isolate* isolate, Handle<JSReceiver> time_zone,
    Handle<JSTemporalPlainDateTime> date_time, Disambiguation disambiguation,
    const char* method) {
  TEMPORAL_ENTER_FUNC();
  // 1. Assert: dateTime has an [[InitializedTemporalDateTime]] internal slot.
  // 2. Let possibleInstants be ? GetPossibleInstantsFor(timeZone, dateTime).
  Handle<FixedArray> possible_instants;
  ASSIGN_RETURN_ON_EXCEPTION(
      isolate, possible_instants,
      GetPossibleInstantsFor(isolate, time_zone, date_time), JSTemporalInstant);
  // 3. Return ? DisambiguatePossibleInstants(possibleInstants, timeZone,
  // dateTime, disambiguation).
  return DisambiguatePossibleInstants(isolate, possible_instants, time_zone,
                                      date_time, disambiguation, method);
}

// #sec-temporal-totemporalcalendar
MaybeHandle<JSReceiver> ToTemporalCalendar(
    Isolate* isolate, Handle<Object> temporal_calendar_like,
    const char* method) {
  Factory* factory = isolate->factory();
  // 1.If Type(temporalCalendarLike) is Object, then
  if (temporal_calendar_like->IsJSReceiver()) {
    // a. If temporalCalendarLike has an [[InitializedTemporalDate]],
    // [[InitializedTemporalDateTime]], [[InitializedTemporalMonthDay]],
    // [[InitializedTemporalTime]], [[InitializedTemporalYearMonth]], or
    // [[InitializedTemporalZonedDateTime]] internal slot, then i. Return
    // temporalCalendarLike.[[Calendar]].

#define EXTRACT_CALENDAR(T, obj)                                            \
  if (obj->IsJSTemporal##T()) {                                             \
    return Handle<JSReceiver>(Handle<JSTemporal##T>::cast(obj)->calendar(), \
                              isolate);                                     \
  }

    EXTRACT_CALENDAR(PlainDate, temporal_calendar_like)
    EXTRACT_CALENDAR(PlainDateTime, temporal_calendar_like)
    EXTRACT_CALENDAR(PlainMonthDay, temporal_calendar_like)
    EXTRACT_CALENDAR(PlainTime, temporal_calendar_like)
    EXTRACT_CALENDAR(PlainYearMonth, temporal_calendar_like)
    EXTRACT_CALENDAR(ZonedDateTime, temporal_calendar_like)

#undef EXTRACT_CALENDAR
    Handle<JSReceiver> obj = Handle<JSReceiver>::cast(temporal_calendar_like);

    // b. If ? HasProperty(temporalCalendarLike, "calendar") is false, return
    // temporalCalendarLike.
    Maybe<bool> maybe_has =
        JSReceiver::HasProperty(obj, factory->calendar_string());

    MAYBE_RETURN(maybe_has, Handle<JSReceiver>());
    if (!maybe_has.FromJust()) {
      return obj;
    }
    // c.  Set temporalCalendarLike to ? Get(temporalCalendarLike, "calendar").
    ASSIGN_RETURN_ON_EXCEPTION(
        isolate, temporal_calendar_like,
        JSReceiver::GetProperty(isolate, obj, factory->calendar_string()),
        JSReceiver);
    // d. If Type(temporalCalendarLike) is Object
    if (temporal_calendar_like->IsJSReceiver()) {
      obj = Handle<JSReceiver>::cast(temporal_calendar_like);
      // and ? HasProperty(temporalCalendarLike, "calendar") is false,
      maybe_has = JSReceiver::HasProperty(obj, factory->calendar_string());
      MAYBE_RETURN(maybe_has, Handle<JSReceiver>());
      if (!maybe_has.FromJust()) {
        // return temporalCalendarLike.
        return obj;
      }
    }
  }

  // 2. Let identifier be ? ToString(temporalCalendarLike).
  Handle<String> identifier;
  ASSIGN_RETURN_ON_EXCEPTION(isolate, identifier,
                             Object::ToString(isolate, temporal_calendar_like),
                             JSReceiver);
  // 3. If ! IsBuiltinCalendar(identifier) is false, then
  if (!IsBuiltinCalendar(isolate, identifier)) {
    // a. Let identifier be ? ParseTemporalCalendarString(identifier).
    ASSIGN_RETURN_ON_EXCEPTION(isolate, identifier,
                               ParseTemporalCalendarString(isolate, identifier),
                               JSReceiver);
  }
  // 4. Return ? CreateTemporalCalendar(identifier).
  return CreateTemporalCalendar(isolate, identifier);
}

// #sec-temporal-totemporalcalendarwithisodefault
MaybeHandle<JSReceiver> ToTemporalCalendarWithISODefault(
    Isolate* isolate, Handle<Object> temporal_calendar_like,
    const char* method) {
  TEMPORAL_ENTER_FUNC();

  // 1. If temporalCalendarLike is undefined, then
  if (temporal_calendar_like->IsUndefined()) {
    // a. Return ? GetISO8601Calendar().
    return temporal::GetISO8601Calendar(isolate);
  }
  // 2. Return ? ToTemporalCalendar(temporalCalendarLike).
  return ToTemporalCalendar(isolate, temporal_calendar_like, method);
}

// #sec-temporal-totemporaltimezone
MaybeHandle<JSReceiver> ToTemporalTimeZone(
    Isolate* isolate, Handle<Object> temporal_time_zone_like,
    const char* method) {
  TEMPORAL_ENTER_FUNC();

  Factory* factory = isolate->factory();
  // 1. If Type(temporalTimeZoneLike) is Object, then
  if (temporal_time_zone_like->IsJSReceiver()) {
    // a. If temporalTimeZoneLike has an [[InitializedTemporalZonedDateTime]]
    // internal slot, then
    if (temporal_time_zone_like->IsJSTemporalZonedDateTime()) {
      // i. Return temporalTimeZoneLike.[[TimeZone]].
      Handle<JSTemporalZonedDateTime> zoned_date_time =
          Handle<JSTemporalZonedDateTime>::cast(temporal_time_zone_like);
      return Handle<JSReceiver>(zoned_date_time->time_zone(), isolate);
    }
    Handle<JSReceiver> obj = Handle<JSReceiver>::cast(temporal_time_zone_like);
    // b. If ? HasProperty(temporalTimeZoneLike, "timeZone") is false,
    Maybe<bool> maybe_has =
        JSReceiver::HasProperty(obj, factory->timeZone_string());
    MAYBE_RETURN(maybe_has, Handle<JSReceiver>());
    if (!maybe_has.FromJust()) {
      // return temporalTimeZoneLike.
      return obj;
    }
    // c. Set temporalTimeZoneLike to ?
    // Get(temporalTimeZoneLike, "timeZone").
    ASSIGN_RETURN_ON_EXCEPTION(
        isolate, temporal_time_zone_like,
        JSReceiver::GetProperty(isolate, obj, factory->timeZone_string()),
        JSReceiver);
    // d. If Type(temporalTimeZoneLike)
    if (temporal_time_zone_like->IsJSReceiver()) {
      // is Object and ? HasProperty(temporalTimeZoneLike, "timeZone") is false,
      obj = Handle<JSReceiver>::cast(temporal_time_zone_like);
      maybe_has = JSReceiver::HasProperty(obj, factory->timeZone_string());
      MAYBE_RETURN(maybe_has, Handle<JSReceiver>());
      if (!maybe_has.FromJust()) {
        // return temporalTimeZoneLike.
        return obj;
      }
    }
  }
  Handle<String> identifier;
  // 2. Let identifier be ? ToString(temporalTimeZoneLike).
  ASSIGN_RETURN_ON_EXCEPTION(isolate, identifier,
                             Object::ToString(isolate, temporal_time_zone_like),
                             JSReceiver);
  // 3. Let result be ? ParseTemporalTimeZone(identifier).
  Handle<String> result;
  ASSIGN_RETURN_ON_EXCEPTION(
      isolate, result, ParseTemporalTimeZone(isolate, identifier), JSReceiver);

  // 4. Return ? CreateTemporalTimeZone(result).
  return temporal::CreateTemporalTimeZone(isolate, result);
}

#define COMPARE_RESULT_TO_SIGN(r)  \
  ((r) == ComparisonResult::kEqual \
       ? 0                         \
       : ((r) == ComparisonResult::kLessThan ? -1 : 1))

// #sec-temporal-formattimezoneoffsetstring
MaybeHandle<String> FormatTimeZoneOffsetString(Isolate* isolate,
                                               int64_t offset_nanoseconds) {
  IncrementalStringBuilder builder(isolate);
  // 1. Assert: offsetNanoseconds is an integer.
  // 2. If offsetNanoseconds ≥ 0, let sign be "+"; otherwise, let sign be "-".
  builder.AppendCString((offset_nanoseconds >= 0) ? "+" : "-");
  // 3. Let offsetNanoseconds be abs(offsetNanoseconds).
  offset_nanoseconds = std::abs(offset_nanoseconds);
  // 3. Let nanoseconds be offsetNanoseconds modulo 10^9.
  int64_t nanoseconds = offset_nanoseconds % 1000000000;
  // 4. Let seconds be floor(offsetNanoseconds / 10^9) modulo 60.
  int64_t seconds = (offset_nanoseconds / 1000000000) % 60;
  // 5. Let minutes be floor(offsetNanoseconds / (6 × 10^10)) modulo 60.
  int64_t minutes = (offset_nanoseconds / 60000000000) % 60;
  // 6. Let hours be floor(offsetNanoseconds / (3.6 × 10^12)).
  int64_t hours = offset_nanoseconds / 3600000000000;
  // 7. Let h be hours, formatted as a two-digit decimal number, padded to the
  // left with a zero if necessary.
  if (hours < 10) {
    builder.AppendCStringLiteral("0");
  }
  builder.AppendInt(static_cast<int32_t>(hours));
  // 8. Let m be minutes, formatted as a two-digit decimal number, padded to the
  // left with a zero if necessary.
  builder.AppendCString((minutes < 10) ? ":0" : ":");
  builder.AppendInt(static_cast<int>(minutes));
  // 9. Let s be seconds, formatted as a two-digit decimal number, padded to the
  // left with a zero if necessary.
  // 10. If nanoseconds ≠ 0, then
  if (nanoseconds != 0) {
    builder.AppendCString((seconds < 10) ? ":0" : ":");
    builder.AppendInt(static_cast<int>(seconds));
    builder.AppendCStringLiteral(".");
    // a. Let fraction be nanoseconds, formatted as a nine-digit decimal number,
    // padded to the left with zeroes if necessary.
    // b. Set fraction to the longest possible substring of fraction starting at
    // position 0 and not ending with the code unit 0x0030 (DIGIT ZERO).
    int64_t divisor = 100000000;
    do {
      builder.AppendInt(static_cast<int>(nanoseconds / divisor));
      nanoseconds %= divisor;
      divisor /= 10;
    } while (nanoseconds > 0);
    // c. Let post be the string-concatenation of the code unit 0x003A (COLON),
    // s, the code unit 0x002E (FULL STOP), and fraction.
    // 11. Else if seconds ≠ 0, then
  } else if (seconds != 0) {
    // a. Let post be the string-concatenation of the code unit 0x003A (COLON)
    // and s.
    builder.AppendCString((seconds < 10) ? ":0" : ":");
    builder.AppendInt(static_cast<int>(seconds));
  }
  // 12. Return the string-concatenation of sign, h, the code unit 0x003A
  // (COLON), m, and post.
  return builder.Finish();
}

// #sec-temporal-builtintimezonegetoffsetstringfor
MaybeHandle<String> BuiltinTimeZoneGetOffsetStringFor(
    Isolate* isolate, Handle<JSReceiver> time_zone,
    Handle<JSTemporalInstant> instant, const char* method) {
  TEMPORAL_ENTER_FUNC();
  // 1. Let offsetNanoseconds be ? GetOffsetNanosecondsFor(timeZone, instant).
  Maybe<int64_t> maybe_offset_nanoseconds =
      GetOffsetNanosecondsFor(isolate, time_zone, instant, method);
  MAYBE_RETURN(maybe_offset_nanoseconds, Handle<String>());
  int64_t offset_nanoseconds = maybe_offset_nanoseconds.FromJust();

  // 2. Return ! FormatTimeZoneOffsetString(offsetNanoseconds).
  return FormatTimeZoneOffsetString(isolate, offset_nanoseconds);
}

// #sec-temporal-parsetemporaltimezonestring
Maybe<TimeZoneRecord> ParseTemporalTimeZoneString(Isolate* isolate,
                                                  Handle<String> iso_string) {
  TEMPORAL_ENTER_FUNC();

  // 1. Assert: Type(isoString) is String.
  // 2. If isoString does not satisfy the syntax of a TemporalTimeZoneString
  // (see 13.33), then
  Maybe<ParsedISO8601Result> maybe_parsed =
      TemporalParser::ParseTemporalTimeZoneString(isolate, iso_string);
  MAYBE_RETURN(maybe_parsed, Nothing<TimeZoneRecord>());
  if (maybe_parsed.IsNothing()) {
    THROW_NEW_ERROR_RETURN_VALUE(isolate, NEW_TEMPORAL_INVALD_ARG_RANGE_ERROR(),
                                 Nothing<TimeZoneRecord>());
  }
  ParsedISO8601Result parsed = maybe_parsed.FromJust();
  // 3. Let z, sign, hours, minutes, seconds, fraction and name be the parts of
  // isoString produced respectively by the UTCDesignator,
  // TimeZoneUTCOffsetSign, TimeZoneUTCOffsetHour, TimeZoneUTCOffsetMinute,
  // TimeZoneUTCOffsetSecond, TimeZoneUTCOffsetFraction, and TimeZoneIANAName
  // productions, or undefined if not present.
  // 4. If z is not undefined, then
  if (parsed.utc_designator) {
    // a. Return the Record { [[Z]]: true, [[OffsetString]]: undefined,
    // [[Name]]: name }.
    if (parsed.tzi_name_length > 0) {
      Handle<String> name = isolate->factory()->NewSubString(
          iso_string, parsed.tzi_name_start,
          parsed.tzi_name_start + parsed.tzi_name_length);
      TimeZoneRecord ret({true, isolate->factory()->empty_string(), name});
      return Just(ret);
    }
    TimeZoneRecord ret({true, isolate->factory()->empty_string(),
                        isolate->factory()->empty_string()});
    return Just(ret);
  }

  // 5. If hours is undefined, then
  // a. Let offsetString be undefined.
  // 6. Else,
  Handle<String> offset_string;
  bool offset_string_is_defined = false;
  if (!parsed.tzuo_hour_is_undefined()) {
    // a. Assert: sign is not undefined.
    DCHECK(!parsed.tzuo_sign_is_undefined());
    // b. Set hours to ! ToIntegerOrInfinity(hours).
    int32_t hours = parsed.tzuo_hour;
    // c. If sign is the code unit 0x002D (HYPHEN-MINUS) or the code unit 0x2212
    // (MINUS SIGN), then i. Set sign to −1. d. Else, i. Set sign to 1.
    int32_t sign = parsed.tzuo_sign;
    // e. Set minutes to ! ToIntegerOrInfinity(minutes).
    int32_t minutes =
        parsed.tzuo_minute_is_undefined() ? 0 : parsed.tzuo_minute;
    // f. Set seconds to ! ToIntegerOrInfinity(seconds).
    int32_t seconds =
        parsed.tzuo_second_is_undefined() ? 0 : parsed.tzuo_second;
    // g. If fraction is not undefined, then
    int32_t nanoseconds;
    if (!parsed.tzuo_nanosecond_is_undefined()) {
      // i. Set fraction to the string-concatenation of the previous value of
      // fraction and the string "000000000".
      // ii. Let nanoseconds be the String value equal to the substring of
      // fraction from 0 to 9. iii. Set nanoseconds to !
      // ToIntegerOrInfinity(nanoseconds).
      nanoseconds = parsed.tzuo_nanosecond;
      // h. Else,
    } else {
      // i. Let nanoseconds be 0.
      nanoseconds = 0;
    }
    // i. Let offsetNanoseconds be sign × (((hours × 60 + minutes) × 60 +
    // seconds) × 10^9 + nanoseconds).
    int64_t offset_nanoseconds =
        sign *
        (((hours * 60 + minutes) * 60 + seconds) * 1000000000L + nanoseconds);
    // j. Let offsetString be ! FormatTimeZoneOffsetString(offsetNanoseconds).
    ASSIGN_RETURN_ON_EXCEPTION_VALUE(
        isolate, offset_string,
        FormatTimeZoneOffsetString(isolate, offset_nanoseconds),
        Nothing<TimeZoneRecord>());
    offset_string_is_defined = true;
  }
  // 7. If name is not undefined, then
  Handle<String> name;
  if (parsed.tzi_name_length > 0) {
    name = isolate->factory()->NewSubString(
        iso_string, parsed.tzi_name_start,
        parsed.tzi_name_start + parsed.tzi_name_length);

    // a. If ! IsValidTimeZoneName(name) is false, throw a RangeError exception.
    if (!IsValidTimeZoneName(isolate, name)) {
      THROW_NEW_ERROR_RETURN_VALUE(isolate,
                                   NEW_TEMPORAL_INVALD_ARG_RANGE_ERROR(),
                                   Nothing<TimeZoneRecord>());
    }
    // b. Set name to ! CanonicalizeTimeZoneName(name).
    ASSIGN_RETURN_ON_EXCEPTION_VALUE(isolate, name,
                                     CanonicalizeTimeZoneName(isolate, name),
                                     Nothing<TimeZoneRecord>());
    // 8. Return the Record { [[Z]]: false, [[OffsetString]]: offsetString,
    // [[Name]]: name }.
    TimeZoneRecord ret({false,
                        offset_string_is_defined
                            ? offset_string
                            : isolate->factory()->empty_string(),
                        name});
    return Just(ret);
  }
  // 8. Return the Record { [[Z]]: false, [[OffsetString]]: offsetString,
  // [[Name]]: name }.
  TimeZoneRecord ret({false,
                      offset_string_is_defined
                          ? offset_string
                          : isolate->factory()->empty_string(),
                      isolate->factory()->empty_string()});
  return Just(ret);
}

// #sec-temporal-parsetemporaltimezone
MaybeHandle<String> ParseTemporalTimeZone(Isolate* isolate,
                                          Handle<String> string) {
  TEMPORAL_ENTER_FUNC();

  // 2. Let result be ? ParseTemporalTimeZoneString(string).
  Maybe<TimeZoneRecord> maybe_result =
      ParseTemporalTimeZoneString(isolate, string);
  MAYBE_RETURN(maybe_result, Handle<String>());
  TimeZoneRecord result = maybe_result.FromJust();

  // 3. If result.[[Name]] is not undefined, return result.[[Name]].
  if (result.name->length() > 0) {
    return result.name;
  }

  // 4. If result.[[Z]] is true, return "UTC".
  if (result.z) {
    return isolate->factory()->UTC_string();
  }

  // 5. Return result.[[OffsetString]].
  return result.offset_string;
}

Maybe<int64_t> ParseTimeZoneOffsetString(Isolate* isolate,
                                         Handle<String> iso_string,
                                         bool throwIfNotSatisfy) {
  TEMPORAL_ENTER_FUNC();

  // 1. Assert: Type(offsetString) is String.
  // 2. If offsetString does not satisfy the syntax of a
  // TimeZoneNumericUTCOffset (see 13.33), then
  Maybe<ParsedISO8601Result> maybe_parsed =
      TemporalParser::ParseTimeZoneNumericUTCOffset(isolate, iso_string);
  MAYBE_RETURN(maybe_parsed, Nothing<int64_t>());
  if (throwIfNotSatisfy && maybe_parsed.IsNothing()) {
    /* a. Throw a RangeError exception. */
    THROW_NEW_ERROR_RETURN_VALUE(isolate, NEW_TEMPORAL_INVALD_ARG_RANGE_ERROR(),
                                 Nothing<int64_t>());
  }
  ParsedISO8601Result parsed = maybe_parsed.FromJust();
  // 3. Let sign, hours, minutes, seconds, and fraction be the parts of
  // offsetString produced respectively by the TimeZoneUTCOffsetSign,
  // TimeZoneUTCOffsetHour, TimeZoneUTCOffsetMinute, TimeZoneUTCOffsetSecond,
  // and TimeZoneUTCOffsetFraction productions, or undefined if not present.
  // 4. If either hours or sign are undefined, throw a RangeError exception.
  if (parsed.tzuo_hour_is_undefined() || parsed.tzuo_sign_is_undefined()) {
    THROW_NEW_ERROR_RETURN_VALUE(isolate, NEW_TEMPORAL_INVALD_ARG_RANGE_ERROR(),
                                 Nothing<int64_t>());
  }
  // 5. If sign is the code unit 0x002D (HYPHEN-MINUS) or 0x2212 (MINUS SIGN),
  // then a. Set sign to −1.
  // 6. Else,
  // a. Set sign to 1.
  int64_t sign = parsed.tzuo_sign;

  // 7. Set hours to ! ToIntegerOrInfinity(hours).
  int64_t hours = parsed.tzuo_hour;
  // 8. Set minutes to ! ToIntegerOrInfinity(minutes).
  int64_t minutes = parsed.tzuo_minute_is_undefined() ? 0 : parsed.tzuo_minute;
  // 9. Set seconds to ! ToIntegerOrInfinity(seconds).
  int64_t seconds = parsed.tzuo_second_is_undefined() ? 0 : parsed.tzuo_second;
  // 10. If fraction is not undefined, then
  int64_t nanoseconds;
  if (!parsed.tzuo_nanosecond_is_undefined()) {
    // a. Set fraction to the string-concatenation of the previous value of
    // fraction and the string "000000000".
    // b. Let nanoseconds be the String value equal to the substring of fraction
    // consisting of the code units with indices 0 (inclusive) through 9
    // (exclusive). c. Set nanoseconds to ! ToIntegerOrInfinity(nanoseconds).
    nanoseconds = parsed.tzuo_nanosecond;
    // 11. Else,
  } else {
    // a. Let nanoseconds be 0.
    nanoseconds = 0;
  }
  // 12. Return sign × (((hours × 60 + minutes) × 60 + seconds) × 10^9 +
  // nanoseconds).
  return Just(sign * (((hours * 60 + minutes) * 60 + seconds) * 1000000000 +
                      nanoseconds));
}

Maybe<bool> IsValidTimeZoneNumericUTCOffsetString(Isolate* isolate,
                                                  Handle<String> iso_string) {
  TEMPORAL_ENTER_FUNC();

  Maybe<ParsedISO8601Result> maybe_parsed =
      TemporalParser::ParseTimeZoneNumericUTCOffset(isolate, iso_string);
  return Just(maybe_parsed.IsJust());
}

// #sec-temporal-parsetemporalcalendarstring
MaybeHandle<String> ParseTemporalCalendarString(Isolate* isolate,
                                                Handle<String> iso_string) {
  TEMPORAL_ENTER_FUNC();

  // 1. Assert: Type(isoString) is String.
  // 2. If isoString does not satisfy the syntax of a TemporalCalendarString
  // (see 13.33), then a. Throw a RangeError exception.
  Maybe<ParsedISO8601Result> maybe_parsed =
      TemporalParser::ParseTemporalCalendarString(isolate, iso_string);
  MAYBE_RETURN(maybe_parsed, Handle<String>());
  if (maybe_parsed.IsNothing()) {
    THROW_NEW_ERROR(isolate, NEW_TEMPORAL_INVALD_ARG_RANGE_ERROR(), String);
  }
  ParsedISO8601Result parsed = maybe_parsed.FromJust();
  // 3. Let id be the part of isoString produced by the CalendarName production,
  // or undefined if not present.
  // 4. If id is undefined, then
  if (parsed.calendar_name_length == 0) {
    // a. Return "iso8601".
    return isolate->factory()->iso8601_string();
  }
  Handle<String> id = isolate->factory()->NewSubString(
      iso_string, parsed.calendar_name_start,
      parsed.calendar_name_start + parsed.calendar_name_length);
  // 5. If ! IsBuiltinCalendar(id) is false, then
  if (!IsBuiltinCalendar(isolate, id)) {
    // a. Throw a RangeError exception.
    THROW_NEW_ERROR(
        isolate, NewRangeError(MessageTemplate::kInvalidCalendar, id), String);
  }
  // 6. Return id.
  return id;
}

MaybeHandle<JSTemporalPlainDate> CalendarDateAdd(Isolate* isolate,
                                                 Handle<JSReceiver> calendar,
                                                 Handle<Object> date,
                                                 Handle<Object> duration,
                                                 Handle<Object> options) {
  return CalendarDateAdd(isolate, calendar, date, duration, options,
                         isolate->factory()->undefined_value());
}

MaybeHandle<JSTemporalPlainDate> CalendarDateAdd(
    Isolate* isolate, Handle<JSReceiver> calendar, Handle<Object> date,
    Handle<Object> duration, Handle<Object> options, Handle<Object> date_add) {
  // 1. Assert: Type(calendar) is Object.
  // 2. If dateAdd is not present, set dateAdd to ? GetMethod(calendar,
  // "dateAdd").
  if (date_add->IsUndefined()) {
    ASSIGN_RETURN_ON_EXCEPTION(
        isolate, date_add,
        Object::GetMethod(calendar, isolate->factory()->dateAdd_string()),
        JSTemporalPlainDate);
  }
  // 3. Let addedDate be ? Call(dateAdd, calendar, « date, duration, options »).
  Handle<Object> argv[] = {date, duration, options};
  Handle<Object> added_date;
  ASSIGN_RETURN_ON_EXCEPTION(
      isolate, added_date,
      Execution::Call(isolate, date_add, calendar, 3, argv),
      JSTemporalPlainDate);
  // 4. Perform ? RequireInternalSlot(addedDate, [[InitializedTemporalDate]]).
  if (!added_date->IsJSTemporalPlainDate()) {
    THROW_NEW_ERROR(isolate, NEW_TEMPORAL_INVALD_ARG_TYPE_ERROR(),
                    JSTemporalPlainDate);
  }
  // 5. Return addedDate.
  return Handle<JSTemporalPlainDate>::cast(added_date);
}

MaybeHandle<JSTemporalDuration> CalendarDateUntil(Isolate* isolate,
                                                  Handle<JSReceiver> calendar,
                                                  Handle<Object> one,
                                                  Handle<Object> two,
                                                  Handle<Object> options) {
  return CalendarDateUntil(isolate, calendar, one, two, options,
                           isolate->factory()->undefined_value());
}

MaybeHandle<JSTemporalDuration> CalendarDateUntil(
    Isolate* isolate, Handle<JSReceiver> calendar, Handle<Object> one,
    Handle<Object> two, Handle<Object> options, Handle<Object> date_until) {
  // 1. Assert: Type(calendar) is Object.
  // 2. If dateUntil is not present, set dateUntil to ? GetMethod(calendar,
  // "dateUntil").
  if (date_until->IsUndefined()) {
    ASSIGN_RETURN_ON_EXCEPTION(
        isolate, date_until,
        Object::GetMethod(calendar, isolate->factory()->dateUntil_string()),
        JSTemporalDuration);
  }
  // 3. Let duration be ? Call(dateUntil, calendar, « one, two, options »).
  Handle<Object> argv[] = {one, two, options};
  Handle<Object> duration;
  ASSIGN_RETURN_ON_EXCEPTION(
      isolate, duration,
      Execution::Call(isolate, date_until, calendar, 3, argv),
      JSTemporalDuration);
  // 4. Perform ? RequireInternalSlot(duration,
  // [[InitializedTemporalDuration]]).
  if (!duration->IsJSTemporalDuration()) {
    THROW_NEW_ERROR(isolate, NEW_TEMPORAL_INVALD_ARG_TYPE_ERROR(),
                    JSTemporalDuration);
  }
  // 5. Return duration.
  return Handle<JSTemporalDuration>::cast(duration);
}

Maybe<int64_t> GetOffsetNanosecondsFor(Isolate* isolate,
                                       Handle<JSReceiver> time_zone_obj,
                                       Handle<Object> instant,
                                       const char* method) {
  TEMPORAL_ENTER_FUNC();
  // 1. Let getOffsetNanosecondsFor be ? GetMethod(timeZone,
  // "getOffsetNanosecondsFor").
  Handle<Object> get_offset_nanoseconds_for;
  ASSIGN_RETURN_ON_EXCEPTION_VALUE(
      isolate, get_offset_nanoseconds_for,
      Object::GetMethod(time_zone_obj,
                        isolate->factory()->getOffsetNanosecondsFor_string()),
      Nothing<int64_t>());
  if (!get_offset_nanoseconds_for->IsCallable()) {
    THROW_NEW_ERROR_RETURN_VALUE(
        isolate,
        NewTypeError(MessageTemplate::kCalledNonCallable,
                     isolate->factory()->getOffsetNanosecondsFor_string()),
        Nothing<int64_t>());
  }
  Handle<Object> offset_nanoseconds_obj;
  // 3. Let offsetNanoseconds be ? Call(getOffsetNanosecondsFor, timeZone, «
  // instant »).
  Handle<Object> argv[] = {instant};
  ASSIGN_RETURN_ON_EXCEPTION_VALUE(
      isolate, offset_nanoseconds_obj,
      Execution::Call(isolate, get_offset_nanoseconds_for, time_zone_obj, 1,
                      argv),
      Nothing<int64_t>());

  // 4. If Type(offsetNanoseconds) is not Number, throw a TypeError exception.
  if (!offset_nanoseconds_obj->IsNumber()) {
    THROW_NEW_ERROR_RETURN_VALUE(isolate, NEW_TEMPORAL_INVALD_ARG_TYPE_ERROR(),
                                 Nothing<int64_t>());
  }

  // 5. If ! IsIntegralNumber(offsetNanoseconds) is false, throw a RangeError
  // exception.
  double offset_nanoseconds = offset_nanoseconds_obj->Number();
  if ((offset_nanoseconds - std::floor(offset_nanoseconds) != 0)) {
    THROW_NEW_ERROR_RETURN_VALUE(isolate, NEW_TEMPORAL_INVALD_ARG_RANGE_ERROR(),
                                 Nothing<int64_t>());
  }

  // 6. Set offsetNanoseconds to ℝ(offsetNanoseconds).
  int64_t offset_nanoseconds_int = static_cast<int64_t>(offset_nanoseconds);
  // 7. If abs(offsetNanoseconds) > 86400 × 10^9, throw a RangeError exception.
  if (std::abs(offset_nanoseconds_int) > 86400e9) {
    THROW_NEW_ERROR_RETURN_VALUE(isolate, NEW_TEMPORAL_INVALD_ARG_RANGE_ERROR(),
                                 Nothing<int64_t>());
  }
  // 8. Return offsetNanoseconds.
  return Just(offset_nanoseconds_int);
}

}  // namespace

namespace temporal {
// #sec-temporal-getiso8601calendar
MaybeHandle<JSTemporalCalendar> GetISO8601Calendar(Isolate* isolate) {
  return CreateTemporalCalendar(isolate, isolate->factory()->iso8601_string());
}

}  // namespace temporal

namespace {

bool IsUTC(Isolate* isolate, Handle<String> time_zone) {
  // 1. Assert: Type(timeZone) is String.
  // 2. Let tzText be ! StringToCodePoints(timeZone).
  // 3. Let tzUpperText be the result of toUppercase(tzText), according to the
  // Unicode Default Case Conversion algorithm.
  // 4. Let tzUpper be ! CodePointsToString(tzUpperText).
  // 5. If tzUpper and "UTC" are the same sequence of code points, return true.
  // 6. Return false.
  if (time_zone->length() != 3) return false;
  time_zone = String::Flatten(isolate, time_zone);
  DisallowGarbageCollection no_gc;
  const String::FlatContent& flat = time_zone->GetFlatContent(no_gc);
  return (flat.Get(0) == u'U' || flat.Get(0) == u'u') &&
         (flat.Get(1) == u'T' || flat.Get(1) == u't') &&
         (flat.Get(2) == u'C' || flat.Get(2) == u'c');
}

#ifdef V8_INTL_SUPPORT
class CalendarMap final {
 public:
  CalendarMap() {
    icu::Locale locale("und");
    UErrorCode status = U_ZERO_ERROR;
    std::unique_ptr<icu::StringEnumeration> enumeration(
        icu::Calendar::getKeywordValuesForLocale("ca", locale, false, status));
    calendar_ids.push_back("iso8601");
    calendar_id_indices.insert({"iso8601", 0});
    int32_t i = 1;
    for (const char* item = enumeration->next(nullptr, status);
         U_SUCCESS(status) && item != nullptr;
         item = enumeration->next(nullptr, status)) {
      if (strcmp(item, "iso8601") != 0) {
        const char* type = uloc_toUnicodeLocaleType("ca", item);
        calendar_ids.push_back(type);
        calendar_id_indices.insert({type, i++});
      }
    }
  }
  bool Contains(const std::string& id) const {
    return calendar_id_indices.find(id) != calendar_id_indices.end();
  }

  std::string Id(int32_t index) const {
    DCHECK_LT(index, calendar_ids.size());
    return calendar_ids[index];
  }

  int32_t Index(const char* id) const {
    return calendar_id_indices.find(id)->second;
  }

 private:
  std::map<std::string, int32_t> calendar_id_indices;
  std::vector<std::string> calendar_ids;
};

DEFINE_LAZY_LEAKY_OBJECT_GETTER(CalendarMap, GetCalendarMap)

// #sec-temporal-isbuiltincalendar
bool IsBuiltinCalendar(Isolate* isolate, const std::string& id) {
  return GetCalendarMap()->Contains(id);
}

bool IsBuiltinCalendar(Isolate* isolate, Handle<String> id) {
  return IsBuiltinCalendar(isolate, id->ToCString().get());
}

Handle<String> CalendarIdentifier(Isolate* isolate, int32_t index) {
  return isolate->factory()->NewStringFromAsciiChecked(
      GetCalendarMap()->Id(index).c_str());
}

int32_t CalendarIndex(Isolate* isolate, Handle<String> id) {
  return GetCalendarMap()->Index(id->ToCString().get());
}

bool IsValidTimeZoneName(Isolate* isolate, Handle<String> time_zone) {
  return Intl::IsValidTimeZoneName(isolate, time_zone);
}

MaybeHandle<String> CanonicalizeTimeZoneName(Isolate* isolate,
                                             Handle<String> identifier) {
  return Intl::CanonicalizeTimeZoneName(isolate, identifier);
}

#else   // V8_INTL_SUPPORT
Handle<String> CalendarIdentifier(Isolate* isolate, int32_t index) {
  DCHECK_EQ(index, 0);
  return isolate->factory()->iso8601_string();
}

// #sec-temporal-isbuiltincalendar
bool IsBuiltinCalendar(Isolate* isolate, Handle<String> id) {
  // 1. If id is not "iso8601", return false.
  // 2. Return true
  return isolate->factory()->iso8601_string()->Equals(*id);
}

int32_t CalendarIndex(Isolate* isolate, Handle<String> id) { return 0; }
// #sec-isvalidtimezonename
bool IsValidTimeZoneName(Isolate* isolate, Handle<String> time_zone) {
  return IsUTC(isolate, time_zone);
}
// #sec-canonicalizetimezonename
MaybeHandle<String> CanonicalizeTimeZoneName(Isolate* isolate,
                                             Handle<String> identifier) {
  return isolate->factory()->UTC_string();
}
#endif  // V8_INTL_SUPPORT

// #sec-temporal-mergelargestunitoption
MaybeHandle<JSObject> MergeLargestUnitOption(Isolate* isolate,
                                             Handle<JSReceiver> options,
                                             Unit largest_unit) {
  TEMPORAL_ENTER_FUNC();
  // 1. Let merged be ! OrdinaryObjectCreate(%Object.prototype%).
  Handle<JSObject> merged =
      isolate->factory()->NewJSObject(isolate->object_function());
  // 2. Let keys be ? EnumerableOwnPropertyNames(options, key).
  // 3. For each element nextKey of keys, do
  // a. Let propValue be ? Get(options, nextKey).
  // b. Perform ! CreateDataPropertyOrThrow(merged, nextKey, propValue).
  JSReceiver::SetOrCopyDataProperties(
      isolate, merged, options, PropertiesEnumerationMode::kEnumerationOrder,
      nullptr, false)
      .Check();

  // 4. Perform ! CreateDataPropertyOrThrow(merged, "largestUnit", largestUnit).
  CHECK(JSReceiver::CreateDataProperty(
            isolate, merged, isolate->factory()->largestUnit_string(),
            UnitToString(isolate, largest_unit), Just(kThrowOnError))
            .FromJust());
  // 5. Return merged.
  return merged;
}

MaybeHandle<Object> ToIntegerThrowOnInfinity(Isolate* isolate,
                                             Handle<Object> argument) {
  TEMPORAL_ENTER_FUNC();

  // 1. Let integer be ? ToIntegerOrInfinity(argument).
  ASSIGN_RETURN_ON_EXCEPTION(isolate, argument,
                             Object::ToInteger(isolate, argument), Object);
  // 2. If integer is +∞ or -∞, throw a RangeError exception.
  if (!std::isfinite(argument->Number())) {
    // a. Throw a RangeError exception.
    THROW_NEW_ERROR(isolate, NEW_TEMPORAL_INVALD_ARG_RANGE_ERROR(), Object);
  }
  return argument;
}

Unit LargerOfTwoTemporalUnits(Isolate* isolate, Unit u1, Unit u2) {
  // 1. If either u1 or u2 is "year", return "year".
  if (u1 == Unit::kYear || u2 == Unit::kYear) return Unit::kYear;
  // 2. If either u1 or u2 is "month", return "month".
  if (u1 == Unit::kMonth || u2 == Unit::kMonth) return Unit::kMonth;
  // 3. If either u1 or u2 is "week", return "week".
  if (u1 == Unit::kWeek || u2 == Unit::kWeek) return Unit::kWeek;
  // 4. If either u1 or u2 is "day", return "day".
  if (u1 == Unit::kDay || u2 == Unit::kDay) return Unit::kDay;
  // 5. If either u1 or u2 is "hour", return "hour".
  if (u1 == Unit::kHour || u2 == Unit::kHour) return Unit::kHour;
  // 6. If either u1 or u2 is "minute", return "minute".
  if (u1 == Unit::kMinute || u2 == Unit::kMinute) return Unit::kMinute;
  // 7. If either u1 or u2 is "second", return "second".
  if (u1 == Unit::kSecond || u2 == Unit::kSecond) return Unit::kSecond;
  // 8. If either u1 or u2 is "millisecond", return "millisecond".
  if (u1 == Unit::kMillisecond || u2 == Unit::kMillisecond)
    return Unit::kMillisecond;
  // 9. If either u1 or u2 is "microsecond", return "microsecond".
  if (u1 == Unit::kMicrosecond || u2 == Unit::kMicrosecond)
    return Unit::kMicrosecond;
  // 10. Return "nanosecond".
  return Unit::kNanosecond;
}

Handle<String> UnitToString(Isolate* isolate, Unit unit) {
  switch (unit) {
    case Unit::kYear:
      return ReadOnlyRoots(isolate).year_string_handle();
    case Unit::kMonth:
      return ReadOnlyRoots(isolate).month_string_handle();
    case Unit::kWeek:
      return ReadOnlyRoots(isolate).week_string_handle();
    case Unit::kDay:
      return ReadOnlyRoots(isolate).day_string_handle();
    case Unit::kHour:
      return ReadOnlyRoots(isolate).hour_string_handle();
    case Unit::kMinute:
      return ReadOnlyRoots(isolate).minute_string_handle();
    case Unit::kSecond:
      return ReadOnlyRoots(isolate).second_string_handle();
    case Unit::kMillisecond:
      return ReadOnlyRoots(isolate).millisecond_string_handle();
    case Unit::kMicrosecond:
      return ReadOnlyRoots(isolate).microsecond_string_handle();
    case Unit::kNanosecond:
      return ReadOnlyRoots(isolate).nanosecond_string_handle();
    default:
      UNREACHABLE();
  }
}

// #sec-temporal-balanceisodate
void BalanceISODate(Isolate* isolate, int32_t* year, int32_t* month,
                    int32_t* day) {
  TEMPORAL_ENTER_FUNC();

  // 1. Assert: year, month, and day are integers.
  // 2. Let balancedYearMonth be ! BalanceISOYearMonth(year, month).
  // 3. Set month to balancedYearMonth.[[Month]].
  // 4. Set year to balancedYearMonth.[[Year]].
  BalanceISOYearMonth(isolate, year, month);
  // 5. NOTE: To deal with negative numbers of days whose absolute value is
  // greater than the number of days in a year, the following section subtracts
  // years and adds days until the number of days is greater than −366 or −365.
  // 6. If month > 2, then
  // a. Let testYear be year.
  // 7. Else,
  // a. Let testYear be year − 1.
  int32_t test_year = (*month > 2) ? *year : *year - 1;
  // 8. Repeat, while day < −1 × ! ISODaysInYear(testYear),
  int32_t iso_days_in_year;
  while (*day < -(iso_days_in_year = ISODaysInYear(isolate, test_year))) {
    // a. Set day to day + ! ISODaysInYear(testYear).
    *day += iso_days_in_year;
    // b. Set year to year − 1.
    (*year)--;
    // c. Set testYear to testYear − 1.
    test_year--;
  }
  // 9. NOTE: To deal with numbers of days greater than the number of days in a
  // year, the following section adds years and subtracts days until the number
  // of days is less than 366 or 365.
  // 10. Let testYear be year + 1.
  test_year = (*year) + 1;
  // 11. Repeat, while day > ! ISODaysInYear(testYear),
  while (*day > (iso_days_in_year = ISODaysInYear(isolate, test_year))) {
    // a. Set day to day − ! ISODaysInYear(testYear).
    *day -= iso_days_in_year;
    // b. Set year to year + 1.
    (*year)++;
    // c. Set testYear to testYear + 1.
    test_year++;
  }
  // 12. NOTE: To deal with negative numbers of days whose absolute value is
  // greater than the number of days in the current month, the following section
  // subtracts months and adds days until the number of days is greater than 0.
  // 13. Repeat, while day < 1,
  while (*day < 1) {
    // a. Set balancedYearMonth to ! BalanceISOYearMonth(year, month − 1).
    // b. Set year to balancedYearMonth.[[Year]].
    // c. Set month to balancedYearMonth.[[Month]].
    *month -= 1;
    BalanceISOYearMonth(isolate, year, month);
    // d. Set day to day + ! ISODaysInMonth(year, month).
    *day += ISODaysInMonth(isolate, *year, *month);
  }
  // 14. NOTE: To deal with numbers of days greater than the number of days in
  // the current month, the following section adds months and subtracts days
  // until the number of days is less than the number of days in the month.
  // 15. Repeat, while day > ! ISODaysInMonth(year, month),
  int32_t iso_days_in_month;
  while (*day > (iso_days_in_month = ISODaysInMonth(isolate, *year, *month))) {
    // a. Set day to day − ! ISODaysInMonth(year, month).
    *day -= iso_days_in_month;
    // b. Set balancedYearMonth to ! BalanceISOYearMonth(year, month + 1).
    // c. Set year to balancedYearMonth.[[Year]].
    // d. Set month to balancedYearMonth.[[Month]].
    *month += 1;
    BalanceISOYearMonth(isolate, year, month);
  }
  // 16. Return the new Record { [[Year]]: year, [[Month]]: month, [[Day]]: day
  // }.
  return;
}

// #sec-temporal-adddatetime
Maybe<DateTimeRecordCommon> AddDateTime(
    Isolate* isolate, int32_t year, int32_t month, int32_t day, int32_t hour,
    int32_t minute, int32_t second, int32_t millisecond, int32_t microsecond,
    int32_t nanosecond, Handle<JSReceiver> calendar, const DurationRecord& dur,
    Handle<Object> options) {
  TEMPORAL_ENTER_FUNC();

  // 1. Assert: year, month, day, hour, minute, second, millisecond,
  // microsecond, and nanosecond are integers.
  // 2. Let timeResult be ! AddTime(hour, minute, second, millisecond,
  // microsecond, nanosecond, hours, minutes, seconds, milliseconds,
  // microseconds, nanoseconds).
  DateTimeRecordCommon time_result =
      AddTime(isolate, hour, minute, second, millisecond, microsecond,
              nanosecond, dur.hours, dur.minutes, dur.seconds, dur.milliseconds,
              dur.microseconds, dur.nanoseconds);

  // 3. Let datePart be ? CreateTemporalDate(year, month, day, calendar).
  Handle<JSTemporalPlainDate> date_part;
  ASSIGN_RETURN_ON_EXCEPTION_VALUE(
      isolate, date_part,
      CreateTemporalDate(isolate, year, month, day, calendar),
      Nothing<DateTimeRecordCommon>());
  // 4. Let dateDuration be ? CreateTemporalDuration(years, months, weeks, days
  // + timeResult.[[Days]], 0, 0, 0, 0, 0, 0).
  Handle<JSTemporalDuration> date_duration;
  ASSIGN_RETURN_ON_EXCEPTION_VALUE(
      isolate, date_duration,
      CreateTemporalDuration(isolate, dur.years, dur.months, dur.weeks,
                             dur.days + time_result.day, 0, 0, 0, 0, 0, 0),
      Nothing<DateTimeRecordCommon>());
  // 5. Let addedDate be ? CalendarDateAdd(calendar, datePart, dateDuration,
  // options).
  Handle<JSTemporalPlainDate> added_date;
  ASSIGN_RETURN_ON_EXCEPTION_VALUE(
      isolate, added_date,
      CalendarDateAdd(isolate, calendar, date_part, date_duration, options),
      Nothing<DateTimeRecordCommon>());
  // 6. Return the new Record { [[Year]]: addedDate.[[ISOYear]], [[Month]]:
  // addedDate.[[ISOMonth]], [[Day]]: addedDate.[[ISODay]], [[Hour]]:
  // timeResult.[[Hour]], [[Minute]]: timeResult.[[Minute]], [[Second]]:
  // timeResult.[[Second]], [[Millisecond]]: timeResult.[[Millisecond]],
  // [[Microsecond]]: timeResult.[[Microsecond]], [[Nanosecond]]:
  // timeResult.[[Nanosecond]], }.
  time_result.year = added_date->iso_year();
  time_result.month = added_date->iso_month();
  time_result.day = added_date->iso_day();
  return Just(time_result);
}

Maybe<bool> BalanceDuration(Isolate* isolate, int64_t* days, int64_t* hours,
                            int64_t* minutes, int64_t* seconds,
                            int64_t* milliseconds, int64_t* microseconds,
                            int64_t* nanoseconds, Unit largest_unit,
                            const char* method) {
  TEMPORAL_ENTER_FUNC();

  // 1. If relativeTo is not present, set relativeTo to undefined.
  return BalanceDuration(isolate, days, hours, minutes, seconds, milliseconds,
                         microseconds, nanoseconds, largest_unit,
                         isolate->factory()->undefined_value(), method);
}

Maybe<bool> BalanceDuration(Isolate* isolate, int64_t* days, int64_t* hours,
                            int64_t* minutes, int64_t* seconds,
                            int64_t* milliseconds, int64_t* microseconds,
                            int64_t* nanoseconds, Unit largest_unit,
                            Handle<Object> relative_to_obj,
                            const char* method) {
  TEMPORAL_ENTER_FUNC();

  // 2. If Type(relativeTo) is Object and relativeTo has an
  // [[InitializedTemporalZonedDateTime]] internal slot, then
  if (relative_to_obj->IsJSTemporalZonedDateTime()) {
    Handle<JSTemporalZonedDateTime> relative_to =
        Handle<JSTemporalZonedDateTime>::cast(relative_to_obj);
    // a. Let endNs be ? AddZonedDateTime(relativeTo.[[Nanoseconds]],
    // relativeTo.[[TimeZone]], relativeTo.[[Calendar]], 0, 0, 0, days, hours,
    // minutes, seconds, milliseconds, microseconds, nanoseconds).
    Handle<BigInt> end_ns;
    ASSIGN_RETURN_ON_EXCEPTION_VALUE(
        isolate, end_ns,
        AddZonedDateTime(isolate,
                         Handle<BigInt>(relative_to->nanoseconds(), isolate),
                         Handle<JSReceiver>(relative_to->time_zone(), isolate),
                         Handle<JSReceiver>(relative_to->calendar(), isolate),
                         {0, 0, 0, *days, *hours, *minutes, *seconds,
                          *milliseconds, *microseconds, *nanoseconds},
                         method),
        Nothing<bool>());
    // b. Set nanoseconds to endNs − relativeTo.[[Nanoseconds]].
    ASSIGN_RETURN_ON_EXCEPTION_VALUE(
        isolate, end_ns,
        BigInt::Subtract(isolate, end_ns,
                         Handle<BigInt>(relative_to->nanoseconds(), isolate)),
        Nothing<bool>());
    *nanoseconds = end_ns->AsInt64();
    // 3. Else,
  } else {
    // a. Set nanoseconds to ℤ(! TotalDurationNanoseconds(days, hours, minutes,
    // seconds, milliseconds, microseconds, nanoseconds, 0)).
    *nanoseconds =
        TotalDurationNanoseconds(isolate, *days, *hours, *minutes, *seconds,
                                 *milliseconds, *microseconds, *nanoseconds, 0);
  }
  // 4. If largestUnit is one of "year", "month", "week", or "day", then
  if (largest_unit == Unit::kYear || largest_unit == Unit::kMonth ||
      largest_unit == Unit::kWeek || largest_unit == Unit::kDay) {
    int64_t result_day_length;
    // a. Let result be ? NanosecondsToDays(nanoseconds, relativeTo).
    Maybe<bool> maybe_result =
        NanosecondsToDays(isolate, *nanoseconds, relative_to_obj, days,
                          nanoseconds, &result_day_length, method);
    MAYBE_RETURN(maybe_result, Nothing<bool>());
    DCHECK(maybe_result.FromJust());
    // b. Set days to result.[[Days]].
    // c. Set nanoseconds to result.[[Nanoseconds]].
    // 5. Else,
  } else {
    // a. Set days to 0.
    *days = 0;
  }
  // 6. Set hours, minutes, seconds, milliseconds, and microseconds to 0.
  *hours = *minutes = *seconds = *milliseconds = *microseconds = 0;
  // 7. Set nanoseconds to ℝ(nanoseconds).

  // 8. If nanoseconds < 0, let sign be −1; else, let sign be 1.
  int32_t sign = (*nanoseconds < 0) ? -1 : 1;
  // 9. Set nanoseconds to abs(nanoseconds).
  *nanoseconds = std::abs(*nanoseconds);
  // 10. If largestUnit is "year", "month", "week", "day", or "hour", then
  switch (largest_unit) {
    case Unit::kYear:
    case Unit::kMonth:
    case Unit::kWeek:
    case Unit::kDay:
    case Unit::kHour:
      // a. Set microseconds to floor(nanoseconds / 1000).
      *microseconds = floor_divide(*nanoseconds, 1000);
      // b. Set nanoseconds to nanoseconds modulo 1000.
      *nanoseconds = modulo(*nanoseconds, 1000);
      // c. Set milliseconds to floor(microseconds / 1000).
      *milliseconds = floor_divide(*microseconds, 1000);
      // d. Set microseconds to microseconds modulo 1000.
      *microseconds = modulo(*microseconds, 1000);
      // e. Set seconds to floor(milliseconds / 1000).
      *seconds = floor_divide(*milliseconds, 1000);
      // f. Set milliseconds to milliseconds modulo 1000.
      *milliseconds = modulo(*milliseconds, 1000);
      // g. Set minutes to floor(seconds, 60).
      *minutes = floor_divide(*seconds, 60);
      // h. Set seconds to seconds modulo 60.
      *seconds = modulo(*seconds, 60);
      // i. Set hours to floor(minutes / 60).
      *hours = floor_divide(*minutes, 60);
      // j. Set minutes to minutes modulo 60.
      *minutes = modulo(*minutes, 60);
      break;
    // 11. Else if largestUnit is "minute", then
    case Unit::kMinute:
      // a. Set microseconds to floor(nanoseconds / 1000).
      *microseconds = floor_divide(*nanoseconds, 1000);
      // b. Set nanoseconds to nanoseconds modulo 1000.
      *nanoseconds = modulo(*nanoseconds, 1000);
      // c. Set milliseconds to floor(microseconds / 1000).
      *milliseconds = floor_divide(*microseconds, 1000);
      // d. Set microseconds to microseconds modulo 1000.
      *microseconds = modulo(*microseconds, 1000);
      // e. Set seconds to floor(milliseconds / 1000).
      *seconds = floor_divide(*milliseconds, 1000);
      // f. Set milliseconds to milliseconds modulo 1000.
      *milliseconds = modulo(*milliseconds, 1000);
      // g. Set minutes to floor(seconds / 60).
      *minutes = floor_divide(*seconds, 60);
      // h. Set seconds to seconds modulo 60.
      *seconds = modulo(*seconds, 60);
      break;
    // 12. Else if largestUnit is "second", then
    case Unit::kSecond:
      // a. Set microseconds to floor(nanoseconds / 1000).
      *microseconds = floor_divide(*nanoseconds, 1000);
      // b. Set nanoseconds to nanoseconds modulo 1000.
      *nanoseconds = modulo(*nanoseconds, 1000);
      // c. Set milliseconds to floor(microseconds / 1000).
      *milliseconds = floor_divide(*microseconds, 1000);
      // d. Set microseconds to microseconds modulo 1000.
      *microseconds = modulo(*microseconds, 1000);
      // e. Set seconds to floor(milliseconds / 1000).
      *seconds = floor_divide(*milliseconds, 1000);
      // f. Set milliseconds to milliseconds modulo 1000.
      *milliseconds = modulo(*milliseconds, 1000);
      break;
    // 13. Else if largestUnit is "millisecond", then
    case Unit::kMillisecond:
      // a. Set microseconds to floor(nanoseconds / 1000).
      *microseconds = floor_divide(*nanoseconds, 1000);
      // b. Set nanoseconds to nanoseconds modulo 1000.
      *nanoseconds = modulo(*nanoseconds, 1000);
      // c. Set milliseconds to floor(microseconds / 1000).
      *milliseconds = floor_divide(*microseconds, 1000);
      // d. Set microseconds to microseconds modulo 1000.
      *microseconds = modulo(*microseconds, 1000);
      break;
    // 14. Else if largestUnit is "microsecond", then
    case Unit::kMicrosecond:
      // a. Set microseconds to floor(nanoseconds / 1000).
      *microseconds = floor_divide(*nanoseconds, 1000);
      // b. Set nanoseconds to nanoseconds modulo 1000.
      *nanoseconds = modulo(*nanoseconds, 1000);
      break;
    // 15. Else,
    default:
      // a. Assert: largestUnit is "nanosecond".
      DCHECK_EQ(largest_unit, Unit::kNanosecond);
      break;
  }
  // 16. Return the new Record { [[Days]]: 𝔽(days), [[Hours]]: 𝔽(hours × sign),
  // [[Minutes]]: 𝔽(minutes × sign), [[Seconds]]: 𝔽(seconds × sign),
  // [[Milliseconds]]: 𝔽(milliseconds × sign), [[Microseconds]]: 𝔽(microseconds
  // × sign), [[Nanoseconds]]: 𝔽(nanoseconds × sign) }.
  *hours *= sign;
  *minutes *= sign;
  *seconds *= sign;
  *milliseconds *= sign;
  *microseconds *= sign;
  *nanoseconds *= sign;
  return Just(true);
}

// #sec-temporal-addinstant
MaybeHandle<BigInt> AddZonedDateTime(Isolate* isolate,
                                     Handle<BigInt> epoch_nanoseconds,
                                     Handle<JSReceiver> time_zone,
                                     Handle<JSReceiver> calendar,
                                     const DurationRecord& duration,
                                     const char* method) {
  TEMPORAL_ENTER_FUNC();

  // 1. If options is not present, set options to ! OrdinaryObjectCreate(null).
  Handle<JSReceiver> options = isolate->factory()->NewJSObjectWithNullProto();
  return AddZonedDateTime(isolate, epoch_nanoseconds, time_zone, calendar,
                          duration, options, method);
}

// #sec-temporal-addzoneddatetime
MaybeHandle<BigInt> AddZonedDateTime(Isolate* isolate,
                                     Handle<BigInt> epoch_nanoseconds,
                                     Handle<JSReceiver> time_zone,
                                     Handle<JSReceiver> calendar,
                                     const DurationRecord& duration,
                                     Handle<JSReceiver> options,
                                     const char* method) {
  TEMPORAL_ENTER_FUNC();

  // 2. If all of years, months, weeks, and days are 0, then
  if (duration.years == 0 && duration.months == 0 && duration.weeks == 0 &&
      duration.days == 0) {
    // a. Return ! AddInstant(epochNanoseconds, hours, minutes, seconds,
    // milliseconds, microseconds, nanoseconds).
    return AddInstant(isolate, epoch_nanoseconds, duration.hours,
                      duration.minutes, duration.seconds, duration.milliseconds,
                      duration.microseconds, duration.nanoseconds);
  }
  // 3. Let instant be ! CreateTemporalInstant(epochNanoseconds).
  Handle<JSTemporalInstant> instant;
  ASSIGN_RETURN_ON_EXCEPTION(isolate, instant,
                             CreateTemporalInstant(isolate, epoch_nanoseconds),
                             BigInt);

  // 4. Let temporalDateTime be ?
  // BuiltinTimeZoneGetPlainDateTimeFor(timeZone, instant, calendar).
  Handle<JSTemporalPlainDateTime> temporal_date_time;
  ASSIGN_RETURN_ON_EXCEPTION(isolate, temporal_date_time,
                             BuiltinTimeZoneGetPlainDateTimeFor(
                                 isolate, time_zone, instant, calendar, method),
                             BigInt);
  // 5. Let datePart be ? CreateTemporalDate(temporalDateTime.[[ISOYear]],
  // temporalDateTime.[[ISOMonth]], temporalDateTime.[[ISODay]], calendar).
  Handle<JSTemporalPlainDate> date_part;
  ASSIGN_RETURN_ON_EXCEPTION(
      isolate, date_part,
      CreateTemporalDate(isolate, temporal_date_time->iso_year(),
                         temporal_date_time->iso_month(),
                         temporal_date_time->iso_day(), calendar),
      BigInt);
  // 6. Let dateDuration be ? CreateTemporalDuration(years, months, weeks, days,
  // 0, 0, 0, 0, 0, 0).
  Handle<JSTemporalDuration> date_duration;
  ASSIGN_RETURN_ON_EXCEPTION(
      isolate, date_duration,
      CreateTemporalDuration(isolate, duration.years, duration.months,
                             duration.weeks, duration.days, 0, 0, 0, 0, 0, 0),
      BigInt);
  // 7. Let addedDate be ? CalendarDateAdd(calendar, datePart, dateDuration,
  // options).
  Handle<JSTemporalPlainDate> added_date;
  ASSIGN_RETURN_ON_EXCEPTION(
      isolate, added_date,
      CalendarDateAdd(isolate, calendar, date_part, date_duration, options),
      BigInt);
  // 8. Let intermediateDateTime be ?
  // CreateTemporalDateTime(addedDate.[[ISOYear]], addedDate.[[ISOMonth]],
  // addedDate.[[ISODay]], temporalDateTime.[[ISOHour]],
  // temporalDateTime.[[ISOMinute]], temporalDateTime.[[ISOSecond]],
  // temporalDateTime.[[ISOMillisecond]], temporalDateTime.[[ISOMicrosecond]],
  // temporalDateTime.[[ISONanosecond]], calendar).
  Handle<JSTemporalPlainDateTime> intermediate_date_time;
  ASSIGN_RETURN_ON_EXCEPTION(
      isolate, intermediate_date_time,
      temporal::CreateTemporalDateTime(
          isolate, added_date->iso_year(), added_date->iso_month(),
          added_date->iso_day(), temporal_date_time->iso_hour(),
          temporal_date_time->iso_minute(), temporal_date_time->iso_second(),
          temporal_date_time->iso_millisecond(),
          temporal_date_time->iso_microsecond(),
          temporal_date_time->iso_nanosecond(), calendar),
      BigInt);
  // 9. Let intermediateInstant be ? BuiltinTimeZoneGetInstantFor(timeZone,
  // intermediateDateTime, "compatible").
  Handle<JSTemporalInstant> intermediate_instant;
  ASSIGN_RETURN_ON_EXCEPTION(
      isolate, intermediate_instant,
      BuiltinTimeZoneGetInstantFor(isolate, time_zone, intermediate_date_time,
                                   Disambiguation::kCompatible, method),
      BigInt);
  // 10. Return ! AddInstant(intermediateInstant.[[Nanoseconds]], hours,
  // minutes, seconds, milliseconds, microseconds, nanoseconds).
  return AddInstant(
      isolate, Handle<BigInt>(intermediate_instant->nanoseconds(), isolate),
      duration.hours, duration.minutes, duration.seconds, duration.milliseconds,
      duration.microseconds, duration.nanoseconds);
}

// #sec-temporal-nanosecondstodays
Maybe<bool> NanosecondsToDays(Isolate* isolate, int64_t nanoseconds,
                              Handle<Object> relative_to_obj,
                              int64_t* result_days, int64_t* result_nanoseconds,
                              int64_t* result_day_length, const char* method) {
  return NanosecondsToDays(isolate, BigInt::FromInt64(isolate, nanoseconds),
                           relative_to_obj, result_days, result_nanoseconds,
                           result_day_length, method);
}

Maybe<bool> NanosecondsToDays(Isolate* isolate, Handle<BigInt> nanoseconds,
                              Handle<Object> relative_to_obj,
                              int64_t* result_days, int64_t* result_nanoseconds,
                              int64_t* result_day_length, const char* method) {
  TEMPORAL_ENTER_FUNC();

  // 1. Assert: Type(nanoseconds) is BigInt.
  // 2. Set nanoseconds to ℝ(nanoseconds).
  // 3. Let sign be ! ℝ(Sign(𝔽(nanoseconds))).
  ComparisonResult compare_result =
      BigInt::CompareToBigInt(nanoseconds, BigInt::FromInt64(isolate, 0));
  int64_t sign = COMPARE_RESULT_TO_SIGN(compare_result);
  // 4. Let dayLengthNs be 8.64 × 10^13.
  Handle<BigInt> day_length_ns = BigInt::FromInt64(isolate, 86400000000000LLU);
  // 5. If sign is 0, then
  if (sign == 0) {
    // a. Return the new Record { [[Days]]: 0, [[Nanoseconds]]: 0,
    // [[DayLength]]: dayLengthNs }.
    *result_days = 0;
    *result_nanoseconds = 0;
    *result_day_length = day_length_ns->AsInt64();
    return Just(true);
  }
  // 6. If Type(relativeTo) is not Object or relativeTo does not have an
  // [[InitializedTemporalZonedDateTime]] internal slot, then
  if (!relative_to_obj->IsJSTemporalZonedDateTime()) {
    // Return the Record {
    // [[Days]]: the integral part of nanoseconds / dayLengthNs,
    // [[Nanoseconds]]: (abs(nanoseconds) modulo dayLengthNs) × sign,
    // [[DayLength]]: dayLengthNs }.
    Handle<BigInt> days_bigint;
    ASSIGN_RETURN_ON_EXCEPTION_VALUE(
        isolate, days_bigint,
        BigInt::Divide(isolate, nanoseconds, day_length_ns), Nothing<bool>());

    if (sign < 0) {
      nanoseconds = BigInt::UnaryMinus(isolate, nanoseconds);
    }

    ASSIGN_RETURN_ON_EXCEPTION_VALUE(
        isolate, nanoseconds,
        BigInt::Remainder(isolate, nanoseconds, day_length_ns),
        Nothing<bool>());
    *result_days = days_bigint->AsInt64();
    *result_nanoseconds = nanoseconds->AsInt64() * sign;
    *result_day_length = day_length_ns->AsInt64();
    return Just(true);
  }
  Handle<JSTemporalZonedDateTime> relative_to =
      Handle<JSTemporalZonedDateTime>::cast(relative_to_obj);
  // 7. Let startNs be ℝ(relativeTo.[[Nanoseconds]]).
  Handle<BigInt> start_ns = Handle<BigInt>(relative_to->nanoseconds(), isolate);
  // 8. Let startInstant be ! CreateTemporalInstant(ℤ(sartNs)).
  Handle<JSTemporalInstant> start_instant;
  ASSIGN_RETURN_ON_EXCEPTION_VALUE(
      isolate, start_instant,
      CreateTemporalInstant(
          isolate, Handle<BigInt>(relative_to->nanoseconds(), isolate)),
      Nothing<bool>());

  // 9. Let startDateTime be ?
  // BuiltinTimeZoneGetPlainDateTimeFor(relativeTo.[[TimeZone]],
  // startInstant, relativeTo.[[Calendar]]).
  Handle<JSReceiver> time_zone =
      Handle<JSReceiver>(relative_to->time_zone(), isolate);
  Handle<JSReceiver> calendar =
      Handle<JSReceiver>(relative_to->calendar(), isolate);
  Handle<JSTemporalPlainDateTime> start_date_time;
  ASSIGN_RETURN_ON_EXCEPTION_VALUE(
      isolate, start_date_time,
      BuiltinTimeZoneGetPlainDateTimeFor(isolate, time_zone, start_instant,
                                         calendar, method),
      Nothing<bool>());

  // 10. Let endNs be startNs + nanoseconds.
  Handle<BigInt> end_ns;
  ASSIGN_RETURN_ON_EXCEPTION_VALUE(isolate, end_ns,
                                   BigInt::Add(isolate, start_ns, nanoseconds),
                                   Nothing<bool>());

  // 11. Let endInstant be ! CreateTemporalInstant(ℤ(endNs)).
  Handle<JSTemporalInstant> end_instant;
  ASSIGN_RETURN_ON_EXCEPTION_VALUE(isolate, end_instant,
                                   CreateTemporalInstant(isolate, end_ns),
                                   Nothing<bool>());
  // 12. Let endDateTime be ?
  // BuiltinTimeZoneGetPlainDateTimeFor(relativeTo.[[TimeZone]],
  // endInstant, relativeTo.[[Calendar]]).
  Handle<JSTemporalPlainDateTime> end_date_time;
  ASSIGN_RETURN_ON_EXCEPTION_VALUE(
      isolate, end_date_time,
      BuiltinTimeZoneGetPlainDateTimeFor(isolate, time_zone, end_instant,
                                         calendar, method),
      Nothing<bool>());

  // 13. Let dateDifference be ?
  // DifferenceISODateTime(startDateTime.[[ISOYear]],
  // startDateTime.[[ISOMonth]], startDateTime.[[ISODay]],
  // startDateTime.[[ISOHour]], startDateTime.[[ISOMinute]],
  // startDateTime.[[ISOSecond]], startDateTime.[[ISOMillisecond]],
  // startDateTime.[[ISOMicrosecond]], startDateTime.[[ISONanosecond]],
  // endDateTime.[[ISOYear]], endDateTime.[[ISOMonth]], endDateTime.[[ISODay]],
  // endDateTime.[[ISOHour]], endDateTime.[[ISOMinute]],
  // endDateTime.[[ISOSecond]], endDateTime.[[ISOMillisecond]],
  // endDateTime.[[ISOMicrosecond]], endDateTime.[[ISONanosecond]],
  // relativeTo.[[Calendar]], "day").
  Maybe<DurationRecord> maybe_date_difference = DifferenceISODateTime(
      isolate, start_date_time->iso_year(), start_date_time->iso_month(),
      start_date_time->iso_day(), start_date_time->iso_hour(),
      start_date_time->iso_minute(), start_date_time->iso_second(),
      start_date_time->iso_millisecond(), start_date_time->iso_microsecond(),
      start_date_time->iso_nanosecond(), end_date_time->iso_year(),
      end_date_time->iso_month(), end_date_time->iso_day(),
      end_date_time->iso_hour(), end_date_time->iso_minute(),
      end_date_time->iso_second(), end_date_time->iso_millisecond(),
      end_date_time->iso_microsecond(), end_date_time->iso_nanosecond(),
      calendar, Unit::kDay, relative_to, method);
  MAYBE_RETURN(maybe_date_difference, Nothing<bool>());

  DurationRecord date_difference = maybe_date_difference.FromJust();
  // 14. Let days be dateDifference.[[Days]].
  int64_t days = date_difference.days;

  // 15. Let intermediateNs be ℝ(? AddZonedDateTime(ℤ(startNs),
  // relativeTo.[[TimeZone]], relativeTo.[[Calendar]], 0, 0, 0, days, 0, 0, 0,
  // 0, 0, 0)).
  Handle<BigInt> intermediate_ns;
  ASSIGN_RETURN_ON_EXCEPTION_VALUE(
      isolate, intermediate_ns,
      AddZonedDateTime(isolate, start_ns, time_zone, calendar,
                       {0, 0, 0, days, 0, 0, 0, 0, 0, 0}, method),
      Nothing<bool>());

  // 16. If sign is 1, then
  if (sign == 1) {
    // a. Repeat, while days > 0 and intermediateNs > endNs,
    while (days > 0 && BigInt::CompareToBigInt(intermediate_ns, end_ns) ==
                           ComparisonResult::kGreaterThan) {
      // i. Set days to days − 1.
      days -= 1;
      // ii. Set intermediateNs to ℝ(? AddZonedDateTime(ℤ(startNs),
      // relativeTo.[[TimeZone]], relativeTo.[[Calendar]], 0, 0, 0, days, 0, 0,
      // 0, 0, 0, 0)).
      ASSIGN_RETURN_ON_EXCEPTION_VALUE(
          isolate, intermediate_ns,
          AddZonedDateTime(isolate, start_ns, time_zone, calendar,
                           {0, 0, 0, days, 0, 0, 0, 0, 0, 0}, method),
          Nothing<bool>());
    }
  }

  // 17. Set nanoseconds to endNs − intermediateNs.
  ASSIGN_RETURN_ON_EXCEPTION_VALUE(
      isolate, nanoseconds, BigInt::Subtract(isolate, end_ns, intermediate_ns),
      Nothing<bool>());

  // 18. Let done be false.
  bool done = false;

  // 19. Repeat, while done is false,
  while (!done) {
    // a. Let oneDayFartherNs be ℝ(? AddZonedDateTime(ℤ(intermediateNs),
    // relativeTo.[[TimeZone]], relativeTo.[[Calendar]], 0, 0, 0, sign, 0, 0, 0,
    // 0, 0, 0)).
    Handle<BigInt> one_day_farther_ns;
    ASSIGN_RETURN_ON_EXCEPTION_VALUE(
        isolate, one_day_farther_ns,
        AddZonedDateTime(isolate, intermediate_ns, time_zone, calendar,
                         {0, 0, 0, sign, 0, 0, 0, 0, 0, 0}, method),
        Nothing<bool>());

    // b. Set dayLengthNs to oneDayFartherNs − intermediateNs.
    Handle<BigInt> day_length_ns;
    ASSIGN_RETURN_ON_EXCEPTION_VALUE(
        isolate, day_length_ns,
        BigInt::Subtract(isolate, one_day_farther_ns, intermediate_ns),
        Nothing<bool>());

    // c. If (nanoseconds − dayLengthNs) × sign ≥ 0, then
    compare_result = BigInt::CompareToBigInt(nanoseconds, day_length_ns);
    if (sign * COMPARE_RESULT_TO_SIGN(compare_result) >= 0) {
      // i. Set nanoseconds to nanoseconds − dayLengthNs.
      ASSIGN_RETURN_ON_EXCEPTION_VALUE(
          isolate, nanoseconds,
          BigInt::Subtract(isolate, nanoseconds, day_length_ns),
          Nothing<bool>());

      // ii. Set intermediateNs to oneDayFartherNs.
      intermediate_ns = one_day_farther_ns;

      // iii. Set days to days + sign.
      days += sign;
      // d. Else,
    } else {
      // i. Set done to true.
      done = true;
    }
  }

  // 20. Return the new Record { [[Days]]: days, [[Nanoseconds]]: nanoseconds,
  // [[DayLength]]: abs(dayLengthNs) }.
  *result_days = days;
  *result_nanoseconds = nanoseconds->AsInt64();
  *result_day_length = std::abs(day_length_ns->AsInt64());
  return Just(true);
}

Maybe<DurationRecord> DifferenceISODateTime(
    Isolate* isolate, int32_t y1, int32_t mon1, int32_t d1, int32_t h1,
    int32_t min1, int32_t s1, int32_t ms1, int32_t mus1, int32_t ns1,
    int32_t y2, int32_t mon2, int32_t d2, int32_t h2, int32_t min2, int32_t s2,
    int32_t ms2, int32_t mus2, int32_t ns2, Handle<JSReceiver> calendar,
    Unit largest_unit, Handle<Object> options_obj, const char* method) {
  TEMPORAL_ENTER_FUNC();

  Factory* factory = isolate->factory();
  DurationRecord result;
  // 1. Assert: y1, mon1, d1, h1, min1, s1, ms1, mus1, ns1, y2, mon2, d2, h2,
  // min2, s2, ms2, mus2, and ns2 are integers.
  // 2. If options is not present, set options to ! OrdinaryObjectCreate(null).
  Handle<JSReceiver> options;
  if (options_obj->IsUndefined()) {
    options = factory->NewJSObjectWithNullProto();
  } else {
    DCHECK(options_obj->IsJSReceiver());
    options = Handle<JSReceiver>::cast(options_obj);
  }
  // 3. Let timeDifference be ! DifferenceTime(h1, min1, s1, ms1, mus1, ns1, h2,
  // min2, s2, ms2, mus2, ns2).
  DurationRecord time_difference = DifferenceTime(
      isolate, h1, min1, s1, ms1, mus1, ns1, h2, min2, s2, ms2, mus2, ns2);

  result.hours = time_difference.hours;
  result.minutes = time_difference.minutes;
  result.seconds = time_difference.seconds;
  result.milliseconds = time_difference.milliseconds;
  result.microseconds = time_difference.microseconds;
  result.nanoseconds = time_difference.nanoseconds;

  // 4. Let timeSign be ! DurationSign(0, 0, 0, timeDifference.[[Days]],
  // timeDifference.[[Hours]], timeDifference.[[Minutes]],
  // timeDifference.[[Seconds]], timeDifference.[[Milliseconds]],
  // timeDifference.[[Microseconds]], timeDifference.[[Nanoseconds]]).
  int32_t time_sign = DurationSign(isolate, time_difference);
  // 5. Let dateSign be ! CompareISODate(y2, mon2, d2, y1, mon1, d1).
  int32_t date_sign = CompareISODate(isolate, y2, mon2, d2, y1, mon1, d1);
  // 6. Let balanceResult be ! BalanceISODate(y1, mon1, d1 +
  // timeDifference.[[Days]]).
  int32_t balanced_year = y1;
  int32_t balanced_month = mon1;
  int32_t balanced_day = d1 + static_cast<int32_t>(time_difference.days);
  BalanceISODate(isolate, &balanced_year, &balanced_month, &balanced_day);

  // 7. If timeSign is -dateSign, then
  if (time_sign == -date_sign) {
    // a. Set balanceResult be ! BalanceISODate(balanceResult.[[Year]],
    // balanceResult.[[Month]], balanceResult.[[Day]] - timeSign).
    balanced_day -= time_sign;
    BalanceISODate(isolate, &balanced_year, &balanced_month, &balanced_day);
    // b. Set timeDifference to ? BalanceDuration(-timeSign,
    // timeDifference.[[Hours]], timeDifference.[[Minutes]],
    // timeDifference.[[Seconds]], timeDifference.[[Milliseconds]],
    // timeDifference.[[Microseconds]], timeDifference.[[Nanoseconds]],
    // largestUnit).
    result.days = -time_sign;
    result.hours = time_difference.hours;
    result.minutes = time_difference.minutes;
    result.seconds = time_difference.seconds;
    result.milliseconds = time_difference.milliseconds;
    result.microseconds = time_difference.microseconds;
    result.nanoseconds = time_difference.nanoseconds;

    Maybe<bool> maybe_time_difference = BalanceDuration(
        isolate, &(result.days), &(result.hours), &(result.minutes),
        &(result.seconds), &(result.milliseconds), &(result.microseconds),
        &(result.nanoseconds), largest_unit, method);
    MAYBE_RETURN(maybe_time_difference, Nothing<DurationRecord>());
    DCHECK(maybe_time_difference.FromJust());
  }
  // 8. Let date1 be ? CreateTemporalDate(balanceResult.[[Year]],
  // balanceResult.[[Month]], balanceResult.[[Day]], calendar).
  Handle<JSTemporalPlainDate> date1;
  ASSIGN_RETURN_ON_EXCEPTION_VALUE(
      isolate, date1,
      CreateTemporalDate(isolate, balanced_year, balanced_month, balanced_day,
                         calendar),
      Nothing<DurationRecord>());
  // 9. Let date2 be ? CreateTemporalDate(y2, mon2, d2, calendar).
  Handle<JSTemporalPlainDate> date2;
  ASSIGN_RETURN_ON_EXCEPTION_VALUE(
      isolate, date2, CreateTemporalDate(isolate, y2, mon2, d2, calendar),
      Nothing<DurationRecord>());
  // 10. Let dateLargestUnit be ! LargerOfTwoTemporalUnits("day", largestUnit).
  Unit date_largest_unit =
      LargerOfTwoTemporalUnits(isolate, Unit::kDay, largest_unit);

  // 11. Let untilOptions be ? MergeLargestUnitOption(options, dateLargestUnit).
  Handle<JSObject> until_options;
  ASSIGN_RETURN_ON_EXCEPTION_VALUE(
      isolate, until_options,
      MergeLargestUnitOption(isolate, options, date_largest_unit),
      Nothing<DurationRecord>());
  // 12. Let dateDifference be ? CalendarDateUntil(calendar, date1, date2,
  // untilOptions).
  Handle<JSTemporalDuration> date_difference;
  ASSIGN_RETURN_ON_EXCEPTION_VALUE(
      isolate, date_difference,
      CalendarDateUntil(isolate, calendar, date1, date2, until_options),
      Nothing<DurationRecord>());
  // 13. Let balanceResult be ? BalanceDuration(dateDifference.[[Days]],
  // timeDifference.[[Hours]], timeDifference.[[Minutes]],
  // timeDifference.[[Seconds]], timeDifference.[[Milliseconds]],
  // timeDifference.[[Microseconds]], timeDifference.[[Nanoseconds]],
  // largestUnit).
  result.days = NumberToInt64(date_difference->days());

  Maybe<bool> maybe_balance_result = BalanceDuration(
      isolate, &(result.days), &(result.hours), &(result.minutes),
      &(result.seconds), &(result.milliseconds), &(result.microseconds),
      &(result.nanoseconds), largest_unit, method);
  MAYBE_RETURN(maybe_balance_result, Nothing<DurationRecord>());
  DCHECK(maybe_balance_result.FromJust());
  // 14. Return the Record { [[Years]]: dateDifference.[[Years]], [[Months]]:
  // dateDifference.[[Months]], [[Weeks]]: dateDifference.[[Weeks]], [[Days]]:
  // balanceResult.[[Days]], [[Hours]]: balanceResult.[[Hours]], [[Minutes]]:
  // balanceResult.[[Minutes]], [[Seconds]]: balanceResult.[[Seconds]],
  // [[Milliseconds]]: balanceResult.[[Milliseconds]], [[Microseconds]]:
  // balanceResult.[[Microseconds]], [[Nanoseconds]]:
  // balanceResult.[[Nanoseconds]] }.
  result.years = NumberToInt64(date_difference->years());
  result.months = NumberToInt64(date_difference->months());
  result.weeks = NumberToInt64(date_difference->weeks());
  return Just(result);
}

// #sec-temporal-addinstant
MaybeHandle<BigInt> AddInstant(Isolate* isolate,
                               Handle<BigInt> epoch_nanoseconds, int64_t hours,
                               int64_t minutes, int64_t seconds,
                               int64_t milliseconds, int64_t microseconds,
                               int64_t nanoseconds) {
  TEMPORAL_ENTER_FUNC();

  // 1. Assert: hours, minutes, seconds, milliseconds, microseconds, and
  // nanoseconds are integer Number values.
  // 2. Let result be epochNanoseconds + ℤ(nanoseconds) +
  // ℤ(microseconds) × 1000ℤ + ℤ(milliseconds) × 10^6ℤ + ℤ(seconds) × 10^9ℤ +
  // ℤ(minutes) × 60ℤ × 10^9ℤ + ℤ(hours) × 3600ℤ × 10^9ℤ.
  Handle<BigInt> result;
  ASSIGN_RETURN_ON_EXCEPTION(
      isolate, result,
      BigInt::Add(isolate, epoch_nanoseconds,
                  BigInt::FromInt64(isolate, nanoseconds)),
      BigInt);
  Handle<BigInt> temp;
  ASSIGN_RETURN_ON_EXCEPTION(
      isolate, temp,
      BigInt::Multiply(isolate, BigInt::FromInt64(isolate, microseconds),
                       BigInt::FromInt64(isolate, 1000)),
      BigInt);
  ASSIGN_RETURN_ON_EXCEPTION(isolate, result,
                             BigInt::Add(isolate, result, temp), BigInt);

  ASSIGN_RETURN_ON_EXCEPTION(
      isolate, temp,
      BigInt::Multiply(isolate, BigInt::FromInt64(isolate, milliseconds),
                       BigInt::FromInt64(isolate, 1000000)),
      BigInt);
  ASSIGN_RETURN_ON_EXCEPTION(isolate, result,
                             BigInt::Add(isolate, result, temp), BigInt);

  ASSIGN_RETURN_ON_EXCEPTION(
      isolate, temp,
      BigInt::Multiply(isolate, BigInt::FromInt64(isolate, seconds),
                       BigInt::FromInt64(isolate, 1000000000)),
      BigInt);
  ASSIGN_RETURN_ON_EXCEPTION(isolate, result,
                             BigInt::Add(isolate, result, temp), BigInt);

  ASSIGN_RETURN_ON_EXCEPTION(
      isolate, temp,
      BigInt::Multiply(isolate, BigInt::FromInt64(isolate, minutes),
                       BigInt::FromInt64(isolate, 1000000000)),
      BigInt);
  ASSIGN_RETURN_ON_EXCEPTION(
      isolate, temp,
      BigInt::Multiply(isolate, temp, BigInt::FromInt64(isolate, 60)), BigInt);
  ASSIGN_RETURN_ON_EXCEPTION(isolate, result,
                             BigInt::Add(isolate, result, temp), BigInt);

  ASSIGN_RETURN_ON_EXCEPTION(
      isolate, temp,
      BigInt::Multiply(isolate, BigInt::FromInt64(isolate, hours),
                       BigInt::FromInt64(isolate, 1000000000)),
      BigInt);
  ASSIGN_RETURN_ON_EXCEPTION(
      isolate, temp,
      BigInt::Multiply(isolate, temp, BigInt::FromInt64(isolate, 3600)),
      BigInt);
  ASSIGN_RETURN_ON_EXCEPTION(isolate, result,
                             BigInt::Add(isolate, result, temp), BigInt);

  // 3. If ! IsValidEpochNanoseconds(result) is false, throw a RangeError
  // exception.
  if (!IsValidEpochNanoseconds(isolate, result)) {
    THROW_NEW_ERROR(isolate, NEW_TEMPORAL_INVALD_ARG_RANGE_ERROR(), BigInt);
  }
  // 4. Return result.
  return result;
}

// #sec-temporal-isvalidepochnanoseconds
bool IsValidEpochNanoseconds(Isolate* isolate,
                             Handle<BigInt> epoch_nanoseconds) {
  TEMPORAL_ENTER_FUNC();

  // 1. Assert: Type(epochNanoseconds) is BigInt.
  // 2. If epochNanoseconds < −86400ℤ × 10^17ℤ or epochNanoseconds > 86400ℤ ×
  // 10^17ℤ, then a. Return false.
  // 3. Return true.
  int64_t ns = epoch_nanoseconds->AsInt64();
  return !(ns < -86400 * 1e17 || ns > 86400 * 1e17);
}

MaybeHandle<BigInt> GetEpochFromISOParts(Isolate* isolate, int32_t year,
                                         int32_t month, int32_t day,
                                         int32_t hour, int32_t minute,
                                         int32_t second, int32_t millisecond,
                                         int32_t microsecond,
                                         int32_t nanosecond) {
  TEMPORAL_ENTER_FUNC();
  // 1. Assert: year, month, day, hour, minute, second, millisecond,
  // microsecond, and nanosecond are integers.
  // 2. Assert: ! IsValidISODate(year, month, day) is true.
  DCHECK(IsValidISODate(isolate, year, month, day));
  // 3. Assert: ! IsValidTime(hour, minute, second, millisecond, microsecond,
  // nanosecond) is true.
  DCHECK(IsValidTime(isolate, hour, minute, second, millisecond, microsecond,
                     nanosecond));
  // 4. Let date be ! MakeDay(𝔽(year), 𝔽(month − 1), 𝔽(day)).
  double date = MakeDay(year, month - 1, day);
  // 5. Let time be ! MakeTime(𝔽(hour), 𝔽(minute), 𝔽(second), 𝔽(millisecond)).
  double time = MakeTime(hour, minute, second, millisecond);
  // 6. Let ms be ! MakeDate(date, time).
  double ms = MakeDate(date, time);
  // 7. Assert: ms is finite.
  // 8. Return ℝ(ms) × 10^6 + microsecond × 10^3 + nanosecond.
  Handle<BigInt> result;
  ASSIGN_RETURN_ON_EXCEPTION(
      isolate, result,
      BigInt::FromNumber(isolate, isolate->factory()->NewNumber(ms)), BigInt);

  ASSIGN_RETURN_ON_EXCEPTION(
      isolate, result,
      BigInt::Multiply(isolate, result, BigInt::FromInt64(isolate, 1000000)),
      BigInt);

  Handle<BigInt> temp;
  ASSIGN_RETURN_ON_EXCEPTION(
      isolate, temp,
      BigInt::Multiply(isolate, BigInt::FromInt64(isolate, microsecond),
                       BigInt::FromInt64(isolate, 1000)),
      BigInt);

  ASSIGN_RETURN_ON_EXCEPTION(isolate, result,
                             BigInt::Add(isolate, result, temp), BigInt);
  return BigInt::Add(isolate, result, BigInt::FromInt64(isolate, nanosecond));
}

// #sec-temporal-durationsign
int32_t DurationSign(Isolate* isolaet, const DurationRecord& dur) {
  TEMPORAL_ENTER_FUNC();

  // 1. For each value v of « years, months, weeks, days, hours, minutes,
  // seconds, milliseconds, microseconds, nanoseconds », do a. If v < 0, return
  // −1. b. If v > 0, return 1.
  // 2. Return 0.
  if (dur.years < 0) return -1;
  if (dur.years > 0) return 1;
  if (dur.months < 0) return -1;
  if (dur.months > 0) return 1;
  if (dur.weeks < 0) return -1;
  if (dur.weeks > 0) return 1;
  if (dur.days < 0) return -1;
  if (dur.days > 0) return 1;
  if (dur.hours < 0) return -1;
  if (dur.hours > 0) return 1;
  if (dur.minutes < 0) return -1;
  if (dur.minutes > 0) return 1;
  if (dur.seconds < 0) return -1;
  if (dur.seconds > 0) return 1;
  if (dur.milliseconds < 0) return -1;
  if (dur.milliseconds > 0) return 1;
  if (dur.microseconds < 0) return -1;
  if (dur.microseconds > 0) return 1;
  if (dur.nanoseconds < 0) return -1;
  if (dur.nanoseconds > 0) return 1;
  return 0;
}

// #sec-temporal-isvalidduration
bool IsValidDuration(Isolate* isolate, const DurationRecord& dur) {
  TEMPORAL_ENTER_FUNC();

  // 1. Let sign be ! DurationSign(years, months, weeks, days, hours, minutes,
  // seconds, milliseconds, microseconds, nanoseconds).
  int32_t sign = DurationSign(isolate, dur);
  // 2. For each value v of « years, months, weeks, days, hours, minutes,
  // seconds, milliseconds, microseconds, nanoseconds », do a. If v is not
  // finite, return false. b. If v < 0 and sign > 0, return false. c. If v > 0
  // and sign < 0, return false.
  // 3. Return true.
  return !((sign > 0 && (dur.years < 0 || dur.months < 0 || dur.weeks < 0 ||
                         dur.days < 0 || dur.hours < 0 || dur.minutes < 0 ||
                         dur.seconds < 0 || dur.milliseconds < 0 ||
                         dur.microseconds < 0 || dur.nanoseconds < 0)) ||
           (sign < 0 && (dur.years > 0 || dur.months > 0 || dur.weeks > 0 ||
                         dur.days > 0 || dur.hours > 0 || dur.minutes > 0 ||
                         dur.seconds > 0 || dur.milliseconds > 0 ||
                         dur.microseconds > 0 || dur.nanoseconds > 0)));
}

// #sec-temporal-isisoleapyear
bool IsISOLeapYear(Isolate* isolate, int32_t year) {
  TEMPORAL_ENTER_FUNC();

  // 1. Assert: year is an integer.
  // 2. If year modulo 4 ≠ 0, return false.
  // 3. If year modulo 400 = 0, return true.
  // 4. If year modulo 100 = 0, return false.
  // 5. Return true.
  return isolate->date_cache()->IsLeap(year);
}

// #sec-temporal-isodaysinmonth
int32_t ISODaysInMonth(Isolate* isolate, int32_t year, int32_t month) {
  TEMPORAL_ENTER_FUNC();

  // 1. Assert: year is an integer.
  // 2. Assert: month is an integer, month ≥ 1, and month ≤ 12.
  DCHECK_GE(month, 1);
  DCHECK_LE(month, 12);
  // 3. If month is 1, 3, 5, 7, 8, 10, or 12, return 31.
  if (month % 2 == ((month < 8) ? 1 : 0)) return 31;
  // 4. If month is 4, 6, 9, or 11, return 30.
  DCHECK(month == 2 || month == 4 || month == 6 || month == 9 || month == 11);
  if (month != 2) return 30;
  // 5. If ! IsISOLeapYear(year) is true, return 29.
  return IsISOLeapYear(isolate, year) ? 29 : 28;
  // 6. Return 28.
}

// #sec-temporal-isodaysinyear
int32_t ISODaysInYear(Isolate* isolate, int32_t year) {
  TEMPORAL_ENTER_FUNC();

  // 1. Assert: year is an integer.
  // 2. If ! IsISOLeapYear(year) is true, then
  // a. Return 366.
  // 3. Return 365.
  return IsISOLeapYear(isolate, year) ? 366 : 365;
}

bool IsValidTime(Isolate* isolate, int32_t hour, int32_t minute, int32_t second,
                 int32_t millisecond, int32_t microsecond, int32_t nanosecond) {
  TEMPORAL_ENTER_FUNC();

  // 2. If hour < 0 or hour > 23, then
  // a. Return false.
  if (hour < 0 || hour > 23) return false;
  // 3. If minute < 0 or minute > 59, then
  // a. Return false.
  if (minute < 0 || minute > 59) return false;
  // 4. If second < 0 or second > 59, then
  // a. Return false.
  if (second < 0 || second > 59) return false;
  // 5. If millisecond < 0 or millisecond > 999, then
  // a. Return false.
  if (millisecond < 0 || millisecond > 999) return false;
  // 6. If microsecond < 0 or microsecond > 999, then
  // a. Return false.
  if (microsecond < 0 || microsecond > 999) return false;
  // 7. If nanosecond < 0 or nanosecond > 999, then
  // a. Return false.
  if (nanosecond < 0 || nanosecond > 999) return false;
  // 8. Return true.
  return true;
}

// #sec-temporal-isvalidisodate
bool IsValidISODate(Isolate* isolate, int32_t year, int32_t month,
                    int32_t day) {
  TEMPORAL_ENTER_FUNC();

  // 1. Assert: year, month, and day are integers.
  // 2. If month < 1 or month > 12, then
  // a. Return false.
  if (month < 1 || month > 12) return false;
  // 3. Let daysInMonth be ! ISODaysInMonth(year, month).
  // 4. If day < 1 or day > daysInMonth, then
  // a. Return false.
  if (day < 1 || day > ISODaysInMonth(isolate, year, month)) return false;
  // 5. Return true.
  return true;
}

// #sec-temporal-compareisodate
int32_t CompareISODate(Isolate* isolate, int32_t y1, int32_t m1, int32_t d1,
                       int32_t y2, int32_t m2, int32_t d2) {
  TEMPORAL_ENTER_FUNC();

  // 1. Assert: y1, m1, d1, y2, m2, and d2 are integers.
  // 2. If y1 > y2, return 1.
  if (y1 > y2) return 1;
  // 3. If y1 < y2, return -1.
  if (y1 < y2) return -1;
  // 4. If m1 > m2, return 1.
  if (m1 > m2) return 1;
  // 5. If m1 < m2, return -1.
  if (m1 < m2) return -1;
  // 6. If d1 > d2, return 1.
  if (d1 > d2) return 1;
  // 7. If d1 < d2, return -1.
  if (d1 < d2) return -1;
  // 8. Return 0.
  return 0;
}

// #sec-temporal-balanceisoyearmonth
void BalanceISOYearMonth(Isolate* isolate, int32_t* year, int32_t* month) {
  TEMPORAL_ENTER_FUNC();

  // 1. Assert: year and month are integers.
  // 2. Set year to year + floor((month - 1) / 12).
  *year += floor_divide((*month - 1), 12);
  // 3. Set month to (month − 1) modulo 12 + 1.
  *month = static_cast<int32_t>(modulo(*month - 1, 12)) + 1;

  // 4. Return the new Record { [[Year]]: year, [[Month]]: month }.
}
// #sec-temporal-balancetime
DateTimeRecordCommon BalanceTime(Isolate* isolate, int64_t hour, int64_t minute,
                                 int64_t second, int64_t millisecond,
                                 int64_t microsecond, int64_t nanosecond) {
  TEMPORAL_ENTER_FUNC();

  // 1. Assert: hour, minute, second, millisecond, microsecond, and nanosecond
  // are integers.
  // 2. Set microsecond to microsecond + floor(nanosecond / 1000).
  microsecond += floor_divide(nanosecond, 1000L);
  // 3. Set nanosecond to nanosecond modulo 1000.
  nanosecond = modulo(nanosecond, 1000L);
  // 4. Set millisecond to millisecond + floor(microsecond / 1000).
  millisecond += floor_divide(microsecond, 1000L);
  // 5. Set microsecond to microsecond modulo 1000.
  microsecond = modulo(microsecond, 1000L);
  // 6. Set second to second + floor(millisecond / 1000).
  second += floor_divide(millisecond, 1000L);
  // 7. Set millisecond to millisecond modulo 1000.
  millisecond = modulo(millisecond, 1000L);
  // 8. Set minute to minute + floor(second / 60).
  minute += floor_divide(second, 60L);
  // 9. Set second to second modulo 60.
  second = modulo(second, 60L);
  // 10. Set hour to hour + floor(minute / 60).
  hour += floor_divide(minute, 60L);
  // 11. Set minute to minute modulo 60.
  minute = modulo(minute, 60L);
  // 12. Let days be floor(hour / 24).
  int64_t days = floor_divide(hour, 24L);
  // 13. Set hour to hour modulo 24.
  hour = modulo(hour, 24L);
  // 14. Return the new Record { [[Days]]: days, [[Hour]]: hour, [[Minute]]:
  // minute, [[Second]]: second, [[Millisecond]]: millisecond, [[Microsecond]]:
  // microsecond, [[Nanosecond]]: nanosecond }.
  return {0,
          0,
          static_cast<int32_t>(days),
          static_cast<int32_t>(hour),
          static_cast<int32_t>(minute),
          static_cast<int32_t>(second),
          static_cast<int32_t>(millisecond),
          static_cast<int32_t>(microsecond),
          static_cast<int32_t>(nanosecond)};
}

// #sec-temporal-differencetime
DurationRecord DifferenceTime(Isolate* isolate, int32_t h1, int32_t min1,
                              int32_t s1, int32_t ms1, int32_t mus1,
                              int32_t ns1, int32_t h2, int32_t min2, int32_t s2,
                              int32_t ms2, int32_t mus2, int32_t ns2) {
  TEMPORAL_ENTER_FUNC();

  // 1. Assert: h1, min1, s1, ms1, mus1, ns1, h2, min2, s2, ms2, mus2, and ns2
  // are integers.
  DurationRecord dur;
  // 2. Let hours be h2 − h1.
  dur.hours = h2 - h1;
  // 3. Let minutes be min2 − min1.
  dur.minutes = min2 - min1;
  // 4. Let seconds be s2 − s1.
  dur.seconds = s2 - s1;
  // 5. Let milliseconds be ms2 − ms1.
  dur.milliseconds = ms2 - ms1;
  // 6. Let microseconds be mus2 − mus1.
  dur.microseconds = mus2 - mus1;
  // 7. Let nanoseconds be ns2 − ns1.
  dur.nanoseconds = ns2 - ns1;
  // 8. Let sign be ! DurationSign(0, 0, 0, 0, hours, minutes, seconds,
  // milliseconds, microseconds, nanoseconds).
  double sign = DurationSign(isolate, dur);

  // See https://github.com/tc39/proposal-temporal/pull/1885
  // 9. Let bt be ! BalanceTime(hours × sign, minutes × sign, seconds × sign,
  // milliseconds × sign, microseconds × sign, nanoseconds × sign).
  DateTimeRecordCommon bt = BalanceTime(
      isolate, dur.hours * sign, dur.minutes * sign, dur.seconds * sign,
      dur.milliseconds * sign, dur.microseconds * sign, dur.nanoseconds * sign);

  // 10. Return the new Record { [[Days]]: bt.[[Days]] × sign, [[Hours]]:
  // bt.[[Hour]] × sign, [[Minutes]]: bt.[[Minute]] × sign, [[Seconds]]:
  // bt.[[Second]] × sign, [[Milliseconds]]: bt.[[Millisecond]] × sign,
  // [[Microseconds]]: bt.[[Microsecond]] × sign, [[Nanoseconds]]:
  // bt.[[Nanosecond]] × sign }.
  return {0,
          0,
          0,
          static_cast<int64_t>(bt.day * sign),
          static_cast<int64_t>(bt.hour * sign),
          static_cast<int64_t>(bt.minute * sign),
          static_cast<int64_t>(bt.second * sign),
          static_cast<int64_t>(bt.millisecond * sign),
          static_cast<int64_t>(bt.microsecond * sign),
          static_cast<int64_t>(bt.nanosecond * sign)};
}

// #sec-temporal-addtime
DateTimeRecordCommon AddTime(Isolate* isolate, int64_t hour, int64_t minute,
                             int64_t second, int64_t millisecond,
                             int64_t microsecond, int64_t nanosecond,
                             int64_t hours, int64_t minutes, int64_t seconds,
                             int64_t milliseconds, int64_t microseconds,
                             int64_t nanoseconds) {
  TEMPORAL_ENTER_FUNC();

  // 1. Assert: hour, minute, second, millisecond, microsecond, nanosecond,
  // hours, minutes, seconds, milliseconds, microseconds, and nanoseconds are
  // integers.
  // 2. Let hour be hour + hours.
  return BalanceTime(isolate, hour + hours,
                     // 3. Let minute be minute + minutes.
                     minute + minutes,
                     // 4. Let second be second + seconds.
                     second + seconds,
                     // 5. Let millisecond be millisecond + milliseconds.
                     millisecond + milliseconds,
                     // 6. Let microsecond be microsecond + microseconds.
                     microsecond + microseconds,
                     // 7. Let nanosecond be nanosecond + nanoseconds.
                     nanosecond + nanoseconds);
  // 8. Return ! BalanceTime(hour, minute, second, millisecond, microsecond,
  // nanosecond).
}

// #sec-temporal-totaldurationnanoseconds
int64_t TotalDurationNanoseconds(Isolate* isolate, int64_t days, int64_t hours,
                                 int64_t minutes, int64_t seconds,
                                 int64_t milliseconds, int64_t microseconds,
                                 int64_t nanoseconds, int64_t offset_shift) {
  TEMPORAL_ENTER_FUNC();

  // 1. Assert: offsetShift is an integer.
  // 2. Set nanoseconds to ℝ(nanoseconds).
  // 3. If days ≠ 0, then
  if (days != 0) {
    // a. Set nanoseconds to nanoseconds − offsetShift.
    nanoseconds -= offset_shift;
  }

  // 4. Set hours to ℝ(hours) + ℝ(days) × 24.
  hours += days * 24;

  // 5. Set minutes to ℝ(minutes) + hours × 60.
  minutes += hours * 60;

  // 6. Set seconds to ℝ(seconds) + minutes × 60.
  seconds += minutes * 60;

  // 7. Set milliseconds to ℝ(milliseconds) + seconds × 1000.
  milliseconds += seconds * 1000;

  // 8. Set microseconds to ℝ(microseconds) + milliseconds × 1000.
  microseconds += milliseconds * 1000;

  // 9. Return nanoseconds + microseconds × 1000.
  return nanoseconds + microseconds * 1000;
}

}  // namespace

// #sec-temporal.duration
MaybeHandle<JSTemporalDuration> JSTemporalDuration::Constructor(
    Isolate* isolate, Handle<JSFunction> target, Handle<HeapObject> new_target,
    Handle<Object> years, Handle<Object> months, Handle<Object> weeks,
    Handle<Object> days, Handle<Object> hours, Handle<Object> minutes,
    Handle<Object> seconds, Handle<Object> milliseconds,
    Handle<Object> microseconds, Handle<Object> nanoseconds) {
  const char* method = "Temporal.Duration";
  // 1. If NewTarget is undefined, then
  if (new_target->IsUndefined()) {
    // a. Throw a TypeError exception.
    THROW_NEW_ERROR(
        isolate,
        NewTypeError(MessageTemplate::kMethodInvokedOnWrongType,
                     isolate->factory()->NewStringFromAsciiChecked(method)),
        JSTemporalDuration);
  }
  // 2. Let y be ? ToIntegerThrowOnInfinity(years).
  Handle<Object> number_years;
  ASSIGN_RETURN_ON_EXCEPTION(isolate, number_years,
                             ToIntegerThrowOnInfinity(isolate, years),
                             JSTemporalDuration);
  int64_t y = NumberToInt64(*number_years);

  // 3. Let mo be ? ToIntegerThrowOnInfinity(months).
  Handle<Object> number_months;
  ASSIGN_RETURN_ON_EXCEPTION(isolate, number_months,
                             ToIntegerThrowOnInfinity(isolate, months),
                             JSTemporalDuration);
  int64_t mo = NumberToInt64(*number_months);

  // 4. Let w be ? ToIntegerThrowOnInfinity(weeks).
  Handle<Object> number_weeks;
  ASSIGN_RETURN_ON_EXCEPTION(isolate, number_weeks,
                             ToIntegerThrowOnInfinity(isolate, weeks),
                             JSTemporalDuration);
  int64_t w = NumberToInt64(*number_weeks);

  // 5. Let d be ? ToIntegerThrowOnInfinity(days).
  Handle<Object> number_days;
  ASSIGN_RETURN_ON_EXCEPTION(isolate, number_days,
                             ToIntegerThrowOnInfinity(isolate, days),
                             JSTemporalDuration);
  int64_t d = NumberToInt64(*number_days);

  // 6. Let h be ? ToIntegerThrowOnInfinity(hours).
  Handle<Object> number_hours;
  ASSIGN_RETURN_ON_EXCEPTION(isolate, number_hours,
                             ToIntegerThrowOnInfinity(isolate, hours),
                             JSTemporalDuration);
  int64_t h = NumberToInt64(*number_hours);

  // 7. Let m be ? ToIntegerThrowOnInfinity(minutes).
  Handle<Object> number_minutes;
  ASSIGN_RETURN_ON_EXCEPTION(isolate, number_minutes,
                             ToIntegerThrowOnInfinity(isolate, minutes),
                             JSTemporalDuration);
  int64_t m = NumberToInt64(*number_minutes);

  // 8. Let s be ? ToIntegerThrowOnInfinity(seconds).
  Handle<Object> number_seconds;
  ASSIGN_RETURN_ON_EXCEPTION(isolate, number_seconds,
                             ToIntegerThrowOnInfinity(isolate, seconds),
                             JSTemporalDuration);
  int64_t s = NumberToInt64(*number_seconds);

  // 9. Let ms be ? ToIntegerThrowOnInfinity(milliseconds).
  Handle<Object> number_milliseconds;
  ASSIGN_RETURN_ON_EXCEPTION(isolate, number_milliseconds,
                             ToIntegerThrowOnInfinity(isolate, milliseconds),
                             JSTemporalDuration);
  int64_t ms = NumberToInt64(*number_milliseconds);

  // 10. Let mis be ? ToIntegerThrowOnInfinity(microseconds).
  Handle<Object> number_microseconds;
  ASSIGN_RETURN_ON_EXCEPTION(isolate, number_microseconds,
                             ToIntegerThrowOnInfinity(isolate, microseconds),
                             JSTemporalDuration);
  int64_t mis = NumberToInt64(*number_microseconds);

  // 11. Let ns be ? ToIntegerThrowOnInfinity(nanoseconds).
  Handle<Object> number_nanoseconds;
  ASSIGN_RETURN_ON_EXCEPTION(isolate, number_nanoseconds,
                             ToIntegerThrowOnInfinity(isolate, nanoseconds),
                             JSTemporalDuration);
  int64_t ns = NumberToInt64(*number_nanoseconds);

  // 12. Return ? CreateTemporalDuration(y, mo, w, d, h, m, s, ms, mis, ns,
  // NewTarget).
  return CreateTemporalDuration(isolate, target, new_target, y, mo, w, d, h, m,
                                s, ms, mis, ns);
}

// #sec-get-temporal.duration.prototype.sign
MaybeHandle<Smi> JSTemporalDuration::Sign(Isolate* isolate,
                                          Handle<JSTemporalDuration> duration) {
  // 1. Let duration be the this value.
  // 2. Perform ? RequireInternalSlot(duration,
  // [[InitializedTemporalDuration]]).
  // 3. Return ! DurationSign(duration.[[Years]], duration.[[Months]],
  // duration.[[Weeks]], duration.[[Days]], duration.[[Hours]],
  // duration.[[Minutes]], duration.[[Seconds]], duration.[[Milliseconds]],
  // duration.[[Microseconds]], duration.[[Nanoseconds]]).
  return Handle<Smi>(
      Smi::FromInt(DurationSign(
          isolate,
          {NumberToInt64(duration->years()), NumberToInt64(duration->months()),
           NumberToInt64(duration->weeks()), NumberToInt64(duration->days()),
           NumberToInt64(duration->hours()), NumberToInt64(duration->minutes()),
           NumberToInt64(duration->seconds()),
           NumberToInt64(duration->milliseconds()),
           NumberToInt64(duration->microseconds()),
           NumberToInt64(duration->nanoseconds())})),
      isolate);
}

// #sec-get-temporal.duration.prototype.blank
MaybeHandle<Oddball> JSTemporalDuration::Blank(
    Isolate* isolate, Handle<JSTemporalDuration> duration) {
  // 1. Let duration be the this value.
  // 2. Perform ? RequireInternalSlot(duration,
  // [[InitializedTemporalDuration]]).
  // 3. Let sign be ! DurationSign(duration.[[Years]], duration.[[Months]],
  // duration.[[Weeks]], duration.[[Days]], duration.[[Hours]],
  // duration.[[Minutes]], duration.[[Seconds]], duration.[[Milliseconds]],
  // duration.[[Microseconds]], duration.[[Nanoseconds]]).
  // 4. If sign = 0, return true.
  // 5. Return false.
  int32_t sign = DurationSign(
      isolate,
      {NumberToInt64(duration->years()), NumberToInt64(duration->months()),
       NumberToInt64(duration->weeks()), NumberToInt64(duration->days()),
       NumberToInt64(duration->hours()), NumberToInt64(duration->minutes()),
       NumberToInt64(duration->seconds()),
       NumberToInt64(duration->milliseconds()),
       NumberToInt64(duration->microseconds()),
       NumberToInt64(duration->nanoseconds())});
  return sign == 0 ? isolate->factory()->true_value()
                   : isolate->factory()->false_value();
}

// #sec-temporal.calendar
MaybeHandle<JSTemporalCalendar> JSTemporalCalendar::Constructor(
    Isolate* isolate, Handle<JSFunction> target, Handle<HeapObject> new_target,
    Handle<Object> identifier_obj) {
  // 1. If NewTarget is undefined, then
  if (new_target->IsUndefined(isolate)) {
    // a. Throw a TypeError exception.
    THROW_NEW_ERROR(isolate,
                    NewTypeError(MessageTemplate::kConstructorNotFunction,
                                 isolate->factory()->NewStringFromStaticChars(
                                     "Temporal.Calendar")),
                    JSTemporalCalendar);
  }
  // 2. Set identifier to ? ToString(identifier).
  Handle<String> identifier;
  ASSIGN_RETURN_ON_EXCEPTION(isolate, identifier,
                             Object::ToString(isolate, identifier_obj),
                             JSTemporalCalendar);
  // 3. If ! IsBuiltinCalendar(id) is false, then
  if (!IsBuiltinCalendar(isolate, identifier)) {
    // a. Throw a RangeError exception.
    THROW_NEW_ERROR(
        isolate, NewRangeError(MessageTemplate::kInvalidCalendar, identifier),
        JSTemporalCalendar);
  }
  return CreateTemporalCalendar(isolate, target, new_target, identifier);
}

// #sec-temporal.calendar.prototype.tostring
MaybeHandle<String> JSTemporalCalendar::ToString(
    Isolate* isolate, Handle<JSTemporalCalendar> calendar, const char* method) {
  return CalendarIdentifier(isolate, calendar->calendar_index());
}

// #sec-temporal.timezone
MaybeHandle<JSTemporalTimeZone> JSTemporalTimeZone::Constructor(
    Isolate* isolate, Handle<JSFunction> target, Handle<HeapObject> new_target,
    Handle<Object> identifier_obj) {
  // 1. If NewTarget is undefined, then
  if (new_target->IsUndefined(isolate)) {
    // a. Throw a TypeError exception.
    THROW_NEW_ERROR(isolate,
                    NewTypeError(MessageTemplate::kConstructorNotFunction,
                                 isolate->factory()->NewStringFromAsciiChecked(
                                     "Temporal.TimeZone")),
                    JSTemporalTimeZone);
  }
  // 2. Set identifier to ? ToString(identifier).
  Handle<String> identifier;
  ASSIGN_RETURN_ON_EXCEPTION(isolate, identifier,
                             Object::ToString(isolate, identifier_obj),
                             JSTemporalTimeZone);
  Handle<String> canonical;
  // 3. If identifier satisfies the syntax of a TimeZoneNumericUTCOffset
  // (see 13.33), then
  Maybe<bool> maybe_valid =
      IsValidTimeZoneNumericUTCOffsetString(isolate, identifier);
  MAYBE_RETURN(maybe_valid, Handle<JSTemporalTimeZone>());

  if (maybe_valid.FromJust()) {
    // a. Let offsetNanoseconds be ? ParseTimeZoneOffsetString(identifier).
    Maybe<int64_t> maybe_offset_nanoseconds =
        ParseTimeZoneOffsetString(isolate, identifier);
    MAYBE_RETURN(maybe_offset_nanoseconds, Handle<JSTemporalTimeZone>());
    int64_t offset_nanoseconds = maybe_offset_nanoseconds.FromJust();

    // b. Let canonical be ! FormatTimeZoneOffsetString(offsetNanoseconds).
    ASSIGN_RETURN_ON_EXCEPTION(
        isolate, canonical,
        FormatTimeZoneOffsetString(isolate, offset_nanoseconds),
        JSTemporalTimeZone);
  } else {
    // 4. Else,
    // a. If ! IsValidTimeZoneName(identifier) is false, then
    if (!IsValidTimeZoneName(isolate, identifier)) {
      // i. Throw a RangeError exception.
      THROW_NEW_ERROR(
          isolate, NewRangeError(MessageTemplate::kInvalidTimeZone, identifier),
          JSTemporalTimeZone);
    }
    // b. Let canonical be ! CanonicalizeTimeZoneName(identifier).
    ASSIGN_RETURN_ON_EXCEPTION(isolate, canonical,
                               CanonicalizeTimeZoneName(isolate, identifier),
                               JSTemporalTimeZone);
  }
  // 5. Return ? CreateTemporalTimeZone(canonical, NewTarget).
  return CreateTemporalTimeZone(isolate, target, new_target, canonical);
}

// #sec-temporal.timezone.prototype.tostring
MaybeHandle<Object> JSTemporalTimeZone::ToString(
    Isolate* isolate, Handle<JSTemporalTimeZone> time_zone,
    const char* method) {
  return time_zone->id(isolate);
}

int32_t JSTemporalTimeZone::time_zone_index() const {
  DCHECK(is_offset() == false);
  return offset_milliseconds_or_time_zone_index();
}

int64_t JSTemporalTimeZone::offset_nanoseconds() const {
  TEMPORAL_ENTER_FUNC();
  DCHECK(is_offset());
  return 1000000L * offset_milliseconds() + offset_sub_milliseconds();
}

void JSTemporalTimeZone::set_offset_nanoseconds(int64_t ns) {
  this->set_offset_milliseconds(static_cast<int32_t>(ns / 1000000L));
  this->set_offset_sub_milliseconds(static_cast<int32_t>(ns % 1000000L));
}

MaybeHandle<String> JSTemporalTimeZone::id(Isolate* isolate) const {
  if (is_offset()) {
    return FormatTimeZoneOffsetString(isolate, offset_nanoseconds());
  }
#ifdef V8_INTL_SUPPORT
  std::string id =
      Intl::TimeZoneIdFromIndex(offset_milliseconds_or_time_zone_index());
  return isolate->factory()->NewStringFromAsciiChecked(id.c_str());
#else   // V8_INTL_SUPPORT
  DCHECK_EQ(0, offset_milliseconds_or_time_zone_index());
  return isolate->factory()->UTC_string();
#endif  // V8_INTL_SUPPORT
}

MaybeHandle<JSTemporalPlainDate> JSTemporalPlainDate::Constructor(
    Isolate* isolate, Handle<JSFunction> target, Handle<HeapObject> new_target,
    Handle<Object> iso_year_obj, Handle<Object> iso_month_obj,
    Handle<Object> iso_day_obj, Handle<Object> calendar_like) {
  const char* method = "Temporal.PlainDate";
  // 1. If NewTarget is undefined, throw a TypeError exception.
  if (new_target->IsUndefined()) {
    THROW_NEW_ERROR(
        isolate,
        NewTypeError(MessageTemplate::kMethodInvokedOnWrongType,
                     isolate->factory()->NewStringFromAsciiChecked(method)),
        JSTemporalPlainDate);
  }
#define TO_INT_THROW_ON_INFTY(name, T)                                        \
  int32_t name;                                                               \
  {                                                                           \
    Handle<Object> number_##name;                                             \
    /* x. Let name be ? ToIntegerThrowOnInfinity(name). */                    \
    ASSIGN_RETURN_ON_EXCEPTION(isolate, number_##name,                        \
                               ToIntegerThrowOnInfinity(isolate, name##_obj), \
                               T);                                            \
    name = NumberToInt32(*number_##name);                                     \
  }

  TO_INT_THROW_ON_INFTY(iso_year, JSTemporalPlainDate);
  TO_INT_THROW_ON_INFTY(iso_month, JSTemporalPlainDate);
  TO_INT_THROW_ON_INFTY(iso_day, JSTemporalPlainDate);

  // 8. Let calendar be ? ToTemporalCalendarWithISODefault(calendarLike).
  Handle<JSReceiver> calendar;
  ASSIGN_RETURN_ON_EXCEPTION(
      isolate, calendar,
      ToTemporalCalendarWithISODefault(isolate, calendar_like, method),
      JSTemporalPlainDate);

  // 9. Return ? CreateTemporalDate(y, m, d, calendar, NewTarget).
  return CreateTemporalDate(isolate, target, new_target, iso_year, iso_month,
                            iso_day, calendar);
}

#define DEFINE_INT_FIELD(obj, str, field, item)                \
  CHECK(JSReceiver::CreateDataProperty(                        \
            isolate, obj, factory->str##_string(),             \
            Handle<Smi>(Smi::FromInt(item->field()), isolate), \
            Just(kThrowOnError))                               \
            .FromJust());

// #sec-temporal.plaindate.prototype.getisofields
MaybeHandle<JSReceiver> JSTemporalPlainDate::GetISOFields(
    Isolate* isolate, Handle<JSTemporalPlainDate> temporal_date) {
  Factory* factory = isolate->factory();
  // 1. Let temporalDate be the this value.
  // 2. Perform ? RequireInternalSlot(temporalDate,
  // [[InitializedTemporalDate]]).
  // 3. Let fields be ! OrdinaryObjectCreate(%Object.prototype%).
  Handle<JSObject> fields =
      isolate->factory()->NewJSObject(isolate->object_function());
  // 4. Perform ! CreateDataPropertyOrThrow(fields, "calendar",
  // temporalDate.[[Calendar]]).
  CHECK(JSReceiver::CreateDataProperty(
            isolate, fields, factory->calendar_string(),
            Handle<JSReceiver>(temporal_date->calendar(), isolate),
            Just(kThrowOnError))
            .FromJust());
  // 5. Perform ! CreateDataPropertyOrThrow(fields, "isoDay",
  // 𝔽(temporalDate.[[ISODay]])).
  // 6. Perform ! CreateDataPropertyOrThrow(fields, "isoMonth",
  // 𝔽(temporalDate.[[ISOMonth]])).
  // 7. Perform ! CreateDataPropertyOrThrow(fields, "isoYear",
  // 𝔽(temporalDate.[[ISOYear]])).
  DEFINE_INT_FIELD(fields, isoDay, iso_day, temporal_date)
  DEFINE_INT_FIELD(fields, isoMonth, iso_month, temporal_date)
  DEFINE_INT_FIELD(fields, isoYear, iso_year, temporal_date)
  // 8. Return fields.
  return fields;
}

// #sec-temporal-createtemporaldatetime
MaybeHandle<JSTemporalPlainDateTime> JSTemporalPlainDateTime::Constructor(
    Isolate* isolate, Handle<JSFunction> target, Handle<HeapObject> new_target,
    Handle<Object> iso_year_obj, Handle<Object> iso_month_obj,
    Handle<Object> iso_day_obj, Handle<Object> hour_obj,
    Handle<Object> minute_obj, Handle<Object> second_obj,
    Handle<Object> millisecond_obj, Handle<Object> microsecond_obj,
    Handle<Object> nanosecond_obj, Handle<Object> calendar_like) {
  const char* method = "Temporal.PlainDateTime";
  // 1. If NewTarget is undefined, throw a TypeError exception.
  if (new_target->IsUndefined()) {
    THROW_NEW_ERROR(
        isolate,
        NewTypeError(MessageTemplate::kMethodInvokedOnWrongType,
                     isolate->factory()->NewStringFromAsciiChecked(method)),
        JSTemporalPlainDateTime);
  }

  TO_INT_THROW_ON_INFTY(iso_year, JSTemporalPlainDateTime);
  TO_INT_THROW_ON_INFTY(iso_month, JSTemporalPlainDateTime);
  TO_INT_THROW_ON_INFTY(iso_day, JSTemporalPlainDateTime);
  TO_INT_THROW_ON_INFTY(hour, JSTemporalPlainDateTime);
  TO_INT_THROW_ON_INFTY(minute, JSTemporalPlainDateTime);
  TO_INT_THROW_ON_INFTY(second, JSTemporalPlainDateTime);
  TO_INT_THROW_ON_INFTY(millisecond, JSTemporalPlainDateTime);
  TO_INT_THROW_ON_INFTY(microsecond, JSTemporalPlainDateTime);
  TO_INT_THROW_ON_INFTY(nanosecond, JSTemporalPlainDateTime);

  // 20. Let calendar be ? ToTemporalCalendarWithISODefault(calendarLike).
  Handle<JSReceiver> calendar;
  ASSIGN_RETURN_ON_EXCEPTION(
      isolate, calendar,
      ToTemporalCalendarWithISODefault(isolate, calendar_like, method),
      JSTemporalPlainDateTime);

  // 21. Return ? CreateTemporalDateTime(isoYear, isoMonth, isoDay, hour,
  // minute, second, millisecond, microsecond, nanosecond, calendar, NewTarget).
  return CreateTemporalDateTime(isolate, target, new_target, iso_year,
                                iso_month, iso_day, hour, minute, second,
                                millisecond, microsecond, nanosecond, calendar);
}

// #sec-temporal.plaindatetime.prototype.getisofields
MaybeHandle<JSReceiver> JSTemporalPlainDateTime::GetISOFields(
    Isolate* isolate, Handle<JSTemporalPlainDateTime> date_time) {
  Factory* factory = isolate->factory();
  // 1. Let dateTime be the this value.
  // 2. Perform ? RequireInternalSlot(temporalDateTime,
  // [[InitializedTemporalDateTime]]).
  // 3. Let fields be ! OrdinaryObjectCreate(%Object.prototype%).
  Handle<JSObject> fields =
      isolate->factory()->NewJSObject(isolate->object_function());
  // 4. Perform ! CreateDataPropertyOrThrow(fields, "calendar",
  // temporalTime.[[Calendar]]).
  CHECK(JSReceiver::CreateDataProperty(
            isolate, fields, factory->calendar_string(),
            Handle<JSReceiver>(date_time->calendar(), isolate),
            Just(kThrowOnError))
            .FromJust());
  // 5. Perform ! CreateDataPropertyOrThrow(fields, "isoDay",
  // 𝔽(dateTime.[[ISODay]])).
  // 6. Perform ! CreateDataPropertyOrThrow(fields, "isoHour",
  // 𝔽(temporalTime.[[ISOHour]])).
  // 7. Perform ! CreateDataPropertyOrThrow(fields, "isoMicrosecond",
  // 𝔽(temporalTime.[[ISOMicrosecond]])).
  // 8. Perform ! CreateDataPropertyOrThrow(fields, "isoMillisecond",
  // 𝔽(temporalTime.[[ISOMillisecond]])).
  // 9. Perform ! CreateDataPropertyOrThrow(fields, "isoMinute",
  // 𝔽(temporalTime.[[ISOMinute]])).
  // 10. Perform ! CreateDataPropertyOrThrow(fields, "isoMonth",
  // 𝔽(temporalTime.[[ISOMonth]])).
  // 11. Perform ! CreateDataPropertyOrThrow(fields, "isoNanosecond",
  // 𝔽(temporalTime.[[ISONanosecond]])).
  // 12. Perform ! CreateDataPropertyOrThrow(fields, "isoSecond",
  // 𝔽(temporalTime.[[ISOSecond]])).
  // 13. Perform ! CreateDataPropertyOrThrow(fields, "isoYear",
  // 𝔽(temporalTime.[[ISOYear]])).
  DEFINE_INT_FIELD(fields, isoDay, iso_day, date_time)
  DEFINE_INT_FIELD(fields, isoHour, iso_hour, date_time)
  DEFINE_INT_FIELD(fields, isoMicrosecond, iso_microsecond, date_time)
  DEFINE_INT_FIELD(fields, isoMillisecond, iso_millisecond, date_time)
  DEFINE_INT_FIELD(fields, isoMinute, iso_minute, date_time)
  DEFINE_INT_FIELD(fields, isoMonth, iso_month, date_time)
  DEFINE_INT_FIELD(fields, isoNanosecond, iso_nanosecond, date_time)
  DEFINE_INT_FIELD(fields, isoSecond, iso_second, date_time)
  DEFINE_INT_FIELD(fields, isoYear, iso_year, date_time)
  // 14. Return fields.
  return fields;
}

// #sec-temporal.plainmonthday
MaybeHandle<JSTemporalPlainMonthDay> JSTemporalPlainMonthDay::Constructor(
    Isolate* isolate, Handle<JSFunction> target, Handle<HeapObject> new_target,
    Handle<Object> iso_month_obj, Handle<Object> iso_day_obj,
    Handle<Object> calendar_like, Handle<Object> reference_iso_year_obj) {
  const char* method = "Temporal.PlainMonthDay";
  // 1. If NewTarget is undefined, throw a TypeError exception.
  if (new_target->IsUndefined()) {
    THROW_NEW_ERROR(
        isolate,
        NewTypeError(MessageTemplate::kMethodInvokedOnWrongType,
                     isolate->factory()->NewStringFromAsciiChecked(method)),
        JSTemporalPlainMonthDay);
  }

  // 3. Let m be ? ToIntegerThrowOnInfinity(isoMonth).
  TO_INT_THROW_ON_INFTY(iso_month, JSTemporalPlainMonthDay);
  // 5. Let d be ? ToIntegerThrowOnInfinity(isoDay).
  TO_INT_THROW_ON_INFTY(iso_day, JSTemporalPlainMonthDay);
  // 7. Let calendar be ? ToTemporalCalendarWithISODefault(calendarLike).
  Handle<JSReceiver> calendar;
  ASSIGN_RETURN_ON_EXCEPTION(
      isolate, calendar,
      ToTemporalCalendarWithISODefault(isolate, calendar_like, method),
      JSTemporalPlainMonthDay);

  // 2. If referenceISOYear is undefined, then
  // a. Set referenceISOYear to 1972𝔽.
  // ...
  // 8. Let ref be ? ToIntegerThrowOnInfinity(referenceISOYear).
  int32_t ref = 1972;
  if (!reference_iso_year_obj->IsUndefined()) {
    TO_INT_THROW_ON_INFTY(reference_iso_year, JSTemporalPlainMonthDay);
    ref = reference_iso_year;
  }

  // 10. Return ? CreateTemporalMonthDay(y, m, calendar, ref, NewTarget).
  return CreateTemporalMonthDay(isolate, target, new_target, iso_month, iso_day,
                                calendar, ref);
}

// #sec-temporal.plainmonthday.prototype.getisofields
MaybeHandle<JSReceiver> JSTemporalPlainMonthDay::GetISOFields(
    Isolate* isolate, Handle<JSTemporalPlainMonthDay> month_day) {
  Factory* factory = isolate->factory();
  // 1. Let monthDay be the this value.
  // 2. Perform ? RequireInternalSlot(monthDay,
  // [[InitializedTemporalMonthDay]]).
  // 3. Let fields be ! OrdinaryObjectCreate(%Object.prototype%).
  Handle<JSObject> fields = factory->NewJSObject(isolate->object_function());
  // 4. Perform ! CreateDataPropertyOrThrow(fields, "calendar",
  // montyDay.[[Calendar]]).
  CHECK(JSReceiver::CreateDataProperty(
            isolate, fields, factory->calendar_string(),
            Handle<JSReceiver>(month_day->calendar(), isolate),
            Just(kThrowOnError))
            .FromJust());

  // 5. Perform ! CreateDataPropertyOrThrow(fields, "isoDay",
  // 𝔽(montyDay.[[ISODay]])).
  // 6. Perform ! CreateDataPropertyOrThrow(fields, "isoMonth",
  // 𝔽(montyDay.[[ISOMonth]])).
  // 7. Perform ! CreateDataPropertyOrThrow(fields, "isoYear",
  // 𝔽(montyDay.[[ISOYear]])).
  DEFINE_INT_FIELD(fields, isoDay, iso_day, month_day)
  DEFINE_INT_FIELD(fields, isoMonth, iso_month, month_day)
  DEFINE_INT_FIELD(fields, isoYear, iso_year, month_day)
  // 8. Return fields.
  return fields;
}

MaybeHandle<JSTemporalPlainYearMonth> JSTemporalPlainYearMonth::Constructor(
    Isolate* isolate, Handle<JSFunction> target, Handle<HeapObject> new_target,
    Handle<Object> iso_year_obj, Handle<Object> iso_month_obj,
    Handle<Object> calendar_like, Handle<Object> reference_iso_day_obj) {
  const char* method = "Temporal.PlainYearMonth";
  // 1. If NewTarget is undefined, throw a TypeError exception.
  if (new_target->IsUndefined()) {
    THROW_NEW_ERROR(
        isolate,
        NewTypeError(MessageTemplate::kMethodInvokedOnWrongType,
                     isolate->factory()->NewStringFromAsciiChecked(method)),
        JSTemporalPlainYearMonth);
  }
  // 7. Let calendar be ? ToTemporalCalendarWithISODefault(calendarLike).
  // 10. Return ? CreateTemporalYearMonth(y, m, calendar, ref, NewTarget).

  // 3. Let y be ? ToIntegerThrowOnInfinity(isoYear).
  TO_INT_THROW_ON_INFTY(iso_year, JSTemporalPlainYearMonth);
  // 5. Let m be ? ToIntegerThrowOnInfinity(isoMonth).
  TO_INT_THROW_ON_INFTY(iso_month, JSTemporalPlainYearMonth);
  // 7. Let calendar be ? ToTemporalCalendarWithISODefault(calendarLike).
  Handle<JSReceiver> calendar;
  ASSIGN_RETURN_ON_EXCEPTION(
      isolate, calendar,
      ToTemporalCalendarWithISODefault(isolate, calendar_like, method),
      JSTemporalPlainYearMonth);

  // 2. If referenceISODay is undefined, then
  // a. Set referenceISODay to 1𝔽.
  // ...
  // 8. Let ref be ? ToIntegerThrowOnInfinity(referenceISODay).
  int32_t ref = 1;
  if (!reference_iso_day_obj->IsUndefined()) {
    TO_INT_THROW_ON_INFTY(reference_iso_day, JSTemporalPlainYearMonth);
    ref = reference_iso_day;
  }

  // 10. Return ? CreateTemporalYearMonth(y, m, calendar, ref, NewTarget).
  return CreateTemporalYearMonth(isolate, target, new_target, iso_year,
                                 iso_month, calendar, ref);
}

// #sec-temporal.plainyearmonth.prototype.getisofields
MaybeHandle<JSReceiver> JSTemporalPlainYearMonth::GetISOFields(
    Isolate* isolate, Handle<JSTemporalPlainYearMonth> year_month) {
  Factory* factory = isolate->factory();
  // 1. Let yearMonth be the this value.
  // 2. Perform ? RequireInternalSlot(yearMonth,
  // [[InitializedTemporalYearMonth]]).
  // 3. Let fields be ! OrdinaryObjectCreate(%Object.prototype%).
  Handle<JSObject> fields =
      isolate->factory()->NewJSObject(isolate->object_function());
  // 4. Perform ! CreateDataPropertyOrThrow(fields, "calendar",
  // yearMonth.[[Calendar]]).
  CHECK(JSReceiver::CreateDataProperty(
            isolate, fields, factory->calendar_string(),
            Handle<JSReceiver>(year_month->calendar(), isolate),
            Just(kThrowOnError))
            .FromJust());
  // 5. Perform ! CreateDataPropertyOrThrow(fields, "isoDay",
  // 𝔽(yearMonth.[[ISODay]])).
  // 6. Perform ! CreateDataPropertyOrThrow(fields, "isoMonth",
  // 𝔽(yearMonth.[[ISOMonth]])).
  // 7. Perform ! CreateDataPropertyOrThrow(fields, "isoYear",
  // 𝔽(yearMonth.[[ISOYear]])).
  DEFINE_INT_FIELD(fields, isoDay, iso_day, year_month)
  DEFINE_INT_FIELD(fields, isoMonth, iso_month, year_month)
  DEFINE_INT_FIELD(fields, isoYear, iso_year, year_month)
  // 8. Return fields.
  return fields;
}

// #sec-temporal-plaintime-constructor
MaybeHandle<JSTemporalPlainTime> JSTemporalPlainTime::Constructor(
    Isolate* isolate, Handle<JSFunction> target, Handle<HeapObject> new_target,
    Handle<Object> hour_obj, Handle<Object> minute_obj,
    Handle<Object> second_obj, Handle<Object> millisecond_obj,
    Handle<Object> microsecond_obj, Handle<Object> nanosecond_obj) {
  const char* method = "Temporal.PlainTime";
  // 1. If NewTarget is undefined, then
  // a. Throw a TypeError exception.
  if (new_target->IsUndefined()) {
    // a. Throw a TypeError exception.
    THROW_NEW_ERROR(
        isolate,
        NewTypeError(MessageTemplate::kMethodInvokedOnWrongType,
                     isolate->factory()->NewStringFromAsciiChecked(method)),
        JSTemporalPlainTime);
  }

  TO_INT_THROW_ON_INFTY(hour, JSTemporalPlainTime);
  TO_INT_THROW_ON_INFTY(minute, JSTemporalPlainTime);
  TO_INT_THROW_ON_INFTY(second, JSTemporalPlainTime);
  TO_INT_THROW_ON_INFTY(millisecond, JSTemporalPlainTime);
  TO_INT_THROW_ON_INFTY(microsecond, JSTemporalPlainTime);
  TO_INT_THROW_ON_INFTY(nanosecond, JSTemporalPlainTime);

  // 14. Return ? CreateTemporalTime(hour, minute, second, millisecond,
  // microsecond, nanosecond, NewTarget).
  return CreateTemporalTime(isolate, target, new_target, hour, minute, second,
                            millisecond, microsecond, nanosecond);
}

// #sec-temporal.plaintime.prototype.getisofields
MaybeHandle<JSReceiver> JSTemporalPlainTime::GetISOFields(
    Isolate* isolate, Handle<JSTemporalPlainTime> temporal_time) {
  Factory* factory = isolate->factory();
  // 1. Let temporalTime be the this value.
  // 2. Perform ? RequireInternalSlot(temporalTime,
  // [[InitializedTemporalTime]]).
  // 3. Let fields be ! OrdinaryObjectCreate(%Object.prototype%).
  Handle<JSObject> fields =
      isolate->factory()->NewJSObject(isolate->object_function());
  // 4. Perform ! CreateDataPropertyOrThrow(fields, "calendar",
  // temporalTime.[[Calendar]]).
  Handle<JSTemporalCalendar> iso8601_calendar;
  ASSIGN_RETURN_ON_EXCEPTION(isolate, iso8601_calendar,
                             temporal::GetISO8601Calendar(isolate),
                             JSTemporalPlainTime);
  CHECK(JSReceiver::CreateDataProperty(isolate, fields,
                                       factory->calendar_string(),
                                       iso8601_calendar, Just(kThrowOnError))
            .FromJust());

  // 5. Perform ! CreateDataPropertyOrThrow(fields, "isoHour",
  // 𝔽(temporalTime.[[ISOHour]])).
  // 6. Perform ! CreateDataPropertyOrThrow(fields, "isoMicrosecond",
  // 𝔽(temporalTime.[[ISOMicrosecond]])).
  // 7. Perform ! CreateDataPropertyOrThrow(fields, "isoMillisecond",
  // 𝔽(temporalTime.[[ISOMillisecond]])).
  // 8. Perform ! CreateDataPropertyOrThrow(fields, "isoMinute",
  // 𝔽(temporalTime.[[ISOMinute]])).
  // 9. Perform ! CreateDataPropertyOrThrow(fields, "isoNanosecond",
  // 𝔽(temporalTime.[[ISONanosecond]])).
  // 10. Perform ! CreateDataPropertyOrThrow(fields, "isoSecond",
  // 𝔽(temporalTime.[[ISOSecond]])).
  DEFINE_INT_FIELD(fields, isoHour, iso_hour, temporal_time)
  DEFINE_INT_FIELD(fields, isoMicrosecond, iso_microsecond, temporal_time)
  DEFINE_INT_FIELD(fields, isoMillisecond, iso_millisecond, temporal_time)
  DEFINE_INT_FIELD(fields, isoMinute, iso_minute, temporal_time)
  DEFINE_INT_FIELD(fields, isoNanosecond, iso_nanosecond, temporal_time)
  DEFINE_INT_FIELD(fields, isoSecond, iso_second, temporal_time)
  // 11. Return fields.
  return fields;
}

// #sec-temporal.zoneddatetime
MaybeHandle<JSTemporalZonedDateTime> JSTemporalZonedDateTime::Constructor(
    Isolate* isolate, Handle<JSFunction> target, Handle<HeapObject> new_target,
    Handle<Object> epoch_nanoseconds_obj, Handle<Object> time_zone_like,
    Handle<Object> calendar_like) {
  const char* method = "Temporal.ZonedDateTime";
  // 1. If NewTarget is undefined, then
  if (new_target->IsUndefined()) {
    // a. Throw a TypeError exception.
    THROW_NEW_ERROR(
        isolate,
        NewTypeError(MessageTemplate::kMethodInvokedOnWrongType,
                     isolate->factory()->NewStringFromAsciiChecked(method)),
        JSTemporalZonedDateTime);
  }
  // 2. Set epochNanoseconds to ? ToBigInt(epochNanoseconds).
  Handle<BigInt> epoch_nanoseconds;
  ASSIGN_RETURN_ON_EXCEPTION(isolate, epoch_nanoseconds,
                             BigInt::FromObject(isolate, epoch_nanoseconds_obj),
                             JSTemporalZonedDateTime);
  // 3. If ! IsValidEpochNanoseconds(epochNanoseconds) is false, throw a
  // RangeError exception.
  if (!IsValidEpochNanoseconds(isolate, epoch_nanoseconds)) {
    THROW_NEW_ERROR(isolate, NEW_TEMPORAL_INVALD_ARG_RANGE_ERROR(),
                    JSTemporalZonedDateTime);
  }

  // 4. Let timeZone be ? ToTemporalTimeZone(timeZoneLike).
  Handle<JSReceiver> time_zone;
  ASSIGN_RETURN_ON_EXCEPTION(
      isolate, time_zone, ToTemporalTimeZone(isolate, time_zone_like, method),
      JSTemporalZonedDateTime);

  // 5. Let calendar be ? ToTemporalCalendarWithISODefault(calendarLike).
  Handle<JSReceiver> calendar;
  ASSIGN_RETURN_ON_EXCEPTION(
      isolate, calendar,
      ToTemporalCalendarWithISODefault(isolate, calendar_like, method),
      JSTemporalZonedDateTime);

  // 6. Return ? CreateTemporalZonedDateTime(epochNanoseconds, timeZone,
  // calendar, NewTarget).
  return CreateTemporalZonedDateTime(isolate, target, new_target,
                                     epoch_nanoseconds, time_zone, calendar);
}

// #sec-temporal.zoneddatetime.prototype.getisofields
MaybeHandle<JSReceiver> JSTemporalZonedDateTime::GetISOFields(
    Isolate* isolate, Handle<JSTemporalZonedDateTime> zoned_date_time) {
  TEMPORAL_ENTER_FUNC();
  const char* method = "Temporal.ZonedDateTime.prototype.getISOFields";
  Factory* factory = isolate->factory();
  // 1. Let zonedDateTime be the this value.
  // 2. Perform ? RequireInternalSlot(zonedDateTime,
  // [[InitializedTemporalZonedDateTime]]).
  // 3. Let fields be ! OrdinaryObjectCreate(%Object.prototype%).
  Handle<JSObject> fields =
      isolate->factory()->NewJSObject(isolate->object_function());
  // 4. Let timeZone be zonedDateTime.[[TimeZone]].
  Handle<JSReceiver> time_zone =
      Handle<JSReceiver>(zoned_date_time->time_zone(), isolate);
  // 5. Let instant be ? CreateTemporalInstant(zonedDateTime.[[Nanoseconds]]).
  Handle<JSTemporalInstant> instant;
  ASSIGN_RETURN_ON_EXCEPTION(
      isolate, instant,
      CreateTemporalInstant(
          isolate, Handle<BigInt>(zoned_date_time->nanoseconds(), isolate)),
      JSReceiver);

  // 6. Let calendar be zonedDateTime.[[Calendar]].
  Handle<JSReceiver> calendar =
      Handle<JSReceiver>(zoned_date_time->calendar(), isolate);
  // 7. Let dateTime be ? BuiltinTimeZoneGetPlainDateTimeFor(timeZone,
  // instant, calendar).
  Handle<JSTemporalPlainDateTime> date_time;
  ASSIGN_RETURN_ON_EXCEPTION(isolate, date_time,
                             BuiltinTimeZoneGetPlainDateTimeFor(
                                 isolate, time_zone, instant, calendar, method),
                             JSReceiver);
  // 8. Let offset be ? BuiltinTimeZoneGetOffsetStringFor(timeZone, instant).
  Handle<String> offset;
  ASSIGN_RETURN_ON_EXCEPTION(
      isolate, offset,
      BuiltinTimeZoneGetOffsetStringFor(isolate, time_zone, instant, method),
      JSReceiver);

#define DEFINE_STRING_FIELD(obj, str, field)                                  \
  CHECK(JSReceiver::CreateDataProperty(isolate, obj, factory->str##_string(), \
                                       field, Just(kThrowOnError))            \
            .FromJust());

  // 9. Perform ! CreateDataPropertyOrThrow(fields, "calendar", calendar).
  // 10. Perform ! CreateDataPropertyOrThrow(fields, "isoDay",
  // 𝔽(dateTime.[[ISODay]])).
  // 11. Perform ! CreateDataPropertyOrThrow(fields, "isoHour",
  // 𝔽(temporalTime.[[ISOHour]])).
  // 12. Perform ! CreateDataPropertyOrThrow(fields, "isoMicrosecond",
  // 𝔽(temporalTime.[[ISOMicrosecond]])).
  // 13. Perform ! CreateDataPropertyOrThrow(fields, "isoMillisecond",
  // 𝔽(temporalTime.[[ISOMillisecond]])).
  // 14. Perform ! CreateDataPropertyOrThrow(fields, "isoMinute",
  // 𝔽(temporalTime.[[ISOMinute]])).
  // 15. Perform ! CreateDataPropertyOrThrow(fields, "isoMonth",
  // 𝔽(temporalTime.[[ISOMonth]])).
  // 16. Perform ! CreateDataPropertyOrThrow(fields, "isoNanosecond",
  // 𝔽(temporalTime.[[ISONanosecond]])).
  // 17. Perform ! CreateDataPropertyOrThrow(fields, "isoSecond",
  // 𝔽(temporalTime.[[ISOSecond]])).
  // 18. Perform ! CreateDataPropertyOrThrow(fields, "isoYear",
  // 𝔽(temporalTime.[[ISOYear]])).
  // 19. Perform ! CreateDataPropertyOrThrow(fields, "offset", offset).
  // 20. Perform ! CreateDataPropertyOrThrow(fields, "timeZone", timeZone).
  DEFINE_STRING_FIELD(fields, calendar, calendar)
  DEFINE_INT_FIELD(fields, isoDay, iso_day, date_time)
  DEFINE_INT_FIELD(fields, isoHour, iso_hour, date_time)
  DEFINE_INT_FIELD(fields, isoMicrosecond, iso_microsecond, date_time)
  DEFINE_INT_FIELD(fields, isoMillisecond, iso_millisecond, date_time)
  DEFINE_INT_FIELD(fields, isoMinute, iso_minute, date_time)
  DEFINE_INT_FIELD(fields, isoMonth, iso_month, date_time)
  DEFINE_INT_FIELD(fields, isoNanosecond, iso_nanosecond, date_time)
  DEFINE_INT_FIELD(fields, isoSecond, iso_second, date_time)
  DEFINE_INT_FIELD(fields, isoYear, iso_year, date_time)
  DEFINE_STRING_FIELD(fields, offset, offset)
  DEFINE_STRING_FIELD(fields, timeZone, time_zone)
  // 21. Return fields.
  return fields;
}

// #sec-temporal.instant
MaybeHandle<JSTemporalInstant> JSTemporalInstant::Constructor(
    Isolate* isolate, Handle<JSFunction> target, Handle<HeapObject> new_target,
    Handle<Object> epoch_nanoseconds_obj) {
  TEMPORAL_ENTER_FUNC();
  const char* method = "Temporal.Instant";
  // 1. If NewTarget is undefined, then
  if (new_target->IsUndefined()) {
    // a. Throw a TypeError exception.
    THROW_NEW_ERROR(
        isolate,
        NewTypeError(MessageTemplate::kMethodInvokedOnWrongType,
                     isolate->factory()->NewStringFromAsciiChecked(method)),
        JSTemporalInstant);
  }
  // 2. Let epochNanoseconds be ? ToBigInt(epochNanoseconds).
  Handle<BigInt> epoch_nanoseconds;
  ASSIGN_RETURN_ON_EXCEPTION(isolate, epoch_nanoseconds,
                             BigInt::FromObject(isolate, epoch_nanoseconds_obj),
                             JSTemporalInstant);
  // 3. If ! IsValidEpochNanoseconds(epochNanoseconds) is false, throw a
  // RangeError exception.
  if (!IsValidEpochNanoseconds(isolate, epoch_nanoseconds)) {
    THROW_NEW_ERROR(isolate, NEW_TEMPORAL_INVALD_ARG_RANGE_ERROR(),
                    JSTemporalInstant);
  }
  // 4. Return ? CreateTemporalInstant(epochNanoseconds, NewTarget).
  return CreateTemporalInstant(isolate, target, new_target, epoch_nanoseconds);
}

}  // namespace internal
}  // namespace v8
