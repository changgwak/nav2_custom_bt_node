cmake_minimum_required(VERSION 3.10)
project(robot_interfaces)

# Default to C99
if(NOT CMAKE_C_STANDARD)
  set(CMAKE_C_STANDARD 99)
endif()

# Default to C++14
if(NOT CMAKE_CXX_STANDARD)
  set(CMAKE_CXX_STANDARD 17)
endif()

if(CMAKE_COMPILER_IS_GNUCXX OR CMAKE_CXX_COMPILER_ID MATCHES "Clang")
  add_compile_options(-Wall -Wextra -Wpedantic)
endif()

# find dependencies
find_package(ament_cmake REQUIRED)
find_package(std_msgs REQUIRED)
find_package(std_srvs REQUIRED)
find_package(geometry_msgs REQUIRED)
find_package(nav_msgs REQUIRED)
find_package(builtin_interfaces REQUIRED)
find_package(rosidl_default_generators REQUIRED)



rosidl_generate_interfaces(${PROJECT_NAME}
  "msg/SmartControl.msg"
  "msg/NavigationCommand.msg"
  "msg/ModifierControl.msg"
  "srv/SmartRecoveryPath.srv"
  "srv/SetNavigationMode.srv"
  
  "action/SmartRecovery.action"
  DEPENDENCIES geometry_msgs std_msgs std_srvs nav_msgs builtin_interfaces
)

# 이 라인은 robot_interfaces의 생성된 인터페이스가 다른 패키지에 노출되도록 함.
# ament_export_dependencies(rosidl_default_runtime)

# # rosidl_generate_interfaces에 의해 생성된 헤더 파일들을 install 공간으로 복사
# install(
#   DIRECTORY include/
#   DESTINATION include
# )

if(BUILD_TESTING)
  find_package(ament_lint_auto REQUIRED)
  # the following line skips the linter which checks for copyrights
  # uncomment the line when a copyright and license is not present in all source files
  #set(ament_cmake_copyright_FOUND TRUE)
  # the following line skips cpplint (only works in a git repo)
  # uncomment the line when this package is not in a git repo
  #set(ament_cmake_cpplint_FOUND TRUE)
  ament_lint_auto_find_test_dependencies()
endif()

ament_package()
