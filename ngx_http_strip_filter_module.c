/*
 * Copyright 2008 Evan Miller
 */

/* mod_strip - Remove unnecessary whitespace in HTML */

/* TODO - rewrite with Ragel */

#include <ngx_config.h>
#include <ngx_core.h>
#include <ngx_http.h>

typedef struct {
    ngx_flag_t enable;
} ngx_http_strip_conf_t;

typedef struct {
    unsigned char state;
} ngx_http_strip_ctx_t;

typedef enum { 
    strip_state_text = 0,
    strip_state_text_whitespace,
    strip_state_tag,
    strip_state_tag_name,
    strip_state_tag_whitespace,
    strip_state_tag_attribute_name,
    strip_state_tag_attribute_equals,
    strip_state_tag_attribute_value,
    strip_state_tag_attribute_value_double_quote,
    strip_state_tag_attribute_value_single_quote,
    strip_state_end_tag,
    strip_state_end_tag_name,
    strip_state_comment,
    strip_state_comment_dash,
    strip_state_comment_dash_dash,
    strip_state_tag_bang,
    strip_state_tag_bang_dash,
    strip_state_tag_bang_stuff,
    strip_state_tag_bang_bracket,
    strip_state_tag_bang_bracket_c,
    strip_state_tag_bang_bracket_cd,
    strip_state_tag_bang_bracket_cda,
    strip_state_tag_bang_bracket_cdat,
    strip_state_tag_bang_bracket_cdata,
    strip_state_cdata,
    strip_state_cdata_bracket,
    strip_state_cdata_bracket_bracket,
    strip_state_tag_name_p,
    strip_state_tag_name_pr,
    strip_state_tag_name_pre,
    strip_state_preformatted,
    strip_state_preformatted_angle,
    strip_state_preformatted_angle_slash,
    strip_state_preformatted_angle_slash_p,
    strip_state_preformatted_angle_slash_pr,
    strip_state_preformatted_angle_slash_pre,
    strip_state_tag_name_t,
    strip_state_tag_name_te,
    strip_state_tag_name_tex,
    strip_state_tag_name_text,
    strip_state_tag_name_texta,
    strip_state_tag_name_textar,
    strip_state_tag_name_textare,
    strip_state_tag_name_textarea,
    strip_state_textarea,
    strip_state_textarea_angle,
    strip_state_textarea_angle_slash,
    strip_state_textarea_angle_slash_t,
    strip_state_textarea_angle_slash_te,
    strip_state_textarea_angle_slash_tex,
    strip_state_textarea_angle_slash_text,
    strip_state_textarea_angle_slash_texta,
    strip_state_textarea_angle_slash_textar,
    strip_state_textarea_angle_slash_textare,
    strip_state_textarea_angle_slash_textarea,
    strip_state_abort
} ngx_http_strip_state_e;

static void *ngx_http_strip_create_conf(ngx_conf_t *cf);
static char *ngx_http_strip_merge_conf(ngx_conf_t *cf, void *parent, void *child);
static ngx_int_t ngx_http_strip_filter_init(ngx_conf_t *cf);
static void ngx_http_strip_process_buffer(ngx_buf_t *b, ngx_http_strip_ctx_t *ctx);

static ngx_command_t ngx_http_strip_filter_commands[] = {
    { ngx_string("strip"),
      NGX_HTTP_MAIN_CONF|NGX_HTTP_SRV_CONF|NGX_HTTP_LOC_CONF|NGX_CONF_FLAG,
      ngx_conf_set_flag_slot,
      NGX_HTTP_LOC_CONF_OFFSET,
      offsetof(ngx_http_strip_conf_t, enable),
      NULL },

    ngx_null_command
};

static ngx_http_module_t ngx_http_strip_filter_module_ctx = {
    NULL,                         /* preconfiguration */
    ngx_http_strip_filter_init,   /* postconfiguration */

    NULL,                         /* create main configuration */
    NULL,                         /* init main configuration */

    NULL,                         /* create server configuration */
    NULL,                         /* merge server configuration */

    ngx_http_strip_create_conf,   /* create location configuration */
    ngx_http_strip_merge_conf     /* merge location configuration */
};

ngx_module_t  ngx_http_strip_filter_module = {
    NGX_MODULE_V1,
    &ngx_http_strip_filter_module_ctx,     /* module context */
    ngx_http_strip_filter_commands,        /* module directives */
    NGX_HTTP_MODULE,                       /* module type */
    NULL,                                  /* init master */
    NULL,                                  /* init module */
    NULL,                                  /* init process */
    NULL,                                  /* init thread */
    NULL,                                  /* exit thread */
    NULL,                                  /* exit process */
    NULL,                                  /* exit master */
    NGX_MODULE_V1_PADDING
};

static ngx_http_output_header_filter_pt  ngx_http_next_header_filter;
static ngx_http_output_body_filter_pt    ngx_http_next_body_filter;

static ngx_int_t
ngx_http_strip_header_filter(ngx_http_request_t *r)
{
    ngx_http_strip_conf_t  *conf;
    ngx_http_strip_ctx_t   *ctx;

    conf = ngx_http_get_module_loc_conf(r, ngx_http_strip_filter_module);

    if (!conf->enable
        || (r->headers_out.status != NGX_HTTP_OK
            && r->headers_out.status != NGX_HTTP_FORBIDDEN
            && r->headers_out.status != NGX_HTTP_NOT_FOUND)
        || r->header_only
        || r->headers_out.content_type.len == 0
        || (r->headers_out.content_encoding
            && r->headers_out.content_encoding->value.len))
    {
        return ngx_http_next_header_filter(r);
    }

    if (ngx_strncasecmp(r->headers_out.content_type.data, (u_char *)"text/html", sizeof("text/html" - 1)) != 0) {
        return ngx_http_next_header_filter(r);
    }

    ctx = ngx_pcalloc(r->pool, sizeof(ngx_http_strip_ctx_t));
    if (ctx == NULL) {
        return NGX_ERROR;
    }

    ngx_http_set_ctx(r, ctx, ngx_http_strip_filter_module);

    ngx_http_clear_content_length(r);
    ngx_http_clear_accept_ranges(r);

    r->main_filter_need_in_memory = 1;

    return ngx_http_next_header_filter(r);
}

static ngx_int_t
ngx_http_strip_body_filter(ngx_http_request_t *r, ngx_chain_t *in)
{
    ngx_http_strip_ctx_t *ctx;
    ngx_chain_t          *chain_link;

    ctx = ngx_http_get_module_ctx(r, ngx_http_strip_filter_module);
    if (ctx == NULL) {
        return ngx_http_next_body_filter(r, in);
    }

    for (chain_link = in; chain_link; chain_link = chain_link->next) {
        ngx_http_strip_process_buffer(chain_link->buf, ctx);
    }

    return ngx_http_next_body_filter(r, in);
}

static void
ngx_http_strip_process_buffer(ngx_buf_t *buffer, ngx_http_strip_ctx_t *ctx)
{
    u_char *reader;
    u_char *writer;

    for (writer = buffer->pos, reader = buffer->pos; reader < buffer->last; reader++) {
        switch(ctx->state) {
            case strip_state_abort:
                break;
            case strip_state_text:
                switch(*reader) {
                    case '\r':
                    case '\n':
                    case '\t':
                        continue;
                    case '<':
                        ctx->state = strip_state_tag;
                        break;
                    case ' ':
                        ctx->state = strip_state_text_whitespace;
                        break;
                    default:
                        break;
                }
                break;
            case strip_state_tag:
                switch(*reader) {
                    case '!':
                        ctx->state = strip_state_tag_bang;
                        break;
                    case '/':
                        ctx->state = strip_state_end_tag;
                        break;
                    case 'p':
                    case 'P':
                        ctx->state = strip_state_tag_name_p;
                        break;
                    case 't':
                    case 'T':
                        ctx->state = strip_state_tag_name_t;
                        break;
                    case ' ':
                        break;
                    default:
                        if ((*reader >= 'a' && *reader <= 'z') ||
                                (*reader >= 'A' && *reader <= 'Z')) {
                            ctx->state = strip_state_tag_name;
                        } else {
                            ctx->state = strip_state_abort;
                        }
                        break;
                }
                break;
            case strip_state_tag_name:
                switch(*reader) {
                    case '>':
                        ctx->state = strip_state_text;
                        break;
                    case ' ':
                        ctx->state = strip_state_tag_whitespace;
                        break;
                    default:
                        break;
                }
                break;
            case strip_state_tag_name_t:
                switch (*reader) {
                    case 'e':
                    case 'E':
                        ctx->state = strip_state_tag_name_te;
                        break;
                    case ' ':
                        ctx->state = strip_state_tag_whitespace;
                        break;
                    case '>':
                        ctx->state = strip_state_text;
                    default:
                        ctx->state = strip_state_tag_name;
                        break;
                }
                break;
            case strip_state_tag_name_te:
                switch (*reader) {
                    case 'x':
                    case 'X':
                        ctx->state = strip_state_tag_name_tex;
                        break;
                    case ' ':
                        ctx->state = strip_state_tag_whitespace;
                        break;
                    case '>':
                        ctx->state = strip_state_text;
                    default:
                        ctx->state = strip_state_tag_name;
                        break;
                }
                break;
            case strip_state_tag_name_tex:
                switch (*reader) {
                    case 't':
                    case 'T':
                        ctx->state = strip_state_tag_name_text;
                        break;
                    case ' ':
                        ctx->state = strip_state_tag_whitespace;
                        break;
                    case '>':
                        ctx->state = strip_state_text;
                    default:
                        ctx->state = strip_state_tag_name;
                        break;
                }
                break;
            case strip_state_tag_name_text:
                switch (*reader) {
                    case 'a':
                    case 'A':
                        ctx->state = strip_state_tag_name_texta;
                        break;
                    case ' ':
                        ctx->state = strip_state_tag_whitespace;
                        break;
                    case '>':
                        ctx->state = strip_state_text;
                    default:
                        ctx->state = strip_state_tag_name;
                        break;
                }
                break;
            case strip_state_tag_name_texta:
                switch (*reader) {
                    case 'r':
                    case 'R':
                        ctx->state = strip_state_tag_name_textar;
                        break;
                    case ' ':
                        ctx->state = strip_state_tag_whitespace;
                        break;
                    case '>':
                        ctx->state = strip_state_text;
                    default:
                        ctx->state = strip_state_tag_name;
                        break;
                }
                break;
            case strip_state_tag_name_textar:
                switch (*reader) {
                    case 'e':
                    case 'E':
                        ctx->state = strip_state_tag_name_textare;
                        break;
                    case ' ':
                        ctx->state = strip_state_tag_whitespace;
                        break;
                    case '>':
                        ctx->state = strip_state_text;
                    default:
                        ctx->state = strip_state_tag_name;
                        break;
                }
                break;
            case strip_state_tag_name_textare:
                switch (*reader) {
                    case 'a':
                    case 'A':
                        ctx->state = strip_state_tag_name_textarea;
                        break;
                    case ' ':
                        ctx->state = strip_state_tag_whitespace;
                        break;
                    case '>':
                        ctx->state = strip_state_text;
                    default:
                        ctx->state = strip_state_tag_name;
                        break;
                }
                break;
            case strip_state_tag_name_textarea:
                switch(*reader) {
                    case ' ':
                    case '>':
                        ctx->state = strip_state_textarea;
                        break;
                    default:
                        ctx->state = strip_state_tag_name;
                        break;
                }
                break;
            case strip_state_textarea:
                if (*reader == '<') {
                    ctx->state = strip_state_textarea_angle;
                }
                break;
            case strip_state_textarea_angle:
                ctx->state = (*reader == '/') ? strip_state_textarea_angle_slash : strip_state_textarea;
                break;
            case strip_state_textarea_angle_slash:
                ctx->state = (*reader == 't' || *reader == 'T') ? strip_state_textarea_angle_slash_t : strip_state_textarea;
                break;
            case strip_state_textarea_angle_slash_t:
                ctx->state = (*reader == 'e' || *reader == 'E') ? strip_state_textarea_angle_slash_te : strip_state_textarea;
                break;
            case strip_state_textarea_angle_slash_te:
                ctx->state = (*reader == 'x' || *reader == 'X') ? strip_state_textarea_angle_slash_tex : strip_state_textarea;
                break;
            case strip_state_textarea_angle_slash_tex:
                ctx->state = (*reader == 't' || *reader == 'T') ? strip_state_textarea_angle_slash_text : strip_state_textarea;
                break;
            case strip_state_textarea_angle_slash_text:
                ctx->state = (*reader == 'a' || *reader == 'A') ? strip_state_textarea_angle_slash_texta : strip_state_textarea;
                break;
            case strip_state_textarea_angle_slash_texta:
                ctx->state = (*reader == 'r' || *reader == 'R') ? strip_state_textarea_angle_slash_textar : strip_state_textarea;
                break;
            case strip_state_textarea_angle_slash_textar:
                ctx->state = (*reader == 'e' || *reader == 'E') ? strip_state_textarea_angle_slash_textare : strip_state_textarea;
                break;
            case strip_state_textarea_angle_slash_textare:
                ctx->state = (*reader == 'a' || *reader == 'A') ? strip_state_textarea_angle_slash_textarea : strip_state_textarea;
                break;
            case strip_state_textarea_angle_slash_textarea:
                ctx->state = (*reader == '>') ? strip_state_text : strip_state_textarea;
                break;
            case strip_state_tag_name_p:
                switch(*reader) {
                    case 'r':
                    case 'R':
                        ctx->state = strip_state_tag_name_pr;
                        break;
                    case ' ':
                        ctx->state = strip_state_tag_whitespace;
                        break;
                    case '>':
                        ctx->state = strip_state_text;
                        break;
                    default:
                        ctx->state = strip_state_tag_name;
                        break;
                }
                break;
            case strip_state_tag_name_pr:
                switch(*reader) {
                    case 'e':
                    case 'E':
                        ctx->state = strip_state_tag_name_pre;
                        break;
                    case ' ':
                        ctx->state = strip_state_tag_whitespace;
                        break;
                    case '>':
                        ctx->state = strip_state_text;
                        break;
                    default:
                        ctx->state = strip_state_tag_name;
                        break;
                }
                break;
            case strip_state_tag_name_pre:
                switch(*reader) {
                    case ' ':
                    case '>':
                        ctx->state = strip_state_preformatted;
                        break;
                    default:
                        ctx->state = strip_state_tag_name;
                        break;
                }
                break;
            case strip_state_preformatted:
                if (*reader == '<') {
                    ctx->state = strip_state_preformatted_angle;
                }
                break;
            case strip_state_preformatted_angle:
                ctx->state = (*reader == '/') ? strip_state_preformatted_angle_slash : strip_state_preformatted;
                break;
            case strip_state_preformatted_angle_slash:
                ctx->state = (*reader == 'p' || *reader == 'P') ? strip_state_preformatted_angle_slash_p : strip_state_preformatted;
                break;
            case strip_state_preformatted_angle_slash_p:
                ctx->state = (*reader == 'r' || *reader == 'R') ? strip_state_preformatted_angle_slash_pr : strip_state_preformatted;
                break;
            case strip_state_preformatted_angle_slash_pr:
                ctx->state = (*reader == 'e' || *reader == 'E') ? strip_state_preformatted_angle_slash_pre : strip_state_preformatted;
                break;
            case strip_state_preformatted_angle_slash_pre:
                ctx->state = (*reader == '>') ? strip_state_text : strip_state_preformatted;
                break;
            case strip_state_tag_bang:
                switch(*reader) {
                    case '-':
                        ctx->state = strip_state_tag_bang_dash;
                        break;
                    case '[':
                        ctx->state = strip_state_tag_bang_bracket;
                    default:
                        ctx->state = strip_state_tag_bang_stuff;
                        break;
                }
                break;
            case strip_state_tag_bang_bracket:
                ctx->state = (*reader == 'C') ? strip_state_tag_bang_bracket_c : strip_state_tag_bang_stuff;
                break;
            case strip_state_tag_bang_bracket_c:
                ctx->state = (*reader == 'D') ? strip_state_tag_bang_bracket_cd : strip_state_tag_bang_stuff;
                break;
            case strip_state_tag_bang_bracket_cd:
                ctx->state = (*reader == 'A') ? strip_state_tag_bang_bracket_cda : strip_state_tag_bang_stuff;
                break;
            case strip_state_tag_bang_bracket_cda:
                ctx->state = (*reader == 'T') ? strip_state_tag_bang_bracket_cdat : strip_state_tag_bang_stuff;
                break;
            case strip_state_tag_bang_bracket_cdat:
                ctx->state = (*reader == 'A') ? strip_state_tag_bang_bracket_cdata : strip_state_tag_bang_stuff;
                break;
            case strip_state_tag_bang_bracket_cdata:
                ctx->state = (*reader == '[') ? strip_state_cdata : strip_state_tag_bang_stuff;
                break;
            case strip_state_cdata:
                ctx->state = (*reader == ']') ? strip_state_cdata_bracket : strip_state_cdata;
                break;
            case strip_state_cdata_bracket:
                ctx->state = (*reader == ']') ? strip_state_cdata_bracket_bracket : strip_state_cdata;
                break;
            case strip_state_cdata_bracket_bracket:
                ctx->state = (*reader = '>') ? strip_state_text : strip_state_cdata;
                break;
            case strip_state_tag_bang_dash:
                switch(*reader) {
                    case '-':
                        ctx->state = strip_state_comment;
                        break;
                    default:
                        ctx->state = strip_state_tag_bang_stuff;
                        break;
                }
                break;
            case strip_state_tag_bang_stuff:
                switch(*reader) {
                    case '>':
                        ctx->state = strip_state_text;
                        break;
                    default:
                        break;
                }
                break;
            case strip_state_comment:
                switch(*reader) {
                    case '-':
                        ctx->state = strip_state_comment_dash;
                        break;
                    default:
                        break;
                }
                break;
            case strip_state_comment_dash:
                switch(*reader) {
                    case '-':
                        ctx->state = strip_state_comment_dash_dash;
                        break;
                    default:
                        ctx->state = strip_state_comment;
                        break;
                }
                break;
            case strip_state_comment_dash_dash:
                switch(*reader) {
                    case '>':
                        ctx->state = strip_state_text;
                        break;
                    case '-':
                        ctx->state = strip_state_comment_dash_dash;
                        break;
                    default:
                        ctx->state = strip_state_comment;
                        break;
                }
                break;
            case strip_state_end_tag:
                switch(*reader) {
                    case ' ':
                        continue;
                    default:
                        ctx->state = strip_state_end_tag_name;
                        break;
                }
                break;
            case strip_state_end_tag_name:
                switch(*reader) {
                    case '>':
                        ctx->state = strip_state_text;
                        break;
                    default:
                        break;
                }
                break;
            case strip_state_tag_whitespace:
                switch(*reader) {
                    case ' ':
                        continue;
                    case '>':
                        ctx->state = strip_state_text;
                        break;
                    default:
                        ctx->state = strip_state_tag_attribute_name;
                        break;
                }
                break;
            case strip_state_tag_attribute_name:
                switch(*reader) {
                    case '=':
                        ctx->state = strip_state_tag_attribute_equals;
                        break;
                    case '>':
                        ctx->state = strip_state_text;
                        break;
                    case ' ':
                        ctx->state = strip_state_tag_whitespace;
                        break;
                    default:
                        break;
                }
                break;
            case strip_state_tag_attribute_equals:
                switch(*reader) {
                    case '"':
                        ctx->state = strip_state_tag_attribute_value_double_quote;
                        break;
                    case '\'':
                        ctx->state = strip_state_tag_attribute_value_single_quote;
                        break;
                    default:
                        ctx->state = strip_state_tag_attribute_value;
                        break;
                }
                break;
            case strip_state_tag_attribute_value:
                switch(*reader) {
                    case '>':
                        ctx->state = strip_state_text;
                        break;
                    case ' ':
                        ctx->state = strip_state_tag_whitespace;
                        break;
                    default:
                        break;
                }
                break;
            case strip_state_tag_attribute_value_double_quote:
                switch(*reader) {
                    case '"':
                        ctx->state = strip_state_tag_attribute_value;
                        break;
                    default:
                        break;
                }
                break;
            case strip_state_tag_attribute_value_single_quote:
                switch(*reader) {
                    case '\'':
                        ctx->state = strip_state_tag_attribute_value;
                        break;
                    default:
                        break;
                }
                break;
            case strip_state_text_whitespace:
                switch(*reader) {
                    case '\r':
                    case '\n':
                    case '\t':
                    case ' ':
                        continue; // <-- here it is, mod_strip's raison d'etre
                    case '<':
                        ctx->state = strip_state_tag;
                        break;
                    default:
                        ctx->state = strip_state_text;
                        break;
                }
            default:
                break;
        }
        *writer++ = *reader;
    }
    buffer->last = writer;
}

static ngx_int_t
ngx_http_strip_filter_init(ngx_conf_t *cf)
{
    ngx_http_next_header_filter = ngx_http_top_header_filter;
    ngx_http_top_header_filter = ngx_http_strip_header_filter;

    ngx_http_next_body_filter = ngx_http_top_body_filter;
    ngx_http_top_body_filter = ngx_http_strip_body_filter;

    return NGX_OK;
}

static void *
ngx_http_strip_create_conf(ngx_conf_t *cf) {
    ngx_http_strip_conf_t *conf;

    conf = ngx_pcalloc(cf->pool, sizeof(ngx_http_strip_conf_t));
    if (conf == NULL) {
        return NGX_CONF_ERROR;
    }

    conf->enable = NGX_CONF_UNSET;

    return conf;
}

static char *
ngx_http_strip_merge_conf(ngx_conf_t *cf, void *parent, void *child) {
    ngx_http_strip_conf_t *prev = parent;
    ngx_http_strip_conf_t *conf = child;

    ngx_conf_merge_value(conf->enable, prev->enable, 0);

    return NGX_CONF_OK;
}
