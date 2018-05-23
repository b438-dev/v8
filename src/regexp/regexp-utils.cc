// Copyright 2016 the V8 project authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "src/regexp/regexp-utils.h"

#include "src/heap/factory.h"
#include "src/isolate.h"
#include "src/objects-inl.h"
#include "src/objects/js-regexp-inl.h"
#include "src/regexp/jsregexp.h"

namespace v8 {
namespace internal {

Handle<String> RegExpUtils::GenericCaptureGetter(
    Isolate* isolate, Handle<RegExpMatchInfo> match_info, int capture,
    bool* ok) {
  const int index = capture * 2;
  if (index >= match_info->NumberOfCaptureRegisters()) {
    if (ok != nullptr) *ok = false;
    return isolate->factory()->empty_string();
  }

  const int match_start = match_info->Capture(index);
  const int match_end = match_info->Capture(index + 1);
  if (match_start == -1 || match_end == -1) {
    if (ok != nullptr) *ok = false;
    return isolate->factory()->empty_string();
  }

  if (ok != nullptr) *ok = true;
  Handle<String> last_subject(match_info->LastSubject());
  return isolate->factory()->NewSubString(last_subject, match_start, match_end);
}

namespace {

V8_INLINE bool HasInitialRegExpMap(Isolate* isolate, Handle<JSReceiver> recv) {
  return recv->map() == isolate->regexp_function()->initial_map();
}

}  // namespace

MaybeHandle<Object> RegExpUtils::SetLastIndex(Isolate* isolate,
                                              Handle<JSReceiver> recv,
                                              uint64_t value) {
  Handle<Object> value_as_object =
      isolate->factory()->NewNumberFromInt64(value);
  if (HasInitialRegExpMap(isolate, recv)) {
    JSRegExp::cast(*recv)->set_last_index(*value_as_object, SKIP_WRITE_BARRIER);
    return recv;
  } else {
    return Object::SetProperty(recv, isolate->factory()->lastIndex_string(),
                               value_as_object, LanguageMode::kStrict);
  }
}

MaybeHandle<Object> RegExpUtils::GetLastIndex(Isolate* isolate,
                                              Handle<JSReceiver> recv) {
  if (HasInitialRegExpMap(isolate, recv)) {
    return handle(JSRegExp::cast(*recv)->last_index(), isolate);
  } else {
    return Object::GetProperty(recv, isolate->factory()->lastIndex_string());
  }
}

// ES#sec-regexpexec Runtime Semantics: RegExpExec ( R, S )
// Also takes an optional exec method in case our caller
// has already fetched exec.
MaybeHandle<Object> RegExpUtils::RegExpExec(Isolate* isolate,
                                            Handle<JSReceiver> regexp,
                                            Handle<String> string,
                                            Handle<Object> exec) {
  if (exec->IsUndefined(isolate)) {
    ASSIGN_RETURN_ON_EXCEPTION(
        isolate, exec,
        Object::GetProperty(regexp, isolate->factory()->exec_string()), Object);
  }

  if (exec->IsCallable()) {
    const int argc = 1;
    ScopedVector<Handle<Object>> argv(argc);
    argv[0] = string;

    Handle<Object> result;
    ASSIGN_RETURN_ON_EXCEPTION(
        isolate, result,
        Execution::Call(isolate, exec, regexp, argc, argv.start()), Object);

    if (!result->IsJSReceiver() && !result->IsNull(isolate)) {
      THROW_NEW_ERROR(isolate,
                      NewTypeError(MessageTemplate::kInvalidRegExpExecResult),
                      Object);
    }
    return result;
  }

  if (!regexp->IsJSRegExp()) {
    THROW_NEW_ERROR(isolate,
                    NewTypeError(MessageTemplate::kIncompatibleMethodReceiver,
                                 isolate->factory()->NewStringFromAsciiChecked(
                                     "RegExp.prototype.exec"),
                                 regexp),
                    Object);
  }

  {
    Handle<JSFunction> regexp_exec = isolate->regexp_exec_function();

    const int argc = 1;
    ScopedVector<Handle<Object>> argv(argc);
    argv[0] = string;

    return Execution::Call(isolate, regexp_exec, regexp, argc, argv.start());
  }
}

Maybe<bool> RegExpUtils::IsRegExp(Isolate* isolate, Handle<Object> object) {
  if (!object->IsJSReceiver()) return Just(false);

  Handle<JSReceiver> receiver = Handle<JSReceiver>::cast(object);

  Handle<Object> match;
  ASSIGN_RETURN_ON_EXCEPTION_VALUE(
      isolate, match,
      JSObject::GetProperty(receiver, isolate->factory()->match_symbol()),
      Nothing<bool>());

  if (!match->IsUndefined(isolate)) return Just(match->BooleanValue());
  return Just(object->IsJSRegExp());
}

bool RegExpUtils::IsUnmodifiedRegExp(Isolate* isolate, Handle<Object> obj) {
  // TODO(ishell): Update this check once map changes for constant field
  // tracking are landing.

#ifdef V8_ENABLE_FORCE_SLOW_PATH
  if (isolate->force_slow_path()) return false;
#endif

  if (!obj->IsJSReceiver()) return false;

  JSReceiver* recv = JSReceiver::cast(*obj);

  // Check the receiver's map.
  Handle<JSFunction> regexp_function = isolate->regexp_function();
  if (recv->map() != regexp_function->initial_map()) return false;

  // Check the receiver's prototype's map.
  Object* proto = recv->map()->prototype();
  if (!proto->IsJSReceiver()) return false;

  Handle<Map> initial_proto_initial_map = isolate->regexp_prototype_map();
  if (JSReceiver::cast(proto)->map() != *initial_proto_initial_map) {
    return false;
  }

  // The smi check is required to omit ToLength(lastIndex) calls with possible
  // user-code execution on the fast path.
  Object* last_index = JSRegExp::cast(recv)->last_index();
  return last_index->IsSmi() && Smi::ToInt(last_index) >= 0;
}

uint64_t RegExpUtils::AdvanceStringIndex(Isolate* isolate,
                                         Handle<String> string, uint64_t index,
                                         bool unicode) {
  DCHECK_LE(static_cast<double>(index), kMaxSafeInteger);
  const uint64_t string_length = static_cast<uint64_t>(string->length());
  if (unicode && index < string_length) {
    const uint16_t first = string->Get(static_cast<uint32_t>(index));
    if (first >= 0xD800 && first <= 0xDBFF && index + 1 < string_length) {
      DCHECK_LT(index, std::numeric_limits<uint64_t>::max());
      const uint16_t second = string->Get(static_cast<uint32_t>(index + 1));
      if (second >= 0xDC00 && second <= 0xDFFF) {
        return index + 2;
      }
    }
  }

  return index + 1;
}

MaybeHandle<Object> RegExpUtils::SetAdvancedStringIndex(
    Isolate* isolate, Handle<JSReceiver> regexp, Handle<String> string,
    bool unicode) {
  Handle<Object> last_index_obj;
  ASSIGN_RETURN_ON_EXCEPTION(
      isolate, last_index_obj,
      Object::GetProperty(regexp, isolate->factory()->lastIndex_string()),
      Object);

  ASSIGN_RETURN_ON_EXCEPTION(isolate, last_index_obj,
                             Object::ToLength(isolate, last_index_obj), Object);
  const uint64_t last_index = PositiveNumberToUint64(*last_index_obj);
  const uint64_t new_last_index =
      AdvanceStringIndex(isolate, string, last_index, unicode);

  return SetLastIndex(isolate, regexp, new_last_index);
}

}  // namespace internal
}  // namespace v8
