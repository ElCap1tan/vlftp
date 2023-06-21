cmake_minimum_required(VERSION 3.25)
project(vlftp C)

set(CMAKE_C_STANDARD 11)

add_executable(vlftp vlftp.c)
add_executable(vlftpd vlftpd.c)
