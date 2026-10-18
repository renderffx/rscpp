cmake_minimum_required(VERSION 3.14)
project(rscpp VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_library(rsc_core INTERFACE)
target_include_directories(rsc_core INTERFACE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_CURRENT_SOURCE_DIR}/third_party/nlohmann)


add_library(rsc_httplib INTERFACE)
target_include_directories(rsc_httplib INTERFACE
    ${CMAKE_CURRENT_SOURCE_DIR}/third_party/httplib)


add_executable(rscpp_server
    src/server/main.cpp
    src/server/config.cpp
    src/server/delivery.cpp
    src/server/logging.cpp
    src/server/metrics.cpp
    src/server/server.cpp)

target_link_libraries(rscpp_server PRIVATE rsc_core rsc_httplib)
target_include_directories(rscpp_server PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${CMAKE_CURRENT_SOURCE_DIR}/src/server)

find_package(OpenSSL QUIET)
if(OPENSSL_FOUND)
  target_link_libraries(rscpp_server PRIVATE OpenSSL::SSL OpenSSL::Crypto)
endif()

if(WIN32)
  target_link_libraries(rscpp_server PRIVATE ws2_32)
endif()

if(MINGW)
  set(RSCPP_STATIC_RUNTIME -static-libgcc -static-libstdc++)


  set(RSCPP_STATIC_RUNTIME ${RSCPP_STATIC_RUNTIME}
      -Wl,-Bstatic -lwinpthread -Wl,-Bdynamic)
  target_link_options(rscpp_server PRIVATE ${RSCPP_STATIC_RUNTIME})
endif()

find_package(Threads REQUIRED)
target_link_libraries(rscpp_server PRIVATE Threads::Threads)

option(RSCPP_BUILD_TESTS "Build and register the C++ test suite" OFF)

if(RSCPP_BUILD_TESTS)
  enable_testing()
  message(STATUS "RSCPP_BUILD_TESTS=ON but tests/ is not present in this tree")
endif()

include(GNUInstallDirs)
install(TARGETS rscpp_server RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
install(DIRECTORY include/ DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
install(DIRECTORY web/ DESTINATION ${CMAKE_INSTALL_DATADIR}/rscpp/web)
install(FILES rscpp.ini DESTINATION ${CMAKE_INSTALL_DATADIR}/rscpp)
