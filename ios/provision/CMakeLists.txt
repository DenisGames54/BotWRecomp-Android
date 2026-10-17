# Tiny Xcode project whose only job is to make Xcode create / renew the development provisioning
# profile for the app's bundle ID + entitlements (free Personal Team profiles last 7 days).
# Driven by ios/scripts/provision.sh; the real app is built with Ninja (ios/CMakeLists.txt).
cmake_minimum_required(VERSION 3.24)
project(BotWProvision LANGUAGES C OBJC)
set(BOTW_TEAM "" CACHE STRING "Apple team ID")
set(BOTW_BUNDLE_ID "com.example.BotWRecomp" CACHE STRING "bundle ID")
set(BOTW_ENTITLEMENTS "" CACHE FILEPATH "entitlements to provision")
add_executable(BotWProvision MACOSX_BUNDLE main.m)
set_target_properties(BotWProvision PROPERTIES
  MACOSX_BUNDLE_GUI_IDENTIFIER ${BOTW_BUNDLE_ID}
  XCODE_ATTRIBUTE_PRODUCT_BUNDLE_IDENTIFIER ${BOTW_BUNDLE_ID}
  XCODE_ATTRIBUTE_DEVELOPMENT_TEAM ${BOTW_TEAM}
  XCODE_ATTRIBUTE_CODE_SIGN_STYLE Automatic
  XCODE_ATTRIBUTE_CODE_SIGN_ENTITLEMENTS ${BOTW_ENTITLEMENTS}
  XCODE_ATTRIBUTE_IPHONEOS_DEPLOYMENT_TARGET 18.0)
target_link_libraries(BotWProvision "-framework UIKit")
