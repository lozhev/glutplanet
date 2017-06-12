cmake_minimum_required(VERSION 3.4)

project (glutplanet)

include_directories(
    ../stb
)

link_libraries (
	m
	GL
	glut
	curl
	pthread
	dl
)

add_executable(glutplanet main.c glad.c)
