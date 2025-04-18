// Copyright 2019 Google LLC
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//      http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#ifndef GOOGLE_FHIR_STU3_CODEABLE_CONCEPTS_H_
#define GOOGLE_FHIR_STU3_CODEABLE_CONCEPTS_H_

#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "google/protobuf/descriptor.pb.h"
#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "absl/types/optional.h"
#include "google/fhir/annotations.h"
#include "google/fhir/fhir_types.h"
#include "google/fhir/proto_util.h"
#include "google/fhir/status/statusor.h"
#include "proto/google/fhir/proto/annotations.pb.h"
#include "proto/google/fhir/proto/stu3/datatypes.pb.h"
#include "google/protobuf/message.h"

namespace google {
namespace fhir {
namespace stu3 {

typedef std::function<void(const std::string& system, const std::string& code)>
    CodeFunc;
typedef std::function<bool(const std::string& system, const std::string& code)>
    CodeBoolFunc;
typedef std::function<void(const stu3::proto::Coding&)> CodingFunc;
typedef std::function<absl::Status(const stu3::proto::Coding&)>
    CodingStatusFunc;
typedef std::function<bool(const stu3::proto::Coding&)> CodingBoolFunc;

namespace internal {

// Gets a profiled field for a given system, or nullptr if none is found.
// This is internal, since outside callers shouldn't care about profiled vs
// unprofiled.
const ::google::protobuf::FieldDescriptor* ProfiledFieldForSystem(
    const ::google::protobuf::Message& concept_proto, const std::string& system);

}  // namespace internal

// Performs a function on all System/Code pairs, where all codes are treated
// like strings, until the function returns true.
// If the function returns true on a Coding, ceases visiting Codings and returns
// true.  If the function returned false for all Codings, returns false.
const bool FindSystemCodeStringPair(const ::google::protobuf::Message& concept_proto,
                                    const CodeBoolFunc& func,
                                    std::string* found_system,
                                    std::string* found_code);

// Variant of FindSystemCodeStringPair that does not explicitly set return
// pointers.
// This can be used when the actual result is unused, or when the coding is used
// to set a return pointers in the function closure itself.
const bool FindSystemCodeStringPair(const ::google::protobuf::Message& concept_proto,
                                    const CodeBoolFunc& func);

// Performs a function on all System/Code pairs, where all codes are treated
// like strings.  Visits all codings.
void ForEachSystemCodeStringPair(const ::google::protobuf::Message& concept_proto,
                                 const CodeFunc& func);

// Performs a function on all Codings, until the function returns true.
// If the function returns true on a Coding, ceases visiting Codings and returns
// a shared_ptr to that coding.  If the function returned false for all Codings,
// returns a null shared_ptr.
// For profiled fields that have no native Coding representation, constructs
// a synthetic Coding to operate on.
// The shared_ptr semantics are used to correctly manage the lifecycle of both
// cases:
//   * For raw codings, the shared_ptr has a no-op deleter, so lifecycle is
//     managed by the CodeableConcept.
//   * For synthetic codings, the returned shared_ptr has the default deleter,
//     so lifecycle is managed by the call site.
// Since the call site has no way of knowing which case it is, the shared_ptr
// semantics accurately convey to the caller that they should never count on
// the Coding living longer than the shared_ptr.
std::shared_ptr<const stu3::proto::Coding> FindCoding(
    const ::google::protobuf::Message& concept_proto, const CodingBoolFunc& func);

// Performs a function on all Codings.
// For profiled fields that have no native Coding representation, constructs
// a synthetic Coding.
void ForEachCoding(const ::google::protobuf::Message& concept_proto,
                   const CodingFunc& func);

// Similar to ForEachCoding, but takes a function that returns a status.
// If the function ever returns anything other than OK,
// this will halt and return that status.
absl::Status ForEachCodingWithStatus(const ::google::protobuf::Message& concept_proto,
                                     const CodingStatusFunc& func);

// Gets a vector of all codes with a given system, where codes are represented
// as strings.
const std::vector<std::string> GetCodesWithSystem(
    const ::google::protobuf::Message& concept_proto,
    const absl::string_view target_system);

// Gets the only code with a given system.
// If no code is found with that system, returns a NotFound status.
// If more than one code is found with that system, returns AlreadyExists
// status.
absl::StatusOr<const std::string> GetOnlyCodeWithSystem(
    const ::google::protobuf::Message& concept_proto, const absl::string_view system);

// Gets the first code with a given system.
// This differs from GetOnlyCodeWithSystem in that it doesn't throw an error
// if there are more than one codes with that syatem.
template <typename CodeableConceptLike>
absl::StatusOr<std::string> ExtractCodeBySystem(
    const CodeableConceptLike& codeable_concept,
    absl::string_view system_value) {
  const std::vector<std::string>& codes =
      GetCodesWithSystem(codeable_concept, system_value);
  if (codes.empty()) {
    return absl::NotFoundError(
        absl::StrCat("No code from system: ", system_value));
  }
  return codes.front();
}

// Adds a Coding to a CodeableConcept.
// TODO(b/244184211): This will have undefined behavior if there is a
// CodingWithFixedCode and CodingWithFixedSystem field with the same system.
// Think about this a bit more.  It might just be illegal since a code might
// fit into two different slices, but might be useful, e.g., if you want
// to specify a required ICD9 code, but make it easy to add other ICD9 codes.
absl::Status AddCoding(::google::protobuf::Message* concept_proto,
                       const stu3::proto::Coding& coding);

absl::Status AddCoding(::google::protobuf::Message* concept_proto,
                       const std::string& system, const std::string& code);

template <typename CodeableConceptLike>
absl::Status ClearAllCodingsWithSystem(CodeableConceptLike* concept_like,
                                       const std::string& system) {
  if (IsProfileOfCodeableConcept(*concept_like)) {
    const ::google::protobuf::FieldDescriptor* profiled_field =
        internal::ProfiledFieldForSystem(*concept_like, system);
    if (profiled_field != nullptr) {
      if (IsMessageType<stu3::proto::CodingWithFixedSystem>(
              profiled_field->message_type())) {
        concept_like->GetReflection()->ClearField(concept_like, profiled_field);
      } else if (IsMessageType<stu3::proto::CodingWithFixedCode>(
                     profiled_field->message_type())) {
        return ::absl::InvalidArgumentError(
            absl::StrCat("Cannot clear coding system: ", system, " from ",
                         concept_like->GetDescriptor()->full_name(),
                         ". It is a fixed code on that profile"));
      }
    }
  }
  for (auto iter = concept_like->mutable_coding()->begin();
       iter != concept_like->mutable_coding()->end();) {
    if (iter->has_system() && iter->system().value() == system) {
      iter = concept_like->mutable_coding()->erase(iter);
    } else {
      iter++;
    }
  }
  return absl::OkStatus();
}

absl::Status CopyCodeableConcept(const ::google::protobuf::Message& source,
                                 ::google::protobuf::Message* target);

int CodingSize(const ::google::protobuf::Message& concept_proto);

}  // namespace stu3
}  // namespace fhir
}  // namespace google

#endif  // GOOGLE_FHIR_STU3_CODEABLE_CONCEPTS_H_
