cmake_minimum_required(VERSION 3.16)
project(spark_email LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 11)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(OpenSSL REQUIRED)
find_package(Threads REQUIRED)

add_library(spark_email STATIC spark_email.cpp)
target_include_directories(spark_email PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(spark_email PRIVATE OpenSSL::SSL OpenSSL::Crypto Threads::Threads)

add_executable(demo_main demo_main.cpp)
target_link_libraries(demo_main PRIVATE spark_email)
