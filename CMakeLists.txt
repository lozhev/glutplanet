cmake_minimum_required(VERSION 3.4)

project (glutplanet)

include_directories(
    .
)

if(APPLE)
    find_package(OpenGL REQUIRED)
    find_package(GLUT REQUIRED)
    #set(GL_LIBS ${OPENGL_LIBRARIES} ${GLUT_LIBRARIES})
    set(GL_LIBS ${GLUT_LIBRARIES})
    include_directories(${OPENGL_INCLUDE_DIR} ${OPENGL_INCLUDE_DIR})
else(APPLE)
    set(GL_LIBS "gl" "glut")
endif(APPLE)
message(${GL_LIBS})

link_libraries (
	${GL_LIBS}
	m
	curl
	pthread
	dl
)

add_executable(glutplanet main.c glad.c)
