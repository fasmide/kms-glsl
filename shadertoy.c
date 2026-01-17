/*
 * Copyright © 2020 Antonin Stefanutti <antonin.stefanutti@gmail.com>
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sub license,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice (including the
 * next paragraph) shall be included in all copies or substantial portions
 * of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NON-INFRINGEMENT. IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
 * DEALINGS IN THE SOFTWARE.
 */

#define _GNU_SOURCE

#include <err.h>
#include <stdio.h>
#include <unistd.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <regex.h>
#include <stdlib.h>

#include <GLES3/gl3.h>

#include "common.h"

#ifdef HAVE_NVML
#include <nvml.h>
static nvmlDevice_t nvml_device;
static bool nvml_available = false;
#endif

GLint iTime, iFrame;
static bool show_hud = false;
static uint32_t screen_width = 0;
static uint32_t screen_height = 0;
static const char *shader_filename = NULL;

// Simple shader for FPS overlay
static GLuint fps_program = 0;
static GLuint fps_vbo = 0;
static GLuint shadertoy_program = 0;
static GLuint shadertoy_vbo = 0;

static const char *shadertoy_vs_tmpl_100 =
		"// version (default: 1.10)              \n"
		"%s                                      \n"
		"                                        \n"
		"attribute vec3 position;                \n"
		"                                        \n"
		"void main()                             \n"
		"{                                       \n"
		"    gl_Position = vec4(position, 1.0);  \n"
		"}                                       \n";

static const char *shadertoy_vs_tmpl_300 =
		"// version                              \n"
		"%s                                      \n"
		"                                        \n"
		"in vec3 position;                       \n"
		"                                        \n"
		"void main()                             \n"
		"{                                       \n"
		"    gl_Position = vec4(position, 1.0);  \n"
		"}                                       \n";

static const char *shadertoy_fs_tmpl_100 =
		"// version (default: 1.10)                                                           \n"
		"%s                                                                                   \n"
		"                                                                                     \n"
		"#ifdef GL_FRAGMENT_PRECISION_HIGH                                                    \n"
		"precision highp float;                                                               \n"
		"#else                                                                                \n"
		"precision mediump float;                                                             \n"
		"#endif                                                                               \n"
		"                                                                                     \n"
		"uniform vec3      iResolution;           // viewport resolution (in pixels)          \n"
		"uniform float     iTime;                 // shader playback time (in seconds)        \n"
		"uniform int       iFrame;                // current frame number                     \n"
		"uniform vec4      iMouse;                // mouse pixel coords                       \n"
		"uniform vec4      iDate;                 // (year, month, day, time in seconds)      \n"
		"                                                                                     \n"
		"// Shader body                                                                       \n"
		"%s                                                                                   \n"
		"                                                                                     \n"
		"void main()                                                                          \n"
		"{                                                                                    \n"
		"    mainImage(gl_FragColor, gl_FragCoord.xy);                                        \n"
		"}                                                                                    \n";

static const char *shadertoy_fs_tmpl_300 =
		"// version                                                                           \n"
		"%s                                                                                   \n"
		"                                                                                     \n"
		"#ifdef GL_FRAGMENT_PRECISION_HIGH                                                    \n"
		"precision highp float;                                                               \n"
		"#else                                                                                \n"
		"precision mediump float;                                                             \n"
		"#endif                                                                               \n"
		"                                                                                     \n"
		"out vec4 fragColor;                                                                  \n"
		"                                                                                     \n"
		"uniform vec3      iResolution;           // viewport resolution (in pixels)          \n"
		"uniform float     iTime;                 // shader playback time (in seconds)        \n"
		"uniform int       iFrame;                // current frame number                     \n"
		"uniform vec4      iMouse;                // mouse pixel coords                       \n"
		"uniform vec4      iDate;                 // (year, month, day, time in seconds)      \n"
		"                                                                                     \n"
		"// Shader body                                                                       \n"
		"%s                                                                                   \n"
		"                                                                                     \n"
		"void main()                                                                          \n"
		"{                                                                                    \n"
		"    mainImage(fragColor, gl_FragCoord.xy);                                           \n"
		"}                                                                                    \n";

static const GLfloat vertices[] = {
		// First triangle:
		1.0f, 1.0f,
		-1.0f, 1.0f,
		-1.0f, -1.0f,
		// Second triangle:
		-1.0f, -1.0f,
		1.0f, -1.0f,
		1.0f, 1.0f,
};

static const char *load_shader(const char *file) {
	struct stat statbuf;
	int fd, ret;

	fd = open(file, 0);
	if (fd < 0) {
		err(fd, "could not open '%s'", file);
	}

	ret = fstat(fd, &statbuf);
	if (ret < 0) {
		err(ret, "could not stat '%s'", file);
	}

	return mmap(NULL, statbuf.st_size, PROT_READ, MAP_PRIVATE, fd, 0);
}

#define GLSL_VERSION_REGEX "GLSL[[:space:]]*(ES)?[[:space:]]*([[:digit:]]+)\\.([[:digit:]]+)"

static char *extract_group(const char *str, regmatch_t group) {
	char *c = calloc(group.rm_eo - group.rm_so, sizeof(char));
	memcpy(c, &str[group.rm_so], group.rm_eo - group.rm_so);
	return c;
}

static char *glsl_version() {
	int ret;
	regex_t regex;
	if ((ret = regcomp(&regex, GLSL_VERSION_REGEX, REG_EXTENDED)) != 0) {
		err(ret, "failed to compile GLSL version regex");
	}

	char *version = "";
	const char *glsl_version = (char *) glGetString(GL_SHADING_LANGUAGE_VERSION);
	if (strlen(glsl_version) == 0) {
		printf("Cannot detect GLSL version from %s\n", "GL_SHADING_LANGUAGE_VERSION");
		return version;
	}

	size_t nGroups = 4;
	regmatch_t groups[nGroups];
	ret = regexec(&regex, glsl_version, nGroups, groups, 0);
	if (ret == REG_NOMATCH) {
		printf("Cannot match GLSL version '%s'\n", glsl_version);
	} else if (ret != 0) {
		err(ret, "failed to match GLSL version '%s'", glsl_version);
	} else {
		char *es = extract_group(glsl_version, groups[1]);
		char *major = extract_group(glsl_version, groups[2]);
		char *minor = extract_group(glsl_version, groups[3]);

		if (strcmp(minor, "0") == 0) {
			free(minor);
			minor = malloc(sizeof(char) * 3);
			strcpy(minor, "00");
		}

		bool is100 = strcmp(major, "1") == 0 && strcmp(minor, "00") == 0;
		bool hasES = strcasecmp(es, "ES") == 0 && !is100;

		asprintf(&version, "%s%s%s", major, minor, hasES ? " es" : "");

		free(es);
		free(major);
		free(minor);
	}
	regfree(&regex);

	return version;
}

typedef void (*onInitCallback)(uint program, uint width, uint height);
typedef void (*onRenderCallback)(uint64_t frame, float time);

typedef struct {
	void (**callbacks)();
	size_t length;
} Callbacks;

void addCallback(Callbacks *callbacks, void callback()) {
	if (!callbacks->callbacks) {
		callbacks->length = 1;
		callbacks->callbacks = malloc(sizeof(callbacks->callbacks));
		callbacks->callbacks[0] = callback;
	} else {
		callbacks->length++;
		callbacks->callbacks = realloc(callbacks->callbacks, callbacks->length * sizeof(callbacks->callbacks));
		callbacks->callbacks[callbacks->length - 1] = callback;
	}
}

Callbacks onInitCallbacks;
void onInit(onInitCallback callback) {
	addCallback(&onInitCallbacks, (void (*)) callback);
}

Callbacks onRenderCallbacks;
void onRender(onRenderCallback callback) {
	addCallback(&onRenderCallbacks, (void (*)) callback);
}

// Simple 5x7 bitmap font - extended for filenames
// Indices: 0-9=digits, 10='.', 11='F', 12='P', 13='S', 14='W', 15='_', 16='-', 17-42=a-z
static const unsigned char font_5x7[][7] = {
	// '0'
	{0x0E, 0x11, 0x13, 0x15, 0x19, 0x11, 0x0E},
	// '1'
	{0x04, 0x0C, 0x04, 0x04, 0x04, 0x04, 0x0E},
	// '2'
	{0x0E, 0x11, 0x01, 0x02, 0x04, 0x08, 0x1F},
	// '3'
	{0x1F, 0x02, 0x04, 0x02, 0x01, 0x11, 0x0E},
	// '4'
	{0x02, 0x06, 0x0A, 0x12, 0x1F, 0x02, 0x02},
	// '5'
	{0x1F, 0x10, 0x1E, 0x01, 0x01, 0x11, 0x0E},
	// '6'
	{0x06, 0x08, 0x10, 0x1E, 0x11, 0x11, 0x0E},
	// '7'
	{0x1F, 0x01, 0x02, 0x04, 0x08, 0x08, 0x08},
	// '8'
	{0x0E, 0x11, 0x11, 0x0E, 0x11, 0x11, 0x0E},
	// '9'
	{0x0E, 0x11, 0x11, 0x0F, 0x01, 0x02, 0x0C},
	// '.' (index 10)
	{0x00, 0x00, 0x00, 0x00, 0x00, 0x0C, 0x0C},
	// 'F' (index 11)
	{0x1F, 0x10, 0x10, 0x1E, 0x10, 0x10, 0x10},
	// 'P' (index 12)
	{0x1E, 0x11, 0x11, 0x1E, 0x10, 0x10, 0x10},
	// 'S' (index 13)
	{0x0E, 0x11, 0x10, 0x0E, 0x01, 0x11, 0x0E},
	// 'W' (index 14)
	{0x11, 0x11, 0x11, 0x15, 0x15, 0x15, 0x0A},
	// '_' (index 15)
	{0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x1F},
	// '-' (index 16)
	{0x00, 0x00, 0x00, 0x0E, 0x00, 0x00, 0x00},
	// 'a' (index 17)
	{0x00, 0x00, 0x0E, 0x01, 0x0F, 0x11, 0x0F},
	// 'b' (index 18)
	{0x10, 0x10, 0x1E, 0x11, 0x11, 0x11, 0x1E},
	// 'c' (index 19)
	{0x00, 0x00, 0x0E, 0x10, 0x10, 0x10, 0x0E},
	// 'd' (index 20)
	{0x01, 0x01, 0x0F, 0x11, 0x11, 0x11, 0x0F},
	// 'e' (index 21)
	{0x00, 0x00, 0x0E, 0x11, 0x1F, 0x10, 0x0E},
	// 'f' (index 22)
	{0x06, 0x09, 0x08, 0x1C, 0x08, 0x08, 0x08},
	// 'g' (index 23)
	{0x00, 0x0F, 0x11, 0x11, 0x0F, 0x01, 0x0E},
	// 'h' (index 24)
	{0x10, 0x10, 0x1E, 0x11, 0x11, 0x11, 0x11},
	// 'i' (index 25)
	{0x04, 0x00, 0x0C, 0x04, 0x04, 0x04, 0x0E},
	// 'j' (index 26)
	{0x02, 0x00, 0x06, 0x02, 0x02, 0x12, 0x0C},
	// 'k' (index 27)
	{0x10, 0x10, 0x12, 0x14, 0x18, 0x14, 0x12},
	// 'l' (index 28)
	{0x0C, 0x04, 0x04, 0x04, 0x04, 0x04, 0x0E},
	// 'm' (index 29)
	{0x00, 0x00, 0x1A, 0x15, 0x15, 0x15, 0x15},
	// 'n' (index 30)
	{0x00, 0x00, 0x1E, 0x11, 0x11, 0x11, 0x11},
	// 'o' (index 31)
	{0x00, 0x00, 0x0E, 0x11, 0x11, 0x11, 0x0E},
	// 'p' (index 32)
	{0x00, 0x00, 0x1E, 0x11, 0x1E, 0x10, 0x10},
	// 'q' (index 33)
	{0x00, 0x00, 0x0F, 0x11, 0x0F, 0x01, 0x01},
	// 'r' (index 34)
	{0x00, 0x00, 0x16, 0x19, 0x10, 0x10, 0x10},
	// 's' (index 35)
	{0x00, 0x00, 0x0E, 0x10, 0x0E, 0x01, 0x1E},
	// 't' (index 36)
	{0x08, 0x08, 0x1C, 0x08, 0x08, 0x09, 0x06},
	// 'u' (index 37)
	{0x00, 0x00, 0x11, 0x11, 0x11, 0x11, 0x0F},
	// 'v' (index 38)
	{0x00, 0x00, 0x11, 0x11, 0x11, 0x0A, 0x04},
	// 'w' (index 39)
	{0x00, 0x00, 0x11, 0x11, 0x15, 0x15, 0x0A},
	// 'x' (index 40)
	{0x00, 0x00, 0x11, 0x0A, 0x04, 0x0A, 0x11},
	// 'y' (index 41)
	{0x00, 0x00, 0x11, 0x11, 0x0F, 0x01, 0x0E},
	// 'z' (index 42)
	{0x00, 0x00, 0x1F, 0x02, 0x04, 0x08, 0x1F},
};

// Map character to font index
static int char_to_font_index(char c) {
	if (c >= '0' && c <= '9') return c - '0';
	if (c == '.') return 10;
	if (c == 'F') return 11;
	if (c == 'P') return 12;
	if (c == 'S') return 13;
	if (c == 'W') return 14;
	if (c == '_') return 15;
	if (c == '-') return 16;
	if (c >= 'a' && c <= 'z') return 17 + (c - 'a');
	if (c >= 'A' && c <= 'Z') return 17 + (c - 'A'); // Map uppercase to lowercase
	return -1; // Unknown character
}

static void draw_char(int x, int y, int char_index, float scale, GLfloat *vertices, int *vertex_count) {
	if (char_index < 0 || char_index >= 43) return;
	
	const unsigned char *glyph = font_5x7[char_index];
	int pixel_size = (int)scale;
	if (pixel_size < 1) pixel_size = 1;
	
	for (int row = 0; row < 7; row++) {
		for (int col = 0; col < 5; col++) {
			if (glyph[row] & (1 << (4 - col))) {
				// Draw a pixel as a small quad
				float px = x + col * pixel_size;
				float py = y + row * pixel_size;
				
				// Convert screen coordinates to OpenGL coordinates (-1 to 1)
				float x1 = (px / (float)screen_width) * 2.0f - 1.0f;
				float y1 = 1.0f - (py / (float)screen_height) * 2.0f;
				float x2 = ((px + pixel_size) / (float)screen_width) * 2.0f - 1.0f;
				float y2 = 1.0f - ((py + pixel_size) / (float)screen_height) * 2.0f;
				
				// Add two triangles for this pixel
				int idx = *vertex_count;
				vertices[idx++] = x1; vertices[idx++] = y1;
				vertices[idx++] = x2; vertices[idx++] = y1;
				vertices[idx++] = x2; vertices[idx++] = y2;
				vertices[idx++] = x2; vertices[idx++] = y2;
				vertices[idx++] = x1; vertices[idx++] = y2;
				vertices[idx++] = x1; vertices[idx++] = y1;
				*vertex_count = idx;
			}
		}
	}
}

static void draw_fps_counter(float fps) {
	if (!show_hud || fps <= 0.0f || fps_program == 0) return;
	
	// Build vertex buffer for all text
	// Each char can be 5x7=35 pixels, 6 floats per pixel (2 triangles), so ~210 floats per char
	// Need space for filename + FPS + power, roughly 60 chars max = ~12600 floats
	GLfloat vertices[16384];
	int vertex_count = 0;
	
	float scale = 2.0f;
	int char_width = (int)(6 * scale);
	int char_height = (int)(8 * scale);
	int padding = 10;
	
	// Draw filename in top left
	if (shader_filename) {
		int x_offset = 0;
		for (int i = 0; shader_filename[i] != '\0'; i++) {
			int char_index = char_to_font_index(shader_filename[i]);
			if (char_index >= 0) {
				draw_char(padding + x_offset, padding, char_index, scale, vertices, &vertex_count);
			}
			x_offset += char_width;
		}
	}
	
	// Format FPS (and optionally power) for bottom right
	char fps_text[64];
#ifdef HAVE_NVML
	if (nvml_available) {
		unsigned int power_mw = 0;
		if (nvmlDeviceGetPowerUsage(nvml_device, &power_mw) == NVML_SUCCESS) {
			snprintf(fps_text, sizeof(fps_text), "%.1f FPS  %.2f W", fps, power_mw / 1000.0);
		} else {
			snprintf(fps_text, sizeof(fps_text), "%.1f FPS", fps);
		}
	} else
#endif
	{
		snprintf(fps_text, sizeof(fps_text), "%.1f FPS", fps);
	}
	
	int text_width = strlen(fps_text) * char_width;
	int start_x = screen_width - text_width - padding;
	int start_y = screen_height - char_height - padding;
	
	// Draw FPS/power in bottom right
	int x_offset = 0;
	for (int i = 0; fps_text[i] != '\0'; i++) {
		int char_index = char_to_font_index(fps_text[i]);
		if (char_index < 0) {
			// Space or unknown char
			x_offset += char_width;
			continue;
		}
		draw_char(start_x + x_offset, start_y, char_index, scale, vertices, &vertex_count);
		x_offset += char_width;
	}
	
	if (vertex_count == 0) return;
	
	// Save current GL state
	GLint current_program;
	GLint current_vbo;
	glGetIntegerv(GL_CURRENT_PROGRAM, &current_program);
	glGetIntegerv(GL_ARRAY_BUFFER_BINDING, &current_vbo);
	
	// Switch to FPS shader program
	glUseProgram(fps_program);
	
	// Update VBO with text vertices
	glBindBuffer(GL_ARRAY_BUFFER, fps_vbo);
	glBufferData(GL_ARRAY_BUFFER, vertex_count * sizeof(GLfloat), vertices, GL_DYNAMIC_DRAW);
	
	// Set up vertex attributes
	glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, (void*)0);
	glEnableVertexAttribArray(0);
	
	// Enable blending for overlay
	glEnable(GL_BLEND);
	glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
	
	// Draw the text
	glDrawArrays(GL_TRIANGLES, 0, vertex_count / 2);
	
	// Restore GL state
	glDisable(GL_BLEND);
	glUseProgram(current_program);
	glBindBuffer(GL_ARRAY_BUFFER, current_vbo);
	// Restore vertex attribute pointer to shadertoy configuration
	glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, (const GLvoid *) (intptr_t) 0);
	glEnableVertexAttribArray(0);
}

static void draw_shadertoy(uint64_t start_time, unsigned frame, float fps) {
	float time = ((float) (get_time_ns() - start_time)) / NSEC_PER_SEC;

	glUniform1f(iTime, time);
	// Replace the above to input elapsed time relative to 60 FPS
	// glUniform1f(iTime, (GLfloat) frame / 60.0f);
	glUniform1ui(iFrame, frame);

	for (uint i = 0; i < onRenderCallbacks.length; i++) {
		((onRenderCallback) onRenderCallbacks.callbacks[i])(frame, time);
	}

	start_perfcntrs();

	glDrawArrays(GL_TRIANGLES, 0, 6);

	end_perfcntrs();
	
	// Draw FPS counter overlay after main shader
	draw_fps_counter(fps);
}

int init_shadertoy(const struct gbm *gbm, struct egl *egl, const char *file, const struct options *options) {
	int ret;
	char *shadertoy_vs, *shadertoy_fs;
	GLuint program;
	GLint iResolution;
	
	// Store settings
	show_hud = options->show_hud;
	screen_width = gbm->width;
	screen_height = gbm->height;
	
	// Extract basename from file path for display
	if (show_hud && file) {
		const char *basename = strrchr(file, '/');
		shader_filename = basename ? basename + 1 : file;
	}

	const char *shader = load_shader(file);

	const char *version = glsl_version();
	if (strlen(version) > 0) {
		char *invalid;
		long v = strtol(version, &invalid, 10);
		if (invalid == version) {
			printf("failed to parse detected GLSL version: %s\n", invalid);
			return -1;
		}
		char *version_directive;
		asprintf(&version_directive, "#version %s", version);
		printf("Using GLSL version directive: %s\n", version_directive);

		bool is_glsl_3 = v >= 300;
		asprintf(&shadertoy_vs, is_glsl_3 ? shadertoy_vs_tmpl_300 : shadertoy_vs_tmpl_100, version_directive);
		asprintf(&shadertoy_fs, is_glsl_3 ? shadertoy_fs_tmpl_300 : shadertoy_fs_tmpl_100, version_directive, shader);
	} else {
		asprintf(&shadertoy_vs, shadertoy_vs_tmpl_100, version);
		asprintf(&shadertoy_fs, shadertoy_fs_tmpl_100, version, shader);
	}

	ret = create_program(shadertoy_vs, shadertoy_fs);
	if (ret < 0) {
		printf("failed to create program\n");
		return -1;
	}

	program = ret;

	ret = link_program(program);
	if (ret) {
		printf("failed to link program\n");
		return -1;
	}

	glViewport(0, 0, gbm->width, gbm->height);
	glUseProgram(program);

	iTime = glGetUniformLocation(program, "iTime");
	iFrame = glGetUniformLocation(program, "iFrame");
	iResolution = glGetUniformLocation(program, "iResolution");
	glUniform3f(iResolution, gbm->width, gbm->height, 0);

	for (uint i = 0; i < onInitCallbacks.length; i++) {
		((onInitCallback) onInitCallbacks.callbacks[i])(program, gbm->width, gbm->height);
	}

	glGenBuffers(1, &shadertoy_vbo);
	glBindBuffer(GL_ARRAY_BUFFER, shadertoy_vbo);
	glBufferData(GL_ARRAY_BUFFER, sizeof(vertices), 0, GL_STATIC_DRAW);
	glBufferSubData(GL_ARRAY_BUFFER, 0, sizeof(vertices), &vertices[0]);
	glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, (const GLvoid *) (intptr_t) 0);
	glEnableVertexAttribArray(0);

	// Store the shadertoy program for later restoration
	shadertoy_program = program;

	// Initialize HUD overlay shader if needed
	if (show_hud) {
		// Determine GLSL version for FPS shader
		char *fps_vs_src, *fps_fs_src;
		if (strlen(version) > 0) {
			char *invalid;
			long v = strtol(version, &invalid, 10);
			bool is_glsl_3 = (invalid != version && v >= 300);
			
			if (is_glsl_3) {
				asprintf(&fps_vs_src, "#version %s\nin vec2 position;\nvoid main() { gl_Position = vec4(position, 0.0, 1.0); }\n", version);
				asprintf(&fps_fs_src, "#version %s\nprecision mediump float;\nout vec4 fragColor;\nvoid main() { fragColor = vec4(1.0, 1.0, 1.0, 1.0); }\n", version);
			} else {
				asprintf(&fps_vs_src, "#version %s\nattribute vec2 position;\nvoid main() { gl_Position = vec4(position, 0.0, 1.0); }\n", version);
				asprintf(&fps_fs_src, "#version %s\nprecision mediump float;\nvoid main() { gl_FragColor = vec4(1.0, 1.0, 1.0, 1.0); }\n", version);
			}
		} else {
			asprintf(&fps_vs_src, "attribute vec2 position;\nvoid main() { gl_Position = vec4(position, 0.0, 1.0); }\n");
			asprintf(&fps_fs_src, "precision mediump float;\nvoid main() { gl_FragColor = vec4(1.0, 1.0, 1.0, 1.0); }\n");
		}
		
		ret = create_program(fps_vs_src, fps_fs_src);
		if (ret < 0) {
			printf("Warning: failed to create HUD shader, HUD display will be disabled\n");
			show_hud = false;
		} else {
			fps_program = ret;
			ret = link_program(fps_program);
			if (ret) {
				printf("Warning: failed to link HUD shader, HUD display will be disabled\n");
				show_hud = false;
				fps_program = 0;
			} else {
				// Create VBO for HUD text
				glGenBuffers(1, &fps_vbo);
			}
		}
		
#ifdef HAVE_NVML
		// Initialize NVML for GPU power monitoring
		if (nvmlInit() == NVML_SUCCESS) {
			if (nvmlDeviceGetHandleByIndex(0, &nvml_device) == NVML_SUCCESS) {
				nvml_available = true;
				printf("NVML initialized for GPU power monitoring\n");
			} else {
				printf("Warning: NVML available but could not get GPU handle\n");
				nvmlShutdown();
			}
		} else {
			printf("Warning: NVML initialization failed, power monitoring disabled\n");
		}
#endif
	}

	egl->draw = draw_shadertoy;

	return 0;
}
