# - Finds ROOT instalation
# This module sets up ROOT information
# It defines:
# ROOT_FOUND          If the ROOT is found
# ROOT_INCLUDE_DIR    PATH to the include directory
# ROOT_INCLUDE_DIRS   PATH to the include directories (not cached)
# ROOT_LIBRARIES      Most common libraries
# ROOT_<name>_LIBRARY Full path to the library <name>
# ROOT_LIBRARY_DIR    PATH to the library directory
# ROOT_DEFINITIONS    Compiler definitions
# ROOT_CXX_FLAGS      Compiler flags to used by client packages
# ROOT_C_FLAGS        Compiler flags to used by client packages
#
# Updated by K. Smith (ksmith37@nd.edu) to properly handle
#  dependencies in ROOT_GENERATE_DICTIONARY

find_program(ROOT_CONFIG_EXECUTABLE root-config
  HINTS $ENV{ROOTSYS}/bin)

execute_process(
    COMMAND ${ROOT_CONFIG_EXECUTABLE} --prefix
    OUTPUT_VARIABLE ROOTSYS
    OUTPUT_STRIP_TRAILING_WHITESPACE)

execute_process(
    COMMAND ${ROOT_CONFIG_EXECUTABLE} --version
    OUTPUT_VARIABLE ROOT_VERSION
    OUTPUT_STRIP_TRAILING_WHITESPACE)

execute_process(
    COMMAND ${ROOT_CONFIG_EXECUTABLE} --incdir
    OUTPUT_VARIABLE ROOT_INCLUDE_DIR
    OUTPUT_STRIP_TRAILING_WHITESPACE)
set(ROOT_INCLUDE_DIRS ${ROOT_INCLUDE_DIR})

execute_process(
    COMMAND ${ROOT_CONFIG_EXECUTABLE} --libdir
    OUTPUT_VARIABLE ROOT_LIBRARY_DIR
    OUTPUT_STRIP_TRAILING_WHITESPACE)
set(ROOT_LIBRARY_DIRS ${ROOT_LIBRARY_DIR})

set(rootlibs Core Cint RIO Net Hist Graf Graf3d Gpad Tree Rint Postscript Matrix Physics MathCore Thread)
set(ROOT_LIBRARIES)
foreach(_cpt ${rootlibs} ${ROOT_FIND_COMPONENTS})
  find_library(ROOT_${_cpt}_LIBRARY ${_cpt} HINTS ${ROOT_LIBRARY_DIR})
  if(ROOT_${_cpt}_LIBRARY)
    mark_as_advanced(ROOT_${_cpt}_LIBRARY)
    list(APPEND ROOT_LIBRARIES ${ROOT_${_cpt}_LIBRARY})
    list(REMOVE_ITEM ROOT_FIND_COMPONENTS ${_cpt})
  endif()
endforeach()
list(REMOVE_DUPLICATES ROOT_LIBRARIES)

execute_process(
    COMMAND ${ROOT_CONFIG_EXECUTABLE} --cflags
    OUTPUT_VARIABLE __cflags
    OUTPUT_STRIP_TRAILING_WHITESPACE)
string(REGEX MATCHALL "-(D|U)[^ ]*" ROOT_DEFINITIONS "${__cflags}")
string(REGEX REPLACE "(^|[ ]*)-I[^ ]*" "" ROOT_CXX_FLAGS "${__cflags}")
string(REGEX REPLACE "(^|[ ]*)-I[^ ]*" "" ROOT_C_FLAGS "${__cflags}")

set(ROOT_USE_FILE ${CMAKE_CURRENT_LIST_DIR}/RootUseFile.cmake)

execute_process(
  COMMAND ${ROOT_CONFIG_EXECUTABLE} --features
  OUTPUT_VARIABLE _root_options
  OUTPUT_STRIP_TRAILING_WHITESPACE)
separate_arguments(_root_options)
foreach(_opt ${_root_options})
  set(ROOT_${_opt}_FOUND TRUE)
endforeach()

include(FindPackageHandleStandardArgs)
find_package_handle_standard_args(ROOT DEFAULT_MSG ROOT_CONFIG_EXECUTABLE
    ROOTSYS ROOT_VERSION ROOT_INCLUDE_DIR ROOT_LIBRARIES ROOT_LIBRARY_DIR)

mark_as_advanced(ROOT_CONFIG_EXECUTABLE)

include(CMakeParseArguments)
find_program(ROOTCINT_EXECUTABLE rootcint HINTS $ENV{ROOTSYS}/bin)
find_program(GENREFLEX_EXECUTABLE genreflex HINTS $ENV{ROOTSYS}/bin)
find_package(GCCXML)

#----------------------------------------------------------------------------
# function ROOT_GENERATE_DICTIONARY( dictionary
#                                    header1 header2 ...
#                                    LINKDEF linkdef1 ...
#                                    OPTIONS opt1...)
function(ROOT_GENERATE_DICTIONARY dictionary)
  CMAKE_PARSE_ARGUMENTS(ARG "" "" "LINKDEF;OPTIONS" "" ${ARGN})
  #---Get the list of include directories------------------
  get_directory_property(incdirs INCLUDE_DIRECTORIES)
  list(REMOVE_ITEM incdirs "/usr/include")
  set(includedirs)
  foreach( d ${incdirs})
     set(includedirs ${includedirs} -I${d})
  endforeach()
  #---Get the list of header files-------------------------
  set(headerfiles)
  foreach(fp ${ARG_UNPARSED_ARGUMENTS})
    if(${fp} MATCHES "[*?]") # Is this header a globbing expression?
      file(GLOB files ${fp})
      foreach(f ${files})
        if(NOT f MATCHES LinkDef) # skip LinkDefs from globbing result
          set(headerfiles ${headerfiles} ${f})
        endif()
      endforeach()
    else()
      if(IS_ABSOLUTE ${fp})
        set(headerFile ${fp})
      else()
        find_file(headerFile ${fp} HINTS ${incdirs})
      endif()
      set(headerfiles ${headerfiles} ${headerFile})
      unset(headerFile CACHE)
    endif()
  endforeach()
  #---Get LinkDef.h file------------------------------------
  set(linkdefs)
  foreach( f ${ARG_LINKDEF})
    if(IS_ABSOLUTE ${f})
      set(linkFile ${f})
    else()
      find_file(linkFile ${f} HINTS ${incdirs})
    endif()
    set(linkdefs ${linkdefs} ${linkFile})
    unset(linkFile CACHE)
  endforeach()
  #---call rootcint------------------------------------------
  add_custom_command(OUTPUT ${dictionary}.cxx
                     COMMAND ${ROOTCINT_EXECUTABLE} -cint -f  ${dictionary}.cxx
                                          -c ${ARG_OPTIONS} ${includedirs} ${headerfiles} ${linkdefs}
                     DEPENDS ${headerfiles} ${linkdefs} VERBATIM)
endfunction()

#----------------------------------------------------------------------------
# function REFLEX_GENERATE_DICTIONARY(dictionary
#                                     header1 header2 ...
#                                     SELECTION selectionfile ...
#                                     OPTIONS opt1...)
function(REFLEX_GENERATE_DICTIONARY dictionary)
  CMAKE_PARSE_ARGUMENTS(ARG "" "" "SELECTION;OPTIONS" "" ${ARGN})
  #---Get the list of header files-------------------------
  set(headerfiles)
  foreach(fp ${ARG_UNPARSED_ARGUMENTS})
    file(GLOB files ${fp})
    if(files)
      foreach(f ${files})
        set(headerfiles ${headerfiles} ${f})
      endforeach()
    else()
      set(headerfiles ${headerfiles} ${fp})
    endif()
  endforeach()
  #---Get Selection file------------------------------------
  if(IS_ABSOLUTE ${ARG_SELECTION})
    set(selectionfile ${ARG_SELECTION})
  else()
    set(selectionfile ${CMAKE_CURRENT_SOURCE_DIR}/${ARG_SELECTION})
  endif()
  #---Get the list of include directories------------------
  get_directory_property(incdirs INCLUDE_DIRECTORIES)
  set(includedirs)
  foreach( d ${incdirs})
    set(includedirs ${includedirs} -I${d})
  endforeach()
  #---Get preprocessor definitions--------------------------
  get_directory_property(defs COMPILE_DEFINITIONS)
  foreach( d ${defs})
   set(definitions ${definitions} -D${d})
  endforeach()
  #---Nanes and others---------------------------------------
  set(gensrcdict ${dictionary}.cpp)
  if(MSVC)
    set(gccxmlopts "--gccxmlopt=\"--gccxml-compiler cl\"")
  else()
    #set(gccxmlopts "--gccxmlopt=\'--gccxml-cxxflags -m64 \'")
    set(gccxmlopts)
  endif()
  #set(rootmapname ${dictionary}Dict.rootmap)
  #set(rootmapopts --rootmap=${rootmapname} --rootmap-lib=${libprefix}${dictionary}Dict)
  #---Check GCCXML and get path-----------------------------
  if(GCCXML)
    get_filename_component(gccxmlpath ${GCCXML} PATH)
  else()
    message(WARNING "GCCXML not found. Install and setup your environment to find 'gccxml' executable")
  endif()
  #---Actual command----------------------------------------
  add_custom_command(OUTPUT ${gensrcdict} ${rootmapname}
                     COMMAND ${GENREFLEX_EXECUTABLE} ${headerfiles} -o ${gensrcdict} ${gccxmlopts} ${rootmapopts} --select=${selectionfile}
                             --gccxmlpath=${gccxmlpath} ${ARG_OPTIONS} ${includedirs} ${definitions}
                     DEPENDS ${headerfiles} ${selectionfile})
endfunction()


#
# function ROOT_GENERATE_LINKDEF( header_linkdef HEADERS header1 header2 ...)
#
# Generates a basic LinkDef header (header_linkdef) by parsing the user provided
# header files with standard linux utilities such as grep, awk, and sed.
#
function(ROOT_GENERATE_LINKDEF header_linkdef)

   message(STATUS "Generating LinkDef header: ${header_linkdef}")

   find_program(EXEC_GREP NAMES grep)
   find_program(EXEC_AWK NAMES gawk awk)
   find_program(EXEC_SED NAMES gsed sed)

   if(NOT EXEC_GREP OR NOT EXEC_AWK OR NOT EXEC_SED)
      message(FATAL_ERROR "FATAL: ROOT_GENERATE_LINKDEF function requires grep, awk, and sed commands")
   endif()

   CMAKE_PARSE_ARGUMENTS(ARG "" "" "HEADERS" ${ARGN})

   # Create the list of header files with ClassDef macros
   set(headers_cint)

   foreach(user_header_arg ${ARG_HEADERS})

      file(GLOB user_headers ${user_header_arg})

      foreach(header ${user_headers})
        if(header MATCHES LinkDef) # skip LinkDefs from globbing result
           continue()
        endif()

        # Build a list of user_headers to use in dictionary generation
        execute_process(COMMAND ${EXEC_GREP} -m1 -H ClassDef ${header} RESULT_VARIABLE exit_code OUTPUT_QUIET)
        if (NOT ${exit_code})
           list(APPEND headers_cint ${header})
        else()
           message(STATUS "WARNING: No ClassDef macro found in ${header}")
        endif()

      endforeach()
   endforeach()


   set(cint_dict_objects)

   foreach(header ${headers_cint})
      set(my_exec_cmd ${EXEC_AWK} "match($0,\"^[[:space:]]*ClassDef(.*)\\\\(([^#]+),(.*)\\\\)\",a){ printf(a[2]\"\\r\") }")

      execute_process(COMMAND ${my_exec_cmd} ${header} COMMAND ${EXEC_SED} -e "s/\\s\\+/;/g"
         RESULT_VARIABLE exit_code OUTPUT_VARIABLE extracted_dict_objects ERROR_VARIABLE extracted_dict_objects
         OUTPUT_STRIP_TRAILING_WHITESPACE)
      list(APPEND cint_dict_objects ${extracted_dict_objects})
   endforeach()

   # Create and write contents to LinkDef file
   set_source_files_properties(${header_linkdef} PROPERTIES GENERATED TRUE)
   file(WRITE ${header_linkdef}
      "#ifdef __CINT__\n\n#pragma link off all globals;\n#pragma link off all classes;\n#pragma link off all functions;\n\n")

   foreach(cint_dict_object ${cint_dict_objects})
      file(APPEND ${header_linkdef} "#pragma link C++ class ${cint_dict_object}+;\n")
   endforeach()

   file(APPEND ${header_linkdef} "\n#endif\n")

endfunction()


#
# function ROOT_GENERATE_LINKDEF( user_base_file_name HEADERS header1 header2 ...)
#
# A high level wrapper around the above function to simplify user calls
#
function(ROOT_GENERATE_LINKDEF_AND_DICTIONARY user_base_file_name)

   CMAKE_PARSE_ARGUMENTS(ARG "" "" "HEADERS" ${ARGN})

   set(header_linkdef "${CMAKE_CURRENT_BINARY_DIR}/${user_base_file_name}_LinkDef.h")
   root_generate_linkdef(${header_linkdef} HEADERS ${ARG_HEADERS})
   root_generate_dictionary(${user_base_file_name}_dict ${ARG_HEADERS} LINKDEF ${header_linkdef} OPTIONS "-p")

endfunction()
