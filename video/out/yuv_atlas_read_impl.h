#ifndef MPV_YUV_ATLAS_READ_IMPL_H
#define MPV_YUV_ATLAS_READ_IMPL_H
// Included only by vo_gpu_next, after the ordinary schema9 implementation.
struct atlas_hash_part { const void *data; size_t bytes; };
static bool atlas_hash(const struct atlas_hash_part *parts, size_t count,
                       bool length_prefix, char out[65])
{
    struct AVSHA *sha = av_sha_alloc();
    if (!sha || av_sha_init(sha, 256) < 0) { av_free(sha); return false; }
    for (size_t i = 0; i < count; i++) {
        if (!parts[i].data && parts[i].bytes) { av_free(sha); return false; }
        if (length_prefix) {
            uint8_t length[8];
            for (int b = 0; b < 8; b++) length[b] = (uint64_t)parts[i].bytes >> (8 * b);
            av_sha_update(sha, length, sizeof(length));
        }
        av_sha_update(sha, parts[i].data, parts[i].bytes);
    }
    uint8_t digest[32]; av_sha_final(sha, digest); av_free(sha);
    const char *hex = "0123456789abcdef";
    for (int i = 0; i < 32; i++) { out[2*i]=hex[digest[i]>>4]; out[2*i+1]=hex[digest[i]&15]; }
    out[64]=0; return true;
}

static bool atlas_read_seed_saved(struct priv *p, const char *json, size_t bytes,
                                  const char *basename)
{
    if (!basename || strlen(basename) + 6 > sizeof(p->atlas_seed_manifest) ||
        !atlas_hash(&(struct atlas_hash_part){json, bytes}, 1, false, p->atlas_seed_sha))
        return false;
    snprintf(p->atlas_seed_manifest, sizeof(p->atlas_seed_manifest), "%s.json", basename);
    return true;
}

static bool atlas_material_json(const struct pl_opengl_uv_atlas_material *m,
                                 char *out, size_t capacity, size_t *length)
{
    if (!m || !out || !length || m->input_width < 1 || m->input_height < 1 ||
        m->sampler_type != PL_SAMPLER_NORMAL || m->sample_mode < 0 || m->sample_mode > 1 ||
        m->address_mode < 0 || m->address_mode > 2 || m->format_type < 1 || m->format_type > 5 ||
        m->num_components < 1 || m->num_components > 4 ||
        !mp_yuv_compare_identifier_valid(m->format_name, sizeof(m->format_name)) ||
        m->num_variables > 16 || m->uniform_bytes > 4096)
        return false;
    for (int i = 0; i < 4; i++) {
        if (m->component_depth[i] < 0 || m->component_depth[i] > 64 ||
            m->host_bits[i] < 0 || m->host_bits[i] > 64 ||
            m->sample_order[i] < 0 || m->sample_order[i] > 3 ||
            (i < m->num_components && (!m->component_depth[i] || !m->host_bits[i])))
            return false;
    }
    size_t used = 0;
#define MAT_APPEND(...) do { \
    int n=snprintf(out+used,capacity-used,__VA_ARGS__); \
    if(n<0 || (size_t)n>=capacity-used) return false; used+=n; \
} while(0)
    MAT_APPEND("{\"input_width\":%d,\"input_height\":%d,\"sampler_type\":%d,\"sample_mode\":%d,\"address_mode\":%d,"
        "\"format_type\":%d,\"num_components\":%d,\"component_depth\":[%d,%d,%d,%d],"
        "\"host_bits\":[%d,%d,%d,%d],\"sample_order\":[%d,%d,%d,%d],\"format_name\":\"%s\","
        "\"num_variables\":%u,\"uniform_bytes\":%u,\"variables\":[",
        m->input_width,m->input_height,m->sampler_type,m->sample_mode,m->address_mode,
        m->format_type,m->num_components,m->component_depth[0],m->component_depth[1],
        m->component_depth[2],m->component_depth[3],m->host_bits[0],m->host_bits[1],
        m->host_bits[2],m->host_bits[3],m->sample_order[0],m->sample_order[1],m->sample_order[2],
        m->sample_order[3],m->format_name,m->num_variables,m->uniform_bytes);
    uint64_t end = 0;
    for (uint32_t i = 0; i < m->num_variables; i++) {
        if (!mp_yuv_compare_identifier_valid(m->uniform_names[i],sizeof(m->uniform_names[i])) ||
            m->uniform_types[i] < 1 || m->uniform_types[i] > 3 ||
            m->uniform_dim_v[i] < 1 || m->uniform_dim_v[i] > 4 ||
            m->uniform_dim_m[i] < 1 || m->uniform_dim_m[i] > 4 ||
            m->uniform_dim_a[i] < 1 || m->uniform_dim_a[i] > 1024 ||
            m->uniform_offsets[i] != end ||
            m->uniform_sizes[i] != (uint64_t)4*m->uniform_dim_v[i]*m->uniform_dim_m[i]*m->uniform_dim_a[i])
            return false;
        for (uint32_t j = 0; j < i; j++)
            if (!strcmp(m->uniform_names[i],m->uniform_names[j])) return false;
        end += m->uniform_sizes[i];
        if (end > m->uniform_bytes) return false;
        MAT_APPEND("%s{\"name\":\"%s\",\"type\":%u,\"dim_v\":%u,\"dim_m\":%u,\"dim_a\":%u,\"offset\":%u,\"size\":%u}",
            i?",":"",m->uniform_names[i],m->uniform_types[i],m->uniform_dim_v[i],m->uniform_dim_m[i],
            m->uniform_dim_a[i],m->uniform_offsets[i],m->uniform_sizes[i]);
    }
    if (end != m->uniform_bytes) return false;
    MAT_APPEND("],\"uniforms_hex\":\"");
    const char *hex = "0123456789abcdef";
    if (capacity-used <= (size_t)m->uniform_bytes*2+4) return false;
    for (uint32_t i = 0; i < m->uniform_bytes; i++) {
        out[used++]=hex[m->uniforms[i]>>4]; out[used++]=hex[m->uniforms[i]&15];
    }
    MAT_APPEND("\"}\n");
#undef MAT_APPEND
    *length=used; return true;
}

static bool atlas_cache_info_valid(struct priv *p)
{
    const struct pl_opengl_uv_atlas_cache_info *c=&p->atlas_cache_info;
    return c->cache_id && c->seed_capture_id == p->atlas_seed_id && c->generation == p->compare_generation &&
        c->width == p->compare_width && c->height == p->compare_height && c->width > 0 && c->height > 0 &&
        (uint64_t)c->width*c->height <= MP_YUV_ATLAS_MAX_PIXELS &&
        c->pixels == (size_t)c->width*c->height && c->atlas_words == c->pixels*3 &&
        c->storage_buffer && !c->storage_offset && c->storage_size == c->atlas_words*4;
}

static bool atlas_read_clean(struct vo *vo, bool destroy_cache)
{
    struct priv *p=vo->priv;
    if (!mks_android_yuv_compare_owner_current(p->ra_ctx) ||
        (p->atlas_read && !pl_opengl_uv_atlas_read_cancel(&p->atlas_read)) ||
        (destroy_cache && p->atlas_cache && !pl_opengl_uv_atlas_cache_destroy(&p->atlas_cache))) {
        compare_fail_transaction(vo,"atlas-read-cleanup-owner-or-resource-failed");
        p->compare_fatal_context=p->compare_terminal=true;
        MP_ERR(vo,"MKS-UV-ATLAS-READ event=cleanup-deferred requires_force_stop=1\n");
        return false;
    }
    mks_android_yuv_compare_cancel(p->ra_ctx);
    return true;
}

static void atlas_read_reject(struct vo *vo, const char *reason, unsigned code,
                               bool hard, bool original_shown)
{
    struct priv *p=vo->priv;
    MP_ERR(vo,"MKS-UV-ATLAS-READ event=capture-failed id=%"PRIu64" ordinal=%u cache_id=%"PRIu64" reason=%s reject_reason=%u fallback_original=%d\n",
        p->compare_id,p->atlas_ordinal,p->atlas_cache_info.cache_id,reason,code,original_shown);
    p->compare_enabled=false;
    p->atlas_phase=MP_ATLAS_REJECTED;
    if (hard) {
        compare_fail_transaction(vo,reason);
        return;
    }
    if (!atlas_read_clean(vo,true)) return;
    p->compare_active=p->compare_started=p->compare_a_ready=p->compare_terminal=false;
}

static void atlas_read_seed_ready(struct vo *vo)
{
    struct priv *p=vo->priv;
    const uint32_t *promoted_atlas = NULL;
    if (p->atlas_phase != MP_ATLAS_SEED_PENDING || !compare_current(p) ||
        !(p->atlas_cache=pl_opengl_uv_atlas_promote(&p->atlas_anchor)) ||
        !pl_opengl_uv_atlas_cache_info(p->atlas_cache,&p->atlas_cache_info,&promoted_atlas) ||
        !atlas_cache_info_valid(p) || !p->atlas_seed_sha[0]) {
        atlas_read_reject(vo,"seed-promote-or-info-failed",5,true,false); return;
    }
    if (!atlas_read_clean(vo,false)) return;
    p->compare_active=p->compare_started=p->compare_a_ready=false;
    p->atlas_phase=MP_ATLAS_T1_REDRAW;
    p->compare_deadline=mp_time_ns()+INT64_C(30000000000);
    MP_ERR(vo,"MKS-UV-ATLAS-READ event=seed-promoted seed_capture_id=%"PRIu64" cache_id=%"PRIu64" cache_generation=%"PRIu64" seed_pts=%.9f nonce=%s\n",
        p->atlas_seed_id,p->atlas_cache_info.cache_id,p->atlas_cache_info.generation,p->atlas_seed_pts,p->atlas_nonce);
    // vo.h guarantees the same media frame_id for this instant redraw.
    vo_redraw(vo);
}

static void atlas_read_ready_event(struct vo *vo, const char *event)
{
    struct priv *p=vo->priv;
    MP_ERR(vo,"MKS-UV-ATLAS-READ event=%s stage=1 ordinal=1 nonce=%s cache_id=%"PRIu64" cache_generation=%"PRIu64" renderer_id=%"PRIu64" renderer_generation=%"PRIu64" signature=%s seed_capture_id=%"PRIu64" seed_pts=%.9f t2_pts=%.9f\n",
        event,p->atlas_nonce,p->atlas_cache_info.cache_id,p->atlas_cache_info.generation,
        p->atlas_renderer_id,p->atlas_renderer_generation,p->atlas_signature,p->atlas_seed_id,
        p->atlas_seed_pts,p->atlas_second_pts);
}

static bool atlas_consumer_result_valid(struct priv *p,
    const struct pl_opengl_uv_atlas_read_result *r)
{
    if (!r || !p->compare_epoch_valid || !p->compare_frame_id || !p->compare_frame_token ||
        !atlas_cache_info_valid(p) || r->capture_id != p->compare_id ||
        r->cache_id != p->atlas_cache_info.cache_id || r->seed_capture_id != p->atlas_seed_id ||
        r->generation != p->compare_generation || r->ordinal != p->atlas_ordinal ||
        (r->ordinal != 1 && r->ordinal != 2) || r->width != p->compare_width || r->height != p->compare_height ||
        r->pixels != p->atlas_cache_info.pixels || r->rgba_words != r->pixels*4 || r->probe_words != r->pixels*4 ||
        !r->signature_match || !r->baseline_ok || !r->fence_signaled || r->reject_reason ||
        !r->program_tokens[0] || !r->program_tokens[1] || r->program_tokens[1] != r->program_tokens[2] ||
        r->program_tokens[0] == r->program_tokens[1] || !r->b0_program_token ||
        !r->seed_input_texture_token || !r->input_texture_token ||
        r->probe_modes[0] != 0 || r->probe_modes[1] != 1 || r->finish_epoch != p->compare_error_epoch ||
        p->atlas_renderer_id != (uint64_t)(uintptr_t)p->rr || !p->atlas_renderer_generation ||
        p->compare_frame_token <= p->atlas_seed_frame_token ||
        !mp_yuv_compare_target_matches(p->compare_source_pts,p->compare_current_pts,
            r->ordinal == 1 ? p->compare_target_pts : p->atlas_second_pts))
        return false;
    if (r->ordinal == 1) {
        if (p->compare_frame_id != p->atlas_seed_frame_id || p->atlas_input_image != p->atlas_seed_image ||
            !mp_yuv_compare_target_matches(p->compare_source_pts,p->compare_current_pts,p->atlas_seed_pts))
            return false;
    } else if (p->compare_frame_id == p->atlas_seed_frame_id ||
               fabs(p->compare_source_pts-p->atlas_seed_pts) <= 0.000002) return false;
    for (int i=0;i<3;i++) {
        if (!r->draw_ok[i] || !r->readback_ok[i] || r->draw_epoch_before[i] != p->compare_error_epoch ||
            r->draw_epoch_after[i] != p->compare_error_epoch || r->readback_epoch_before[i] != p->compare_error_epoch ||
            r->readback_epoch_after[i] != p->compare_error_epoch) return false;
    }
    return true;
}

// Validates the one quad layout shared by seed and consumer and serializes it
// in the exact compact form the analyzer hashes back (json.dumps separators).
static bool atlas_read_quad_json(struct priv *p, const uint8_t *vertices,
    const struct pl_opengl_yuv_compare_quad_layout *q, char *out, size_t capacity, size_t *length)
{
    if (!vertices || !q || !out || !length ||
        q->primitive != PL_PRIM_TRIANGLE_STRIP || q->indexed || q->vertex_count != 4 ||
        q->num_attributes < 1 || q->num_attributes > 16 ||
        q->position_attrib < 0 || q->position_attrib >= q->num_attributes ||
        q->viewport.x0 || q->viewport.y0 || q->viewport.x1 != p->compare_width ||
        q->viewport.y1 != p->compare_height || q->scissors.x0 || q->scissors.y0 ||
        q->scissors.x1 != p->compare_width || q->scissors.y1 != p->compare_height ||
        q->stride < 4 || q->stride > 1024 || q->stride % 4 || q->byte_size != q->stride * 4 ||
        !mp_yuv_compare_quad_payloads_valid(vertices, vertices, q->stride, q->byte_size, false))
        return false;
    size_t used = 0;
#define QUAD_JSON(...) do { \
    int n=snprintf(out+used,capacity-used,__VA_ARGS__); \
    if(n<0 || (size_t)n>=capacity-used) return false; used+=(size_t)n; \
} while(0)
    QUAD_JSON("{\"primitive\":\"triangle_strip\",\"indexed\":false,\"vertex_count\":4,"
        "\"num_attributes\":%d,\"position_attrib\":%d,\"stride\":%zu,\"byte_size\":%zu,"
        "\"viewport\":[%d,%d,%d,%d],\"scissors\":[%d,%d,%d,%d],\"attributes\":[",
        q->num_attributes,q->position_attrib,q->stride,q->byte_size,
        q->viewport.x0,q->viewport.y0,q->viewport.x1,q->viewport.y1,
        q->scissors.x0,q->scissors.y0,q->scissors.x1,q->scissors.y1);
    for (int i = 0; i < q->num_attributes; i++) {
        const struct pl_opengl_yuv_compare_quad_attrib *at = &q->attributes[i];
        if (!mp_yuv_compare_identifier_valid(at->name, sizeof(at->name)) ||
            !mp_yuv_compare_identifier_valid(at->format_name, sizeof(at->format_name)) ||
            !mp_yuv_compare_identifier_valid(at->glsl_type, sizeof(at->glsl_type)) ||
            at->fmt_type != PL_FMT_FLOAT || at->opaque || at->num_planes || at->location < 0 ||
            at->location > 15 || at->num_components < 1 || at->num_components > 4 ||
            at->texel_size != (size_t)at->num_components * 4 || at->offset % 4 ||
            at->offset > q->stride || at->texel_size > q->stride - at->offset ||
            (i == q->position_attrib &&
             (at->num_components != 2 || strcmp(at->glsl_type, "vec2"))))
            return false;
        const char *types[] = {"float", "vec2", "vec3", "vec4"};
        if (strcmp(at->glsl_type, types[at->num_components - 1]))
            return false;
        for (int j = 0; j < 4; j++) {
            if (at->component_depth[j] < 0 || at->component_depth[j] > 32 ||
                at->host_bits[j] < 0 || at->host_bits[j] > 32 ||
                at->sample_order[j] < 0 || at->sample_order[j] > 3 ||
                (j < at->num_components && (at->component_depth[j] != 32 ||
                 at->host_bits[j] != 32 || at->sample_order[j] != j)))
                return false;
        }
        for (int j = 0; j < i; j++) {
            const struct pl_opengl_yuv_compare_quad_attrib *prev = &q->attributes[j];
            if (prev->location == at->location || !strcmp(prev->name, at->name) ||
                (at->offset < prev->offset + prev->texel_size &&
                 prev->offset < at->offset + at->texel_size))
                return false;
        }
        for (size_t v = 0; v < 4; v++) {
            for (int c = 0; c < at->num_components; c++) {
                uint32_t bits;
                memcpy(&bits, vertices + v * q->stride + at->offset + c * 4, 4);
                if ((bits & 0x7f800000u) == 0x7f800000u)
                    return false;
            }
        }
        QUAD_JSON("%s{\"name\":\"%s\",\"format_name\":\"%s\",\"glsl_type\":\"%s\","
            "\"fmt_type\":\"float32\",\"opaque\":false,\"num_planes\":0,\"location\":%d,"
            "\"num_components\":%d,\"component_depth\":[%d,%d,%d,%d],"
            "\"host_bits\":[%d,%d,%d,%d],\"sample_order\":[%d,%d,%d,%d],"
            "\"offset\":%zu,\"texel_size\":%zu}",
            i ? "," : "", at->name, at->format_name, at->glsl_type,
            at->location, at->num_components, at->component_depth[0],
            at->component_depth[1], at->component_depth[2], at->component_depth[3],
            at->host_bits[0], at->host_bits[1], at->host_bits[2], at->host_bits[3],
            at->sample_order[0], at->sample_order[1], at->sample_order[2], at->sample_order[3],
            at->offset, at->texel_size);
    }
    if (!mp_yuv_compare_quad_positions_valid(vertices, q->stride, q->byte_size,
                                             q->attributes[q->position_attrib].offset))
        return false;
    QUAD_JSON("]}");
#undef QUAD_JSON
    *length = used;
    return true;
}

// Serializes one completed consumer read: 13 archive members, the schema10
// manifest and the exact signature digest the analyzer recomputes from the
// archived bytes. Numerical differences stay archived as diagnostic-invalid.
static bool atlas_read_save(struct vo *vo,
    const struct pl_opengl_uv_atlas_read_result *r,
    const struct mks_yuv_compare_result *android)
{
    struct priv *p = vo->priv;
    const uint32_t *a0 = NULL, *uv = NULL, *half = NULL, *seed_atlas = NULL;
    const char *a0_fragment = NULL, *b0 = NULL, *probe = NULL, *vertex = NULL, *body = NULL;
    const struct pl_opengl_uv_atlas_material *seed_material = NULL, *current_material = NULL;
    const uint8_t *vertices = NULL;
    struct pl_opengl_yuv_compare_quad_layout layout = {0};
    size_t bad_uv = 0, bad_half = 0;
    uint32_t mask = 0;
    if (!atlas_consumer_result_valid(p, r) || !compare_current(p) ||
        !pl_opengl_uv_atlas_read_data(p->atlas_read, &a0, &uv, &half) ||
        !pl_opengl_uv_atlas_read_sources(p->atlas_read, &a0_fragment, &b0, &probe,
                                         &vertex, &body) ||
        !pl_opengl_uv_atlas_read_signature(p->atlas_read, &seed_material,
                                           &current_material, &vertices, &layout) ||
        !pl_opengl_uv_atlas_cache_info(p->atlas_cache, &p->atlas_cache_info, &seed_atlas) ||
        !atlas_cache_info_valid(p) ||
        !mp_yuv_atlas_read_data_valid(a0, uv, half, seed_atlas, r->pixels, r->rgba_words,
                                      r->probe_words, p->atlas_cache_info.atlas_words,
                                      &bad_uv, &bad_half, &mask) ||
        r->bad_uv_pixels != bad_uv || r->bad_half_pixels != bad_half ||
        r->uv_equal != !bad_uv || r->half_equal != !bad_half ||
        !r->b0_program_token || r->b0_program_token == r->program_tokens[0] ||
        r->b0_program_token == r->program_tokens[1] ||
        !p->atlas_seed_image || !p->atlas_input_image ||
        !mp_yuv_atlas_uninstrumented_source(a0_fragment) ||
        !b0 || !*b0 || !probe || !*probe || !strcmp(b0, probe) ||
        strstr(b0, "MKS_READ_") || strstr(b0, "mks_read_") ||
        !vertex || !*vertex || !body || !*body ||
        !strstr(a0_fragment, body) || strstr(strstr(a0_fragment, body) + 1, body))
        return false;
    char quad_json[8192], seed_json[16384], current_json[16384];
    size_t quad_len = 0, seed_len = 0, current_len = 0;
    if (!atlas_read_quad_json(p, vertices, &layout, quad_json, sizeof(quad_json), &quad_len) ||
        !atlas_material_json(seed_material, seed_json, sizeof(seed_json), &seed_len) ||
        !atlas_material_json(current_material, current_json, sizeof(current_json), &current_len) ||
        seed_len != current_len || memcmp(seed_json, current_json, seed_len))
        return false;
    uint64_t tail[3] = {(uint64_t)r->width, (uint64_t)r->height, r->generation};
    char signature[65];
    const struct atlas_hash_part parts[] = {
        {seed_json, seed_len}, {a0_fragment, strlen(a0_fragment)},
        {vertex, strlen(vertex)}, {quad_json, quad_len},
        {vertices, layout.byte_size}, {tail, sizeof(tail)},
    };
    if (!atlas_hash(parts, 6, true, signature))
        return false;
    if (p->atlas_ordinal == 1 ? p->atlas_signature[0] :
        !p->atlas_signature[0] || strcmp(p->atlas_signature, signature))
        return false;
    memcpy(p->atlas_signature, signature, sizeof(p->atlas_signature));
    char basename[128], base[256], file[320];
    snprintf(basename, sizeof(basename), "mks-uv-atlas-read-%ld-%"PRIu64"%s",
        (long)getpid(), p->compare_id, mask ? ".diagnostic-invalid" : "");
    snprintf(base, sizeof(base), "/sdcard/Android/data/com.example.media_kit_hdr_lab/files/%s",
             basename);
#define READ_SAVE(suffix, data, bytes) do { \
    snprintf(file, sizeof(file), "%s%s", base, suffix); \
    if (!compare_write_exclusive(file, data, bytes)) return false; \
} while (0)
    READ_SAVE(".a0.f32", a0, r->rgba_words * 4);
    READ_SAVE(".uv-probe.bin", uv, r->probe_words * 4);
    READ_SAVE(".half-probe.bin", half, r->probe_words * 4);
    READ_SAVE(".seed-atlas.bin", seed_atlas, p->atlas_cache_info.atlas_words * 4);
    READ_SAVE(".android.bin", android->words, android->num_words * 4);
    READ_SAVE(".a0.frag.glsl", a0_fragment, strlen(a0_fragment));
    READ_SAVE(".b0.frag.glsl", b0, strlen(b0));
    READ_SAVE(".probe.frag.glsl", probe, strlen(probe));
    READ_SAVE(".vertex.glsl", vertex, strlen(vertex));
    READ_SAVE(".original-body.glsl", body, strlen(body));
    READ_SAVE(".baseline-vertices.bin", vertices, layout.byte_size);
    READ_SAVE(".seed-material.json", seed_json, seed_len);
    READ_SAVE(".current-material.json", current_json, current_len);
    char json[20480];
    size_t used = 0;
#define READ_APPEND(...) do { \
    int n=snprintf(json+used,sizeof(json)-used,__VA_ARGS__); \
    if(n<0 || (size_t)n>=sizeof(json)-used) return false; used+=(size_t)n; \
} while(0)
    READ_APPEND("{\"schema\":10,\"status\":\"%s\",\"scope\":\"%s\",\"source_scope\":\"%s\","
        "\"cache_policy\":\"%s\",\"signature_scope\":\"%s\","
        "\"signature_algorithm\":\"sha256-length-prefix-v1\","
        "\"diagnostic_cache_settings\":{\"skip_caching_single_frame\":true,"
        "\"preserve_mixing_cache\":false},"
        "\"pid\":%ld,\"capture_id\":%"PRIu64",\"generation\":%"PRIu64","
        "\"frame_id\":%"PRIu64",\"frame_token\":%"PRIu64","
        "\"seed_capture_id\":%"PRIu64",\"seed_frame_id\":%"PRIu64","
        "\"seed_frame_token\":%"PRIu64",\"cache_id\":%"PRIu64","
        "\"cache_generation\":%"PRIu64",\"ordinal\":%u,"
        "\"source_pts\":%.9f,\"seed_pts\":%.9f,\"current_pts\":%.9f,\"target_pts\":%.9f,"
        "\"seed_input_image_token\":\"%"PRIu64"\",\"input_image_token\":\"%"PRIu64"\","
        "\"seed_input_texture_token\":\"%"PRIu64"\",\"input_texture_token\":\"%"PRIu64"\","
        "\"renderer_id\":\"%"PRIu64"\",\"renderer_generation\":\"%"PRIu64"\","
        "\"b0_program_token\":\"%"PRIu64"\","
        "\"cache_storage_buffer\":%u,\"cache_storage_offset\":\"%"PRIu64"\","
        "\"cache_storage_size\":\"%"PRIu64"\","
        "\"width\":%d,\"height\":%d,\"pixels\":%zu,\"rgba_words\":%zu,"
        "\"probe_words\":%zu,\"atlas_words\":%zu,"
        "\"byte_order\":\"little\",\"read_format\":6408,\"read_type\":5126,"
        "\"draw_order\":[\"a0\",\"uv\",\"half\"],\"probe_modes\":[%u,%u],"
        "\"draw_ok\":[true,true,true],\"readback_ok\":[true,true,true],"
        "\"baseline_ok\":true,\"fence_signaled\":true,\"signature_match\":true,"
        "\"android_swap_succeeded\":true,\"android_fence_signaled\":true,"
        "\"reject_reason\":0,\"b0_drawn\":false,"
        "\"program_tokens\":[\"%"PRIu64"\",\"%"PRIu64"\",\"%"PRIu64"\"],"
        "\"finish_epoch\":\"%"PRIu64"\","
        "\"draw_epoch_before\":[\"%"PRIu64"\",\"%"PRIu64"\",\"%"PRIu64"\"],"
        "\"draw_epoch_after\":[\"%"PRIu64"\",\"%"PRIu64"\",\"%"PRIu64"\"],"
        "\"readback_epoch_before\":[\"%"PRIu64"\",\"%"PRIu64"\",\"%"PRIu64"\"],"
        "\"readback_epoch_after\":[\"%"PRIu64"\",\"%"PRIu64"\",\"%"PRIu64"\"],"
        "\"uv_equal\":%s,\"half_equal\":%s,"
        "\"bad_uv_pixels\":%zu,\"bad_half_pixels\":%zu,"
        "\"read_validation_mask\":%u,\"read_validation_reasons\":[%s],"
        "\"comparison_pass\":\"%s\","
        "\"android_status\":\"ready\",\"android_roi\":{\"x\":%d,\"y\":%d,\"w\":%d,\"h\":%d},"
        "\"android_words\":%zu,\"vertex_layout\":%s,"
        "\"seed_reference\":{\"manifest\":\"%s\",\"sha256\":\"%s\"},"
        "\"signature\":\"%s\",\"files\":{",
        mask ? "diagnostic-invalid" : "complete",
        "uv-half-atlas-read-capture-only-not-production-or-yuv-pass",
        "B0 is created but not drawn; probe source strip is not uninstrumented B0 numerical proof; YUV equality is unmeasured",
        "diagnostic-only immutable atlas; no rebuild or production cache",
        "archive relation only; not a replay of the native cache key",
        (long)getpid(), p->compare_id, p->compare_generation,
        p->compare_frame_id, p->compare_frame_token,
        p->atlas_seed_id, p->atlas_seed_frame_id, p->atlas_seed_frame_token,
        p->atlas_cache_info.cache_id, p->atlas_cache_info.generation, p->atlas_ordinal,
        p->compare_source_pts, p->atlas_seed_pts, p->compare_current_pts,
        r->ordinal == 1 ? p->compare_target_pts : p->atlas_second_pts,
        p->atlas_seed_image, p->atlas_input_image,
        r->seed_input_texture_token, r->input_texture_token,
        p->atlas_renderer_id, p->atlas_renderer_generation, r->b0_program_token,
        p->atlas_cache_info.storage_buffer, p->atlas_cache_info.storage_offset,
        p->atlas_cache_info.storage_size,
        r->width, r->height, r->pixels, r->rgba_words, r->probe_words,
        p->atlas_cache_info.atlas_words,
        r->probe_modes[0], r->probe_modes[1],
        r->program_tokens[0], r->program_tokens[1], r->program_tokens[2],
        r->finish_epoch,
        r->draw_epoch_before[0], r->draw_epoch_before[1], r->draw_epoch_before[2],
        r->draw_epoch_after[0], r->draw_epoch_after[1], r->draw_epoch_after[2],
        r->readback_epoch_before[0], r->readback_epoch_before[1], r->readback_epoch_before[2],
        r->readback_epoch_after[0], r->readback_epoch_after[1], r->readback_epoch_after[2],
        r->uv_equal ? "true" : "false", r->half_equal ? "true" : "false",
        bad_uv, bad_half, mask,
        mask == 3 ? "\"uv-mismatch\",\"half-mismatch\"" :
        mask == 1 ? "\"uv-mismatch\"" : mask == 2 ? "\"half-mismatch\"" : "",
        mask ? "refused" : "unknown",
        p->compare_x, p->compare_y, p->compare_w, p->compare_h,
        android->num_words, quad_json,
        p->atlas_seed_manifest, p->atlas_seed_sha, signature);
    const char *keys[] = {"a0", "uv", "half", "seed_atlas", "android", "a0_fragment",
        "b0_fragment", "probe_fragment", "vertex", "original_body", "baseline_vertices",
        "seed_material", "current_material"};
    const char *suffixes[] = {".a0.f32", ".uv-probe.bin", ".half-probe.bin", ".seed-atlas.bin",
        ".android.bin", ".a0.frag.glsl", ".b0.frag.glsl", ".probe.frag.glsl", ".vertex.glsl",
        ".original-body.glsl", ".baseline-vertices.bin", ".seed-material.json",
        ".current-material.json"};
    for (int i = 0; i < 13; i++)
        READ_APPEND("%s\"%s\":\"%s%s\"", i ? "," : "", keys[i], basename, suffixes[i]);
    READ_APPEND("}}\n");
#undef READ_APPEND
    snprintf(file, sizeof(file), "%s.json", base);
    if (!compare_write_exclusive(file, json, used))
        return false;
#undef READ_SAVE
    MP_ERR(vo, "MKS-UV-ATLAS-READ event=%s ordinal=%u id=%"PRIu64" generation=%"PRIu64
        " cache_id=%"PRIu64" source_pts=%.9f file=%s.json comparison_pass=%s"
        " read_validation_mask=%u signature=%s\n",
        mask ? "capture-diagnostic-invalid" : "capture-complete", p->atlas_ordinal,
        p->compare_id, p->compare_generation, p->atlas_cache_info.cache_id,
        p->compare_source_pts, base, mask ? "refused" : "unknown", mask, signature);
    return true;
}

static void atlas_read_poll_transaction(struct vo *vo)
{
    struct priv *p = vo->priv;
    if (!p->compare_active || !p->atlas_read || !p->atlas_cache)
        return;
    if (mp_time_ns() > p->compare_deadline) {
        atlas_read_reject(vo, "consumer-read-deadline-expired", 7, false, true);
        return;
    }
    if (!compare_current(p)) {
        compare_fail_transaction(vo, "atlas-read-context-or-identity-mismatch");
        return;
    }
    struct pl_opengl_uv_atlas_read_result result = {0};
    int status = pl_opengl_uv_atlas_read_poll(p->atlas_read, &result);
    if (status == PL_OPENGL_UV_ATLAS_ANCHOR_PENDING)
        return;
    bool failed = status == PL_OPENGL_UV_ATLAS_ANCHOR_FAILED;
    struct mks_yuv_compare_result android = {0};
    int astatus = failed ? MKS_YUV_COMPARE_FAILED :
        mks_android_yuv_compare_poll(p->ra_ctx, &android);
    failed |= astatus == MKS_YUV_COMPARE_FAILED;
    if (failed) {
        atlas_read_reject(vo, "consumer-read-or-android-failed",
                          result.reject_reason ? result.reject_reason : 5, false, true);
        return;
    }
    if (astatus != MKS_YUV_COMPARE_READY || status != PL_OPENGL_UV_ATLAS_ANCHOR_COMPLETE)
        return;
    bool identities = android.capture_id == p->compare_id &&
        android.generation == p->compare_generation && android.roi.x == p->compare_x &&
        android.roi.y == p->compare_y && android.roi.w == p->compare_w &&
        android.roi.h == p->compare_h;
    bool saved = identities && android.swap_succeeded &&
        android.num_words == (size_t)p->compare_w * p->compare_h * 7 &&
        mp_yuv_compare_android_words_valid(android.words, android.num_words,
            (size_t)p->compare_w * p->compare_h, NULL) && atlas_read_save(vo, &result, &android);
    p->compare_active = p->compare_started = p->compare_a_ready = false;
    // The Android side stays READY after a consumed capture and every new arm
    // requires IDLE; recycle the capture so the next ordinal can re-arm.
    mks_android_yuv_compare_cancel(p->ra_ctx);
    if (!saved) {
        atlas_read_reject(vo, "consumer-save-or-android-identity-failed", 8, false, true);
        return;
    }
    if (p->atlas_ordinal == 1) {
        // Host validates the archived stage before writing the ACK property;
        // wait_events watches for it without requiring a new frame.
        atlas_read_ready_event(vo, "promoted-ready");
        p->atlas_phase = MP_ATLAS_WAIT_ACK;
        p->compare_deadline = mp_time_ns() + INT64_C(300000000000);
    } else {
        p->atlas_phase = MP_ATLAS_DONE;
    }
}

// wait_events hook: no GL work. Deadline expiry only latches a flag that the
// next draw/flip poll turns into the ordinary soft rejection with the owner
// restored. The ACK property read is a plain memory load per tick.
static void atlas_read_wait_poll(struct vo *vo)
{
    struct priv *p = vo->priv;
    if (p->atlas_phase != MP_ATLAS_WAIT_ACK && p->atlas_phase != MP_ATLAS_T1_REDRAW &&
        p->atlas_phase != MP_ATLAS_WAIT_T2)
        return;
    if (mp_time_ns() > p->compare_deadline) {
        p->atlas_wait_expired = true;
        return;
    }
    if (p->atlas_phase != MP_ATLAS_WAIT_ACK)
        return;
    char ack[PROP_VALUE_MAX] = {0};
    __system_property_get("debug.mks.atlas_ack", ack);
    if (mp_yuv_atlas_ack_matches(ack, p->atlas_nonce, p->atlas_cache_info.cache_id)) {
        atlas_read_ready_event(vo, "ack-accepted");
        p->atlas_phase = MP_ATLAS_WAIT_T2;
        p->compare_deadline = mp_time_ns() + INT64_C(300000000000);
    }
}

static void atlas_reuse_poll(struct vo *vo)
{
    struct priv *p = vo->priv;
    if (p->atlas_wait_expired) {
        p->atlas_wait_expired = false;
        if (p->atlas_phase != MP_ATLAS_OFF && p->atlas_phase != MP_ATLAS_SEED &&
            p->atlas_phase != MP_ATLAS_DONE && p->atlas_phase != MP_ATLAS_REJECTED)
            atlas_read_reject(vo, "atlas-wait-deadline-expired", 7, false, true);
        return;
    }
    switch (p->atlas_phase) {
    case MP_ATLAS_SEED_PENDING:
        atlas_poll(vo);
        return;
    case MP_ATLAS_T1_PENDING:
    case MP_ATLAS_T2_PENDING:
        atlas_read_poll_transaction(vo);
        return;
    default:
        return;
    }
}

static bool atlas_consumer_render(struct vo *vo, const struct vo_frame *frame,
    const struct pl_frame_mix *mix, const struct pl_frame *target,
    const struct pl_render_params *params, const struct pl_swapchain_frame *swframe,
    const struct ra_yuv_fusion_target *desc)
{
    struct priv *p = vo->priv;
    const struct mp_image *source = mix->frames[0]->user_data;
    bool first = p->atlas_phase == MP_ATLAS_T1_REDRAW;
    // The completed ordinal-1 receipt is archived; the backend refuses to arm
    // a second read while the first still owns the cache slot.
    if (!first && p->atlas_read &&
        !pl_opengl_uv_atlas_read_cancel(&p->atlas_read)) {
        atlas_read_reject(vo, "consumer-previous-read-cancel-failed", 5, false, true);
        return false;
    }
    if (mp_time_ns() > p->compare_deadline) {
        atlas_read_reject(vo, "consumer-wait-deadline-expired", 7, false, true);
        return false;
    }
    if (p->atlas_renderer_id != (uint64_t)(uintptr_t)p->rr || !p->atlas_renderer_generation ||
        !p->atlas_cache || p->atlas_read) {
        atlas_read_reject(vo, "consumer-renderer-or-cache-identity", 6, false, true);
        return false;
    }
    if (first) {
        // vo.h guarantees the redraw carries the seed's media frame; the
        // fusion token and sampling identity must still be re-checked here.
        if (frame->frame_id != p->atlas_seed_frame_id ||
            desc->token <= p->atlas_seed_frame_token ||
            !mp_yuv_compare_target_matches(source->pts, frame->current->pts,
                                           p->atlas_seed_pts)) {
            atlas_read_reject(vo, "consumer-t1-frame-identity", 6, false, true);
            return false;
        }
    } else {
        char ack[PROP_VALUE_MAX] = {0};
        __system_property_get("debug.mks.atlas_ack", ack);
        if (frame->frame_id == p->atlas_seed_frame_id ||
            fabs(source->pts - p->atlas_seed_pts) <= 0.000002 ||
            !mp_yuv_compare_target_matches(source->pts, frame->current->pts,
                                           p->atlas_second_pts) ||
            !mp_yuv_atlas_ack_matches(ack, p->atlas_nonce, p->atlas_cache_info.cache_id)) {
            atlas_read_reject(vo, "consumer-t2-frame-or-ack-identity", 6, false, true);
            return false;
        }
    }
    p->compare_started = p->compare_active = true;
    p->compare_id++;
    p->compare_generation = desc->generation;
    p->compare_frame_id = frame->frame_id;
    p->compare_frame_token = desc->token;
    p->compare_source_pts = source->pts;
    p->compare_current_pts = frame->current->pts;
    p->compare_width = desc->w;
    p->compare_height = desc->h;
    p->compare_w = MPMIN(64, desc->w);
    p->compare_h = MPMIN(64, desc->h);
    p->compare_x = (desc->w - p->compare_w) / 2;
    p->compare_y = (desc->h - p->compare_h) / 2;
    p->atlas_input_image = (uint64_t)(uintptr_t)source;
    p->atlas_ordinal = first ? 1 : 2;
    p->compare_deadline = mp_time_ns() + INT64_C(30000000000);
    pl_tex_destroy(p->gpu, &p->fusion_target);
    p->fusion_target = pl_opengl_wrap(p->gpu, pl_opengl_wrap_params(
        .framebuffer = desc->framebuffer, .width = desc->w, .height = desc->h,
        .iformat = GL_RGBA16F, // logical output; physical YUV drawable
        .output_encoding = PL_OPENGL_OUTPUT_RGB16F_TO_BT2020_FULL_YUV,
    ));
    p->fusion_generation = desc->generation;
    bool android = p->fusion_target &&
        mks_android_yuv_compare_arm(p->ra_ctx, p->compare_id, desc->generation,
            (struct mks_yuv_compare_roi) {p->compare_x, p->compare_y,
                                          p->compare_w, p->compare_h});
    p->atlas_read = android && compare_current(p) ?
        pl_opengl_uv_atlas_read_arm(p->atlas_cache, swframe->fbo, p->fusion_target,
            &(struct pl_opengl_uv_atlas_read_params) {
                .capture_id = p->compare_id, .generation = p->compare_generation,
                .ordinal = p->atlas_ordinal}) : NULL;
    MP_ERR(vo, "MKS-UV-ATLAS-READ event=armed id=%"PRIu64" generation=%"PRIu64
        " ordinal=%u cache_id=%"PRIu64" source_pts=%.9f android=%d lib=%d\n",
        p->compare_id, p->compare_generation, p->atlas_ordinal,
        p->atlas_cache_info.cache_id, p->compare_source_pts, android, !!p->atlas_read);
    if (!p->atlas_read) {
        atlas_read_reject(vo, "consumer-arm-failed", 5, false, true);
        return false;
    }
    p->compare_epoch_valid = compare_current(p) &&
        pl_opengl_error_epoch(p->gpu, &p->compare_error_epoch);
    if (!p->compare_epoch_valid) {
        atlas_read_reject(vo, "consumer-prerender-epoch-failed", 5, false, true);
        return false;
    }
    // The ordinary target and renderer planning are unchanged. The armed
    // reader turns this one final pass into A plus the two probe draws, and
    // the swap-time CSC still presents only original A afterwards.
    bool rendered = pl_render_image_mix(p->rr, mix, target, params);
    if (!compare_current(p)) {
        compare_fail_transaction(vo, "atlas-read-postrender-owner-mismatch");
        return false;
    }
    if (rendered)
        pl_gpu_flush(p->gpu);
    uint64_t after = 0;
    p->compare_a_ready = rendered && compare_baseline_ok(p) &&
        pl_opengl_error_epoch(p->gpu, &after) && after == p->compare_error_epoch;
    if (!p->compare_a_ready) {
        atlas_read_reject(vo, "consumer-render-or-receipt-failed", 5, false, true);
        return false;
    }
    p->atlas_phase = first ? MP_ATLAS_T1_PENDING : MP_ATLAS_T2_PENDING;
    return false;
}
#endif
