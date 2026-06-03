#CMakeLists.txt
cmake_minimum_required(VERSION 3.10)
project(npserver_project C)
#Set C standard if needed (optional)
set(CMAKE_C_STANDARD 11)
#Define the executable
add_executable(npserver server.c)
# includes
target_include_directories(npserver PRIVATE
	${CMAKE_SOURCE_DIR}/include
)
#Find system OpenSSL if available
find_package(OpenSSL QUIET)
if(OpenSSL_FOUND)
	target_link_libraries(npserver PRIVATE OpenSSL::SSL OpenSSL::Crypto)
else()
#Fallback to local OpenSSL build
target_include_directories(npserver PRIVATE
	${CMAKE_SOURCE_DIR}/openssl-3.5.2/include
)
if(APPLE)
target_link_libraries(npserver PRIVATE
	${CMAKE_SOURCE_DIR}/openssl-3.5.2/libssl.dylib
	${CMAKE_SOURCE_DIR}/openssl-3.5.2/libcrypto.dylib
)
elseif(UNIX AND NOT APPLE)
target_link_libraries(npserver PRIVATE
	${CMAKE_SOURCE_DIR}/openssl-3.5.2/libssl.so
	${CMAKE_SOURCE_DIR}/openssl-3.5.2/libcrypto.so
)
else()
message(FATAL_ERROR "Platform not supported")
endif()
endif()
#Find zlib for gzip compression
find_package(ZLIB QUIET)
if(ZLIB_FOUND)
	target_link_libraries(npserver PRIVATE ZLIB::ZLIB)
	target_compile_definitions(npserver PRIVATE HAVE_ZLIB)
else()
	message(STATUS "zlib not found — gzip compression disabled")
endif()
#Add debug symbols (equivalent to -g3)
target_compile_options(npserver PRIVATE -g3 -O3)
#Define HAVE_OPENSSL preprocessor macro (equivalent to -DHAVE_OPENSSL)
target_compile_definitions(npserver PRIVATE HAVE_OPENSSL)
