cmake_minimum_required(VERSION 3.16)

project(thediscordlist-dpp-example LANGUAGES CXX)

find_package(thediscordlist-dpp 0.1 CONFIG REQUIRED)

add_executable(bot bot.cpp)
target_link_libraries(bot PRIVATE thediscordlist::dpp)
