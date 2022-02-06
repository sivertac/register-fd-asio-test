# CMakeLists.txt
cmake_minimum_required(VERSION 3.15.2)

set(CMAKE_ENABLE_EXPORTS ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

project(register-fd-asio-test VERSION 0.0.1 LANGUAGES CXX)

find_package(Boost REQUIRED)
find_package(Threads REQUIRED)
#find_package(X11 REQUIRED)
find_package(Libssh2 REQUIRED CONFIG)

#add_executable(x11-clipboard-asio
#    ${CMAKE_CURRENT_SOURCE_DIR}/src/x11clipboard_asio.cpp
#)
#target_include_directories(x11-clipboard-asio PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src)
#target_compile_features(x11-clipboard-asio PUBLIC cxx_std_20)
#target_include_directories(x11-clipboard-asio PUBLIC ${Boost_INCLUDE_DIRS})
#target_link_libraries(x11-clipboard-asio PUBLIC ${Boost_LIBRARIES})
#target_link_libraries(x11-clipboard-asio PUBLIC ${CMAKE_THREAD_LIBS_INIT})
#target_include_directories(x11-clipboard-asio PUBLIC ${X11_INCLUDE_DIR})
#target_link_libraries(x11-clipboard-asio PUBLIC ${X11_LIBRARIES})

add_executable(libssh2-asio
    ${CMAKE_CURRENT_SOURCE_DIR}/src/libssh2_asio.cpp
)
target_include_directories(libssh2-asio PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_compile_features(libssh2-asio PUBLIC cxx_std_20)
target_include_directories(libssh2-asio PUBLIC ${Libssh2_INLUDE_DIRS})
target_include_directories(libssh2-asio PUBLIC ${Boost_INCLUDE_DIRS})
target_link_libraries(libssh2-asio PUBLIC ${CMAKE_THREAD_LIBS_INIT})
target_link_libraries(libssh2-asio PUBLIC Libssh2::libssh2)