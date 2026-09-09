cmake_minimum_required(VERSION 3.16)
set(CMAKE_POLICY_VERSION_MINIMUM 3.5)

project(jh_game_server CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

include(FetchContent)

add_executable(jh_server
  src/main.cpp
  src/config.cpp
  src/crypto.cpp
  src/handlers.cpp
  src/admin.cpp
  src/auth.cpp
  src/session.cpp
  src/db.cpp
  src/storage.cpp
  src/mail.cpp
  src/save_crypto.cpp
  src/save_blob.cpp
  src/update.cpp
)

target_include_directories(jh_server PRIVATE include third_party)
target_compile_definitions(jh_server PRIVATE
  CPPHTTPLIB_FORM_URL_ENCODED_PAYLOAD_MAX_LENGTH=67108864
)

set(JH_MYSQL_HINTS
  "${CMAKE_SOURCE_DIR}/third_party/mysql"
  "${CMAKE_SOURCE_DIR}/third_party/mariadb-connector"
  "$ENV{MYSQL_DIR}"
  "C:/Program Files/MySQL/MySQL Server 8.0"
  "C:/Program Files/MySQL/MySQL Server 8.4"
  "C:/Program Files/MariaDB 11.4"
)

find_path(MYSQL_INCLUDE_DIR mysql.h
  HINTS ${JH_MYSQL_HINTS}
  PATH_SUFFIXES include include/mysql mysql
)

find_library(MYSQL_LIBRARY
  NAMES libmysql libmariadb mariadbclient mysqlclient
  HINTS ${JH_MYSQL_HINTS}
  PATH_SUFFIXES lib lib64 vs14
)

if(MYSQL_INCLUDE_DIR AND MYSQL_LIBRARY)
  message(STATUS "Using MySQL client: ${MYSQL_LIBRARY}")
  target_include_directories(jh_server PRIVATE ${MYSQL_INCLUDE_DIR})
  target_link_libraries(jh_server PRIVATE ${MYSQL_LIBRARY})
else()
  message(STATUS "System MySQL not found, building MariaDB Connector/C")
  set(INSTALL_PLUGINDIR "${CMAKE_BINARY_DIR}/mariadb_plugins" CACHE PATH "" FORCE)
  set(INSTALL_LIBDIR "lib" CACHE STRING "" FORCE)
  set(INSTALL_INCLUDEDIR "include" CACHE STRING "" FORCE)
  set(INSTALL_BINDIR "bin" CACHE STRING "" FORCE)
  set(WITH_UNIT_TESTS OFF CACHE BOOL "" FORCE)
  set(WITH_CURL OFF CACHE BOOL "" FORCE)
  set(WITH_SSL OFF CACHE BOOL "" FORCE)
  set(WITH_DYNCOL OFF CACHE BOOL "" FORCE)
  set(WITH_EXTERNAL_ZLIB OFF CACHE BOOL "" FORCE)

  FetchContent_Declare(mariadb_connector
    GIT_REPOSITORY https://github.com/mariadb-corporation/mariadb-connector-c.git
    GIT_TAG v3.3.10
    GIT_SHALLOW TRUE
  )
  FetchContent_MakeAvailable(mariadb_connector)

  target_link_libraries(jh_server PRIVATE mariadbclient)
  target_include_directories(jh_server PRIVATE
    ${mariadb_connector_SOURCE_DIR}/include
    ${mariadb_connector_BINARY_DIR}/include
  )
  add_custom_command(TARGET jh_server POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
    "$<TARGET_FILE:mariadbclient>"
    "$<TARGET_FILE_DIR:jh_server>"
  )
endif()

if(WIN32)
  target_link_libraries(jh_server PRIVATE ws2_32 secur32 shlwapi advapi32 crypt32)
  file(GLOB JH_MYSQL_DLLS
    "${CMAKE_SOURCE_DIR}/third_party/mysql/lib/libmysql.dll"
    "${CMAKE_SOURCE_DIR}/third_party/mysql/lib/libmariadb.dll"
    "${CMAKE_SOURCE_DIR}/third_party/mariadb-connector/lib/mariadb/mariadbclient.dll"
  )
  if(JH_MYSQL_DLLS)
    add_custom_command(TARGET jh_server POST_BUILD
      COMMAND ${CMAKE_COMMAND} -E copy_if_different
      "${JH_MYSQL_DLLS}"
      "$<TARGET_FILE_DIR:jh_server>"
    )
  endif()
else()
  find_package(Threads REQUIRED)
  target_link_libraries(jh_server PRIVATE Threads::Threads)
  find_package(PkgConfig QUIET)
  if(PkgConfig_FOUND)
    pkg_check_modules(MARIADB QUIET libmariadb)
  endif()
  if(MARIADB_FOUND)
    target_include_directories(jh_server PRIVATE ${MARIADB_INCLUDE_DIRS})
    target_link_libraries(jh_server PRIVATE ${MARIADB_LIBRARIES})
  else()
    target_link_libraries(jh_server PRIVATE mariadb mysqlclient)
  endif()
  target_compile_options(jh_server PRIVATE -O1 -g0)
endif()
