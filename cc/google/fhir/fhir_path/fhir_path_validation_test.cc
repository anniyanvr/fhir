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

#include "google/fhir/fhir_path/fhir_path_validation.h"

#include <ostream>
#include <string>
#include <utility>

#include "gmock/gmock.h"
#include "gtest/gtest.h"
#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "google/fhir/error_reporter.h"
#include "google/fhir/r4/primitive_handler.h"
#include "google/fhir/status/status.h"
#include "google/fhir/status/statusor.h"
#include "google/fhir/stu3/primitive_handler.h"
#include "google/fhir/testutil/fhir_test_env.h"
#include "proto/google/fhir/proto/r4/core/datatypes.pb.h"
#include "proto/google/fhir/proto/r4/core/resources/bundle_and_contained_resource.pb.h"
#include "proto/google/fhir/proto/r4/core/resources/encounter.pb.h"
#include "proto/google/fhir/proto/r4/core/resources/medication_knowledge.pb.h"
#include "proto/google/fhir/proto/r4/core/resources/observation.pb.h"
#include "proto/google/fhir/proto/r4/core/resources/organization.pb.h"
#include "proto/google/fhir/proto/r4/core/resources/value_set.pb.h"
#include "proto/google/fhir/proto/r4/uscore.pb.h"
#include "proto/google/fhir/proto/r4/uscore_codes.pb.h"
#include "proto/google/fhir/proto/stu3/codes.pb.h"
#include "proto/google/fhir/proto/stu3/datatypes.pb.h"
#include "proto/google/fhir/proto/stu3/metadatatypes.pb.h"
#include "proto/google/fhir/proto/stu3/resources.pb.h"
#include "proto/google/fhir/proto/stu3/uscore.pb.h"
#include "proto/google/fhir/proto/stu3/uscore_codes.pb.h"
#include "google/protobuf/message.h"
#include "google/protobuf/text_format.h"

namespace google {
namespace fhir {
namespace fhir_path {

// Provides a human-readable string representation of a ValidationResult object
// for googletest.
//
// https://github.com/google/googletest/blob/master/googletest/docs/advanced.md#teaching-googletest-how-to-print-your-values
void PrintTo(const ValidationResult& result, std::ostream* os) {
  *os << "[constraint = \"" << result.Constraint() << "\", constraint_path = \""
      << result.ConstraintPath() << "\", node_path = " << result.NodePath()
      << "\", result = "
      << (result.EvaluationResult().ok()
              ? (result.EvaluationResult().value() ? "true" : "false")
              : result.EvaluationResult().status().ToString())
      << "]";
}

namespace {

using ::testing::AllOf;
using ::testing::Contains;
using ::testing::Eq;
using ::testing::IsSupersetOf;
using ::testing::Property;
using ::testing::ResultOf;
using ::testing::StrEq;

static ::google::protobuf::TextFormat::Parser parser;  // NOLINT

template <typename T>
class FhirPathValidationTest : public ::testing::Test {
 public:
  static absl::StatusOr<ValidationResults> Validate(
      const ::google::protobuf::Message& message) {
    return FhirPathValidator::GetInstance().Validate(
        message, T::PrimitiveHandler::GetInstance());
  }

  static void ExpectValid(const ::google::protobuf::Message& message) {
    FHIR_ASSERT_OK_AND_ASSIGN(ValidationResults results,
                              FhirPathValidator::GetInstance().Validate(
                                  message, T::PrimitiveHandler::GetInstance()));
    EXPECT_TRUE(results.IsValid());
  }
};

struct Stu3CoreTestEnv : public testutil::Stu3CoreTestEnv {
  using SimpleQuantity = ::google::fhir::stu3::proto::SimpleQuantity;
  using PrimitiveHandler = stu3::Stu3PrimitiveHandler;
};

struct R4CoreTestEnv : public testutil::R4CoreTestEnv {
  using SimpleQuantity = ::google::fhir::r4::core::SimpleQuantity;
  using PrimitiveHandler = r4::R4PrimitiveHandler;
};

using TesEnvs = ::testing::Types<Stu3CoreTestEnv, R4CoreTestEnv>;
TYPED_TEST_SUITE(FhirPathValidationTest, TesEnvs);

template <typename T>
T ParseFromString(const std::string& str) {
  google::protobuf::TextFormat::Parser parser;
  parser.AllowPartialMessage(true);
  T t;
  EXPECT_TRUE(parser.ParseFromString(str, &t));
  return t;
}

template <typename T>
T ValidObservation() {
  return ParseFromString<T>(R"pb(
    status { value: FINAL }
    code {
      coding {
        system { value: "foo" }
        code { value: "bar" }
      }
    }
    id { value: "123" }
  )pb");
}

template <typename T>
T ValidValueSet() {
  return ParseFromString<T>(R"pb(
    url { value: "http://example.com/valueset" }
  )pb");
}

template <typename T>
T ValidUsCorePatient() {
  return ParseFromString<T>(R"pb(
    identifier {
      system { value: "foo" },
      value: { value: "http://example.com/patient" }
    }
  )pb");
}

TYPED_TEST(FhirPathValidationTest, EmptyContainedResource) {
  auto resource = ParseFromString<typename TypeParam::ContainedResource>("");

  FHIR_ASSERT_STATUS(TestFixture::Validate(resource).status(),
                     "ContainedResource is empty.");
}

TYPED_TEST(FhirPathValidationTest, ConstraintViolation) {
  auto organization = ParseFromString<typename TypeParam::Organization>(
      R"pb(
        name: { value: 'myorg' }
        telecom: { use: { value: HOME } }
      )pb");

  absl::StatusOr<ValidationResults> results =
      TestFixture::Validate(organization);
  FHIR_ASSERT_OK(results.status());
  EXPECT_FALSE(results->IsValid());

  EXPECT_THAT(results->Results(),
              Contains(AllOf(
                  Property(&ValidationResult::Constraint,
                           StrEq("where(use = 'home').empty()")),
                  Property(&ValidationResult::ConstraintPath,
                           StrEq("Organization.telecom")),
                  Property(&ValidationResult::NodePath,
                           StrEq("Organization.telecom[0]")),
                  ResultOf([](auto x) { return x.EvaluationResult().value(); },
                           Eq(false)))));
}

TYPED_TEST(FhirPathValidationTest, ConstraintViolationResultPaths) {
  auto bundle = ParseFromString<typename TypeParam::Bundle>(
      R"pb(entry: {
             resource: { organization: { telecom: { use: { value: HOME } } } }
           })pb");

  FHIR_ASSERT_OK_AND_ASSIGN(ValidationResults results,
                            TestFixture::Validate(bundle));

  ASSERT_THAT(
      results.Results(),
      IsSupersetOf({AllOf(
          Property(&ValidationResult::Constraint,
                   StrEq("where(use = 'home').empty()")),
          Property(&ValidationResult::ConstraintPath,
                   StrEq("Bundle.entry.resource.ofType(Organization).telecom")),
          Property(&ValidationResult::NodePath,
                   StrEq("Bundle.entry[0].resource.ofType(Organization)."
                         "telecom[0]")))}));
}

TYPED_TEST(FhirPathValidationTest, ConstraintSatisfied) {
  auto observation = ValidObservation<typename TypeParam::Observation>();

  // Ensure constraint succeeds with a value in the reference range
  // as required by FHIR.
  auto ref_range = observation.add_reference_range();

  auto value = new typename TypeParam::Decimal();
  value->set_allocated_value(new std::string("123.45"));

  auto high = new typename TypeParam::SimpleQuantity();
  high->set_allocated_value(value);

  ref_range->set_allocated_high(high);

  TestFixture::ExpectValid(observation);
}

TYPED_TEST(FhirPathValidationTest, NestedConstraintViolated) {
  auto value_set = ValidValueSet<typename TypeParam::ValueSet>();
  auto expansion = new typename TypeParam::ValueSet::Expansion;

  // Add empty contains structure to violate FHIR constraint.
  expansion->add_contains();
  value_set.mutable_name()->set_value("Placeholder");
  value_set.set_allocated_expansion(expansion);

  FHIR_ASSERT_OK_AND_ASSIGN(ValidationResults results,
                            TestFixture::Validate(value_set));
  EXPECT_FALSE(results.IsValid());

  ASSERT_THAT(results.Results(),
              Contains(AllOf(
                  Property(&ValidationResult::Constraint,
                           StrEq("code.exists() or display.exists()")),
                  Property(&ValidationResult::ConstraintPath,
                           StrEq("ValueSet.expansion.contains")),
                  Property(&ValidationResult::NodePath,
                           StrEq("ValueSet.expansion.contains[0]")),
                  ResultOf([](auto x) { return x.EvaluationResult().value(); },
                           Eq(false)))));
}

TYPED_TEST(FhirPathValidationTest, NestedConstraintSatisfied) {
  auto value_set = ValidValueSet<typename TypeParam::ValueSet>();
  value_set.mutable_name()->set_value("Placeholder");

  auto expansion = new typename TypeParam::ValueSet::Expansion;
  auto contains = expansion->add_contains();

  // Contains struct has value to satisfy FHIR constraint.
  auto proto_string = new typename TypeParam::String();
  proto_string->set_value("Placeholder value");
  contains->set_allocated_display(proto_string);

  auto proto_boolean = new typename TypeParam::Boolean();
  proto_boolean->set_value(true);
  contains->set_allocated_abstract(proto_boolean);

  value_set.set_allocated_expansion(expansion);

  TestFixture::ExpectValid(value_set);
}

TYPED_TEST(FhirPathValidationTest, MessageLevelConstraint) {
  auto period = ParseFromString<typename TypeParam::Period>(R"pb(
    start: { value_us: 1556750000000000 timezone: "America/Los_Angeles" }
    end: { value_us: 1556750153000000 timezone: "America/Los_Angeles" }
  )pb");

  TestFixture::ExpectValid(period);
}

// TODO(b/148992850): Templatize tests to work with both STU3 and R4
TEST(FhirPathValidationTest, MessageLevelConstraintViolated) {
  auto end_before_start_period = ParseFromString<r4::core::Period>(R"pb(
    start: { value_us: 1556750153000000 timezone: "America/Los_Angeles" }
    end: { value_us: 1556750000000000 timezone: "America/Los_Angeles" }
  )pb");

  FHIR_ASSERT_OK_AND_ASSIGN(
      ValidationResults results,
      FhirPathValidator::GetInstance().Validate(
          end_before_start_period, r4::R4PrimitiveHandler::GetInstance()));
  EXPECT_FALSE(results.IsValid());
}

TYPED_TEST(FhirPathValidationTest, NestedMessageLevelConstraint) {
  auto start_with_no_end_encounter =
      ParseFromString<typename TypeParam::Encounter>(R"pb(
        status { value: TRIAGED }
        id { value: "123" }
        period {
          start: { value_us: 1556750153000000 timezone: "America/Los_Angeles" }
        }
      )pb");

  TestFixture::ExpectValid(start_with_no_end_encounter);
}

TEST(FhirPathValidationTest, NestedMessageLevelConstraintViolated) {
  auto end_before_start_encounter = ParseFromString<r4::core::Encounter>(R"pb(
    status { value: TRIAGED }
    id { value: "123" }
    period {
      start: { value_us: 1556750153000000 timezone: "America/Los_Angeles" }
      end: { value_us: 1556750000000000 timezone: "America/Los_Angeles" }
    }
  )pb");

  EXPECT_FALSE(FhirPathValidator::GetInstance()
                   .Validate(end_before_start_encounter,
                             r4::R4PrimitiveHandler::GetInstance(),
                             FailFastErrorHandler::FailOnErrorOrFatal())
                   .ok());
}

// TODO(b/148992850): Templatize tests to work with both STU3 and R4
TEST(FhirPathValidationTest, ProfiledEmptyExtension) {
  r4::uscore::USCorePatientProfile patient =
      ValidUsCorePatient<r4::uscore::USCorePatientProfile>();

  FHIR_ASSERT_OK_AND_ASSIGN(
      ValidationResults results,
      FhirPathValidator::GetInstance().Validate(
          patient, r4::R4PrimitiveHandler::GetInstance()));
  EXPECT_TRUE(results.IsValid());
}

TEST(FhirPathValidationTest, ProfiledWithExtensionsR4) {
  auto patient = ValidUsCorePatient<r4::uscore::USCorePatientProfile>();
  auto race = new r4::uscore::PatientUSCoreRaceExtension();

  r4::uscore::PatientUSCoreRaceExtension::OmbCategoryCoding* coding =
      race->add_omb_category();
  coding->mutable_code()->set_value(
      r4::uscore::OmbRaceCategoriesValueSet::AMERICAN_INDIAN_OR_ALASKA_NATIVE);
  patient.set_allocated_race(race);

  patient.mutable_birthsex()->set_value(r4::uscore::BirthSexValueSet::M);

  FHIR_ASSERT_OK_AND_ASSIGN(
      ValidationResults results,
      FhirPathValidator::GetInstance().Validate(
          patient, r4::R4PrimitiveHandler::GetInstance()));
  EXPECT_TRUE(results.IsValid());
}

TEST(FhirPathValidationTest, ProfiledWithExtensionsSTU3) {
  auto patient = ValidUsCorePatient<stu3::uscore::UsCorePatient>();
  auto race = new stu3::uscore::PatientUSCoreRaceExtension();

  stu3::proto::Coding* coding = race->add_omb_category();
  coding->mutable_code()->set_value("urn:oid:2.16.840.1.113883.6.238");
  coding->mutable_code()->set_value("1002-5");
  patient.set_allocated_race(race);

  patient.mutable_birthsex()->set_value(stu3::uscore::UsCoreBirthSexCode::MALE);

  FHIR_ASSERT_OK_AND_ASSIGN(
      ValidationResults results,
      FhirPathValidator::GetInstance().Validate(
          patient, stu3::Stu3PrimitiveHandler::GetInstance()));
  EXPECT_TRUE(results.IsValid());
}

}  // namespace

}  // namespace fhir_path
}  // namespace fhir
}  // namespace google
