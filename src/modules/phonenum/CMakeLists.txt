file(GLOB MODULE_SOURCES "*.c")

add_library(${module_name} SHARED ${MODULE_SOURCES})

# TODO: Support for find_package added after version 8.13.4
# TODO: Debian and Ubuntu are missing static installation of libphonenumber
# and find_package() does not work for them.
# find_package(libphonenumber CONFIG QUIET)
if(libphonenumber_FOUND)
  message(STATUS "Found libphonenumber: ${libphonenumber_INCLUDE_DIRS}")
  message(STATUS "Found libphonenumber: ${libphonenumber_LIBRARY}")
else()
  message(STATUS "Looking for libphonenumber...")
  # add additional logic here to handle the missing dependency TODO: Fix lint
  # issue
  find_library(libphonenumber_LIBRARY NAMES phonenumber)
  find_library(libgeocoding_LIBRARY NAMES geocoding)

  if(libphonenumber_LIBRARY AND libgeocoding_LIBRARY)
    set(libphonenum_LIBRARY ${libphonenumber_LIBRARY} ${libgeocoding_LIBRARY})
    find_path(libphonenumber_INCLUDE_DIR phonenumbers/geocoding/phonenumber_offline_geocoder.h
              phonenumbers/phonenumberutil.h
    )
    message(STATUS "Found libphonenumber include directory: ${libphonenumber_INCLUDE_DIR}")
    message(STATUS "Found libphonenumber library: ${libphonenum_LIBRARY}")
  else()
    message(FATAL_ERROR "libphonenumber library not found")
  endif()

endif()

target_compile_features(${module_name} PRIVATE cxx_std_17)

# Apply compiler flags  only when the build configuration is not DEBUG (Release
# or RelWithDebInfo ...)
target_compile_options(
  ${module_name}
  PRIVATE $<$<NOT:$<CONFIG:DEBUG>>:-Wno-write-strings>
          $<$<NOT:$<CONFIG:DEBUG>>:-Wno-deprecated>
          $<$<NOT:$<CONFIG:DEBUG>>:-Wno-unused-function>
          $<$<NOT:$<CONFIG:DEBUG>>:-Wno-sign-compare>
          $<$<NOT:$<CONFIG:DEBUG>>:-Wno-strict-aliasing>
)

target_include_directories(${module_name} PRIVATE ${libphonenumber_INCLUDE_DIRS})
target_link_libraries(${module_name} PRIVATE ${libphonenum_LIBRARY})
