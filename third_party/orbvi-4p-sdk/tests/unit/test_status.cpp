#include "orbvi_sdk/status.hpp"

#include "test_support.hpp"

int main() {
  using orbvi_sdk::Result;
  using orbvi_sdk::Status;
  using orbvi_sdk::StatusCode;

  const Status ok;
  CHECK_TRUE(ok.ok());
  CHECK_EQ(ok.code(), StatusCode::ok);
  CHECK_TRUE(ok.message().empty());

  const Status error{StatusCode::not_supported, "no hardware backend"};
  CHECK_FALSE(error.ok());
  CHECK_EQ(error.code(), StatusCode::not_supported);
  CHECK_TRUE(error.to_string().find("not_supported") != std::string::npos);

  const Result<int> value{42};
  CHECK_TRUE(value.ok());
  CHECK_EQ(value.value(), 42);
  CHECK_TRUE(value.status().ok());

  const Result<int> failure{Status{StatusCode::unavailable, "offline"}};
  CHECK_FALSE(failure.ok());
  CHECK_EQ(failure.status().code(), StatusCode::unavailable);
  CHECK_EQ(failure.value_or(7), 7);

  return orbvi_test::finish();
}