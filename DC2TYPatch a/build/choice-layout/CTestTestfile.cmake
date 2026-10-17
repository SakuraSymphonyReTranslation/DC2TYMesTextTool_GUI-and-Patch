# CMake generated Testfile for 
# Source directory: E:/Games/DC2TY/DC2TYMesTextTool_GUI-and-Patch/DC2TYPatch a
# Build directory: E:/Games/DC2TY/DC2TYMesTextTool_GUI-and-Patch/DC2TYPatch a/build/choice-layout
# 
# This file includes the relevant testing commands required for 
# testing this directory and lists subdirectories to be tested as well.
if(CTEST_CONFIGURATION_TYPE MATCHES "^([Dd][Ee][Bb][Uu][Gg])$")
  add_test(choice_layout "E:/Games/DC2TY/DC2TYMesTextTool_GUI-and-Patch/DC2TYPatch a/build/choice-layout/Debug/choice_layout_test.exe")
  set_tests_properties(choice_layout PROPERTIES  _BACKTRACE_TRIPLES "E:/Games/DC2TY/DC2TYMesTextTool_GUI-and-Patch/DC2TYPatch a/CMakeLists.txt;68;add_test;E:/Games/DC2TY/DC2TYMesTextTool_GUI-and-Patch/DC2TYPatch a/CMakeLists.txt;0;")
elseif(CTEST_CONFIGURATION_TYPE MATCHES "^([Rr][Ee][Ll][Ee][Aa][Ss][Ee])$")
  add_test(choice_layout "E:/Games/DC2TY/DC2TYMesTextTool_GUI-and-Patch/DC2TYPatch a/build/choice-layout/Release/choice_layout_test.exe")
  set_tests_properties(choice_layout PROPERTIES  _BACKTRACE_TRIPLES "E:/Games/DC2TY/DC2TYMesTextTool_GUI-and-Patch/DC2TYPatch a/CMakeLists.txt;68;add_test;E:/Games/DC2TY/DC2TYMesTextTool_GUI-and-Patch/DC2TYPatch a/CMakeLists.txt;0;")
elseif(CTEST_CONFIGURATION_TYPE MATCHES "^([Mm][Ii][Nn][Ss][Ii][Zz][Ee][Rr][Ee][Ll])$")
  add_test(choice_layout "E:/Games/DC2TY/DC2TYMesTextTool_GUI-and-Patch/DC2TYPatch a/build/choice-layout/MinSizeRel/choice_layout_test.exe")
  set_tests_properties(choice_layout PROPERTIES  _BACKTRACE_TRIPLES "E:/Games/DC2TY/DC2TYMesTextTool_GUI-and-Patch/DC2TYPatch a/CMakeLists.txt;68;add_test;E:/Games/DC2TY/DC2TYMesTextTool_GUI-and-Patch/DC2TYPatch a/CMakeLists.txt;0;")
elseif(CTEST_CONFIGURATION_TYPE MATCHES "^([Rr][Ee][Ll][Ww][Ii][Tt][Hh][Dd][Ee][Bb][Ii][Nn][Ff][Oo])$")
  add_test(choice_layout "E:/Games/DC2TY/DC2TYMesTextTool_GUI-and-Patch/DC2TYPatch a/build/choice-layout/RelWithDebInfo/choice_layout_test.exe")
  set_tests_properties(choice_layout PROPERTIES  _BACKTRACE_TRIPLES "E:/Games/DC2TY/DC2TYMesTextTool_GUI-and-Patch/DC2TYPatch a/CMakeLists.txt;68;add_test;E:/Games/DC2TY/DC2TYMesTextTool_GUI-and-Patch/DC2TYPatch a/CMakeLists.txt;0;")
else()
  add_test(choice_layout NOT_AVAILABLE)
endif()
