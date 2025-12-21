cmake_minimum_required(VERSION 3.20)
project(trace_client LANGUAGES C CXX)

find_package(DynamoRIO REQUIRED)
add_library(trace_client SHARED trace_client.cpp)
configure_DynamoRIO_client(trace_client)
use_DynamoRIO_extension(trace_client drmgr)

set_target_properties(trace_client PROPERTIES PREFIX "" SUFFIX ".dll")

target_compile_features(trace_client PRIVATE cxx_std_20)