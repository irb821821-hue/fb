cmake_minimum_required(VERSION 3.25)
project(fb)

find_package(geode REQUIRED)

add_geode_mod(mymod main.cpp)
