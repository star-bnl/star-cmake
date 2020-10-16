cmake_minimum_required(VERSION 3.6 FATAL_ERROR)

find_program(CCACHE_EXECUTABLE ccache)
if(CCACHE_EXECUTABLE)
	set_property(GLOBAL PROPERTY RULE_LAUNCH_COMPILE ${CCACHE_EXECUTABLE})
	set_property(GLOBAL PROPERTY RULE_LAUNCH_LINK ${CCACHE_EXECUTABLE})
endif()

project(star-sw LANGUAGES C CXX Fortran)

set(STAR_SRC "$ENV{STAR}" CACHE PATH "Path to star software source code")

# Add to path in order to pick up the FindXXX.cmake files included in this project
list(APPEND CMAKE_MODULE_PATH "${PROJECT_SOURCE_DIR}/cmake")

# STAR software depends on ROOT
find_package(ROOT REQUIRED COMPONENTS Table HistPainter Minuit Geom Spectrum Vc)

if(${ROOT_CXX_FLAGS} MATCHES "-m32")
	message(STATUS "star-sw: Found -m32 option in $ROOT_CXX_FLAGS (root-config). Will proceed with 32 bit build")
	set_property(GLOBAL PROPERTY FIND_LIBRARY_USE_LIB64_PATHS FALSE)
	set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -m32")
	set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -m32")
	set(CMAKE_Fortran_FLAGS "${CMAKE_Fortran_FLAGS} -m32")
endif()

find_package(LibXml2 REQUIRED)
find_package(Log4Cxx REQUIRED)
find_package(MySQL REQUIRED)
find_package(CERNLIB REQUIRED COMPONENTS pawlib graflib packlib grafX11 mathlib kernlib jetset74 lapack blas)

# Perform some setup standard to STAR experiment environment
include(cmake/StarCommon.cmake)

message(STATUS "star-sw: CMAKE_CXX_FLAGS = \"${CMAKE_CXX_FLAGS}\"")
message(STATUS "star-sw: CMAKE_C_FLAGS = \"${CMAKE_C_FLAGS}\"")
message(STATUS "star-sw: CMAKE_Fortran_FLAGS = \"${CMAKE_Fortran_FLAGS}\"")

set(STAR_PATCH "NO" CACHE STRING "Requested patch ID")
star_apply_patch()

star_preinstall_headers(${CMAKE_BINARY_DIR}/include)
star_preinstall_geo_pams(${CMAKE_BINARY_DIR})

# Include directories common to all targets
include_directories(
	${ROOT_INCLUDE_DIR}
	${STAR_SRC}/StRoot
	# Inlude path to header files with flatten directory hierarchy
	# created by star_preinstall_headers()
	${CMAKE_BINARY_DIR}/include
)

include("cmake/source_properties.cmake")

# Build executables
star_add_executable_root4star(asps/rexe)
star_add_executable_stic(asps/staf/sdd/src)
star_add_executable_agetof(asps/Simulation/agetof)
star_add_executable_rts_examples()

# Build libraries containing idl/*.idl table descriptions
star_add_library_table(StDb)
star_add_library_table(pams/ctf)
star_add_library_table(pams/emc)
star_add_library_table(pams/ftpc)
star_add_library_table(pams/gen)
star_add_library_table(pams/geometry)
star_add_library_table(pams/global)
star_add_library_table(pams/sim)
star_add_library_table(pams/svt)

# Build library with STAR geometry
star_add_library_geometry(StarVMC/Geometry StarGeometry RootTGeo)
star_add_library_geometry(StarVMC/Geometry xgeometry GeantGeo)
star_add_library_starsim(asps/Simulation/starsim)
star_add_library_basic(asps/Simulation/geant321 "" STATIC)
star_add_library_basic(asps/Simulation/gcalor)
star_add_library_basic(StRoot/StarMagField StarMagFieldNoDict)
star_add_library_basic(StarVMC/StarAgmlUtil)

star_add_library(pams/geometry)
star_add_library(pams/ctf St_ctf)
star_add_library(pams/sim/g2t St_g2t)
star_add_library(pams/sim/gstar)

GET_SUBDIRS(star_lib_dirs "StarVMC;StRoot")
if(EXISTS ${PROJECT_SOURCE_DIR}/StRoot)
	GET_SUBDIRS(star_lib_dirs "StarVMC;StRoot;${PROJECT_SOURCE_DIR}/StRoot")
endif()
FILTER_LIST(star_lib_dirs EXCLUDE ${STAR_BLACKLISTED_LIB_DIRS})

foreach(star_lib_dir ${star_lib_dirs})
	star_add_library(${star_lib_dir})
endforeach()

# Generate cmake file with library dependencies
set(STAR_TARGET_DEPENDENCIES_FILE ${CMAKE_BINARY_DIR}/target_dependencies.cmake)
if(NOT EXISTS ${STAR_TARGET_DEPENDENCIES_FILE})
	execute_process(COMMAND ${PROJECT_SOURCE_DIR}/scripts/dep_scan.py ${STAR_SRC}
		OUTPUT_FILE ${STAR_TARGET_DEPENDENCIES_FILE})
endif()

include(${STAR_TARGET_DEPENDENCIES_FILE})
include("cmake/target_properties.cmake")


# Copy all header files preserving the subdirectory tree hierarchy
install(DIRECTORY
	${STAR_SRC}/StarVMC
	${STAR_SRC}/StRoot
	DESTINATION ${STAR_INSTALL_INCLUDEDIR}
	FILES_MATCHING REGEX "\\.(h|hh)$")

# Copy all generated headers (and the flatten hierarchy of the headers files)
# from the build directory. This is done for backward compatibility when the
# dependent code does not specify the prefix relative to $STAR_SRC in #include
# directives
install(DIRECTORY
	${CMAKE_BINARY_DIR}/include/
	DESTINATION ${STAR_INSTALL_PREFIX}/include_all/
	FILES_MATCHING REGEX "\\.(h|hh)$")

install(DIRECTORY
	${STAR_SRC}/StRoot/StBFChain
	${STAR_SRC}/StRoot/macros
	DESTINATION StRoot/)

# Some modules depend on DB entries available on disk
install(DIRECTORY
	${STAR_SRC}/StarDb/AgMLGeometry
	${STAR_SRC}/StarDb/Geometry
	${STAR_SRC}/StarDb/StMagF
	${STAR_SRC}/StarDb/dEdxModel
	${STAR_SRC}/StarDb/tpc
	${STAR_SRC}/StarDb/ctf
	DESTINATION StarDb/)

install(DIRECTORY
	${STAR_SRC}/StarDb/Calibrations
	DESTINATION StarDb/
	REGEX "tpcDriftVelocity.20000" EXCLUDE)

install(DIRECTORY
	${STAR_SRC}/StDb/servers
	DESTINATION StDb/)

install(FILES
	${PROJECT_SOURCE_DIR}/star-aux/rootlogon.C
	${PROJECT_SOURCE_DIR}/star-aux/rootlogoff.C
	DESTINATION StRoot/macros)

configure_file(${PROJECT_SOURCE_DIR}/scripts/thisstar.sh.in ${STAR_INSTALL_BINDIR}/thisstar.sh @ONLY NEWLINE_STYLE UNIX)
configure_file(${PROJECT_SOURCE_DIR}/scripts/.rootrc.in ${CMAKE_INSTALL_PREFIX}/.rootrc @ONLY NEWLINE_STYLE UNIX)
configure_file(${PROJECT_SOURCE_DIR}/scripts/entrypoint.sh.in ${CMAKE_INSTALL_PREFIX}/.entrypoint.sh @ONLY NEWLINE_STYLE UNIX)
