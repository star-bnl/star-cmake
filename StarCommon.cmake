# Load this cmake file only once
if( StarCommonLoaded )
	return()
else()
	set(StarCommonLoaded TRUE)
endif()


# Special treatment of linker options for MacOS X to get a gcc linux-like behavior
if(APPLE)
	set(CMAKE_SHARED_LINKER_FLAGS "${CMAKE_SHARED_LINKER_FLAGS} -undefined dynamic_lookup")
	set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} -undefined dynamic_lookup")
endif()

# Set compile warning options for gcc compilers
if( CMAKE_COMPILER_IS_GNUCC OR CMAKE_COMPILER_IS_GNUCXX )
	set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -Wall")
endif()


# Check whether the compiler supports c++11
include(CheckCXXCompilerFlag)
CHECK_CXX_COMPILER_FLAG("-std=c++11" COMPILER_SUPPORTS_CXX11)
CHECK_CXX_COMPILER_FLAG("-std=c++0x" COMPILER_SUPPORTS_CXX0X)
if(COMPILER_SUPPORTS_CXX11)
	set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -std=c++11")
elseif(COMPILER_SUPPORTS_CXX0X)
	set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -std=c++0x")
else()
	message(STATUS "StarCommon: The compiler ${CMAKE_CXX_COMPILER} has no C++11 support. Please use a different C++ compiler.")
endif()


# Since most of STAR projects depend on ROOT check the flags and use the same
if(ROOT_FOUND)

	string(REGEX MATCH "(^|[\t ]+)-m([\t ]*)(32|64)([\t ]+|$)" STAR_ROOT_CXX_FLAGS_M ${ROOT_CXX_FLAGS})

	if (STAR_ROOT_CXX_FLAGS_M)
		set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -m${CMAKE_MATCH_3}")
		message(STATUS "StarCommon: Found -m${CMAKE_MATCH_3} option in $ROOT_CXX_FLAGS (root-config). Will add it to $CMAKE_CXX_FLAGS")

		if (CMAKE_MATCH_3 EQUAL 32)
			set_property(GLOBAL PROPERTY FIND_LIBRARY_USE_LIB64_PATHS OFF)
		endif()

	endif()

else()
	message(FATAL_ERROR "StarCommon: FATAL: ROOT package not found")
endif()


message(STATUS "StarCommon: CMAKE_CXX_FLAGS = \"${CMAKE_CXX_FLAGS}\"")


#
# Builds a list of header files from which a ROOT dictionary can be created for
# a given subdirectory `stroot_dir`. The list is put into the `headers_for_dict`
# variable that is returned to the parent scope. Only *.h and *.hh files
# containing ROOT's ClassDef macro are selected while any LinkDef files are
# ignored. With optional argument VERIFY the headers can be checked to contain
# the 'ClassDef' macro.
#
function( STAR_HEADERS_FOR_ROOT_DICTIONARY stroot_dir headers_for_dict )

	cmake_parse_arguments(ARG "VERIFY" "" "" ${ARGN})

	# Get all header files in 'stroot_dir'
	file(GLOB_RECURSE stroot_dir_headers "${stroot_dir}/*.h" "${stroot_dir}/*.hh")

	# Create an empty list
	set(valid_headers)

	# stroot_dir_headers should containd absolute paths to globed headers
	foreach( full_path_header ${stroot_dir_headers} )

		get_filename_component( header_file_name ${full_path_header} NAME )

		string( TOLOWER ${header_file_name} header_file_name )

		# Skip LinkDef files from globbing result
		if( ${header_file_name} MATCHES "linkdef" )
			# Uncomment next line to make it verbose
			#message( STATUS "StarCommon: WARNING: Skipping LinkDef header ${full_path_header}" )
			continue()
		endif()

		set( valid_header TRUE )

		if( ${ARG_VERIFY} )
			star_verify_header_for_root_dictionary( ${full_path_header} valid_header)
		endif()

		if( ${valid_header} )
			list( APPEND valid_headers ${full_path_header} )
		endif()

	endforeach()

	set( ${headers_for_dict} ${valid_headers} PARENT_SCOPE )

endfunction()


#
# Checks whether the file contains at least one ClassDef macro
#
function( STAR_VERIFY_HEADER_FOR_ROOT_DICTIONARY header_file valid_header )

	find_program( EXEC_GREP NAMES grep )

	if( NOT EXEC_GREP )
		message( FATAL_ERROR "StarCommon: FATAL: STAR_VERIFY_HEADER_FOR_ROOT_DICTIONARY function requires grep" )
	endif()

	# May want to verify that the header file does exist in the include directories
	#get_filename_component( headerFileName ${userHeader} NAME)
	#find_file(headerFile ${headerFileName} HINTS ${incdirs})

	set( valid )

	# Check for at least one ClassDef macro in the header file
	execute_process( COMMAND ${EXEC_GREP} -m1 -H "^[[:space:]]*ClassDef" ${header_file} RESULT_VARIABLE exit_code OUTPUT_QUIET )

	if( NOT ${exit_code} )
		set( valid TRUE )
	else()
		message( STATUS "StarCommon: WARNING: No ClassDef macro found in ${header_file}" )
		set( valid FALSE )
	endif()

	set( ${valid_header} ${valid} PARENT_SCOPE )

endfunction()


#
# Generates a basic LinkDef header ${CMAKE_CURRENT_BINARY_DIR}/${stroot_dir}_LinkDef.h
# by parsing the user provided header files with standard linux utilities awk and sed.
#
function(STAR_GENERATE_LINKDEF stroot_dir dict_headers)

	# Set default name for LinkDef file
	set( linkdef_file "${CMAKE_CURRENT_BINARY_DIR}/${stroot_dir}_LinkDef.h" )
	set_source_files_properties(${linkdef_file} PROPERTIES GENERATED TRUE)

	message(STATUS "StarCommon: Generating LinkDef header: ${linkdef_file}")

	# Check availability of required system tools
	find_program(EXEC_AWK NAMES gawk awk)
	find_program(EXEC_SED NAMES gsed sed)

	if(NOT EXEC_AWK OR NOT EXEC_SED)
		message(FATAL_ERROR "StarCommon: FATAL: STAR_GENERATE_LINKDEF function requires awk and sed commands")
	endif()

	cmake_parse_arguments(ARG "" "" "LINKDEF;LINKDEF_HEADERS" ${ARGN})

	# If provided by the user read the contents of their LinkDef file into a list
	set( linkdef_contents )

	if( ARG_LINKDEF )
		file(READ ${ARG_LINKDEF} linkdef_contents)
		# Convert file lines into a CMake list
		STRING(REGEX REPLACE ";" "\\\\;" linkdef_contents "${linkdef_contents}")
		STRING(REGEX REPLACE "\n" ";" linkdef_contents "${linkdef_contents}")
	endif()

	# Parse header files for ClassDef() statements and collect all entities into
	# a list
	set( dict_entities )
	set( dict_valid_headers )

	foreach( header ${ARG_LINKDEF_HEADERS} )
		set( my_exec_cmd ${EXEC_AWK} "match($0,\"^[[:space:]]*ClassDef[[:space:]]*\\\\(([^#]+),.*\\\\)\",a){ printf(a[1]\"\\r\") }" )

		execute_process( COMMAND ${my_exec_cmd} ${header} COMMAND ${EXEC_SED} -e "s/\\s\\+/;/g"
			OUTPUT_VARIABLE extracted_dict_objects OUTPUT_STRIP_TRAILING_WHITESPACE )

		list( APPEND dict_entities ${extracted_dict_objects} )

		if( extracted_dict_objects )
			list( APPEND dict_valid_headers ${header} )
		else()
			# if header base name matches linkdef_contents then use this header
			get_filename_component( header_base_name ${header} NAME_WE )

			if( "${linkdef_contents}" MATCHES "${header_base_name}" )
				list( APPEND dict_valid_headers ${header} )
			endif()
		endif()

	endforeach()


	# Special case dealing with StEvent containers
	set( dict_entities_stcontainers )

	if( "${stroot_dir}" MATCHES "StEvent" )
		foreach( header ${ARG_LINKDEF_HEADERS} )
			set( my_exec_cmd ${EXEC_AWK} "match($0,\"^[[:space:]]*StCollectionDef[[:space:]]*\\\\(([^#]+)\\\\)\",a){ printf(a[1]\"\\r\") }" )
			
			execute_process( COMMAND ${my_exec_cmd} ${header} COMMAND ${EXEC_SED} -e "s/\\s\\+/;/g"
				OUTPUT_VARIABLE extracted_dict_objects OUTPUT_STRIP_TRAILING_WHITESPACE )

			list( APPEND dict_entities_stcontainers ${extracted_dict_objects} )

			if( extracted_dict_objects )
				list( APPEND dict_valid_headers ${header} )
			endif()
		endforeach()
	endif()

	# Assign new validated headers 
	set( ${dict_headers} ${dict_valid_headers} PARENT_SCOPE )

	# Write contents to the generated *_LinkDef.h file

	if( "${linkdef_contents}" MATCHES "pragma[ \t]+link[ \t]+off[ \t]+all" )
		file(WRITE ${linkdef_file} "#ifdef __CINT__\n\n")
	else()
		file(WRITE ${linkdef_file} "#ifdef __CINT__\n\n#pragma link off all globals;\n#pragma link off all classes;\n#pragma link off all functions;\n\n")
	endif()

	foreach( linkdef_line ${linkdef_contents} )
		if( "${linkdef_line}" MATCHES "#pragma[ \t]+link")
			file( APPEND ${linkdef_file} "${linkdef_line}\n" )
		endif()
	endforeach()
	
	file( APPEND ${linkdef_file} "\n\n// Collected dictionary entities\n\n" )

	foreach( dict_entity ${dict_entities} )
		if( NOT "${linkdef_contents}" MATCHES "class[ ]+${dict_entity}[< ;+-]+")
			file( APPEND ${linkdef_file} "#pragma link C++ class ${dict_entity}+;\n" )
		endif()
	endforeach()

	file( APPEND ${linkdef_file} "\n\n" )

	foreach( dict_entity ${dict_entities_stcontainers} )
		file( APPEND ${linkdef_file} "#pragma link C++ class StPtrVec${dict_entity}-;\n" )
		file( APPEND ${linkdef_file} "#pragma link C++ class StSPtrVec${dict_entity}-;\n" )
	endforeach()

	file( APPEND ${linkdef_file} "#endif\n" )

endfunction()


#
# Generates a ROOT dictionary for `stroot_dir`.
#
function(STAR_GENERATE_DICTIONARY stroot_dir)

	cmake_parse_arguments(ARG "" "" "LINKDEF;LINKDEF_HEADERS;LINKDEF_OPTIONS;EXCLUDE" "" ${ARGN})

	# If the user provided header files use them in addition to automatically
	# collected ones.
	set( linkdef_headers )
	star_headers_for_root_dictionary( ${stroot_dir} linkdef_headers )

	if( ARG_EXCLUDE )
		FILTER_LIST( linkdef_headers ARG_EXCLUDE )
	endif()

	# Generate a basic LinkDef file and, if available, merge with the one
	# provided by the user
	set( dict_headers )
	star_generate_linkdef( ${stroot_dir} dict_headers LINKDEF ${ARG_LINKDEF} LINKDEF_HEADERS ${linkdef_headers})

	file( GLOB_RECURSE user_linkdef_headers ${ARG_LINKDEF_HEADERS} )
	list( APPEND dict_headers ${user_linkdef_headers} )

	root_generate_dictionary( ${stroot_dir}_dict ${dict_headers}
		LINKDEF ${CMAKE_CURRENT_BINARY_DIR}/${stroot_dir}_LinkDef.h
		OPTIONS ${ARG_LINKDEF_OPTIONS}
	)

endfunction()


#
# Adds a target to build a library from all source files (*.cxx, *.cc, and *.cpp)
# recursively found in the specified subdirectory `stroot_dir`. It is possible
# to EXCLUDE some files matching an optional pattern.
#
function(STAR_ADD_LIBRARY stroot_dir)

	cmake_parse_arguments(ARG "" "" "LINKDEF;LINKDEF_HEADERS;LINKDEF_OPTIONS;EXCLUDE" "" ${ARGN})

	# Set default regex'es to exclude from globbed
	list( APPEND ARG_EXCLUDE "${stroot_dir}/macros;${stroot_dir}/doc;${stroot_dir}/examples" )

	# Deal with headers
	if( NOT TARGET ${stroot_dir}_dict.cxx )

		# Search for default LinkDef if not specified
		file( GLOB user_linkdefs "${stroot_dir}/*LinkDef.h" "${stroot_dir}/*LinkDef.hh" )

		if( NOT ARG_LINKDEF AND user_linkdefs )
			# Get the first LinkDef from the list
			list( GET user_linkdefs 0 user_linkdef )
			set( ARG_LINKDEF ${user_linkdef} )
		endif()

		# Set default options
		list(APPEND ARG_LINKDEF_OPTIONS "-p;-D__ROOT__" )

		star_generate_dictionary( ${stroot_dir}
			LINKDEF ${ARG_LINKDEF}
			LINKDEF_HEADERS ${ARG_LINKDEF_HEADERS}
			LINKDEF_OPTIONS ${ARG_LINKDEF_OPTIONS}
			EXCLUDE ${ARG_EXCLUDE}
		)

	endif()

	# Deal with sources
	file(GLOB_RECURSE sources "${stroot_dir}/*.cxx" "${stroot_dir}/*.cc" "${stroot_dir}/*.cpp")

	if( ARG_EXCLUDE )
		FILTER_LIST( sources ARG_EXCLUDE )
	endif()

	add_library(${stroot_dir} SHARED ${sources} ${stroot_dir}_dict.cxx)

endfunction()


function( FILTER_LIST arg_list arg_regexs )

	# Starting cmake 3.6 one can simply use list( FILTER ... )
	#list( FILTER sources EXCLUDE REGEX "${ARG_EXCLUDE}" )

	foreach( item ${${arg_list}} )
		foreach( regex ${${arg_regexs}} )

			if( ${item} MATCHES "${regex}" )
				list(REMOVE_ITEM ${arg_list} ${item})
				break()
			endif()

		endforeach()
	endforeach()

	set( ${arg_list} ${${arg_list}} PARENT_SCOPE)

endfunction()


# Make use of the $STAR_HOST_SYS evironment variable. If it is set use it as the
# typical STAR installation prefix
set(STAR_ADDITIONAL_INSTALL_PREFIX ".")

if(DEFINED ENV{STAR_HOST_SYS})
	set(STAR_ADDITIONAL_INSTALL_PREFIX ".$ENV{STAR_HOST_SYS}")
endif()
