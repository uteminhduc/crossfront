add_executable(CrossFrontSafetyTest
  CrossFrontSafetyTest.cpp
  ${REPO_ROOT}/src/crossfront/CrossFrontCrypto.cpp
)

target_include_directories(CrossFrontSafetyTest PRIVATE
  ${REPO_ROOT}/src
  ${CMAKE_CURRENT_SOURCE_DIR}/stubs
)

target_link_libraries(CrossFrontSafetyTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(CrossFrontSafetyTest)
