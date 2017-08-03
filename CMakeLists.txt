cmake_minimum_required(VERSION 3.4)

project (glutplanet)

include_directories(
    .
)

if(APPLE)
    find_package(GLUT REQUIRED)
    set(GL_LIBS ${GLUT_LIBRARIES})
    include_directories(${OPENGL_INCLUDE_DIR})
else(APPLE)
    set(GL_LIBS "glut")
endif(APPLE)

link_libraries (
    ${GL_LIBS}
    m
    curl
    pthread
    dl
)

add_executable(glutplanet main.c glad.c)
