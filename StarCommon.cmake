# Check whether the compiler supports c++11
include(CheckCXXCompilerFlag)
CHECK_CXX_COMPILER_FLAG("-std=c++11" COMPILER_SUPPORTS_CXX11)
CHECK_CXX_COMPILER_FLAG("-std=c++0x" COMPILER_SUPPORTS_CXX0X)
if(COMPILER_SUPPORTS_CXX11)
	set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -std=c++11")
elseif(COMPILER_SUPPORTS_CXX0X)
	set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -std=c++0x")
else()
	message(STATUS "The compiler ${CMAKE_CXX_COMPILER} has no C++11 support. Please use a different C++ compiler.")
endif()


# Since most of STAR projects depend on ROOT check the flags and use the same
if(ROOT_FOUND)

	string(REGEX MATCH "(^|[\t ]+)-m([\t ]*)(32|64)([\t ]+|$)" STAR_ROOT_CXX_FLAGS_M ${ROOT_CXX_FLAGS})

	if (STAR_ROOT_CXX_FLAGS_M)
		set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -m${CMAKE_MATCH_3}")
		message(STATUS "Found -m${CMAKE_MATCH_3} option in $ROOT_CXX_FLAGS (root-config). Will add it to $CMAKE_CXX_FLAGS")
	endif()

endif()
