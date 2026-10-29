# Inverted TaskFlow Server
#   Linux (recommended):  cmake -S server -B build/server -DCMAKE_BUILD_TYPE=Release && cmake --build build/server -j
#   Needs: build-essential cmake libssl-dev   (TLS + e-mail).  -DTF_TLS=OFF builds without OpenSSL.
cmake_minimum_required(VERSION 3.16)
file(STRINGS ${CMAKE_CURRENT_SOURCE_DIR}/../common/version.h TF_VER_LINE REGEX "define TF_VERSION")
string(REGEX MATCH "[0-9]+\\.[0-9]+\\.[0-9]+" TF_VER "${TF_VER_LINE}")
project(InvertedTaskFlowServer VERSION ${TF_VER} LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release)
endif()
option(TF_TLS "HTTPS and encrypted e-mail (needs OpenSSL)" ON)
set(TP ${CMAKE_CURRENT_SOURCE_DIR}/../third_party)

add_library(tf_sqlite3 STATIC ${TP}/sqlite/sqlite3.c)
target_compile_definitions(tf_sqlite3 PRIVATE SQLITE_THREADSAFE=1 SQLITE_OMIT_LOAD_EXTENSION SQLITE_DQS=0
                           SQLITE_DEFAULT_FOREIGN_KEYS=1)
target_include_directories(tf_sqlite3 PUBLIC ${TP}/sqlite)

file(GLOB TF_SERVER_SRC CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/src/*.cpp)
add_executable(TaskFlowServer ${TF_SERVER_SRC})
target_include_directories(TaskFlowServer PRIVATE ${TP} ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(TaskFlowServer PRIVATE tf_sqlite3)
target_compile_definitions(TaskFlowServer PRIVATE CPPHTTPLIB_THREAD_POOL_COUNT=8)

if(TF_TLS)
  find_package(OpenSSL 1.1.1 REQUIRED)
  target_compile_definitions(TaskFlowServer PRIVATE CPPHTTPLIB_OPENSSL_SUPPORT)
  target_link_libraries(TaskFlowServer PRIVATE OpenSSL::SSL OpenSSL::Crypto)
endif()

if(WIN32)
  target_link_libraries(TaskFlowServer PRIVATE ws2_32 bcrypt crypt32 advapi32)
  if(MSVC)
    target_compile_options(TaskFlowServer PRIVATE /utf-8 /EHsc /W3)
    set_property(TARGET TaskFlowServer tf_sqlite3 PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded")
  else()
    target_link_options(TaskFlowServer PRIVATE -static)
  endif()
else()
  find_package(Threads REQUIRED)
  target_link_libraries(TaskFlowServer PRIVATE Threads::Threads)
  target_compile_options(TaskFlowServer PRIVATE -Wall -Wextra -Wno-unused-parameter -Wno-missing-field-initializers -fstack-protector-strong -D_FORTIFY_SOURCE=2)
  target_link_options(TaskFlowServer PRIVATE -Wl,-z,relro,-z,now -pie)
  set_property(TARGET TaskFlowServer PROPERTY POSITION_INDEPENDENT_CODE ON)
endif()
