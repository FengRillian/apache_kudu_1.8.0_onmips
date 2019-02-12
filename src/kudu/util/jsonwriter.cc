// Licensed to the Apache Software Foundation (ASF) under one
// or more contributor license agreements.  See the NOTICE file
// distributed with this work for additional information
// regarding copyright ownership.  The ASF licenses this file
// to you under the Apache License, Version 2.0 (the
// "License"); you may not use this file except in compliance
// with the License.  You may obtain a copy of the License at
//
//   http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing,
// software distributed under the License is distributed on an
// "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
// KIND, either express or implied.  See the License for the
// specific language governing permissions and limitations
// under the License.

#include "kudu/util/jsonwriter.h"

#include <new>
#include <sstream>
#include <string>
#include <vector>

#include <glog/logging.h>
#include <google/protobuf/descriptor.h>
#include <google/protobuf/descriptor.pb.h>
#include <google/protobuf/message.h>
#include <rapidjson/writer.h>
#include <rapidjson/prettywriter.h>
#include <rapidjson/rapidjson.h>

#include "kudu/gutil/port.h"
#include "kudu/util/faststring.h"
#include "kudu/util/logging.h"
#include "kudu/util/pb_util.pb.h"

using google::protobuf::FieldDescriptor;
using google::protobuf::Message;
using google::protobuf::Reflection;

using std::ostringstream;
using std::string;
using std::vector;

namespace kudu {

// Adapter to allow RapidJSON to write directly to a stringstream.
// Since Squeasel exposes a stringstream as its interface, this is needed to avoid overcopying.
class UTF8StringStreamBuffer {
 public:
  explicit UTF8StringStreamBuffer(std::ostringstream* out);
  ~UTF8StringStreamBuffer();
  void Put(rapidjson::UTF8<>::Ch c);

  void Flush();

 private:
  faststring buf_;
  std::ostringstream* out_;
};

// rapidjson doesn't provide any common interface between the PrettyWriter and
// Writer classes. So, we create our own pure virtual interface here, and then
// use JsonWriterImpl<T> below to make the two different rapidjson implementations
// correspond to this subclass.
class JsonWriterIf {
 public:
  virtual void Null() = 0;
  virtual void Bool(bool b) = 0;
  virtual void Int(int i) = 0;
  virtual void Uint(unsigned u) = 0;
  virtual void Int64(int64_t i64) = 0;
  virtual void Uint64(uint64_t u64) = 0;
  virtual void Double(double d) = 0;
  virtual void String(const char* str, size_t length) = 0;
  virtual void String(const char* str) = 0;
  virtual void String(const std::string& str) = 0;

  virtual void StartObject() = 0;
  virtual void EndObject() = 0;
  virtual void StartArray() = 0;
  virtual void EndArray() = 0;

  virtual ~JsonWriterIf() {}
};

// Adapts the different rapidjson Writer implementations to our virtual
// interface above.
template<class T>
class JsonWriterImpl : public JsonWriterIf {
 public:
  explicit JsonWriterImpl(ostringstream* out);

  virtual void Null() OVERRIDE;
  virtual void Bool(bool b) OVERRIDE;
  virtual void Int(int i) OVERRIDE;
  virtual void Uint(unsigned u) OVERRIDE;
  virtual void Int64(int64_t i64) OVERRIDE;
  virtual void Uint64(uint64_t u64) OVERRIDE;
  virtual void Double(double d) OVERRIDE;
  virtual void String(const char* str, size_t length) OVERRIDE;
  virtual void String(const char* str) OVERRIDE;
  virtual void String(const std::string& str) OVERRIDE;

  virtual void StartObject() OVERRIDE;
  virtual void EndObject() OVERRIDE;
  virtual void StartArray() OVERRIDE;
  virtual void EndArray() OVERRIDE;

 private:
  UTF8StringStreamBuffer stream_;
  T writer_;
  DISALLOW_COPY_AND_ASSIGN(JsonWriterImpl);
};

//
// JsonWriter
//

typedef rapidjson::PrettyWriter<UTF8StringStreamBuffer> PrettyWriterClass;
typedef rapidjson::Writer<UTF8StringStreamBuffer> CompactWriterClass;

JsonWriter::JsonWriter(ostringstream* out, Mode m) {
  switch (m) {
    case PRETTY:
      impl_.reset(new JsonWriterImpl<PrettyWriterClass>(DCHECK_NOTNULL(out)));
      break;
    case COMPACT:
      impl_.reset(new JsonWriterImpl<CompactWriterClass>(DCHECK_NOTNULL(out)));
      break;
  }
}
JsonWriter::~JsonWriter() {
}

void JsonWriter::Null() { impl_->Null(); }
void JsonWriter::Bool(bool b) { impl_->Bool(b); }
void JsonWriter::Int(int i) { impl_->Int(i); }
void JsonWriter::Uint(unsigned u) { impl_->Uint(u); }
void JsonWriter::Int64(int64_t i64) { impl_->Int64(i64); }
void JsonWriter::Uint64(uint64_t u64) { impl_->Uint64(u64); }
void JsonWriter::Double(double d) { impl_->Double(d); }
void JsonWriter::String(const char* str, size_t length) { impl_->String(str, length); }
void JsonWriter::String(const char* str) { impl_->String(str); }
void JsonWriter::String(const string& str) { impl_->String(str); }
void JsonWriter::StartObject() { impl_->StartObject(); }
void JsonWriter::EndObject() { impl_->EndObject(); }
void JsonWriter::StartArray() { impl_->StartArray(); }
void JsonWriter::EndArray() { impl_->EndArray(); }

// Specializations for common primitive metric types.
template<> void JsonWriter::Value(const bool& val) {
  Bool(val);
}
template<> void JsonWriter::Value(const int32_t& val) {
  Int(val);
}
template<> void JsonWriter::Value(const uint32_t& val) {
  Uint(val);
}
template<> void JsonWriter::Value(const int64_t& val) {
  Int64(val);
}
template<> void JsonWriter::Value(const uint64_t& val) {
  Uint64(val);
}
template<> void JsonWriter::Value(const double& val) {
  Double(val);
}
template<> void JsonWriter::Value(const string& val) {
  String(val);
}

#if defined(__APPLE__)
template<> void JsonWriter::Value(const size_t& val) {
  Uint64(val);
}
#endif

void JsonWriter::Protobuf(const Message& pb) {
  const Reflection* reflection = pb.GetReflection();
  vector<const FieldDescriptor*> fields;
  reflection->ListFields(pb, &fields);

  StartObject();
  for (const FieldDescriptor* field : fields) {
    String(field->name());
    if (field->is_repeated()) {
      StartArray();
      int size = reflection->FieldSize(pb, field);
      for (int i = 0; i < size; i++) {
        ProtobufRepeatedField(pb, reflection, field, i);
      }
      EndArray();
    } else {
      ProtobufField(pb, reflection, field);
    }
  }
  EndObject();
}

void JsonWriter::ProtobufField(const Message& pb, const Reflection* reflection,
                               const FieldDescriptor* field) {
  switch (field->cpp_type()) {
    case FieldDescriptor::CPPTYPE_INT32:
      Int(reflection->GetInt32(pb, field));
      break;
    case FieldDescriptor::CPPTYPE_INT64:
      Int64(reflection->GetInt64(pb, field));
      break;
    case FieldDescriptor::CPPTYPE_UINT32:
      Uint(reflection->GetUInt32(pb, field));
      break;
    case FieldDescriptor::CPPTYPE_UINT64:
      Uint64(reflection->GetUInt64(pb, field));
      break;
    case FieldDescriptor::CPPTYPE_DOUBLE:
      Double(reflection->GetDouble(pb, field));
      break;
    case FieldDescriptor::CPPTYPE_FLOAT:
      Double(reflection->GetFloat(pb, field));
      break;
    case FieldDescriptor::CPPTYPE_BOOL:
      Bool(reflection->GetBool(pb, field));
      break;
    case FieldDescriptor::CPPTYPE_ENUM:
      String(reflection->GetEnum(pb, field)->name());
      break;
    case FieldDescriptor::CPPTYPE_STRING:
      String(KUDU_MAYBE_REDACT_IF(field->options().GetExtension(REDACT),
                                  reflection->GetString(pb, field)));
      break;
    case FieldDescriptor::CPPTYPE_MESSAGE:
      Protobuf(reflection->GetMessage(pb, field));
      break;
    default:
      LOG(FATAL) << "Unknown cpp_type: " << field->cpp_type();
  }
}

void JsonWriter::ProtobufRepeatedField(const Message& pb, const Reflection* reflection,
                                       const FieldDescriptor* field, int index) {
  switch (field->cpp_type()) {
    case FieldDescriptor::CPPTYPE_INT32:
      Int(reflection->GetRepeatedInt32(pb, field, index));
      break;
    case FieldDescriptor::CPPTYPE_INT64:
      Int64(reflection->GetRepeatedInt64(pb, field, index));
      break;
    case FieldDescriptor::CPPTYPE_UINT32:
      Uint(reflection->GetRepeatedUInt32(pb, field, index));
      break;
    case FieldDescriptor::CPPTYPE_UINT64:
      Uint64(reflection->GetRepeatedUInt64(pb, field, index));
      break;
    case FieldDescriptor::CPPTYPE_DOUBLE:
      Double(reflection->GetRepeatedDouble(pb, field, index));
      break;
    case FieldDescriptor::CPPTYPE_FLOAT:
      Double(reflection->GetRepeatedFloat(pb, field, index));
      break;
    case FieldDescriptor::CPPTYPE_BOOL:
      Bool(reflection->GetRepeatedBool(pb, field, index));
      break;
    case FieldDescriptor::CPPTYPE_ENUM:
      String(reflection->GetRepeatedEnum(pb, field, index)->name());
      break;
    case FieldDescriptor::CPPTYPE_STRING:
      String(KUDU_MAYBE_REDACT_IF(field->options().GetExtension(REDACT),
                                  reflection->GetRepeatedString(pb, field, index)));
      break;
    case FieldDescriptor::CPPTYPE_MESSAGE:
      Protobuf(reflection->GetRepeatedMessage(pb, field, index));
      break;
    default:
      LOG(FATAL) << "Unknown cpp_type: " << field->cpp_type();
  }
}

string JsonWriter::ToJson(const Message& pb, Mode mode) {
  ostringstream stream;
  JsonWriter writer(&stream, mode);
  writer.Protobuf(pb);
  return stream.str();
}

//
// UTF8StringStreamBuffer
//

UTF8StringStreamBuffer::UTF8StringStreamBuffer(std::ostringstream* out)
  : out_(DCHECK_NOTNULL(out)) {
}
UTF8StringStreamBuffer::~UTF8StringStreamBuffer() {
  DCHECK_EQ(buf_.size(), 0) << "Forgot to flush!";
}

void UTF8StringStreamBuffer::Put(rapidjson::UTF8<>::Ch c) {
  buf_.push_back(c);
}

void UTF8StringStreamBuffer::Flush() {
  out_->write(reinterpret_cast<char*>(buf_.data()), buf_.size());
  buf_.clear();
}

//
// JsonWriterImpl: simply forward to the underlying implementation.
//

template<class T>
JsonWriterImpl<T>::JsonWriterImpl(ostringstream* out)
  : stream_(DCHECK_NOTNULL(out)),
    writer_(stream_) {
}
template<class T>
void JsonWriterImpl<T>::Null() { writer_.Null(); }
template<class T>
void JsonWriterImpl<T>::Bool(bool b) { writer_.Bool(b); }
template<class T>
void JsonWriterImpl<T>::Int(int i) { writer_.Int(i); }
template<class T>
void JsonWriterImpl<T>::Uint(unsigned u) { writer_.Uint(u); }
template<class T>
void JsonWriterImpl<T>::Int64(int64_t i64) { writer_.Int64(i64); }
template<class T>
void JsonWriterImpl<T>::Uint64(uint64_t u64) { writer_.Uint64(u64); }
template<class T>
void JsonWriterImpl<T>::Double(double d) { writer_.Double(d); }
template<class T>
void JsonWriterImpl<T>::String(const char* str, size_t length) { writer_.String(str, length); }
template<class T>
void JsonWriterImpl<T>::String(const char* str) { writer_.String(str); }
template<class T>
void JsonWriterImpl<T>::String(const string& str) { writer_.String(str.c_str(), str.length()); }
template<class T>
void JsonWriterImpl<T>::StartObject() { writer_.StartObject(); }
template<class T>
void JsonWriterImpl<T>::EndObject() {
  writer_.EndObject();
  stream_.Flush();
}
template<class T>
void JsonWriterImpl<T>::StartArray() { writer_.StartArray(); }
template<class T>
void JsonWriterImpl<T>::EndArray() {
  writer_.EndArray();
  stream_.Flush();
}

} // namespace kudu
