/* The FBX files of model.fbx (spec 8.2): a flat interface on ufbx (vendor/ufbx.c, MIT or public
   domain, THIRD_PARTY_NOTICES.md), so that Sosu reads a scene through plain functions. ufbx reads
   binary and ASCII FBX, built to take files from anywhere: every read is bounded, and its memory
   is limited here. The scene is turned to the axes of math.linear (right-handed, y up) and to
   meters; the meshes are cut into triangles, one part per material. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "vendor/ufbx.h"

/* What a file asks of memory, at most: beyond, the load fails as a file too large. */
#define SOSU_FBX_MEMORY_LIMIT ((size_t)1 << 30)

typedef struct {
    ufbx_scene *scene;
    size_t parts;          /* the parts of every mesh, in order */
    uint32_t *part_mesh;   /* the mesh of each part */
    uint32_t *part_index;  /* its index among the parts of its mesh */
} SosuFbx;

/* A scene read from `data`, or NULL with what went wrong in `error`. */
void *sosu_fbx_load(const unsigned char *data, size_t size, char *error, size_t error_size) {
    ufbx_load_opts opts;
    memset(&opts, 0, sizeof opts);
    opts.target_axes = ufbx_axes_right_handed_y_up;
    opts.target_unit_meters = 1.0;
    opts.space_conversion = UFBX_SPACE_CONVERSION_MODIFY_GEOMETRY;
    opts.generate_missing_normals = true;
    opts.temp_allocator.memory_limit = SOSU_FBX_MEMORY_LIMIT;
    opts.result_allocator.memory_limit = SOSU_FBX_MEMORY_LIMIT;
    ufbx_error failed;
    ufbx_scene *scene = ufbx_load_memory(data, size, &opts, &failed);
    if (!scene) {
        if (error && error_size > 0) ufbx_format_error(error, error_size, &failed);
        return NULL;
    }
    SosuFbx *f = calloc(1, sizeof *f);
    if (!f) {
        ufbx_free_scene(scene);
        if (error && error_size > 0) snprintf(error, error_size, "out of memory");
        return NULL;
    }
    f->scene = scene;
    size_t total = 0;
    for (size_t m = 0; m < scene->meshes.count; m++) total += scene->meshes.data[m]->material_parts.count;
    f->part_mesh = calloc(total ? total : 1, sizeof *f->part_mesh);
    f->part_index = calloc(total ? total : 1, sizeof *f->part_index);
    if (!f->part_mesh || !f->part_index) {
        free(f->part_mesh);
        free(f->part_index);
        ufbx_free_scene(scene);
        free(f);
        if (error && error_size > 0) snprintf(error, error_size, "out of memory");
        return NULL;
    }
    for (size_t m = 0; m < scene->meshes.count; m++) {
        for (size_t p = 0; p < scene->meshes.data[m]->material_parts.count; p++) {
            f->part_mesh[f->parts] = (uint32_t)m;
            f->part_index[f->parts] = (uint32_t)p;
            f->parts++;
        }
    }
    return f;
}

void sosu_fbx_free(void *handle) {
    SosuFbx *f = handle;
    if (!f) return;
    ufbx_free_scene(f->scene);
    free(f->part_mesh);
    free(f->part_index);
    free(f);
}

/* ---- Parts: the triangles of one material of one mesh ---- */

size_t sosu_fbx_parts(void *handle) {
    return ((SosuFbx *)handle)->parts;
}

/* The mesh of a part (its index among the meshes of the scene). */
size_t sosu_fbx_part_mesh(void *handle, size_t part) {
    SosuFbx *f = handle;
    return part < f->parts ? f->part_mesh[part] : 0;
}

size_t sosu_fbx_part_triangles(void *handle, size_t part) {
    SosuFbx *f = handle;
    if (part >= f->parts) return 0;
    ufbx_mesh *mesh = f->scene->meshes.data[f->part_mesh[part]];
    return mesh->material_parts.data[f->part_index[part]].num_triangles;
}

/* The material of a part (its index among the materials of the scene), or -1. */
long long sosu_fbx_part_material(void *handle, size_t part) {
    SosuFbx *f = handle;
    if (part >= f->parts) return -1;
    ufbx_mesh *mesh = f->scene->meshes.data[f->part_mesh[part]];
    uint32_t index = f->part_index[part];
    if (index >= mesh->materials.count || !mesh->materials.data[index]) return -1;
    return (long long)mesh->materials.data[index]->typed_id;
}

/* The corners of the triangles of a part, 8 floats each (position, normal, uv with 0 at the top
   of the texture), into `out` of `capacity` floats; how many corners were written. */
size_t sosu_fbx_part_corners(void *handle, size_t part, float *out, size_t capacity) {
    SosuFbx *f = handle;
    if (part >= f->parts) return 0;
    ufbx_mesh *mesh = f->scene->meshes.data[f->part_mesh[part]];
    ufbx_mesh_part *p = &mesh->material_parts.data[f->part_index[part]];
    uint32_t *tri = malloc(mesh->max_face_triangles * 3 * sizeof *tri + 3 * sizeof *tri);
    if (!tri) return 0;
    size_t written = 0;
    for (size_t i = 0; i < p->face_indices.count; i++) {
        ufbx_face face = mesh->faces.data[p->face_indices.data[i]];
        uint32_t count = ufbx_triangulate_face(tri, mesh->max_face_triangles * 3, mesh, face);
        for (uint32_t c = 0; c < count * 3; c++) {
            if ((written + 1) * 8 > capacity) {
                free(tri);
                return written;
            }
            uint32_t index = tri[c];
            ufbx_vec3 pos = ufbx_get_vertex_vec3(&mesh->vertex_position, index);
            ufbx_vec3 n;
            n.x = 0;
            n.y = 1;
            n.z = 0;
            if (mesh->vertex_normal.exists) n = ufbx_get_vertex_vec3(&mesh->vertex_normal, index);
            ufbx_vec2 uv;
            uv.x = 0;
            uv.y = 0;
            if (mesh->vertex_uv.exists) uv = ufbx_get_vertex_vec2(&mesh->vertex_uv, index);
            float *v = out + written * 8;
            v[0] = (float)pos.x;
            v[1] = (float)pos.y;
            v[2] = (float)pos.z;
            v[3] = (float)n.x;
            v[4] = (float)n.y;
            v[5] = (float)n.z;
            v[6] = (float)uv.x;
            v[7] = (float)(1.0 - uv.y);
            written++;
        }
    }
    free(tri);
    return written;
}

/* ---- Materials ---- */

size_t sosu_fbx_materials(void *handle) {
    return ((SosuFbx *)handle)->scene->materials.count;
}

/* A UTF-8 string of ufbx into `out` of `size` bytes, ended by 0. */
static size_t copy_string(ufbx_string s, char *out, size_t size) {
    if (!out || size == 0) return 0;
    size_t n = s.length < size - 1 ? s.length : size - 1;
    memcpy(out, s.data, n);
    out[n] = 0;
    return n;
}

size_t sosu_fbx_material_name(void *handle, size_t m, char *out, size_t size) {
    SosuFbx *f = handle;
    if (m >= f->scene->materials.count) return 0;
    return copy_string(f->scene->materials.data[m]->name, out, size);
}

/* Its base color (4 floats), metalness and roughness, from its PBR maps (ufbx fills them from
   the older models of shading too). */
void sosu_fbx_material_factors(void *handle, size_t m, float *color, float *metallic, float *roughness) {
    SosuFbx *f = handle;
    color[0] = color[1] = color[2] = color[3] = 1.0f;
    *metallic = 0.0f;
    *roughness = 1.0f;
    if (m >= f->scene->materials.count) return;
    ufbx_material *mat = f->scene->materials.data[m];
    if (mat->pbr.base_color.has_value) {
        ufbx_vec4 c = mat->pbr.base_color.value_vec4;
        double factor = mat->pbr.base_factor.has_value ? mat->pbr.base_factor.value_real : 1.0;
        color[0] = (float)(c.x * factor);
        color[1] = (float)(c.y * factor);
        color[2] = (float)(c.z * factor);
        color[3] = (float)(mat->pbr.base_color.value_components >= 4 ? c.w : 1.0);
    }
    if (mat->pbr.metalness.has_value) *metallic = (float)mat->pbr.metalness.value_real;
    if (mat->pbr.roughness.has_value) *roughness = (float)mat->pbr.roughness.value_real;
}

/* The texture of its base color: 1 with its bytes when the file holds them, 2 when it names a
   file only (`name` gets its path relative to the FBX), 0 when it has none. */
int sosu_fbx_material_texture(void *handle, size_t m, const unsigned char **bytes, size_t *size, char *name, size_t name_size) {
    SosuFbx *f = handle;
    *bytes = NULL;
    *size = 0;
    if (name && name_size) name[0] = 0;
    if (m >= f->scene->materials.count) return 0;
    ufbx_texture *t = f->scene->materials.data[m]->pbr.base_color.texture;
    if (!t) return 0;
    copy_string(t->relative_filename.length ? t->relative_filename : t->filename, name, name_size);
    if (t->content.size > 0) {
        *bytes = t->content.data;
        *size = t->content.size;
        return 1;
    }
    return t->relative_filename.length || t->filename.length ? 2 : 0;
}

/* ---- Nodes ---- */

size_t sosu_fbx_nodes(void *handle) {
    return ((SosuFbx *)handle)->scene->nodes.count;
}

/* The node at the root of the scene. */
size_t sosu_fbx_root(void *handle) {
    return ((SosuFbx *)handle)->scene->root_node->typed_id;
}

size_t sosu_fbx_node_name(void *handle, size_t n, char *out, size_t size) {
    SosuFbx *f = handle;
    if (n >= f->scene->nodes.count) return 0;
    return copy_string(f->scene->nodes.data[n]->name, out, size);
}

/* Where the node is relative to its parent: 16 floats by columns. */
void sosu_fbx_node_matrix(void *handle, size_t n, float *out) {
    SosuFbx *f = handle;
    static const float identity[16] = { 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1 };
    memcpy(out, identity, sizeof identity);
    if (n >= f->scene->nodes.count) return;
    ufbx_matrix m = f->scene->nodes.data[n]->node_to_parent;
    for (int c = 0; c < 4; c++) {
        out[c * 4] = (float)m.cols[c].x;
        out[c * 4 + 1] = (float)m.cols[c].y;
        out[c * 4 + 2] = (float)m.cols[c].z;
    }
}

/* The mesh of the node (its index among the meshes), or -1. */
long long sosu_fbx_node_mesh(void *handle, size_t n) {
    SosuFbx *f = handle;
    if (n >= f->scene->nodes.count || !f->scene->nodes.data[n]->mesh) return -1;
    return (long long)f->scene->nodes.data[n]->mesh->typed_id;
}

size_t sosu_fbx_node_children(void *handle, size_t n) {
    SosuFbx *f = handle;
    if (n >= f->scene->nodes.count) return 0;
    return f->scene->nodes.data[n]->children.count;
}

size_t sosu_fbx_node_child(void *handle, size_t n, size_t k) {
    SosuFbx *f = handle;
    if (n >= f->scene->nodes.count || k >= f->scene->nodes.data[n]->children.count) return 0;
    return f->scene->nodes.data[n]->children.data[k]->typed_id;
}
