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

#ifndef GOOGLE_FHIR_FHIR_PATH_FHIR_PATH_VALIDATION_H_
#define GOOGLE_FHIR_FHIR_PATH_FHIR_PATH_VALIDATION_H_

#include <functional>
#include <memory>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "absl/base/attributes.h"
#include "absl/base/thread_annotations.h"
#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "absl/strings/string_view.h"
#include "absl/synchronization/mutex.h"
#include "google/fhir/error_reporter.h"
#include "google/fhir/fhir_path/fhir_path.h"
#include "google/fhir/primitive_handler.h"
#include "google/protobuf/descriptor.h"
#include "google/protobuf/message.h"

namespace google {
namespace fhir {
namespace fhir_path {

// Class the holds the results of evaluating a FHIRPath constraint on a
// FHIR resource.
class ValidationResult {
 public:
  ValidationResult(absl::string_view constraint_path,
                   absl::string_view node_path,
                   absl::string_view fhirpath_constraint,
                   absl::StatusOr<bool> result)
      : constraint_path_(constraint_path),
        node_path_(node_path),
        fhirpath_constraint_(fhirpath_constraint),
        result_(result) {}

  // Returns a FHIRPath expression to the generic node that the FHIRPath
  // constraint is attached to.
  //
  // Example: "Bundle.entry.resource.ofType(Organization).telecom"
  //
  std::string ConstraintPath() const { return constraint_path_; }

  // Returns a FHIRPath expression to the specific node that the FHIRPath
  // constraint was evaluated on.
  //
  // Example: "Bundle.entry[3].resource.ofType(Organization).telecom[2]"
  //
  std::string NodePath() const { return node_path_; }

  // Returns the FHIRPath constraint that was evaluated.
  std::string Constraint() const { return fhirpath_constraint_; }

  // Returns the result of evaluating the FHIRPath constraint.
  //
  // For constraints that fail to compile/evaluate or do not evaluate to a
  // boolean, a status other than OK is returned.
  absl::StatusOr<bool> EvaluationResult() const { return result_; }

 private:
  const std::string constraint_path_;
  const std::string node_path_;
  const std::string fhirpath_constraint_;
  const absl::StatusOr<bool> result_;
};

// A ValidationRule is a function that takes a ValidationResult and returns
// false if the resource under consideration should be considered invalid and
// true in all other cases. In order for a resource to be considered valid, from
// the perspective of FHIRPath, this function must return true for all
// ValidationResult objects for that resource.
using ValidationRule = std::function<bool(const ValidationResult&)>;

// Class the holds the results of evaluating all FHIRPath constraints defined
// on a particular resource.
class ValidationResults {
 public:
  // Returns the result of the constraint's evaluation, if it evaluated to a
  // boolean. Otherwise returns true. Common causes of an expression failing to
  // evaluate to a boolean could be:
  //   - the constraint uses a portion of FHIRPath not currently supported by
  //     this library
  //   - the constraint is not a valid FHIRPath expression
  //   - the constraint does not yield a boolean value
  static bool StrictValidationFn(const ValidationResult& result);

  // Returns true if the result of the constraint's evaluation is true. False
  // if the constraint was unmet or failed to evaluate to a boolean.
  static bool RelaxedValidationFn(const ValidationResult& result);

  explicit ValidationResults(std::vector<ValidationResult> results)
      : results_(results) {}

  // Returns true if all FHIRPath constraints on the particular resource satisfy
  // the provided validation function.
  //
  // See ValidationResults::StrictValidationFn and
  // ValidationResults::RelaxedValidationFn for common definitions of validity.
  bool IsValid(ValidationRule validation_fn =
                   &ValidationResults::StrictValidationFn) const;

  // Returns Status::OK or the status of the first constraint violation
  // encountered.
  absl::Status LegacyValidationResult() const;

  // Returns the result for each FHIRPath expressions that was evaluated.
  // TODO(b/151745508): Expose expressions that failed to compile.
  std::vector<ValidationResult> Results() const;

 private:
  const std::vector<ValidationResult> results_;
};

// This class validates that all fhir_path_constraint annotations on
// the given messages are valid. It will compile and cache the
// constraint expressions as it encounters them, so users are encouraged
// to create a single instance of this for the lifetime of the process.
// This class is thread safe.
class FhirPathValidator {
 public:
  // Validates the fhir_path_constraint annotations on the given message.
  ABSL_MUST_USE_RESULT
  absl::Status Validate(const ::google::protobuf::Message& message,
                        const PrimitiveHandler* primitive_handler,
                        ErrorHandler& error_handler) const;

  // Validates the fhir_path_constraint annotations on the given message,
  // and returns a ValidationResults object.
  // Deprecated - use the variant that takes an ErrorHandler.
  ABSL_MUST_USE_RESULT
  absl::StatusOr<ValidationResults> Validate(
      const ::google::protobuf::Message& message,
      const PrimitiveHandler* primitive_handler) const;

  // Provide a singleton instance to make use of efficient caching.
  static const FhirPathValidator& GetInstance() {
    static const FhirPathValidator* const instance = new FhirPathValidator();
    return *instance;
  }

 private:
  // This class should only be referenced via the singleton instance.
  FhirPathValidator() = default;

  // A cache of constraints for a given message definition
  struct MessageConstraints {
    // FHIRPath constraints at the "root" FHIR element, which is just the
    // protobuf message.
    std::vector<CompiledExpression> message_error_expressions;
    std::vector<CompiledExpression> message_warning_expressions;

    // FHIRPath constraints on fields
    std::vector<
        std::pair<const ::google::protobuf::FieldDescriptor*, const CompiledExpression>>
        field_error_expressions;
    std::vector<
        std::pair<const ::google::protobuf::FieldDescriptor*, const CompiledExpression>>
        field_warning_expressions;

    // Nested messages that have constraints, so the evaluation logic
    // knows to check them.
    std::vector<const ::google::protobuf::FieldDescriptor*> nested_with_constraints;
  };

  // Loads constraints for the given descriptor.
  MessageConstraints* ConstraintsFor(const ::google::protobuf::Descriptor* descriptor,
                                     const PrimitiveHandler* primitive_handler)
      const ABSL_SHARED_LOCKS_REQUIRED(mutex_);

  // Recursively called validation method that aggregates results into the
  // provided ScopedErrorReporter
  absl::Status Validate(const internal::WorkspaceMessage& message,
                        const PrimitiveHandler* primitive_handler,
                        const ScopedErrorReporter& error_reporter) const;

  mutable absl::Mutex mutex_;

  // Cache from Message descriptor full name to MessageConstraints for that
  // Message.  Note that since this uses full name, it includes version
  // information - e.g., R4 Patient will have a different key/entry than R5
  // Patient.
  // TODO(b/315352519): change this to a LRU cache to ensure it doesn't balloon.
  mutable std::unordered_map<std::string, std::unique_ptr<MessageConstraints>>
      constraints_cache_ ABSL_GUARDED_BY(mutex_);
};

}  // namespace fhir_path
}  // namespace fhir
}  // namespace google

#endif  // GOOGLE_FHIR_FHIR_PATH_FHIR_PATH_VALIDATION_H_
