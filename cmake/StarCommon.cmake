# Load this cmake file only once
if(StarCommonLoaded)
	message(STATUS "StarCommon: Should be included only once")
	return()
else()
	set(StarCommonLoaded TRUE)
endif()

# By default build shared libraries but allow the user to change if desired
OPTION(BUILD_SHARED_LIBS "Global flag to cause add_library to create shared libraries if on" ON)

# Special treatment for gcc linker options to get a linux-like behavior on MacOS X
if(APPLE)
	set(CMAKE_SHARED_LINKER_FLAGS "${CMAKE_SHARED_LINKER_FLAGS} -undefined dynamic_lookup")
	set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} -undefined dynamic_lookup")
endif()


find_program(ROOT_DICTGEN_EXECUTABLE NAMES rootcling rootcint HINTS $ENV{ROOTSYS}/bin)


# Define common STAR_ and CMAKE_ variables used to build the STAR code

# Define the build "DATE" and "TIME" variables only when Release is built.
if(CMAKE_BUILD_TYPE STREQUAL "Release")
	string(TIMESTAMP STAR_BUILD_DATE "%y%m%d")
	string(TIMESTAMP STAR_BUILD_TIME "%H%M")
else()
	set(STAR_BUILD_DATE "000000")
	set(STAR_BUILD_TIME "0000")
endif()

# -D__ROOT__ is used by many classes guarding calls to ClassDef() macro
set(STAR_C_CXX_DEFINITIONS "-D__ROOT__")
set(STAR_Fortran_DEFINITIONS "-DCERNLIB_TYPE")
set(STAR_Fortran_FLAGS "-std=legacy -fno-second-underscore -fno-automatic")

set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} ${STAR_C_CXX_DEFINITIONS}")
set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} ${STAR_C_CXX_DEFINITIONS}")
set(CMAKE_Fortran_FLAGS "${CMAKE_Fortran_FLAGS} ${STAR_Fortran_FLAGS} ${STAR_Fortran_DEFINITIONS}")

set(CMAKE_CXX_STANDARD 11)
set(CMAKE_CXX_EXTENSIONS OFF)

# Remove dependency of "install" target on "all" target. This allows to
# build and install individual libraries
set(CMAKE_SKIP_INSTALL_ALL_DEPENDENCY TRUE)

# Make use of the $STAR_HOST_SYS evironment variable. If it is set use it as the
# typical STAR installation prefix
set(STAR_INSTALL_PREFIX "${CMAKE_INSTALL_PREFIX}")
if(DEFINED ENV{STAR_HOST_SYS})
	set(STAR_INSTALL_PREFIX "${STAR_INSTALL_PREFIX}/$ENV{STAR_HOST_SYS}")
endif()

set(STAR_INSTALL_BINDIR "${STAR_INSTALL_PREFIX}/bin" CACHE PATH "Path to installed executables")
set(STAR_INSTALL_LIBDIR "${STAR_INSTALL_PREFIX}/lib" CACHE PATH "Path to installed libraries")
set(STAR_INSTALL_INCLUDEDIR "${STAR_INSTALL_PREFIX}/include" CACHE PATH "Path to installed header files")

mark_as_advanced(
	STAR_INSTALL_PREFIX
	STAR_INSTALL_BINDIR
	STAR_INSTALL_LIBDIR
	STAR_INSTALL_INCLUDEDIR
)

set(CMAKE_INSTALL_RPATH "${STAR_INSTALL_LIBDIR}")

# Read blacklisted directories from a file into a list
file(STRINGS "${PROJECT_SOURCE_DIR}/cmake/blacklisted_lib_dirs.txt" _star_blacklisted_libs)
foreach(black ${_star_blacklisted_libs})
	string(REGEX REPLACE "^([^ ]+).*" "\\1" black ${black})
	list(APPEND STAR_BLACKLISTED_LIB_DIRS ${black})
endforeach()


# Applies patches found in `patch` subdirectory only to $STAR_SRC code checked
# out from a git repository.
function(STAR_APPLY_PATCH)
	# Do nothing if STAR_PATCH is not set (default value is NO)
	if(NOT STAR_PATCH)
		return()
	endif()

	file(GLOB _patch_files "${PROJECT_SOURCE_DIR}/patches/${STAR_PATCH}/*.patch")

	if(NOT _patch_files)
		message(WARNING "StarCommon: Patch set ${STAR_PATCH} not found")
		return()
	endif()

	foreach(_patch_file ${_patch_files})
		message(STATUS "StarCommon: Applying patch from ${STAR_PATCH}: ${_patch_file}")
		execute_process(COMMAND patch -p1 -d "${STAR_SRC}"
			INPUT_FILE "${_patch_file}" RESULT_VARIABLE _result OUTPUT_VARIABLE _output)
		if(_result EQUAL 0)
			message(STATUS "StarCommon: Patch succesfully applied")
		else() # distinguish between failed patch and already applied one
			execute_process(COMMAND patch -p1 -d "${STAR_SRC}" --reverse --dry-run
				INPUT_FILE "${_patch_file}" RESULT_VARIABLE _result OUTPUT_VARIABLE _output)
			if(_result EQUAL 0)
				message(WARNING "StarCommon: Patch from ${STAR_PATCH} already applied: ${_patch_file}")
			else()
				message(FATAL_ERROR "StarCommon: Failed to apply patch from ${STAR_PATCH}: ${_patch_file}")
			endif()
		endif()
	endforeach()
endfunction()


#
# Preselects all header files found in a `star_lib_dir` directory to be used by
# rootcint/rootcling during ROOT dictionary creation. The list is put into the
# `headers_for_dict` variable which is returned to the parent scope. Only files
# with extensions *.h, *.hh, and *.hpp are considered while files containing
# a 'LinkDef' substring are ignored.
#
function(STAR_HEADERS_FOR_ROOT_DICTIONARY star_lib_dir headers_for_dict)
	# Get all header files in 'star_lib_dir'
	file(GLOB_RECURSE star_lib_dir_headers "${star_lib_dir}/*.h"
	                                       "${star_lib_dir}/*.hh"
	                                       "${star_lib_dir}/*.hpp")
	# Create an empty list
	set(valid_headers)

	# star_lib_dir_headers should containd absolute paths to globed headers
	foreach(full_path_header ${star_lib_dir_headers})

		get_filename_component(header_file_name ${full_path_header} NAME)
		string(TOLOWER ${header_file_name} header_file_name)
		# Skip LinkDef files from globbing result
		if(${header_file_name} MATCHES "linkdef")
			continue()
		endif()

		list(APPEND valid_headers ${full_path_header})
	endforeach()

	set(${headers_for_dict} ${valid_headers} PARENT_SCOPE)
endfunction()


#
# Generates `linkdef_file` and `dictinc_file` header files used in ROOT
# dictionary generation by rootcint/rootcling. Only header files passed in
# `headers` argument are used. The user can optionally pass an existing LinkDef
# file in `user_linkdef_file` argument to be incorporated in the generated
# `linkdef_file`.
#
function(STAR_GENERATE_LINKDEF header_files linkdef_file dictinc_file user_linkdef_file)
	# Pass both files to get_likdef.sh as -o arguments
	set(gen_linkdef_args "-l;${linkdef_file};-d;${dictinc_file};${header_files}")

	if(user_linkdef_file)
		list(APPEND gen_linkdef_args "-i;${user_linkdef_file}")
	endif()

	# Generate the above files to be used in dictionary generation by ROOT
	add_custom_command(
		OUTPUT ${linkdef_file} ${dictinc_file}
		COMMAND "${PROJECT_SOURCE_DIR}/scripts/gen_linkdef.sh" ${gen_linkdef_args}
		DEPENDS "${PROJECT_SOURCE_DIR}/scripts/gen_linkdef.sh" ${header_files}
		VERBATIM)
endfunction()


#
# Generates two files `_dict_source` and `_dict_header` in `star_lib_dir_out`.
# The files contain the ROOT dictionary for the `star_lib_name` library and its
# source files in `star_lib_dir`.
#
function(STAR_GENERATE_DICTIONARY star_lib_name star_lib_dir star_lib_dir_out)
	cmake_parse_arguments(ARG "" "" "LINKDEF_HEADERS;LINKDEF_OPTIONS;EXCLUDE" "" ${ARGN})

	# We assume the header files are generated when the directory containing
	# input files `star_lib_dir` is inside the build directory
	if(star_lib_dir MATCHES "^${CMAKE_CURRENT_BINARY_DIR}")
		set(linkdef_headers "${ARG_LINKDEF_HEADERS}")
		set(user_linkdef)
	else()
		# Search for default LinkDef if not specified
		file( GLOB user_linkdefs "${star_lib_dir}/*LinkDef.h"
		                         "${star_lib_dir}/*LinkDef.hh" )
		# Get the first LinkDef from the list
		if( user_linkdefs )
			list( GET user_linkdefs 0 user_linkdef )
		endif()

		# Preselect header files from `star_lib_dir`
		star_headers_for_root_dictionary(${star_lib_dir} linkdef_headers)
		FILTER_LIST(linkdef_headers EXCLUDE ${ARG_EXCLUDE})
	endif()

	GET_ROOT_DICT_FILE_NAMES(_linkdef_file _dictinc_file _dict_source _dict_header)

	# Generate a basic LinkDef file and, if available, merge with the one
	# possibly provided by the user in `star_lib_dir`
	star_generate_linkdef("${linkdef_headers}" "${_linkdef_file}" "${_dictinc_file}" "${user_linkdef}")

	# Prepare include directories to be used during ROOT dictionary generation.
	# These directories are tied to the `star_lib_name` target via the
	# INCLUDE_DIRECTORIES property.
	get_property(tgt_include_dirs TARGET ${star_lib_name} PROPERTY INCLUDE_DIRECTORIES)
	get_property(lib_include_dirs SOURCE ${star_lib_dir}  PROPERTY INCLUDE_DIRECTORIES)
	list(APPEND tgt_include_dirs ${lib_include_dirs})
	string(REGEX REPLACE "([^;]+)" "-I\\1" dict_include_dirs "${tgt_include_dirs}")

	# Generate `_dict_source` and `_dict_header` using the `_linkdef_file` and `_dictinc_file` files
	add_custom_command(OUTPUT ${_dict_source} ${_dict_header}
		COMMAND ${ROOT_DICTGEN_EXECUTABLE} -cint -f ${_dict_source}
		-c ${ARG_LINKDEF_OPTIONS} ${dict_include_dirs}
		${ARG_LINKDEF_HEADERS} ${_dictinc_file} ${_linkdef_file}
		DEPENDS ${ARG_LINKDEF_HEADERS} ${_dictinc_file} ${_linkdef_file}
		VERBATIM)
endfunction()


#
# Adds a target to build a library from all source files (*.cxx, *.cc, *.c,
# *.cpp, *.g, *.idl) recursively found in the specified subdirectory
# `star_lib_dir`. It is possible to EXCLUDE some files matching an optional
# regex pattern.
#
function(STAR_ADD_LIBRARY star_lib_dir)

	star_target_paths(${star_lib_dir} star_lib_name star_lib_dir_abs star_lib_dir_out)

	# Get first optional unnamed parameter
	set(user_lib_name ${ARGV1})
	set(star_lib_name_for_tables)
	if(user_lib_name)
		set(star_lib_name_for_tables ${star_lib_name})
		set(star_lib_name ${user_lib_name})
	endif()

	# Deal with sources
	GET_EXCLUDE_LIST(star_lib_exclude)

	file(GLOB_RECURSE sources_cpp
		"${star_lib_dir_abs}/*.cxx"
		"${star_lib_dir_abs}/*.cc"
		"${star_lib_dir_abs}/*.c"
		"${star_lib_dir_abs}/*.cpp")
	FILTER_LIST(sources_cpp EXCLUDE ${star_lib_exclude})

	file(GLOB_RECURSE f_files "${star_lib_dir_abs}/*.F")
	FILTER_LIST(f_files EXCLUDE ${star_lib_exclude})
	star_process_f("${f_files}" ${star_lib_dir_out} sources_F)

	file(GLOB_RECURSE g_files "${star_lib_dir_abs}/*.g")
	FILTER_LIST(g_files EXCLUDE ${star_lib_exclude})
	star_process_g("${g_files}" sources_gtoF)

	file(GLOB_RECURSE idl_files "${star_lib_dir_abs}/*.idl")
	FILTER_LIST(idl_files EXCLUDE ${star_lib_exclude})
	# Exclude *.idl files if they are located in the idl subdirectory
	# Such files are processed separately by STAR_ADD_LIBRARY_TABLE
	string(REPLACE "+" "\\\\+" idl_path_exclude_regex "${star_lib_dir_abs}/idl")
	FILTER_LIST(idl_files EXCLUDE ${idl_path_exclude_regex})
	star_process_idl("${idl_files}" "${star_lib_name_for_tables}" "${star_lib_dir_out}" sources_idl headers_idl)

	set(sources ${sources_cpp} ${sources_idl} ${sources_gtoF} ${sources_F})

	GET_ROOT_DICT_FILE_NAMES(_linkdef_file _dictinc_file _dict_source _dict_header)

	add_library(${star_lib_name} ${sources} ${_dict_source})

	# Output the library to the respecitve subdirectory in the binary directory
	set_target_properties(${star_lib_name} PROPERTIES LIBRARY_OUTPUT_DIRECTORY ${star_lib_dir_out})

	GET_SUBDIRS(_star_lib_subdirs ${star_lib_dir_abs} INCLUDE_PARENT)
	target_include_directories(${star_lib_name} PRIVATE
		"${STAR_SRC}"           # Some files in StvXXX include "StarVMC/.../*.h"
		"${_star_lib_subdirs}"
		"${CMAKE_CURRENT_BINARY_DIR}/include"
		"${CMAKE_CURRENT_BINARY_DIR}/include/tables/${star_lib_name_for_tables}")

	# Generate the dictionary file for the library
	star_generate_dictionary(${star_lib_name} ${star_lib_dir_abs} ${star_lib_dir_out}
		LINKDEF_HEADERS ${${star_lib_name}_LINKDEF_HEADERS}
		LINKDEF_OPTIONS "-p;-D__ROOT__"
		EXCLUDE ${star_lib_exclude})

	install(TARGETS ${star_lib_name}
		LIBRARY DESTINATION "${STAR_INSTALL_LIBDIR}" OPTIONAL
		ARCHIVE DESTINATION "${STAR_INSTALL_LIBDIR}" OPTIONAL)

	install(FILES ${headers_idl}
		DESTINATION "${STAR_INSTALL_INCLUDEDIR}/tables/${star_lib_name_for_tables}" OPTIONAL)
endfunction()


#
# Forms file names used in ROOT dictionary generation. Note that
# `star_lib_dir_out` and `star_lib_name` variables must be set to appropriate
# values in the scope where this macro is called.
#
macro(GET_ROOT_DICT_FILE_NAMES linkdef_file dictinc_file dict_source dict_header)
	# Set default names for generated files
	set(${linkdef_file} "${star_lib_dir_out}/${star_lib_name}_LinkDef.h")
	set(${dictinc_file} "${star_lib_dir_out}/${star_lib_name}_DictInc.h")
	set(${dict_source}  "${star_lib_dir_out}/${star_lib_name}_dict.cxx")
	set(${dict_header}  "${star_lib_dir_out}/${star_lib_name}_dict.h")
endmacro()


macro(FILTER_LIST list_to_filter)
	cmake_parse_arguments(ARG "" "" "EXCLUDE" ${ARGN})

	if(${list_to_filter} AND ARG_EXCLUDE)
		foreach(regex ${ARG_EXCLUDE})
			list(FILTER ${list_to_filter} EXCLUDE REGEX ${regex})
		endforeach()
	endif()
endmacro()


# Return list of regex'es to exclude from globbed paths for target `star_lib_name`
macro(GET_EXCLUDE_LIST exclude_list)
	set(${exclude_list}
		"${star_lib_name}.*macros"
		"${star_lib_name}.*doc"
		"${star_lib_name}.*examples"
		"geant321/gxuser")
endmacro()


#
# Builds a list of subdirectories with complete path found in the
# 'directories' list
#
macro(GET_SUBDIRS subdirectories directories)
	# Check wether the parent directory should be included in the output
	# list of directories
	cmake_parse_arguments(ARG "INCLUDE_PARENT" "" "" ${ARGN})

	set(_sub_dirs "")

	foreach(dir ${directories})
		if(NOT IS_ABSOLUTE ${dir})
			set(dir ${STAR_SRC}/${dir})
		endif()

		if(NOT IS_DIRECTORY ${dir})
			message(WARNING "StarCommon: Directory ${dir} not found")
			continue()
		endif()

		file(GLOB all_files RELATIVE ${dir} ${dir}/*)

		# Include the parent directory in the list
		if(${ARG_INCLUDE_PARENT})
			list(APPEND _sub_dirs ${dir})
		endif()

		foreach(sub_dir ${all_files})
			if(IS_DIRECTORY ${dir}/${sub_dir})
				list(APPEND _sub_dirs ${dir}/${sub_dir})
			endif()
		endforeach()
	endforeach()
	
	set(${subdirectories} ${_sub_dirs})
endmacro()


#
# From the path provided by the user (`star_lib_dir`) extracts the library
# target name (`lib_name`), an absolute path to the library source code
# (`path_abs`), and the corresponding output path where the generated or built
# code can be placed (`path_out`). The input path `star_lib_dir` can be either
# absolute or relative to ${STAR_SRC}
#
function(STAR_TARGET_PATHS star_lib_dir lib_name path_abs path_out)

	set(_path_abs)
	set(_path_out)

	if(IS_ABSOLUTE ${star_lib_dir})
		if(star_lib_dir MATCHES "^${STAR_SRC}")
			file(RELATIVE_PATH _path_rel ${STAR_SRC} ${star_lib_dir})
		elseif(star_lib_dir MATCHES "^${PROJECT_SOURCE_DIR}")
			file(RELATIVE_PATH _path_rel ${PROJECT_SOURCE_DIR} ${star_lib_dir})
		else()
			message(FATAL_ERROR
				"StarCommon: Absolute path \"${star_lib_dir}\" must match either \"${STAR_SRC}\" or \"${PROJECT_SOURCE_DIR}\"")
		endif()

		set(_path_abs ${star_lib_dir})
		set(_path_out ${CMAKE_CURRENT_BINARY_DIR}/${_path_rel})
	else()
		set(_path_abs ${STAR_SRC}/${star_lib_dir})
		set(_path_out ${CMAKE_CURRENT_BINARY_DIR}/${star_lib_dir})
	endif()

	# First check that the path exists
	if(NOT IS_DIRECTORY ${_path_abs})
		message(FATAL_ERROR "StarCommon: Directory \"${_path_abs}\" not found")
	endif()

	get_filename_component(_lib_name ${star_lib_dir} NAME)

	set(${lib_name} ${_lib_name} PARENT_SCOPE)
	set(${path_abs} ${_path_abs} PARENT_SCOPE)
	set(${path_out} ${_path_out} PARENT_SCOPE)

endfunction()


#
# Flattens the hierarchy of header files found in select subdirectories in
# `${STAR_SRC}` by copying them to `destination_dir` at the same level
#
function(STAR_PREINSTALL_HEADERS destination_dir)

	# Collect files from some subdirectiries
	file(GLOB header_files
		"${STAR_SRC}/StRoot/*/*.h"
		"${STAR_SRC}/StRoot/*/*.hh"
		"${STAR_SRC}/StRoot/*/*.inc"
		"${STAR_SRC}/StarVMC/*/*.h"
		"${STAR_SRC}/StarVMC/*/*.hh"
		"${STAR_SRC}/StarVMC/*/*.inc"
		"${STAR_SRC}/asps/rexe/TGeant3/*.h"
		"${STAR_SRC}/pams/*/inc/*.h"
		"${STAR_SRC}/pams/*/inc/*.inc")

	FILTER_LIST(header_files EXCLUDE "StPicoEvent/SystemOfUnits.h;StPicoEvent/PhysicalConstants.h")

	foreach( header_file ${header_files} )
		get_filename_component( header_file_name ${header_file} NAME )
		configure_file( "${header_file}" "${destination_dir}/${header_file_name}" COPYONLY )
	endforeach()

endfunction()


#
# Install some older Geant3 geometry files into the `destination_dir` where they
# can be found by xgeometry library while generating Geant3 geometry sources
# from *.xml files in StarVMC/Geometry/Compat/.
#
function(STAR_PREINSTALL_GEO_PAMS destination_dir)

	set(geo_pams_files
		"${STAR_SRC}/pams/geometry/calbgeo/calbpar.g"
		"${STAR_SRC}/pams/geometry/calbgeo/etsphit.g"
		"${STAR_SRC}/pams/geometry/fpdmgeo/ffpdstep.g"
		"${STAR_SRC}/pams/geometry/fgtdgeo/fgtdgeo.g"
		"${STAR_SRC}/pams/geometry/fgtdgeo/fgtdgeo1.g"
		"${STAR_SRC}/pams/geometry/fgtdgeo/fgtdgeo2.g"
		"${STAR_SRC}/pams/geometry/fhcmgeo/fhcmgeo.g"
		"${STAR_SRC}/pams/geometry/fpdmgeo/fpdmgeo.g"
		"${STAR_SRC}/pams/geometry/fstdgeo/fstdgeo.g"
		"${STAR_SRC}/pams/geometry/gembgeo/gembgeo.g"
		"${STAR_SRC}/pams/geometry/hpdtgeo/hpdtgeo.g"
		"${STAR_SRC}/pams/geometry/igtdgeo/igtdgeo.g"
		"${STAR_SRC}/pams/geometry/istbgeo/istbgeo.g"
		"${STAR_SRC}/pams/geometry/istbgeo/istbgeo1.g"
		"${STAR_SRC}/pams/geometry/istbgeo/istbgeo2.g"
		"${STAR_SRC}/pams/geometry/istbgeo/istbgeo3.g"
		"${STAR_SRC}/pams/geometry/istbgeo/istbgeo4.g"
		"${STAR_SRC}/pams/geometry/istbgeo/istbgeo5.g"
		"${STAR_SRC}/pams/geometry/istbgeo/istbgeo6.g"
		"${STAR_SRC}/pams/geometry/istbgeo/istbgeo00.g"
		"${STAR_SRC}/pams/geometry/itspgeo/itspgeo.g"
		"${STAR_SRC}/pams/geometry/pixlgeo/pixlgeo00.g"
		"${STAR_SRC}/pams/geometry/pixlgeo/pixlgeo.g"
		"${STAR_SRC}/pams/geometry/pixlgeo/pixlgeo1.g"
		"${STAR_SRC}/pams/geometry/pixlgeo/pixlgeo2.g"
		"${STAR_SRC}/pams/geometry/richgeo/richgeo.g"
		"${STAR_SRC}/pams/geometry/tpcegeo/tpcegeo.g"
		"${STAR_SRC}/pams/geometry/wallgeo/wallgeo.g")

	foreach(geo_pams_file ${geo_pams_files})
		file(RELATIVE_PATH rel_path ${STAR_SRC} ${geo_pams_file})
		configure_file("${geo_pams_file}" "${destination_dir}/${rel_path}" COPYONLY)
	endforeach()

endfunction()


#
# Creates ROOT TGeo geometries from xml files
#
# [ARGV1, ARGV2] = [StarGeometry, RootTGeo] OR [xgeometry, GeantGeo]
#
function(STAR_ADD_LIBRARY_GEOMETRY star_lib_dir)

	star_target_paths(${star_lib_dir} star_lib_name star_lib_dir_abs star_lib_dir_out)

	# Change the name of the library/target and output directory if an
	# optional unnamed parameter is provided by the user
	set(user_lib_name ${ARGV1})
	if(user_lib_name)
		string(REPLACE ${star_lib_name} ${user_lib_name} star_lib_dir_out ${star_lib_dir_out})
		set(star_lib_name ${user_lib_name})
	endif()

	set(geo_py_parser ${STAR_SRC}/mgr/agmlParser.py)

	find_program(PYTHON_EXECUTABLE NAMES python2.7 python2)

	file(GLOB_RECURSE geo_xml_files "${star_lib_dir_abs}/*.xml")

	# The geometry library can be generated by choosing either of the two
	# available options "RootTGeo" and "GeantGeo". Default is "RootTGeo"
	if(ARGV2 STREQUAL "GeantGeo")
		_star_parse_geoxml_GeantGeo("${geo_xml_files}" "${star_lib_dir_out}" geo_sources)
		list(APPEND geo_sources ${STAR_SRC}/StarVMC/Geometry/Helpers.h)
	else()
		# Exclude some xml files
		FILTER_LIST(geo_xml_files EXCLUDE "Compat")
		_star_parse_geoxml_RootTGeo("${geo_xml_files}" "${star_lib_dir_out}" geo_sources geo_headers)
		GET_ROOT_DICT_FILE_NAMES(_linkdef_file _dictinc_file _dict_source _dict_header)
		list(APPEND geo_sources ${_dict_source})
	endif()

	add_library(${star_lib_name} ${geo_sources})
	set_target_properties(${star_lib_name} PROPERTIES
		LIBRARY_OUTPUT_DIRECTORY ${star_lib_dir_out}
		PUBLIC_HEADER "${geo_headers}")

	if(ARGV2 STREQUAL "RootTGeo")
		target_include_directories(${star_lib_name} PRIVATE "${CMAKE_CURRENT_BINARY_DIR};${STAR_SRC}")
		# Generate the dictionary file for the library
		star_generate_dictionary(${star_lib_name} ${star_lib_dir_out} ${star_lib_dir_out}
			LINKDEF_HEADERS ${geo_headers}
			LINKDEF_OPTIONS "-p;-D__ROOT__")
	endif()

	# Get relative path for the generated headers to be used at installation stage
	file(RELATIVE_PATH geo_headers_rel_path ${CMAKE_CURRENT_BINARY_DIR} ${star_lib_dir_out})
	install(TARGETS ${star_lib_name}
		LIBRARY DESTINATION "${STAR_INSTALL_LIBDIR}" OPTIONAL
		ARCHIVE DESTINATION "${STAR_INSTALL_LIBDIR}" OPTIONAL
		PUBLIC_HEADER DESTINATION "${STAR_INSTALL_INCLUDEDIR}/${geo_headers_rel_path}" OPTIONAL)
endfunction()


function(_STAR_PARSE_GEOXML_GeantGeo geo_xml_files out_dir out_sources)
	# For each *.xml file found in `star_lib_dir` (e.g.
	# $STAR_SRC/StarVMC/Geometry) generate the source and header files
	foreach(geo_xml_file ${geo_xml_files})
		get_filename_component(geo_name ${geo_xml_file} NAME_WE)
		set(geo_agesrc ${out_dir}/${geo_name}.age)

		add_custom_command(
			OUTPUT ${geo_agesrc}
			COMMAND ${CMAKE_COMMAND} -E make_directory ${out_dir}
			COMMAND ${PYTHON_EXECUTABLE} ${geo_py_parser} --file=${geo_xml_file} --module=${geo_name} --export=Mortran > ${geo_agesrc}
			DEPENDS ${geo_py_parser} ${geo_xml_file})

		list(APPEND geo_sources_generated ${geo_agesrc})
	endforeach()

	# Add an existing *.age file to the generated ones
	star_process_g("${geo_sources_generated};${STAR_SRC}/StarVMC/xgeometry/xgeometry.age" out_sources_)

	# Return complete list of *.age files
	set(${out_sources} ${out_sources_} PARENT_SCOPE)
endfunction()


function(_STAR_PARSE_GEOXML_RootTGeo geo_xml_files out_dir out_sources out_headers)
	# For each *.xml file found in `star_lib_dir` (e.g.
	# $STAR_SRC/StarVMC/Geometry) generate the source and header files
	foreach(geo_xml_file ${geo_xml_files})
		get_filename_component(geo_name ${geo_xml_file} NAME_WE)
		set(geo_header ${out_dir}/${geo_name}.h)
		set(geo_source ${out_dir}/${geo_name}.cxx)

		add_custom_command(
			OUTPUT ${geo_source} ${geo_header}
			COMMAND ${PYTHON_EXECUTABLE} ${geo_py_parser} --file=${geo_xml_file} --module=${geo_name} --export=AgROOT --path=${out_dir}
			DEPENDS ${geo_py_parser} ${geo_xml_file})

		if(${geo_name} MATCHES "Config")
			list(APPEND geo_config_headers ${geo_header})
		endif()
		list(APPEND geo_sources_generated ${geo_source})
		list(APPEND geo_headers_generated ${geo_header})
	endforeach()

	# Create a string by replacing ; (i.e. semicolon) with gcc compiler options
	string(REGEX REPLACE "([^;]+);" "-include \\1 " geo_config_headers_include "${geo_config_headers};")
	# Special treatment required for the aggregate geometry file because the generated files do
	# not include proper header files! Including that many files is likely to slow down the
	# compilation
	set_source_files_properties(${out_dir}/StarGeo.cxx
		PROPERTIES COMPILE_FLAGS "${geo_config_headers_include}")

	# Return generated lists
	set(${out_sources} ${geo_sources_generated} PARENT_SCOPE)
	set(${out_headers} ${geo_headers_generated} PARENT_SCOPE)
endfunction()


#
# Builds the starsim library and executable
#
function(STAR_ADD_LIBRARY_STARSIM starsim_dir)

	star_target_paths(${starsim_dir} dummy starsim_dir_abs star_lib_dir_out)

	set(_starsim_subdirs ".;rebank;geant;dzdoc;deccc;comis;comis/comis;atutil;atmain;atgeant;agzio")
	set(_starsimlib_sources)

	foreach(_sub_dir ${_starsim_subdirs})
		file(GLOB _cxx_files "${starsim_dir_abs}/${_sub_dir}/*.cxx")
		file(GLOB _c_files "${starsim_dir_abs}/${_sub_dir}/*.c")
		file(GLOB _f_files "${starsim_dir_abs}/${_sub_dir}/*.F")
		file(GLOB _g_files "${starsim_dir_abs}/${_sub_dir}/*.g")
		file(GLOB _age_files "${starsim_dir_abs}/${_sub_dir}/*.age")

		set(_all_files ${_cxx_files} ${_c_files} ${_f_files} ${_g_files} ${_age_files})
		FILTER_LIST(_all_files EXCLUDE
			"acmain.cxx"
			"deccc/ctype.c"
			"deccc/mykuip.c"
			"deccc/traceqc.c"
			"atmain/sterror.age"
			"atmain/traceq.age"
		)
		if(_all_files)
			list(SORT _all_files)
		endif()

		foreach(_source_file ${_all_files})
			get_filename_component(_source_file_ext ${_source_file} EXT)
			if(_source_file_ext STREQUAL ".g" OR _source_file_ext STREQUAL ".age")
				star_process_g("${_source_file}" _f_file)
				list(APPEND _starsimlib_sources ${_f_file})
			else()
				list(APPEND _starsimlib_sources ${_source_file})
			endif()
		endforeach()
	endforeach()

	set(starsim_INCLUDE_DIRECTORIES
		"${starsim_dir_abs}/include"
		"${STAR_SRC}/asps/Simulation/geant321/include"
		"${STAR_SRC}/asps/Simulation/gcalor/include"
		"${CERNLIB_INCLUDE_DIRS}")

	# Build and install starsim library
	add_library(starsimlib STATIC ${_starsimlib_sources})
	target_include_directories(starsimlib PRIVATE ${starsim_INCLUDE_DIRECTORIES})
	set_target_properties(starsimlib PROPERTIES LIBRARY_OUTPUT_DIRECTORY ${star_lib_dir_out})

	install(TARGETS starsimlib
		LIBRARY DESTINATION "${STAR_INSTALL_LIBDIR}" OPTIONAL
		ARCHIVE DESTINATION "${STAR_INSTALL_LIBDIR}" OPTIONAL)

	# Build and install starsim executable
	add_executable(starsim ${starsim_dir_abs}/acmain.cxx)

	target_link_libraries(starsim -Wl,--whole-archive -Wl,-Bstatic -Wl,-z -Wl,muldefs starsimlib -Wl,--no-whole-archive -Wl,-Bdynamic geant321 gcalor
		 ${CERNLIB_LIBRARIES} ${MYSQL_LIBRARIES} gfortran Xt Xpm X11 z m rt dl crypt)
	set_target_properties(starsim PROPERTIES LINK_FLAGS "-Wl,-export-dynamic")

	install(TARGETS starsim RUNTIME DESTINATION "${STAR_INSTALL_BINDIR}" OPTIONAL)
	install(FILES "${starsim_dir_abs}/atlsim.bank"        DESTINATION "${STAR_INSTALL_BINDIR}" RENAME "starsim.bank"        OPTIONAL)
	install(FILES "${starsim_dir_abs}/atlsim.logon.kumac" DESTINATION "${STAR_INSTALL_BINDIR}" RENAME "starsim.logon.kumac" OPTIONAL)
	install(FILES "${starsim_dir_abs}/atlsim.makefile"    DESTINATION "${STAR_INSTALL_BINDIR}" RENAME "starsim.makefile"    OPTIONAL)
endfunction()


#
# Creates a library from fortran sources without doing any preprocessing
#
function(STAR_ADD_LIBRARY_BASIC star_lib_dir)

	star_target_paths(${star_lib_dir} star_lib_name star_lib_dir_abs star_lib_dir_out)

	# Change the name of the library/target and output directory if an
	# optional unnamed parameter is provided by the user
	set(user_lib_name ${ARGV1})
	if(user_lib_name)
		string(REPLACE ${star_lib_name} ${user_lib_name} star_lib_dir_out ${star_lib_dir_out})
		set(star_lib_name ${user_lib_name})
	endif()

	set(library_type ${ARGV2})

	# Deal with sources
	GET_EXCLUDE_LIST(star_lib_exclude)

	file(GLOB_RECURSE cxx_files "${star_lib_dir_abs}/*.cxx")
	file(GLOB_RECURSE c_files "${star_lib_dir_abs}/*.c")
	file(GLOB_RECURSE f_files "${star_lib_dir_abs}/*.F")
	FILTER_LIST(f_files EXCLUDE "${star_lib_exclude}")
	# Also find all *.g and *.age files. They need to be processed with agetof
	file(GLOB_RECURSE g_files "${star_lib_dir_abs}/*.g")
	star_process_g("${g_files}" f_g_files)
	file(GLOB_RECURSE age_files "${star_lib_dir_abs}/*.age")
	star_process_g("${age_files}" f_age_files)

	add_library(${star_lib_name} ${library_type} ${f_files} ${f_g_files} ${f_age_files} ${cxx_files} ${c_files})
	set_target_properties(${star_lib_name} PROPERTIES LIBRARY_OUTPUT_DIRECTORY ${star_lib_dir_out})

	install(TARGETS ${star_lib_name}
		LIBRARY DESTINATION "${STAR_INSTALL_LIBDIR}" OPTIONAL
		ARCHIVE DESTINATION "${STAR_INSTALL_LIBDIR}" OPTIONAL)
endfunction()


#
# Builds a ${star_lib_name}_Tables library from ${star_lib_dir}/idl/*.idl files
#
function(STAR_ADD_LIBRARY_TABLE star_lib_dir)

	star_target_paths(${star_lib_dir} star_lib_name star_lib_dir_abs star_lib_dir_out)

	set(_star_lib_name ${star_lib_name}_Tables)

	file(GLOB_RECURSE idl_files ${star_lib_dir_abs}/idl/*.idl)
	star_process_idl("${idl_files}" "" "${star_lib_dir_out}" sources_idl headers_idl)

	add_library(${_star_lib_name} ${sources_idl})
	set_target_properties(${_star_lib_name} PROPERTIES
		LIBRARY_OUTPUT_DIRECTORY ${star_lib_dir_out}
		PUBLIC_HEADER "${headers_idl}")

	install(TARGETS ${_star_lib_name}
		LIBRARY DESTINATION "${STAR_INSTALL_LIBDIR}" OPTIONAL
		ARCHIVE DESTINATION "${STAR_INSTALL_LIBDIR}" OPTIONAL
		PUBLIC_HEADER DESTINATION "${STAR_INSTALL_INCLUDEDIR}/tables" OPTIONAL)
endfunction()


#
# Build customized library StGenericVertexMakerNoSti based on StGenericVertexMaker
#
function(STAR_ADD_LIBRARY_VERTEXNOSTI star_lib_dir)

	star_target_paths(${star_lib_dir} star_lib_name star_lib_dir_abs star_lib_dir_out)

	string(REPLACE ${star_lib_name} StGenericVertexMakerNoSti star_lib_dir_out ${star_lib_dir_out})
	set(vtxnosti_headers
		${star_lib_dir_abs}/StGenericVertexMaker.h
		${star_lib_dir_abs}/Minuit/St_VertexCutsC.h)

	GET_ROOT_DICT_FILE_NAMES(_linkdef_file _dictinc_file _dict_source _dict_header)

	file(GLOB_RECURSE _cxx_files "${star_lib_dir_abs}/*.cxx")
	# Remove source files coming from the following subdirectories
	FILTER_LIST(_cxx_files EXCLUDE "StiPPVertex/" "StvPPVertex/" "macros/")

	add_library(StGenericVertexMakerNoSti ${_cxx_files} ${_dict_source})
	# Output the library to the respecitve subdirectory in the binary directory
	set_target_properties(StGenericVertexMakerNoSti PROPERTIES LIBRARY_OUTPUT_DIRECTORY ${star_lib_dir_out})

	add_custom_command(
		OUTPUT ${_dictinc_file} ${_linkdef_file}
		COMMAND ${PROJECT_SOURCE_DIR}/scripts/gen_linkdef.sh -l ${_linkdef_file} -d ${_dictinc_file} ${vtxnosti_headers}
		DEPENDS ${PROJECT_SOURCE_DIR}/scripts/gen_linkdef.sh ${vtxnosti_headers} )

	get_property(global_include_dirs DIRECTORY PROPERTY INCLUDE_DIRECTORIES)
	string( REGEX REPLACE "([^;]+)" "-I\\1" global_include_dirs "${global_include_dirs}" )

	# Generate ROOT dictionary using the *_LinkDef.h and *_DictInc.h files
	add_custom_command(
		OUTPUT ${_dict_source}
		COMMAND ${ROOT_DICTGEN_EXECUTABLE} -cint -f ${_dict_source}
		-c -p -D__ROOT__ ${global_include_dirs}
		${vtxnosti_headers} ${_dictinc_file} ${_linkdef_file}
		DEPENDS ${vtxnosti_headers} ${_dictinc_file} ${_linkdef_file}
		VERBATIM )

	install(TARGETS StGenericVertexMakerNoSti
		LIBRARY DESTINATION "${STAR_INSTALL_LIBDIR}" OPTIONAL
		ARCHIVE DESTINATION "${STAR_INSTALL_LIBDIR}" OPTIONAL)
endfunction()


#
# Generate source and header files from idl files
#
function(STAR_PROCESS_IDL idl_files star_lib_name star_lib_dir_out out_sources_idl out_headers_idl)
	# Define internal lists to be filled
	set(_out_sources_idl)
	set(_out_headers_idl)

	# Define common output directories for files generated from the input idl files
	# in addition to star_lib_dir_out
	set(outpath_table_struct ${CMAKE_CURRENT_BINARY_DIR}/include)
	set(outpath_table_ttable ${CMAKE_CURRENT_BINARY_DIR}/include/tables/${star_lib_name})

	foreach(idll ${idl_files})

		file(STRINGS ${idll} idl_matches_module REGEX "interface.*:.*amiModule")

		if(idl_matches_module)
			_star_process_idl_module(${idll} _sources_idl _headers_idl)
		else()
			_star_process_idl_file(${idll} _sources_idl _headers_idl)
		endif()

		list(APPEND _out_sources_idl ${_sources_idl})
		list(APPEND _out_headers_idl ${_headers_idl})
	endforeach()

	set(${out_sources_idl} ${_out_sources_idl} PARENT_SCOPE)
	set(${out_headers_idl} ${_out_headers_idl} PARENT_SCOPE)
endfunction()



function(_STAR_PROCESS_IDL_FILE idll out_sources out_headers)

	find_program(PERL_EXECUTABLE perl)

	# For the file and variable names we closely follow the convention in mgr/Conscript-standard
	get_filename_component(idl ${idll} NAME_WE)
	set(idlh "${outpath_table_struct}/${idl}.h")
	set(idli "${outpath_table_struct}/${idl}.inc")
	set(idlH "${outpath_table_ttable}/St_${idl}_Table.h")
	set(idlC "${star_lib_dir_out}/St_${idl}_Table.cxx")
	set(idlLinkDef "${star_lib_dir_out}/${idl}LinkDef.h")
	set(idlCintH "${star_lib_dir_out}/St_${idl}_TableCint.h")
	set(idlCintC "${star_lib_dir_out}/St_${idl}_TableCint.cxx")


	add_custom_command(
		OUTPUT ${idlh} ${idli}
		COMMAND ${CMAKE_CURRENT_BINARY_DIR}/stic -q ${idll}
		COMMAND ${CMAKE_COMMAND} -E make_directory ${outpath_table_struct}
		COMMAND ${CMAKE_COMMAND} -E rename ${idl}.h ${idlh}
		COMMAND ${CMAKE_COMMAND} -E rename ${idl}.inc ${idli}
		DEPENDS ${idll} stic
		VERBATIM )

	add_custom_command(
		OUTPUT ${idlH} ${idlC} ${idlLinkDef}
		COMMAND ${CMAKE_COMMAND} -E make_directory ${star_lib_dir_out}
		COMMAND ${CMAKE_COMMAND} -E make_directory ${outpath_table_ttable}
		COMMAND ${PERL_EXECUTABLE} ${STAR_SRC}/mgr/ConstructTable.pl ${idll} ${idlH}
		COMMAND ${PERL_EXECUTABLE} ${STAR_SRC}/mgr/ConstructTable.pl ${idll} ${idlC}
		COMMAND ${PERL_EXECUTABLE} ${STAR_SRC}/mgr/ConstructTable.pl ${idll} ${idlLinkDef}
		DEPENDS ${STAR_SRC}/mgr/ConstructTable.pl ${idll}
		VERBATIM )

	add_custom_command(
		OUTPUT ${idlCintC} ${idlCintH}
		COMMAND ${ROOT_DICTGEN_EXECUTABLE} -cint -f ${idlCintC} -c -p -D__ROOT__
		-I${outpath_table_struct} -I${outpath_table_ttable} ${idlh} ${idlH} ${idlLinkDef}
		DEPENDS ${idlh} ${idlH} ${idlLinkDef}
		VERBATIM )

	set(${out_sources} ${idlC} ${idlCintC} PARENT_SCOPE)
	set(${out_headers} ${idlH} PARENT_SCOPE)
endfunction()



function(_STAR_PROCESS_IDL_MODULE idll out_sources out_headers)
	# For the file and variable names we closely follow the convention in mgr/Conscript-standard
	get_filename_component(idl ${idll} NAME_WE)
	set(idlh "${outpath_table_struct}/${idl}.h")
	set(idli "${outpath_table_struct}/${idl}.inc")
	set(modH "${outpath_table_ttable}/St_${idl}_Module.h")
	set(modC "${star_lib_dir_out}/St_${idl}_Module.cxx")
	set(idlLinkDef "${star_lib_dir_out}/${idl}LinkDef.h")
	set(idlCintH "${star_lib_dir_out}/St_${idl}_ModuleCint.h")
	set(idlCintC "${star_lib_dir_out}/St_${idl}_ModuleCint.cxx")

	add_custom_command(
		OUTPUT ${idlh} ${idli} ${modH} ${modC} ${idlLinkDef}
		COMMAND ${CMAKE_CURRENT_BINARY_DIR}/stic -s -r -q ${idll}
		COMMAND ${CMAKE_COMMAND} -E make_directory ${star_lib_dir_out}
		COMMAND ${CMAKE_COMMAND} -E make_directory ${outpath_table_struct}
		COMMAND ${CMAKE_COMMAND} -E make_directory ${outpath_table_ttable}
		COMMAND ${CMAKE_COMMAND} -E rename St_${idl}_Module.h ${modH}
		COMMAND ${CMAKE_COMMAND} -E rename St_${idl}_Module.cxx ${modC}
		COMMAND ${CMAKE_COMMAND} -E rename ${idl}.h ${idlh}
		COMMAND ${CMAKE_COMMAND} -E rename ${idl}.inc ${idli}
		COMMAND ${CMAKE_COMMAND} -E remove ${idl}.c.template ${idl}.F.template ${idl}_i.cc
		COMMAND ${CMAKE_COMMAND} -E echo "#pragma link C++ class St_${idl}-;" > ${idlLinkDef}
		DEPENDS ${idll} stic
		VERBATIM )

	add_custom_command(
		OUTPUT ${idlCintC} ${idlCintH}
		COMMAND ${ROOT_DICTGEN_EXECUTABLE} -cint -f ${idlCintC} -c -p -D__ROOT__
		-I${outpath_table_struct} -I${outpath_table_ttable} ${modH} ${idlLinkDef}
		DEPENDS ${modH} ${idlLinkDef}
		VERBATIM )

	set(${out_sources} ${modC} ${idlCintC} PARENT_SCOPE)
	set(${out_headers} ${modH} PARENT_SCOPE)
endfunction()



function(STAR_PROCESS_F in_F_files star_lib_dir_out out_F_files)

	set(_out_F_files)

	foreach(f_file ${in_F_files})
		get_filename_component(f_file_name_we ${f_file} NAME_WE)
		set(g_file "${star_lib_dir_out}/${f_file_name_we}.g")
		set(out_F_file "${star_lib_dir_out}/${f_file_name_we}.F")

		get_property(_agetof_additional_options SOURCE ${f_file} PROPERTY AGETOF_ADDITIONAL_OPTIONS)
		get_property(_f_file_include_dirs SOURCE ${f_file} PROPERTY INCLUDE_DIRECTORIES)
		string(REGEX REPLACE "([^;]+)" "-I\\1" _f_file_include_dirs "${_f_file_include_dirs}")

		add_custom_command(
			OUTPUT ${g_file} ${out_F_file}
			COMMAND ${CMAKE_COMMAND} -E make_directory ${star_lib_dir_out}
			COMMAND ${CMAKE_C_COMPILER} -E -P ${STAR_Fortran_DEFINITIONS} ${_f_file_include_dirs} ${f_file} -o ${g_file}
			COMMAND ${CMAKE_CURRENT_BINARY_DIR}/agetof -V 1 ${_agetof_additional_options} ${g_file} -o ${out_F_file}
			DEPENDS ${f_file} agetof VERBATIM)

		list(APPEND _out_F_files ${out_F_file})
	endforeach()

	set(${out_F_files} ${_out_F_files} PARENT_SCOPE)
endfunction()



function(STAR_PROCESS_G in_g_files out_F_files)

	set(_out_F_files)

	foreach(g_file ${in_g_files})
		get_filename_component(g_file_name_we ${g_file} NAME_WE)
		get_filename_component(g_file_directory ${g_file} DIRECTORY)
		file(RELATIVE_PATH _rel_path ${STAR_SRC} ${g_file_directory})
		set(_star_lib_dir_out ${CMAKE_CURRENT_BINARY_DIR}/${_rel_path})
		set(out_F_file "${_star_lib_dir_out}/${g_file_name_we}.F")

		add_custom_command(
			OUTPUT ${out_F_file}
			COMMAND ${CMAKE_COMMAND} -E make_directory ${_star_lib_dir_out}
			COMMAND ${CMAKE_CURRENT_BINARY_DIR}/agetof -V 1 ${g_file} -o ${out_F_file}
			DEPENDS ${g_file} agetof VERBATIM)

		list(APPEND _out_F_files ${out_F_file})
	endforeach()

	set(${out_F_files} ${_out_F_files} PARENT_SCOPE)
endfunction()



function(STAR_ADD_EXECUTABLE_ROOT4STAR star_exec_dir)

	star_target_paths(${star_exec_dir} dummy exec_dir_abs exec_dir_out)

	add_executable(root4star
		${exec_dir_abs}/MAIN_rmain.cxx
		${exec_dir_abs}/df.F
		${exec_dir_abs}/TGeant3/StarMC.cxx
		${exec_dir_abs}/TGeant3/TGiant3.cxx
		${exec_dir_abs}/TGeant3/gcadd.cxx
		${exec_dir_abs}/TGeant3/galicef.F
		${exec_dir_abs}/TGeant3/gcomad.F
		${exec_dir_out}/rexe_dict.cxx
	)

	target_include_directories(root4star PRIVATE
		"${STAR_SRC}/asps/Simulation/geant321/include")

	# Generate rexe_dict.cxx
	add_custom_command(
		OUTPUT ${exec_dir_out}/rexe_dict.cxx
		COMMAND ${CMAKE_COMMAND} -E make_directory ${exec_dir_out}
		COMMAND ${ROOT_DICTGEN_EXECUTABLE} -cint -f  ${exec_dir_out}/rexe_dict.cxx -c -p -D__ROOT__
		-I${exec_dir_abs}/TGeant3/ StarMC.h TGiant3.h ${exec_dir_abs}/rexeLinkDef.h
		DEPENDS  ${exec_dir_abs}/TGeant3/StarMC.h ${exec_dir_abs}/TGeant3/TGiant3.h ${exec_dir_abs}/rexeLinkDef.h
		VERBATIM )

	target_link_libraries(root4star -Wl,--whole-archive -Wl,-Bstatic -Wl,-z -Wl,muldefs starsimlib -Wl,--no-whole-archive -Wl,-Bdynamic geant321 gcalor
		${ROOT_LIBRARIES} ${CERNLIB_LIBRARIES} ${MYSQL_LIBRARIES} gfortran Xt Xpm X11 Xm dl crypt)
	set_target_properties(root4star PROPERTIES LINK_FLAGS "-Wl,-export-dynamic")

	install(TARGETS root4star
		RUNTIME DESTINATION "${STAR_INSTALL_BINDIR}" OPTIONAL)
endfunction()



function(STAR_ADD_EXECUTABLE_AGETOF star_exec_dir)

	star_target_paths(${star_exec_dir} dummy exec_dir_abs dummy)

	add_executable(agetof
		${exec_dir_abs}/readlink.c
		${exec_dir_abs}/afexist.F
		${exec_dir_abs}/afname.F
		${exec_dir_abs}/cccard.F
		${exec_dir_abs}/crit.F
		${exec_dir_abs}/define.F
		${exec_dir_abs}/doit.F
		${exec_dir_abs}/dumdum.F
		${exec_dir_abs}/expand.F
		${exec_dir_abs}/filter.F
		${exec_dir_abs}/find.F
		${exec_dir_abs}/inital.F
		${exec_dir_abs}/ispec.F
		${exec_dir_abs}/kat.F
		${exec_dir_abs}/krunc.F
		${exec_dir_abs}/lexp.F
		${exec_dir_abs}/linf.F
		${exec_dir_abs}/llong.F
		${exec_dir_abs}/mactrc.F
		${exec_dir_abs}/main.F
		${exec_dir_abs}/mesage.F
		${exec_dir_abs}/muser.F
		${exec_dir_abs}/n5.F
		${exec_dir_abs}/nib.F
		${exec_dir_abs}/nxtcrd.F
		${exec_dir_abs}/raw.F
		${exec_dir_abs}/rw.F
		${exec_dir_abs}/user.F)

	add_custom_command(TARGET agetof PRE_BUILD
		COMMAND ${CMAKE_COMMAND} -E copy ${exec_dir_abs}/agetof.def ${CMAKE_CURRENT_BINARY_DIR}
		VERBATIM)

	target_link_libraries(agetof ${ROOT_LIBRARIES} ${CERNLIB_LIBRARIES})
endfunction()



function(STAR_ADD_EXECUTABLE_STIC star_exec_dir)

	star_target_paths(${star_exec_dir} orig_exec_name exec_dir_abs exec_dir_out)

	string(REPLACE ${orig_exec_name} stic exec_dir_out ${exec_dir_out})

	add_executable(stic ${exec_dir_out}/idl-yacc.c ${exec_dir_abs}/templateStuff.c)
	target_include_directories(stic PRIVATE ${exec_dir_abs} ${exec_dir_out})
	target_link_libraries(stic fl)

	find_program(BISON_EXECUTABLE NAMES bison yacc)
	add_custom_command(
		OUTPUT ${exec_dir_out}/idl-yacc.c
		COMMAND ${CMAKE_COMMAND} -E make_directory ${exec_dir_out}
		COMMAND ${BISON_EXECUTABLE} -o ${exec_dir_out}/idl-yacc.c ${exec_dir_abs}/idl.y
		DEPENDS ${exec_dir_abs}/idl.y ${exec_dir_abs}/stic.h ${exec_dir_out}/idl-lex.c
		VERBATIM)

	find_program(FLEX_EXECUTABLE flex)
	add_custom_command(
		OUTPUT ${exec_dir_out}/idl-lex.c
		COMMAND ${CMAKE_COMMAND} -E make_directory ${exec_dir_out}
		COMMAND ${FLEX_EXECUTABLE} -o ${exec_dir_out}/idl-lex.c ${exec_dir_abs}/idl.l
		DEPENDS ${exec_dir_abs}/idl.l
		VERBATIM)
endfunction()

#
# Build executable tools in RTS_EXAMPLE
#
function(STAR_ADD_EXECUTABLE_RTS_EXAMPLES)
	file(GLOB rts_main_files "${STAR_SRC}/StRoot/RTS/src/RTS_EXAMPLE/*.C")
	foreach(main_file ${rts_main_files})
		get_filename_component(exe ${main_file} NAME_WLE)
		add_executable(${exe} ${STAR_SRC}/StRoot/RTS/src/RTS_EXAMPLE/${exe}.C)
		target_link_libraries(${exe} RTS ${ROOT_LIBRARIES})
		target_include_directories(${exe} BEFORE PRIVATE
			"${STAR_SRC}/StRoot/RTS/include"
			"${STAR_SRC}/StRoot/RTS/trg/include"
			"${STAR_SRC}/StRoot/RTS/src")

		install(TARGETS ${exe}
			RUNTIME DESTINATION "${STAR_INSTALL_BINDIR}" OPTIONAL)
	endforeach()
endfunction()

#
# Some targets require specific treatment due to missing functionality
# originally available in cons
#
set(St_base_LINKDEF_HEADERS "${STAR_SRC}/StRoot/St_base/Stypes.h")
set(StEvent_LINKDEF_HEADERS
	"${STAR_SRC}/StRoot/StDaqLib/TRG/trgStructures.h"
	"${STAR_SRC}/StRoot/StDaqLib/TRG/trgStructures2000.h"
	"${STAR_SRC}/StRoot/StDaqLib/TRG/trgStructures2002.h"
	"${STAR_SRC}/StRoot/StDaqLib/TRG/trgStructures2003.h"
	"${STAR_SRC}/StRoot/StDaqLib/TRG/trgStructures2004.h"
	"${STAR_SRC}/StRoot/StDaqLib/TRG/trgStructures2005.h"
	"${STAR_SRC}/StRoot/StDaqLib/TRG/trgStructures2007.h"
	"${PROJECT_SOURCE_DIR}/star-aux/StArray_cint.h")
set(StEStructPool_LINKDEF_HEADERS
	"${PROJECT_SOURCE_DIR}/star-aux/StArray_cint.h")
