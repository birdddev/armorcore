#include "io_obj.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>
#include "iron_array.h"
#include "iron_vec4.h"

static obj_part_t *part = NULL;
static array_f32_t pos_temp;
static array_f32_t uv_temp;
static array_f32_t nor_temp;
static uint32_t va[60];
static uint32_t ua[60];
static uint32_t na[60];
static int vi = 0;
static int ui = 0;
static int ni = 0;
static uint8_t buf[64];
static uint8_t str[256];

static int vind_off = 0;
static int tind_off = 0;
static int nind_off = 0;
static uint8_t *bytes = NULL;
static size_t bytes_length = 0;
static array_f32_t *pos_first;
static array_f32_t *uv_first;
static array_f32_t *nor_first;

static int read_int() {
	int bi = 0;
	while (true) { // Read into buffer
		char c = bytes[part->pos];
		if (c == '/' || c == '\n' || c == '\r' || c == ' ') break;
		part->pos++;
		buf[bi++] = c;
	}
	int res = 0; // Parse buffer into int
	int dec = 1;
	int off = buf[0] == '-' ? 1 : 0;
	int len = bi - 1;
	for (int i = 0; i < bi - off; ++i) {
		res += (buf[len - i] - 48) * dec;
		dec *= 10;
	}
	if (off > 0) res *= -1;
	return res;
}

static void read_face_fast() {
	while (true) {
		va[vi++] = read_int() - 1;
		part->pos++; // '/'
		ua[ui++] = read_int() - 1;
		part->pos++; // '/'
		na[ni++] = read_int() - 1;
		if (bytes[part->pos] == '\n' || bytes[part->pos] == '\r') break;
		part->pos++; // ' '
		// Some exporters put space at the end of "f" line
		if (vi >= 3 && (bytes[part->pos] == '\n' || bytes[part->pos] == '\r')) break;
	}
}

static void read_face() {
	while (true) {
		va[vi++] = read_int() - 1;
		if (uv_temp.length > 0 || nor_temp.length > 0) {
			part->pos++; // "/"

			if (uv_temp.length > 0) {
				ua[ui++] = read_int() - 1;
			}
			if (nor_temp.length > 0) {
				part->pos++; // "/"
				na[ni++] = read_int() - 1;
			}
		}
		// Some exporters put "//" even when normal and uv data are not present (f 1//)
		else if (uv_temp.length == 0 && nor_temp.length == 0 && bytes[part->pos] == '/') {
			part->pos += 2;
		}

		if (bytes[part->pos] == '\n' || bytes[part->pos] == '\r') {
			break;
		}
		part->pos++; // " "
		// Some exporters put space at the end of "f" line
		if (vi >= 3 && (bytes[part->pos] == '\n' || bytes[part->pos] == '\r')) {
			break;
		}
	}
}

static float read_float() {
	int bi = 0;
	while (true) { // Read into buffer
		char c = bytes[part->pos];
		if (c == ' ' || c == '\n' || c == '\r') break;
		if (c == 'E' || c == 'e') {
			part->pos++;
			int first = buf[0] == '-' ? -(buf[1] - 48) : buf[0] - 48;
			int exp = read_int();
			int dec = 1;
			int loop = exp > 0 ? exp : -exp;
			for (int i = 0; i < loop; ++i) dec *= 10;
			return exp > 0 ? (float)first * dec : (float)first / dec;
		}
		part->pos++;
		buf[bi++] = c;
	}
	float res = 0.0; // Parse buffer into float
	int dot = 1;
	int dec = 1;
	int off = buf[0] == '-' ? 1 : 0;
	int len = bi - 1;
	for (int i = 0; i < bi - off; ++i) {
		char c = buf[len - i];
		if (c == '.') {
			dot = dec;
			continue;
		}
		res += (c - 48) * dec;
		dec *= 10;
	}
	if (off > 0) res /= -dot;
	else res /= dot;
	return res;
}

static char *read_string() {
	size_t begin = part->pos;
	while (true) {
		char c = bytes[part->pos];
		if (c == '\n' || c == '\r' || c == ' ') break;
		part->pos++;
	}
	for (int i = 0; i < part->pos - begin; ++i) str[i] = bytes[begin + i];
	str[part->pos - begin] = 0;
	return str;
}

static void next_line() {
	while (true) {
		char c = bytes[part->pos++];
		if (c == '\n' || part->pos >= bytes_length) break; // \n, \r\n
	}
}

static int get_tile(int i1, int i2, int i3, array_i32_t *uv_indices, int tiles_u) {
	float u1 = uv_temp.data[uv_indices->data[i1] * 2    ];
	float v1 = uv_temp.data[uv_indices->data[i1] * 2 + 1];
	float u2 = uv_temp.data[uv_indices->data[i2] * 2    ];
	float v2 = uv_temp.data[uv_indices->data[i2] * 2 + 1];
	float u3 = uv_temp.data[uv_indices->data[i3] * 2    ];
	float v3 = uv_temp.data[uv_indices->data[i3] * 2 + 1];
	int tile_u = (int)((u1 + u2 + u3) / 3);
	int tile_v = (int)((v1 + v2 + v3) / 3);
	return tile_u + tile_v * tiles_u;
}

static bool pnpoly(float v0x, float v0y, float v1x, float v1y, float v2x, float v2y, float px, float py) {
	// https://wrf.ecse.rpi.edu//Research/Short_Notes/pnpoly.html
	bool c = false;
	if (((v0y > py) != (v2y > py)) && (px < (v2x - v0x) * (py - v0y) / (v2y - v0y) + v0x)) c = !c;
	if (((v1y > py) != (v0y > py)) && (px < (v0x - v1x) * (py - v1y) / (v0y - v1y) + v1x)) c = !c;
	if (((v2y > py) != (v1y > py)) && (px < (v1x - v2x) * (py - v2y) / (v1y - v2y) + v2x)) c = !c;
	return c;
}

kinc_vector4_t calc_normal(kinc_vector4_t a, kinc_vector4_t b, kinc_vector4_t c) {
	kinc_vector4_t cb = vec4_sub(c, b);
	kinc_vector4_t ab = vec4_sub(a, b);
	cb = vec4_cross(cb, ab);
	cb = vec4_normalize(cb);
	return cb;
}

// 'o' for object split, 'g' for groups, 'u'semtl for materials
obj_part_t *io_obj_parse(uint8_t *file_bytes, char split_code, uint32_t start_pos, bool udim) {
	bytes = file_bytes;
	bytes_length = strlen(file_bytes);

	part = (obj_part_t *)malloc(sizeof(obj_part_t));
	part->scale_pos = 1.0;
	part->scale_tex = 1.0;
	part->pos = start_pos;
	part->udims_u = 1;
	part->udims_v = 1;
	part->udims = NULL;
	part->has_next = false;
	part->texa = NULL;
	part->name = str;

	array_i32_t pos_indices = {0};
	array_i32_t uv_indices = {0};
	array_i32_t nor_indices = {0};

	bool reading_faces = false;
	bool reading_object = false;
	bool full_attrib = false;

	if (start_pos == 0) vind_off = tind_off = nind_off = 0;

	if (split_code == 'u' && start_pos > 0) {
		pos_temp = *pos_first;
		nor_temp = *nor_first;
		uv_temp = *uv_first;
	}
	else {
		memset(&pos_temp, 0, sizeof(pos_temp));
		memset(&uv_temp, 0, sizeof(uv_temp));
		memset(&nor_temp, 0, sizeof(nor_temp));
	}

	while (true) {
		if (part->pos >= bytes_length) break;

		char c0 = bytes[part->pos++];
		if (reading_object && reading_faces && (c0 == 'v' || c0 == split_code)) {
			part->pos--;
			part->has_next = true;
			break;
		}

		if (c0 == 'v') {
			char c1 = bytes[part->pos++];
			if (c1 == ' ') {
				if (bytes[part->pos] == ' ') part->pos++; // Some exporters put additional space directly after "v"
				array_f32_push(&pos_temp, read_float());
				part->pos++; // Space
				array_f32_push(&pos_temp, read_float());
				part->pos++; // Space
				array_f32_push(&pos_temp, read_float());
			}
			else if (c1 == 't') {
				part->pos++; // Space
				array_f32_push(&uv_temp, read_float());
				part->pos++; // Space
				array_f32_push(&uv_temp, read_float());
				if (nor_temp.length > 0) full_attrib = true;
			}
			else if (c1 == 'n') {
				part->pos++; // Space
				array_f32_push(&nor_temp, read_float());
				part->pos++; // Space
				array_f32_push(&nor_temp, read_float());
				part->pos++; // Space
				array_f32_push(&nor_temp, read_float());
				if (uv_temp.length > 0) full_attrib = true;
			}
		}
		else if (c0 == 'f') {
			part->pos++; // Space
			if (bytes[part->pos] == ' ') part->pos++; // Some exporters put additional space directly after "f"
			reading_faces = true;
			vi = ui = ni = 0;
			full_attrib ? read_face_fast() : read_face();

			if (vi <= 4) { // Convex, fan triangulation
				array_i32_push(&pos_indices, va[0]);
				array_i32_push(&pos_indices, va[1]);
				array_i32_push(&pos_indices, va[2]);
				for (int i = 3; i < vi; ++i) {
					array_i32_push(&pos_indices, va[0]);
					array_i32_push(&pos_indices, va[i - 1]);
					array_i32_push(&pos_indices, va[i]);
				}
				if (uv_temp.length > 0) {
					array_i32_push(&uv_indices, ua[0]);
					array_i32_push(&uv_indices, ua[1]);
					array_i32_push(&uv_indices, ua[2]);
					for (int i = 3; i < ui; ++i) {
						array_i32_push(&uv_indices, ua[0]);
						array_i32_push(&uv_indices, ua[i - 1]);
						array_i32_push(&uv_indices, ua[i]);
					}
				}
				if (nor_temp.length > 0) {
					array_i32_push(&nor_indices, na[0]);
					array_i32_push(&nor_indices, na[1]);
					array_i32_push(&nor_indices, na[2]);
					for (int i = 3; i < ni; ++i) {
						array_i32_push(&nor_indices, na[0]);
						array_i32_push(&nor_indices, na[i - 1]);
						array_i32_push(&nor_indices, na[i]);
					}
				}
			}
			else { // Convex or concave, ear clipping
				int _vind_off = split_code == 'u' ? 0 : vind_off;
				int _nind_off = split_code == 'u' ? 0 : nind_off;
				float nx = 0.0;
				float ny = 0.0;
				float nz = 0.0;
				if (nor_temp.length > 0) {
					nx = nor_temp.data[(na[0] - _nind_off) * 3    ];
					ny = nor_temp.data[(na[0] - _nind_off) * 3 + 1];
					nz = nor_temp.data[(na[0] - _nind_off) * 3 + 2];
				}
				else {
					kinc_vector4_t n = calc_normal(
						vec4_new(pos_temp.data[(va[0] - _vind_off) * 3], pos_temp.data[(va[0] - _vind_off) * 3 + 1], pos_temp.data[(va[0] - _vind_off) * 3 + 2], 1.0f),
						vec4_new(pos_temp.data[(va[1] - _vind_off) * 3], pos_temp.data[(va[1] - _vind_off) * 3 + 1], pos_temp.data[(va[1] - _vind_off) * 3 + 2], 1.0f),
						vec4_new(pos_temp.data[(va[2] - _vind_off) * 3], pos_temp.data[(va[2] - _vind_off) * 3 + 1], pos_temp.data[(va[2] - _vind_off) * 3 + 2], 1.0f)
					);
					nx = n.x;
					ny = n.y;
					nz = n.z;
				}
				float nxabs = (float)fabs(nx);
				float nyabs = (float)fabs(ny);
				float nzabs = (float)fabs(nz);
				bool flip = nx + ny + nz > 0;
				int axis = nxabs > nyabs && nxabs > nzabs ? 0 : nyabs > nxabs && nyabs > nzabs ? 1 : 2;
				int axis0 = axis == 0 ? (flip ? 2 : 1) : axis == 1 ? (flip ? 0 : 2) : (flip ? 1 : 0);
				int axis1 = axis == 0 ? (flip ? 1 : 2) : axis == 1 ? (flip ? 2 : 0) : (flip ? 0 : 1);

				int loops = 0;
				int i = -1;
				while (vi > 3 && loops++ < vi) {
					i = (i + 1) % vi;
					int i1 = (i + 1) % vi;
					int i2 = (i + 2) % vi;
					int vi0 = (va[i ] - _vind_off) * 3;
					int vi1 = (va[i1] - _vind_off) * 3;
					int vi2 = (va[i2] - _vind_off) * 3;
					float v0x = pos_temp.data[vi0 + axis0];
					float v0y = pos_temp.data[vi0 + axis1];
					float v1x = pos_temp.data[vi1 + axis0];
					float v1y = pos_temp.data[vi1 + axis1];
					float v2x = pos_temp.data[vi2 + axis0];
					float v2y = pos_temp.data[vi2 + axis1];

					float e0x = v0x - v1x; // Not an interior vertex
					float e0y = v0y - v1y;
					float e1x = v2x - v1x;
					float e1y = v2y - v1y;
					float cross = e0x * e1y - e0y * e1x;
					if (cross <= 0) continue;

					bool overlap = false; // Other vertex found inside this triangle
					for (int j = 0; j < vi - 3; ++j) {
						int j0 = (va[(i + 3 + j) % vi] - _vind_off) * 3;
						float px = pos_temp.data[j0 + axis0];
						float py = pos_temp.data[j0 + axis1];
						if (pnpoly(v0x, v0y, v1x, v1y, v2x, v2y, px, py)) {
							overlap = true;
							break;
						}
					}
					if (overlap) continue;

					array_i32_push(&pos_indices, va[i ]); // Found ear
					array_i32_push(&pos_indices, va[i1]);
					array_i32_push(&pos_indices, va[i2]);
					if (uv_temp.length > 0) {
						array_i32_push(&uv_indices, ua[i ]);
						array_i32_push(&uv_indices, ua[i1]);
						array_i32_push(&uv_indices, ua[i2]);
					}
					if (nor_temp.length > 0) {
						array_i32_push(&nor_indices, na[i ]);
						array_i32_push(&nor_indices, na[i1]);
						array_i32_push(&nor_indices, na[i2]);
					}

					for (int j = ((i + 1) % vi); j < vi - 1; ++j) { // Consume vertex
						va[j] = va[j + 1];
						ua[j] = ua[j + 1];
						na[j] = na[j + 1];
					}
					vi--;
					i--;
					loops = 0;
				}
				array_i32_push(&pos_indices, va[0]); // Last one
				array_i32_push(&pos_indices, va[1]);
				array_i32_push(&pos_indices, va[2]);
				if (uv_temp.length > 0) {
					array_i32_push(&uv_indices, ua[0]);
					array_i32_push(&uv_indices, ua[1]);
					array_i32_push(&uv_indices, ua[2]);
				}
				if (nor_temp.length > 0) {
					array_i32_push(&nor_indices, na[0]);
					array_i32_push(&nor_indices, na[1]);
					array_i32_push(&nor_indices, na[2]);
				}
			}
		}
		else if (c0 == split_code) {
			if (split_code == 'u') part->pos += 5; // "u"semtl
			part->pos++; // Space
			if (!udim) reading_object = true;
			part->name = read_string();
		}
		next_line();
	}

	if (start_pos > 0) {
		if (split_code != 'u') {
			for (int i = 0; i < pos_indices.length; ++i) pos_indices.data[i] -= vind_off;
			for (int i = 0; i < uv_indices.length; ++i) uv_indices.data[i] -= tind_off;
			for (int i = 0; i < nor_indices.length; ++i) nor_indices.data[i] -= nind_off;
		}
	}
	else {
		if (split_code == 'u') {
			pos_first = &pos_temp;
			nor_first = &nor_temp;
			uv_first = &uv_temp;
		}
	}
	vind_off += (int)(pos_temp.length / 3); // Assumes separate vertex data per object
	tind_off += (int)(uv_temp.length / 2);
	nind_off += (int)(nor_temp.length / 3);

	// Pack positions to (-1, 1) range
	part->scale_pos = 0.0;
	for (int i = 0; i < pos_temp.length; ++i) {
		float f = (float)fabs(pos_temp.data[i]);
		if (part->scale_pos < f) part->scale_pos = f;
	}
	float inv = 32767 * (1 / part->scale_pos);

	part->posa = (int16_t *)malloc(pos_indices.length * 4 * sizeof(int16_t));
	part->inda = (uint32_t *)malloc(pos_indices.length * sizeof(uint32_t));
	part->vertex_count = pos_indices.length;
	part->index_count = pos_indices.length;
	int inda_length = pos_indices.length;
	for (int i = 0; i < pos_indices.length; ++i) {
		part->posa[i * 4    ] = (int)( pos_temp.data[pos_indices.data[i] * 3    ] * inv);
		part->posa[i * 4 + 1] = (int)(-pos_temp.data[pos_indices.data[i] * 3 + 2] * inv);
		part->posa[i * 4 + 2] = (int)( pos_temp.data[pos_indices.data[i] * 3 + 1] * inv);
		part->inda[i] = i;
	}

	if (nor_indices.length > 0) {
		part->nora = (int16_t *)malloc(nor_indices.length * 2 * sizeof(int16_t));
		for (int i = 0; i < pos_indices.length; ++i) {
			part->nora[i * 2    ] = (int)( nor_temp.data[nor_indices.data[i] * 3    ] * 32767);
			part->nora[i * 2 + 1] = (int)(-nor_temp.data[nor_indices.data[i] * 3 + 2] * 32767);
			part->posa[i * 4 + 3] = (int)( nor_temp.data[nor_indices.data[i] * 3 + 1] * 32767);
		}
	}
	else {
		// Calc normals
		part->nora = (int16_t *)malloc(inda_length * 2 * sizeof(int16_t));
		for (int i = 0; i < (int)(inda_length / 3); ++i) {
			int i1 = part->inda[i * 3    ];
			int i2 = part->inda[i * 3 + 1];
			int i3 = part->inda[i * 3 + 2];
			kinc_vector4_t n = calc_normal(
				vec4_new(part->posa[i1 * 4], part->posa[i1 * 4 + 1], part->posa[i1 * 4 + 2], 1.0),
				vec4_new(part->posa[i2 * 4], part->posa[i2 * 4 + 1], part->posa[i2 * 4 + 2], 1.0),
				vec4_new(part->posa[i3 * 4], part->posa[i3 * 4 + 1], part->posa[i3 * 4 + 2], 1.0)
			);
			part->nora[i1 * 2    ] = (int)(n.x * 32767);
			part->nora[i1 * 2 + 1] = (int)(n.y * 32767);
			part->posa[i1 * 4 + 3] = (int)(n.z * 32767);
			part->nora[i2 * 2    ] = (int)(n.x * 32767);
			part->nora[i2 * 2 + 1] = (int)(n.y * 32767);
			part->posa[i2 * 4 + 3] = (int)(n.z * 32767);
			part->nora[i3 * 2    ] = (int)(n.x * 32767);
			part->nora[i3 * 2 + 1] = (int)(n.y * 32767);
			part->posa[i3 * 4 + 3] = (int)(n.z * 32767);
		}
	}

	if (uv_indices.length > 0) {
		if (udim) {
			// Find number of tiles
			int tiles_u = 1;
			int tiles_v = 1;
			for (int i = 0; i < (int)(uv_temp.length / 2); ++i) {
				while (uv_temp.data[i * 2    ] > tiles_u) tiles_u++;
				while (uv_temp.data[i * 2 + 1] > tiles_v) tiles_v++;
			}

			// Amount of indices pre tile
			uint32_t *num = (uint32_t *)malloc(tiles_u * tiles_v * sizeof(uint32_t));
			memset(num, 0, tiles_u * tiles_v * sizeof(uint32_t));
			for (int i = 0; i < (int)(inda_length / 3); ++i) {
				int tile = get_tile(part->inda[i * 3], part->inda[i * 3 + 1], part->inda[i * 3 + 2], &uv_indices, tiles_u);
				num[tile] += 3;
			}

			// Split indices per tile
			part->udims = (uint32_t **)malloc(tiles_u * tiles_v * sizeof(uint32_t *));
			part->udims_count = (int *)malloc(tiles_u * tiles_v * sizeof(int));
			part->udims_u = tiles_u;
			part->udims_v = tiles_v;
			for (int i = 0; i < tiles_u * tiles_v; ++i) {
				part->udims[i] = (uint32_t *)malloc(num[i] * sizeof(uint32_t));
				part->udims_count[i] = num[i];
				num[i] = 0;
			}

			for (int i = 0; i < (int)(inda_length / 3); ++i) {
				int i1 = part->inda[i * 3    ];
				int i2 = part->inda[i * 3 + 1];
				int i3 = part->inda[i * 3 + 2];
				int tile = get_tile(i1, i2, i3, &uv_indices, tiles_u);
				part->udims[tile][num[tile]++] = i1;
				part->udims[tile][num[tile]++] = i2;
				part->udims[tile][num[tile]++] = i3;
			}

			// Normalize uvs to 0-1 range
			int16_t *uvtiles = (int16_t *)malloc(uv_temp.length * sizeof(int16_t));
			for (int i = 0; i < (int)(inda_length / 3); ++i) { // TODO: merge loops
				int i1 = part->inda[i * 3    ];
				int i2 = part->inda[i * 3 + 1];
				int i3 = part->inda[i * 3 + 2];
				int tile = get_tile(i1, i2, i3, &uv_indices, tiles_u);
				int tile_u = tile % tiles_u;
				int tile_v = (int)(tile / tiles_u);
				uvtiles[uv_indices.data[i1] * 2    ] = tile_u;
				uvtiles[uv_indices.data[i1] * 2 + 1] = tile_v;
				uvtiles[uv_indices.data[i2] * 2    ] = tile_u;
				uvtiles[uv_indices.data[i2] * 2 + 1] = tile_v;
				uvtiles[uv_indices.data[i3] * 2    ] = tile_u;
				uvtiles[uv_indices.data[i3] * 2 + 1] = tile_v;
			}
			for (int i = 0; i < uv_temp.length; ++i) uv_temp.data[i] -= uvtiles[i];
			free(uvtiles);
		}

		part->texa = (int16_t *)malloc(uv_indices.length * 2 * sizeof(int16_t));
		for (int i = 0; i < pos_indices.length; ++i) {
			float uvx = uv_temp.data[uv_indices.data[i] * 2];
			if (uvx > 1.0) uvx = uvx - (int)(uvx);
			float uvy = uv_temp.data[uv_indices.data[i] * 2 + 1];
			if (uvy > 1.0) uvy = uvy - (int)(uvy);
			part->texa[i * 2    ] = (int)(       uvx  * 32767);
			part->texa[i * 2 + 1] = (int)((1.0 - uvy) * 32767);
		}
	}
	bytes = NULL;
	if (!part->has_next) {
		pos_first = nor_first = uv_first = NULL;
		array_f32_free(&pos_temp);
		array_f32_free(&uv_temp);
		array_f32_free(&nor_temp);
	}

	array_i32_free(&pos_indices);
	array_i32_free(&uv_indices);
	array_i32_free(&nor_indices);

	return part;
}

void io_obj_destroy(obj_part_t *part) {
	if (part->udims != NULL) {
		for (int i = 0; i < part->udims_u * part->udims_v; ++i) {
			free(part->udims[i]);
		}
		free(part->udims);
		free(part->udims_count);
	}

	free(part->posa);
	free(part->nora);
	free(part->texa);
	free(part->inda);
	free(part);
}
