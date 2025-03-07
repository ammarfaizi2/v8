// Copyright 2021 the V8 project authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef V8_OBJECTS_JS_TEMPORAL_OBJECTS_H_
#define V8_OBJECTS_JS_TEMPORAL_OBJECTS_H_

#include "src/execution/isolate.h"
#include "src/heap/factory.h"
#include "src/objects/objects.h"

// Has to be the last include (doesn't have include guards):
#include "src/objects/object-macros.h"

namespace v8 {
namespace internal {

#include "torque-generated/src/objects/js-temporal-objects-tq.inc"

#define DECLARE_TEMPORAL_INLINE_GETTER_SETTER(field) \
  inline void set_##field(int32_t field);            \
  inline int32_t field() const;

#define DECLARE_TEMPORAL_TIME_INLINE_GETTER_SETTER()     \
  DECLARE_TEMPORAL_INLINE_GETTER_SETTER(iso_hour)        \
  DECLARE_TEMPORAL_INLINE_GETTER_SETTER(iso_minute)      \
  DECLARE_TEMPORAL_INLINE_GETTER_SETTER(iso_second)      \
  DECLARE_TEMPORAL_INLINE_GETTER_SETTER(iso_millisecond) \
  DECLARE_TEMPORAL_INLINE_GETTER_SETTER(iso_microsecond) \
  DECLARE_TEMPORAL_INLINE_GETTER_SETTER(iso_nanosecond)

#define DECLARE_TEMPORAL_DATE_INLINE_GETTER_SETTER() \
  DECLARE_TEMPORAL_INLINE_GETTER_SETTER(iso_year)    \
  DECLARE_TEMPORAL_INLINE_GETTER_SETTER(iso_month)   \
  DECLARE_TEMPORAL_INLINE_GETTER_SETTER(iso_day)

#define TEMPORAL_UNIMPLEMENTED(T)            \
  {                                          \
    printf("TBW %s\n", __PRETTY_FUNCTION__); \
    UNIMPLEMENTED();                         \
  }

class JSTemporalPlainDate;
class JSTemporalPlainMonthDay;
class JSTemporalPlainYearMonth;

class JSTemporalCalendar
    : public TorqueGeneratedJSTemporalCalendar<JSTemporalCalendar, JSObject> {
 public:
  // #sec-temporal.calendar
  V8_WARN_UNUSED_RESULT static MaybeHandle<JSTemporalCalendar> Constructor(
      Isolate* isolate, Handle<JSFunction> target,
      Handle<HeapObject> new_target, Handle<Object> identifier);

  // #sec-temporal.calendar.prototype.tostring
  static MaybeHandle<String> ToString(Isolate* isolate,
                                      Handle<JSTemporalCalendar> calendar,
                                      const char* method);

  DECL_PRINTER(JSTemporalCalendar)

  DEFINE_TORQUE_GENERATED_JS_TEMPORAL_CALENDAR_FLAGS()

  DECL_INT_ACCESSORS(calendar_index)

  TQ_OBJECT_CONSTRUCTORS(JSTemporalCalendar)
};

class JSTemporalDuration
    : public TorqueGeneratedJSTemporalDuration<JSTemporalDuration, JSObject> {
 public:
  // #sec-temporal.duration
  V8_WARN_UNUSED_RESULT static MaybeHandle<JSTemporalDuration> Constructor(
      Isolate* isolate, Handle<JSFunction> target,
      Handle<HeapObject> new_target, Handle<Object> years,
      Handle<Object> months, Handle<Object> weeks, Handle<Object> days,
      Handle<Object> hours, Handle<Object> minutes, Handle<Object> seconds,
      Handle<Object> milliseconds, Handle<Object> microseconds,
      Handle<Object> nanoseconds);

  // #sec-get-temporal.duration.prototype.sign
  V8_WARN_UNUSED_RESULT static MaybeHandle<Smi> Sign(
      Isolate* isolate, Handle<JSTemporalDuration> duration);

  // #sec-get-temporal.duration.prototype.blank
  V8_WARN_UNUSED_RESULT static MaybeHandle<Oddball> Blank(
      Isolate* isolate, Handle<JSTemporalDuration> duration);

  DECL_PRINTER(JSTemporalDuration)

  TQ_OBJECT_CONSTRUCTORS(JSTemporalDuration)
};

class JSTemporalInstant
    : public TorqueGeneratedJSTemporalInstant<JSTemporalInstant, JSObject> {
 public:
  // #sec-temporal-instant-constructor
  V8_WARN_UNUSED_RESULT static MaybeHandle<JSTemporalInstant> Constructor(
      Isolate* isolate, Handle<JSFunction> target,
      Handle<HeapObject> new_target, Handle<Object> epoch_nanoseconds);

  DECL_PRINTER(JSTemporalInstant)

  TQ_OBJECT_CONSTRUCTORS(JSTemporalInstant)
};

class JSTemporalPlainDate
    : public TorqueGeneratedJSTemporalPlainDate<JSTemporalPlainDate, JSObject> {
 public:
  // #sec-temporal-createtemporaldate
  V8_WARN_UNUSED_RESULT static MaybeHandle<JSTemporalPlainDate> Constructor(
      Isolate* isolate, Handle<JSFunction> target,
      Handle<HeapObject> new_target, Handle<Object> iso_year,
      Handle<Object> iso_month, Handle<Object> iso_day,
      Handle<Object> calendar_like);

  // #sec-temporal.plaindate.prototype.getisofields
  V8_WARN_UNUSED_RESULT static MaybeHandle<JSReceiver> GetISOFields(
      Isolate* isolate, Handle<JSTemporalPlainDate> plain_date);

  DECL_PRINTER(JSTemporalPlainDate)

  DEFINE_TORQUE_GENERATED_JS_TEMPORAL_YEAR_MONTH_DAY()

  DECLARE_TEMPORAL_DATE_INLINE_GETTER_SETTER()

  TQ_OBJECT_CONSTRUCTORS(JSTemporalPlainDate)
};

class JSTemporalPlainDateTime
    : public TorqueGeneratedJSTemporalPlainDateTime<JSTemporalPlainDateTime,
                                                    JSObject> {
 public:
  // #sec-temporal-createtemporaldatetime
  V8_WARN_UNUSED_RESULT static MaybeHandle<JSTemporalPlainDateTime> Constructor(
      Isolate* isolate, Handle<JSFunction> target,
      Handle<HeapObject> new_target, Handle<Object> iso_year,
      Handle<Object> iso_month, Handle<Object> iso_day, Handle<Object> hour,
      Handle<Object> minute, Handle<Object> second, Handle<Object> millisecond,
      Handle<Object> microsecond, Handle<Object> nanosecond,
      Handle<Object> calendar_like);

  // #sec-temporal.plaindatetime.prototype.getisofields
  V8_WARN_UNUSED_RESULT static MaybeHandle<JSReceiver> GetISOFields(
      Isolate* isolate, Handle<JSTemporalPlainDateTime> date_time);

  DECL_PRINTER(JSTemporalPlainDateTime)

  DEFINE_TORQUE_GENERATED_JS_TEMPORAL_YEAR_MONTH_DAY()
  DEFINE_TORQUE_GENERATED_JS_TEMPORAL_HOUR_MINUTE_SECOND()
  DEFINE_TORQUE_GENERATED_JS_TEMPORAL_SECOND_PARTS()

  DECLARE_TEMPORAL_DATE_INLINE_GETTER_SETTER()
  DECLARE_TEMPORAL_TIME_INLINE_GETTER_SETTER()

  TQ_OBJECT_CONSTRUCTORS(JSTemporalPlainDateTime)
};

class JSTemporalPlainMonthDay
    : public TorqueGeneratedJSTemporalPlainMonthDay<JSTemporalPlainMonthDay,
                                                    JSObject> {
 public:
  // ##sec-temporal.plainmonthday
  V8_WARN_UNUSED_RESULT static MaybeHandle<JSTemporalPlainMonthDay> Constructor(
      Isolate* isolate, Handle<JSFunction> target,
      Handle<HeapObject> new_target, Handle<Object> iso_month,
      Handle<Object> iso_day, Handle<Object> calendar_like,
      Handle<Object> reference_iso_year);

  // #sec-temporal.plainmonthday.prototype.getisofields
  V8_WARN_UNUSED_RESULT static MaybeHandle<JSReceiver> GetISOFields(
      Isolate* isolate, Handle<JSTemporalPlainMonthDay> month_day);

  DECL_PRINTER(JSTemporalPlainMonthDay)

  DEFINE_TORQUE_GENERATED_JS_TEMPORAL_YEAR_MONTH_DAY()

  DECLARE_TEMPORAL_DATE_INLINE_GETTER_SETTER()

  TQ_OBJECT_CONSTRUCTORS(JSTemporalPlainMonthDay)
};

class JSTemporalPlainTime
    : public TorqueGeneratedJSTemporalPlainTime<JSTemporalPlainTime, JSObject> {
 public:
  // #sec-temporal-plaintime-constructor
  V8_WARN_UNUSED_RESULT static MaybeHandle<JSTemporalPlainTime> Constructor(
      Isolate* isolate, Handle<JSFunction> target,
      Handle<HeapObject> new_target, Handle<Object> hour, Handle<Object> minute,
      Handle<Object> second, Handle<Object> millisecond,
      Handle<Object> microsecond, Handle<Object> nanosecond);

  // #sec-temporal.plaintime.prototype.getisofields
  V8_WARN_UNUSED_RESULT static MaybeHandle<JSReceiver> GetISOFields(
      Isolate* isolate, Handle<JSTemporalPlainTime> plain_time);

  DECL_PRINTER(JSTemporalPlainTime)

  DEFINE_TORQUE_GENERATED_JS_TEMPORAL_HOUR_MINUTE_SECOND()
  DEFINE_TORQUE_GENERATED_JS_TEMPORAL_SECOND_PARTS()

  DECLARE_TEMPORAL_TIME_INLINE_GETTER_SETTER()

  TQ_OBJECT_CONSTRUCTORS(JSTemporalPlainTime)
};

class JSTemporalPlainYearMonth
    : public TorqueGeneratedJSTemporalPlainYearMonth<JSTemporalPlainYearMonth,
                                                     JSObject> {
 public:
  // ##sec-temporal.plainyearmonth
  V8_WARN_UNUSED_RESULT static MaybeHandle<JSTemporalPlainYearMonth>
  Constructor(Isolate* isolate, Handle<JSFunction> target,
              Handle<HeapObject> new_target, Handle<Object> iso_year,
              Handle<Object> iso_month, Handle<Object> calendar_like,
              Handle<Object> reference_iso_day);

  // #sec-temporal.plainyearmonth.prototype.getisofields
  V8_WARN_UNUSED_RESULT static MaybeHandle<JSReceiver> GetISOFields(
      Isolate* isolate, Handle<JSTemporalPlainYearMonth> year_month);

  // Abstract Operations

  DECL_PRINTER(JSTemporalPlainYearMonth)

  DEFINE_TORQUE_GENERATED_JS_TEMPORAL_YEAR_MONTH_DAY()

  DECLARE_TEMPORAL_DATE_INLINE_GETTER_SETTER()

  TQ_OBJECT_CONSTRUCTORS(JSTemporalPlainYearMonth)
};

class JSTemporalTimeZone
    : public TorqueGeneratedJSTemporalTimeZone<JSTemporalTimeZone, JSObject> {
 public:
  // #sec-temporal.timezone
  V8_WARN_UNUSED_RESULT static MaybeHandle<JSTemporalTimeZone> Constructor(
      Isolate* isolate, Handle<JSFunction> target,
      Handle<HeapObject> new_target, Handle<Object> identifier);

  // #sec-temporal.timezone.prototype.tostring
  static MaybeHandle<Object> ToString(Isolate* isolate,
                                      Handle<JSTemporalTimeZone> time_zone,
                                      const char* method);

  DECL_PRINTER(JSTemporalTimeZone)

  DEFINE_TORQUE_GENERATED_JS_TEMPORAL_TIME_ZONE_FLAGS()
  DEFINE_TORQUE_GENERATED_JS_TEMPORAL_TIME_ZONE_SUB_MILLISECONDS()

  DECL_BOOLEAN_ACCESSORS(is_offset)
  DECL_INT_ACCESSORS(offset_milliseconds_or_time_zone_index)

  DECLARE_TEMPORAL_INLINE_GETTER_SETTER(offset_milliseconds)
  DECLARE_TEMPORAL_INLINE_GETTER_SETTER(offset_sub_milliseconds)

  int32_t time_zone_index() const;
  int64_t offset_nanoseconds() const;
  void set_offset_nanoseconds(int64_t offset_nanoseconds);

  MaybeHandle<String> id(Isolate* isolate) const;

  TQ_OBJECT_CONSTRUCTORS(JSTemporalTimeZone)
};

class JSTemporalZonedDateTime
    : public TorqueGeneratedJSTemporalZonedDateTime<JSTemporalZonedDateTime,
                                                    JSObject> {
 public:
  // #sec-temporal.zoneddatetime
  V8_WARN_UNUSED_RESULT static MaybeHandle<JSTemporalZonedDateTime> Constructor(
      Isolate* isolate, Handle<JSFunction> target,
      Handle<HeapObject> new_target, Handle<Object> epoch_nanoseconds,
      Handle<Object> time_zone_like, Handle<Object> calendar_like);

  // #sec-temporal.zoneddatetime.prototype.getisofields
  V8_WARN_UNUSED_RESULT static MaybeHandle<JSReceiver> GetISOFields(
      Isolate* isolate, Handle<JSTemporalZonedDateTime> zoned_date_time);

  DECL_PRINTER(JSTemporalZonedDateTime)

  TQ_OBJECT_CONSTRUCTORS(JSTemporalZonedDateTime)
};

namespace temporal {

// #sec-temporal-getiso8601calendar
V8_WARN_UNUSED_RESULT MaybeHandle<JSTemporalCalendar> GetISO8601Calendar(
    Isolate* isolate);

}  // namespace temporal
}  // namespace internal
}  // namespace v8
#include "src/objects/object-macros-undef.h"
#endif  // V8_OBJECTS_JS_TEMPORAL_OBJECTS_H_
