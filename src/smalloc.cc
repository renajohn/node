// Copyright Joyent, Inc. and other Node contributors.
//
// Permission is hereby granted, free of charge, to any person obtaining a
// copy of this software and associated documentation files (the
// "Software"), to deal in the Software without restriction, including
// without limitation the rights to use, copy, modify, merge, publish,
// distribute, sublicense, and/or sell copies of the Software, and to permit
// persons to whom the Software is furnished to do so, subject to the
// following conditions:
//
// The above copyright notice and this permission notice shall be included
// in all copies or substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS
// OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
// MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN
// NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM,
// DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR
// OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE
// USE OR OTHER DEALINGS IN THE SOFTWARE.

#include "smalloc.h"

#include "env.h"
#include "env-inl.h"
#include "node.h"
#include "node_internals.h"
#include "v8-profiler.h"
#include "v8.h"

#include <string.h>
#include <assert.h>

#define ALLOC_ID (0xA10C)

namespace node {
namespace smalloc {

using v8::Context;
using v8::External;
using v8::ExternalArrayType;
using v8::FunctionCallbackInfo;
using v8::Handle;
using v8::HandleScope;
using v8::HeapProfiler;
using v8::Isolate;
using v8::Local;
using v8::Object;
using v8::Persistent;
using v8::RetainedObjectInfo;
using v8::Uint32;
using v8::Value;
using v8::WeakCallbackData;
using v8::kExternalUnsignedByteArray;


template <typename Payload>
class CallbackInfo {
 public:
  static inline CallbackInfo* New(Isolate* isolate,
                                  Handle<Object> object,
                                  Payload payload);
  inline void Dispose();
  inline Persistent<Object>* persistent();
  inline Payload* payload();
 private:
  static void WeakCallback(const WeakCallbackData<Object, CallbackInfo>&);
  inline CallbackInfo(Isolate* isolate,
                      Handle<Object> object,
                      Payload payload);
  ~CallbackInfo();
  Persistent<Object> persistent_;
  Payload payload_;
};


class Free {
 public:
  explicit Free(char* data);
  void WeakCallback(Isolate* isolate,
                    Local<Object> object,
                    CallbackInfo<Free>* info);
 private:
  char* const data_;
};


class Dispose {
 public:
  typedef void (*Callback)(char* data, void* hint);
  Dispose(Callback callback, void* hint);
  void WeakCallback(Isolate* isolate,
                    Local<Object> object,
                    CallbackInfo<Dispose>* info);
 private:
  Callback const callback_;
  void* const hint_;
};


template <typename Payload>
CallbackInfo<Payload>* CallbackInfo<Payload>::New(Isolate* isolate,
                                                  Handle<Object> object,
                                                  Payload payload) {
  return new CallbackInfo(isolate, object, payload);
}


template <typename Payload>
void CallbackInfo<Payload>::Dispose() {
  delete this;
}


template <typename Payload>
Persistent<Object>* CallbackInfo<Payload>::persistent() {
  return &persistent_;
}


template <typename Payload>
Payload* CallbackInfo<Payload>::payload() {
  return &payload_;
}


template <typename Payload>
CallbackInfo<Payload>::CallbackInfo(Isolate* isolate,
                                    Handle<Object> object,
                                    Payload payload)
    : persistent_(isolate, object),
      payload_(payload) {
  persistent_.SetWeak(this, WeakCallback);
  persistent_.SetWrapperClassId(ALLOC_ID);
  persistent_.MarkIndependent();
}


template <typename Payload>
CallbackInfo<Payload>::~CallbackInfo() {
  persistent_.Reset();
}


template <typename Payload>
void CallbackInfo<Payload>::WeakCallback(
    const WeakCallbackData<Object, CallbackInfo>& data) {
  CallbackInfo* info = data.GetParameter();
  info->payload()->WeakCallback(data.GetIsolate(), data.GetValue(), info);
}


Free::Free(char* data) : data_(data) {
}


void Free::WeakCallback(Isolate* isolate,
                        Local<Object> object,
                        CallbackInfo<Free>* info) {
  free(data_);
  size_t length = object->GetIndexedPropertiesExternalArrayDataLength();
  enum ExternalArrayType array_type =
      object->GetIndexedPropertiesExternalArrayDataType();
  size_t array_size = ExternalArraySize(array_type);
  CHECK_GT(array_size, 0);
  CHECK_GE(array_size * length, length);  // Overflow check.
  length *= array_size;
  int64_t change_in_bytes = -static_cast<int64_t>(length);
  isolate->AdjustAmountOfExternalAllocatedMemory(change_in_bytes);
  info->Dispose();
}


Dispose::Dispose(Callback callback, void* hint)
    : callback_(callback),
      hint_(hint) {
}


void Dispose::WeakCallback(Isolate* isolate,
                           Local<Object> object,
                           CallbackInfo<Dispose>* info) {
  char* data =
      static_cast<char*>(object->GetIndexedPropertiesExternalArrayData());
  size_t len = object->GetIndexedPropertiesExternalArrayDataLength();
  enum ExternalArrayType array_type =
      object->GetIndexedPropertiesExternalArrayDataType();
  size_t array_size = ExternalArraySize(array_type);
  CHECK_GT(array_size, 0);
  if (array_size > 1) {
    CHECK_GT(len * array_size, len);  // Overflow check.
    len *= array_size;
  }
  int64_t change_in_bytes = -static_cast<int64_t>(len + sizeof(*info));
  isolate->AdjustAmountOfExternalAllocatedMemory(change_in_bytes);
  callback_(data, hint_);
  info->Dispose();
}


// return size of external array type, or 0 if unrecognized
size_t ExternalArraySize(enum ExternalArrayType type) {
  switch (type) {
    case v8::kExternalUnsignedByteArray:
      return sizeof(uint8_t);
    case v8::kExternalByteArray:
      return sizeof(int8_t);
    case v8::kExternalShortArray:
      return sizeof(int16_t);
    case v8::kExternalUnsignedShortArray:
      return sizeof(uint16_t);
    case v8::kExternalIntArray:
      return sizeof(int32_t);
    case v8::kExternalUnsignedIntArray:
      return sizeof(uint32_t);
    case v8::kExternalFloatArray:
      return sizeof(float);   // NOLINT(runtime/sizeof)
    case v8::kExternalDoubleArray:
      return sizeof(double);  // NOLINT(runtime/sizeof)
    case v8::kExternalPixelArray:
      return sizeof(uint8_t);
  }
  return 0;
}


// copyOnto(source, source_start, dest, dest_start, copy_length)
void CopyOnto(const FunctionCallbackInfo<Value>& args) {
  Environment* env = Environment::GetCurrent(args.GetIsolate());
  HandleScope scope(env->isolate());

  if (!args[0]->IsObject())
    return env->ThrowTypeError("source must be an object");
  if (!args[2]->IsObject())
    return env->ThrowTypeError("dest must be an object");

  Local<Object> source = args[0].As<Object>();
  Local<Object> dest = args[2].As<Object>();

  if (!source->HasIndexedPropertiesInExternalArrayData())
    return env->ThrowError("source has no external array data");
  if (!dest->HasIndexedPropertiesInExternalArrayData())
    return env->ThrowError("dest has no external array data");

  size_t source_start = args[1]->Uint32Value();
  size_t dest_start = args[3]->Uint32Value();
  size_t copy_length = args[4]->Uint32Value();
  char* source_data = static_cast<char*>(
      source->GetIndexedPropertiesExternalArrayData());
  char* dest_data = static_cast<char*>(
      dest->GetIndexedPropertiesExternalArrayData());

  size_t source_length = source->GetIndexedPropertiesExternalArrayDataLength();
  enum ExternalArrayType source_type =
    source->GetIndexedPropertiesExternalArrayDataType();
  size_t source_size = ExternalArraySize(source_type);

  size_t dest_length = dest->GetIndexedPropertiesExternalArrayDataLength();
  enum ExternalArrayType dest_type =
    dest->GetIndexedPropertiesExternalArrayDataType();
  size_t dest_size = ExternalArraySize(dest_type);

  // optimization for Uint8 arrays (i.e. Buffers)
  if (source_size != 1 && dest_size != 1) {
    if (source_size == 0)
      return env->ThrowTypeError("unknown source external array type");
    if (dest_size == 0)
      return env->ThrowTypeError("unknown dest external array type");

    if (source_length * source_size < source_length)
      return env->ThrowRangeError("source_length * source_size overflow");
    if (copy_length * source_size < copy_length)
      return env->ThrowRangeError("copy_length * source_size overflow");
    if (dest_length * dest_size < dest_length)
      return env->ThrowRangeError("dest_length * dest_size overflow");

    source_length *= source_size;
    copy_length *= source_size;
    dest_length *= dest_size;
  }

  // necessary to check in case (source|dest)_start _and_ copy_length overflow
  if (copy_length > source_length)
    return env->ThrowRangeError("copy_length > source_length");
  if (copy_length > dest_length)
    return env->ThrowRangeError("copy_length > dest_length");
  if (source_start > source_length)
    return env->ThrowRangeError("source_start > source_length");
  if (dest_start > dest_length)
    return env->ThrowRangeError("dest_start > dest_length");

  // now we can guarantee these will catch oob access and *_start overflow
  if (source_start + copy_length > source_length)
    return env->ThrowRangeError("source_start + copy_length > source_length");
  if (dest_start + copy_length > dest_length)
    return env->ThrowRangeError("dest_start + copy_length > dest_length");

  memmove(dest_data + dest_start, source_data + source_start, copy_length);
}


// dest will always be same type as source
// for internal use:
//    dest._data = sliceOnto(source, dest, start, end);
void SliceOnto(const FunctionCallbackInfo<Value>& args) {
  Environment* env = Environment::GetCurrent(args.GetIsolate());
  HandleScope scope(env->isolate());

  Local<Object> source = args[0].As<Object>();
  Local<Object> dest = args[1].As<Object>();

  assert(source->HasIndexedPropertiesInExternalArrayData());
  assert(!dest->HasIndexedPropertiesInExternalArrayData());

  char* source_data = static_cast<char*>(
      source->GetIndexedPropertiesExternalArrayData());
  size_t source_len = source->GetIndexedPropertiesExternalArrayDataLength();
  enum ExternalArrayType source_type =
    source->GetIndexedPropertiesExternalArrayDataType();
  size_t source_size = ExternalArraySize(source_type);

  assert(source_size != 0);

  size_t start = args[2]->Uint32Value();
  size_t end = args[3]->Uint32Value();
  size_t length = end - start;

  if (source_size > 1) {
    assert(length * source_size >= length);
    length *= source_size;
  }

  assert(source_data != NULL || length == 0);
  assert(end <= source_len);
  assert(start <= end);

  dest->SetIndexedPropertiesToExternalArrayData(source_data + start,
                                                source_type,
                                                length);
  args.GetReturnValue().Set(source);
}


// for internal use:
//    alloc(obj, n[, type]);
void Alloc(const FunctionCallbackInfo<Value>& args) {
  Environment* env = Environment::GetCurrent(args.GetIsolate());
  HandleScope scope(env->isolate());

  Local<Object> obj = args[0].As<Object>();

  // can't perform this check in JS
  if (obj->HasIndexedPropertiesInExternalArrayData())
    return env->ThrowTypeError("object already has external array data");

  size_t length = args[1]->Uint32Value();
  enum ExternalArrayType array_type;

  // it's faster to not pass the default argument then use Uint32Value
  if (args[2]->IsUndefined()) {
    array_type = kExternalUnsignedByteArray;
  } else {
    array_type = static_cast<ExternalArrayType>(args[2]->Uint32Value());
    size_t type_length = ExternalArraySize(array_type);
    assert(type_length * length >= length);
    length *= type_length;
  }

  Alloc(env, obj, length, array_type);
  args.GetReturnValue().Set(obj);
}


void Alloc(Environment* env,
           Handle<Object> obj,
           size_t length,
           enum ExternalArrayType type) {
  size_t type_size = ExternalArraySize(type);

  assert(length <= kMaxLength);
  assert(type_size > 0);

  if (length == 0)
    return Alloc(env, obj, NULL, length, type);

  char* data = static_cast<char*>(malloc(length));
  if (data == NULL) {
    FatalError("node::smalloc::Alloc(v8::Handle<v8::Object>, size_t,"
               " v8::ExternalArrayType)", "Out Of Memory");
  }

  Alloc(env, obj, data, length, type);
}


void Alloc(Environment* env,
           Handle<Object> obj,
           char* data,
           size_t length,
           enum ExternalArrayType type) {
  assert(!obj->HasIndexedPropertiesInExternalArrayData());
  env->isolate()->AdjustAmountOfExternalAllocatedMemory(length);
  size_t size = length / ExternalArraySize(type);
  obj->SetIndexedPropertiesToExternalArrayData(data, type, size);
  CallbackInfo<Free>::New(env->isolate(), obj, Free(data));
}


// for internal use: dispose(obj);
void AllocDispose(const FunctionCallbackInfo<Value>& args) {
  Environment* env = Environment::GetCurrent(args.GetIsolate());
  AllocDispose(env, args[0].As<Object>());
}


void AllocDispose(Environment* env, Handle<Object> obj) {
  HandleScope handle_scope(env->isolate());

  if (env->using_smalloc_alloc_cb()) {
    Local<Value> ext_v = obj->GetHiddenValue(env->smalloc_p_string());
    if (ext_v->IsExternal()) {
      Local<External> ext = ext_v.As<External>();
      CallbackInfo<Free>* info = static_cast<CallbackInfo<Free>*>(ext->Value());
      Local<Object> object = PersistentToLocal(env->isolate(),
                                               *info->persistent());
      info->payload()->WeakCallback(env->isolate(), object, info);
      return;
    }
  }

  char* data = static_cast<char*>(obj->GetIndexedPropertiesExternalArrayData());
  size_t length = obj->GetIndexedPropertiesExternalArrayDataLength();
  enum ExternalArrayType array_type =
    obj->GetIndexedPropertiesExternalArrayDataType();
  size_t array_size = ExternalArraySize(array_type);

  assert(array_size > 0);
  assert(length * array_size >= length);

  length *= array_size;

  if (data != NULL) {
    obj->SetIndexedPropertiesToExternalArrayData(NULL,
                                                 kExternalUnsignedByteArray,
                                                 0);
    free(data);
  }
  if (length != 0) {
    int64_t change_in_bytes = -static_cast<int64_t>(length);
    env->isolate()->AdjustAmountOfExternalAllocatedMemory(change_in_bytes);
  }
}


void Alloc(Environment* env,
           Handle<Object> obj,
           size_t length,
           FreeCallback fn,
           void* hint,
           enum ExternalArrayType type) {
  assert(length <= kMaxLength);

  size_t type_size = ExternalArraySize(type);

  assert(type_size > 0);
  assert(length * type_size >= length);

  length *= type_size;

  char* data = new char[length];
  Alloc(env, obj, data, length, fn, hint, type);
}


void Alloc(Environment* env,
           Handle<Object> obj,
           char* data,
           size_t length,
           FreeCallback fn,
           void* hint,
           enum ExternalArrayType type) {
  assert(!obj->HasIndexedPropertiesInExternalArrayData());
  Isolate* isolate = env->isolate();
  HandleScope handle_scope(isolate);
  env->set_using_smalloc_alloc_cb(true);
  CallbackInfo<Dispose>* info =
      CallbackInfo<Dispose>::New(isolate, obj, Dispose(fn, hint));
  obj->SetHiddenValue(env->smalloc_p_string(), External::New(isolate, info));
  isolate->AdjustAmountOfExternalAllocatedMemory(length + sizeof(*info));
  size_t size = length / ExternalArraySize(type);
  obj->SetIndexedPropertiesToExternalArrayData(data, type, size);
}


void HasExternalData(const FunctionCallbackInfo<Value>& args) {
  Environment* env = Environment::GetCurrent(args.GetIsolate());
  args.GetReturnValue().Set(args[0]->IsObject() &&
                            HasExternalData(env, args[0].As<Object>()));
}


bool HasExternalData(Environment* env, Local<Object> obj) {
  return obj->HasIndexedPropertiesInExternalArrayData();
}


class RetainedAllocInfo: public RetainedObjectInfo {
 public:
  explicit RetainedAllocInfo(Handle<Value> wrapper);

  virtual void Dispose();
  virtual bool IsEquivalent(RetainedObjectInfo* other);
  virtual intptr_t GetHash();
  virtual const char* GetLabel();
  virtual intptr_t GetSizeInBytes();

 private:
  static const char label_[];
  char* data_;
  int length_;
};


const char RetainedAllocInfo::label_[] = "smalloc";


RetainedAllocInfo::RetainedAllocInfo(Handle<Value> wrapper) {
  Local<Object> obj = wrapper->ToObject();
  length_ = obj->GetIndexedPropertiesExternalArrayDataLength();
  data_ = static_cast<char*>(obj->GetIndexedPropertiesExternalArrayData());
}


void RetainedAllocInfo::Dispose() {
  delete this;
}


bool RetainedAllocInfo::IsEquivalent(RetainedObjectInfo* other) {
  return label_ == other->GetLabel() &&
         data_ == static_cast<RetainedAllocInfo*>(other)->data_;
}


intptr_t RetainedAllocInfo::GetHash() {
  return reinterpret_cast<intptr_t>(data_);
}


const char* RetainedAllocInfo::GetLabel() {
  return label_;
}


intptr_t RetainedAllocInfo::GetSizeInBytes() {
  return length_;
}


RetainedObjectInfo* WrapperInfo(uint16_t class_id, Handle<Value> wrapper) {
  return new RetainedAllocInfo(wrapper);
}


void Initialize(Handle<Object> exports,
                Handle<Value> unused,
                Handle<Context> context) {
  Environment* env = Environment::GetCurrent(context);

  NODE_SET_METHOD(exports, "copyOnto", CopyOnto);
  NODE_SET_METHOD(exports, "sliceOnto", SliceOnto);

  NODE_SET_METHOD(exports, "alloc", Alloc);
  NODE_SET_METHOD(exports, "dispose", AllocDispose);

  NODE_SET_METHOD(exports, "hasExternalData", HasExternalData);

  exports->Set(FIXED_ONE_BYTE_STRING(env->isolate(), "kMaxLength"),
               Uint32::NewFromUnsigned(env->isolate(), kMaxLength));

  HeapProfiler* heap_profiler = env->isolate()->GetHeapProfiler();
  heap_profiler->SetWrapperClassInfoProvider(ALLOC_ID, WrapperInfo);
}


}  // namespace smalloc
}  // namespace node

NODE_MODULE_CONTEXT_AWARE_BUILTIN(smalloc, node::smalloc::Initialize)
