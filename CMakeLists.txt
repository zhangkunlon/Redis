# cmake 最低版本要求
cmake_minimum_required(VERSION 2.8)
add_definitions("-O0 -g") 
# 本项目的工程名
project(learn_redis)
# 源文件路径 可以存在多个
aux_source_directory(./src SERVER_LIST)
# 头文件路径 可以存在多个
include_directories(./include)
# 第一个变量：要生成的可执行文件名为server，后面的参数是需要的依赖 可以有多个 空格分割
add_executable(server server.cpp ${SERVER_LIST})
add_executable(client client.cpp ${SERVER_LIST})

# 添加编译选项
add_compile_options(-std=c++11 -Wall -O0 -g)

# 指定生成库
# add_library(lib_name STATIC/SHARED ${SERVER_LIST})