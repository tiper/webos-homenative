/* SPDX-License-Identifier: GPL-3.0-or-later */
/* The GLES2 the launcher uses, faked: see fake.c. */
#pragma once
#include <stddef.h>
typedef unsigned GLenum, GLuint, GLbitfield; typedef int GLint, GLsizei; typedef float GLfloat, GLclampf;
typedef char GLchar; typedef unsigned char GLubyte, GLboolean; typedef unsigned short GLushort; typedef long GLsizeiptr;
enum { GL_FALSE, GL_TRUE };
enum { GL_NO_ERROR = 0, GL_TRIANGLES = 4, GL_TRIANGLE_STRIP = 5, GL_SRC_ALPHA = 0x302, GL_ONE_MINUS_SRC_ALPHA = 0x303, GL_ONE = 1,
       GL_FLOAT = 0x1406, GL_UNSIGNED_SHORT = 0x1403, GL_UNSIGNED_BYTE = 0x1401, GL_ARRAY_BUFFER = 0x8892, GL_ELEMENT_ARRAY_BUFFER = 0x8893,
       GL_STATIC_DRAW = 0x88E4, GL_STREAM_DRAW = 0x88E0, GL_VERTEX_SHADER = 0x8B31, GL_FRAGMENT_SHADER = 0x8B30,
       GL_COMPILE_STATUS = 0x8B81, GL_LINK_STATUS = 0x8B82, GL_TEXTURE_2D = 0xDE1, GL_TEXTURE_MIN_FILTER = 0x2801,
       GL_TEXTURE_MAG_FILTER = 0x2800, GL_TEXTURE_WRAP_S = 0x2802, GL_TEXTURE_WRAP_T = 0x2803, GL_LINEAR = 0x2601,
       GL_CLAMP_TO_EDGE = 0x812F, GL_UNPACK_ALIGNMENT = 0xCF5, GL_RGBA = 0x1908, GL_BLEND = 0xBE2, GL_DEPTH_TEST = 0xB71,
       GL_SCISSOR_TEST = 0xC11, GL_COLOR_BUFFER_BIT = 0x4000, GL_VERSION = 0x1F02, GL_RENDERER = 0x1F01, GL_OUT_OF_MEMORY = 0x505, GL_CONTEXT_LOST = 0x507 };
GLuint glCreateShader(GLenum); void glShaderSource(GLuint, GLsizei, const char *const *, const GLint *); void glCompileShader(GLuint);
void glGetShaderiv(GLuint, GLenum, GLint *); void glGetShaderInfoLog(GLuint, GLsizei, GLsizei *, char *); GLuint glCreateProgram(void);
void glAttachShader(GLuint, GLuint); void glBindAttribLocation(GLuint, GLuint, const char *); void glLinkProgram(GLuint);
void glGetProgramiv(GLuint, GLenum, GLint *); void glGetProgramInfoLog(GLuint, GLsizei, GLsizei *, char *); void glUseProgram(GLuint);
GLint glGetUniformLocation(GLuint, const char *); void glUniform1i(GLint, GLint); void glUniform2f(GLint, float, float);
void glUniform4f(GLint, float, float, float, float); void glGenBuffers(GLsizei, GLuint *); void glBindBuffer(GLenum, GLuint);
void glBufferData(GLenum, GLsizeiptr, const void *, GLenum); void glEnableVertexAttribArray(GLuint); void glDisableVertexAttribArray(GLuint);
void glVertexAttribPointer(GLuint, GLint, GLenum, GLboolean, GLsizei, const void *); void glBlendFuncSeparate(GLenum, GLenum, GLenum, GLenum);
void glDisable(GLenum); void glEnable(GLenum); void glDrawElements(GLenum, GLsizei, GLenum, const void *); void glDrawArrays(GLenum, GLint, GLsizei);
void glBindTexture(GLenum, GLuint); void glGenTextures(GLsizei, GLuint *); void glTexParameteri(GLenum, GLenum, GLint); void glPixelStorei(GLenum, GLint);
void glTexImage2D(GLenum, GLint, GLint, GLsizei, GLsizei, GLint, GLenum, GLenum, const void *); void glDeleteTextures(GLsizei, const GLuint *);
void glScissor(GLint, GLint, GLsizei, GLsizei); void glViewport(GLint, GLint, GLsizei, GLsizei); void glClearColor(float, float, float, float);
void glClear(GLbitfield); const GLubyte *glGetString(GLenum); void glFinish(void); GLenum glGetError(void);
