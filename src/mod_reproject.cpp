/*
 * mod_reproject.cpp
 * An AHTSE tile to tile conversion module, should do most of the functionality required by a WMS server
 * Uses a 3-4 paramter rest tile service as a data source
 *
 * (C) Lucian Plesea 2016
 */

// TODO: Test
// TODO: Handle ETag conditional requests
// TODO: Implement GCS to and from WM.
// TODO: Add LERC support
// TODO: Allow overlap between tiles

#include "mod_reproject.h"
#include <cmath>
#include <clocale>
#include <vector>
#include <cctype>

extern module AP_MODULE_DECLARE_DATA reproject_module;

#if defined(APLOG_USE_MODULE)
APLOG_USE_MODULE(reproject);
#endif

// From mod_receive
#include <receive_context.h>

using namespace std;

// Rather than use the _USE_MATH_DEFINES, just calculate pi once, C++ style
const static double pi = acos(-1.0);

#define USER_AGENT "AHTSE Reproject"

// Given a data type name, returns a data type
static GDALDataType GetDT(const char *name) {
    if (name == NULL) return GDT_Byte;
    if (!apr_strnatcasecmp(name, "UINT16"))
        return GDT_UInt16;
    else if (!apr_strnatcasecmp(name, "INT16") || !apr_strnatcasecmp(name, "INT"))
        return GDT_Int16;
    else if (!apr_strnatcasecmp(name, "UINT32"))
        return GDT_UInt32;
    else if (!apr_strnatcasecmp(name, "INT32") || !apr_strnatcasecmp(name, "INT"))
        return GDT_Int32;
    else if (!apr_strnatcasecmp(name, "FLOAT32") || !apr_strnatcasecmp(name, "FLOAT"))
        return GDT_Float32;
    else if (!apr_strnatcasecmp(name, "FLOAT64") || !apr_strnatcasecmp(name, "DOUBLE"))
        return GDT_Float64;
    else
        return GDT_Byte;
}

static int send_image(request_rec *r, const storage_manager &src, const char *mime_type = NULL)
{
    if (mime_type)
        ap_set_content_type(r, mime_type);
    else
        switch (hton32(*src.buffer)) {
        case JPEG_SIG:
            ap_set_content_type(r, "image/jpeg");
            break;
        case PNG_SIG:
            ap_set_content_type(r, "image/png");
            break;
        default: // LERC goes here too
            ap_set_content_type(r, "application/octet-stream");
    }
    // Is it gzipped content?
    if (GZIP_SIG == hton32(*src.buffer))
        apr_table_setn(r->headers_out, "Content-Encoding", "gzip");

    // TODO: Set headers, as chosen by user
    ap_set_content_length(r, src.size);
    ap_rwrite(src.buffer, src.size, r);
    return OK;
}

// Returns NULL if it worked as expected, returns a four integer value from 
// "x y", "x y z" or "x y z c"
static const char *get_xyzc_size(struct sz *size, const char *value) {
    char *s;
    if (!value)
        return " values missing";
    size->x = apr_strtoi64(value, &s, 0);
    size->y = apr_strtoi64(s, &s, 0);
    size->c = 3;
    size->z = 1;
    if (errno == 0 && *s != 0) {
        // Read optional third and fourth integers
        size->z = apr_strtoi64(s, &s, 0);
        if (*s != 0)
            size->c = apr_strtoi64(s, &s, 0);
    }
    if (errno != 0 || *s != 0) {
        // Raster size is 4 params max
        return " incorrect format";
    }
    return NULL;
}

// Converts a 64bit value into 13 trigesimal chars
static void uint64tobase32(apr_uint64_t value, char *buffer, int flag = 0) {
    static char b32digits[] = "0123456789abcdefghijklmnopqrstuv";
    // From the bottom up
    buffer[13] = 0; // End of string marker
    for (int i = 0; i < 13; i++, value >>= 5)
        buffer[12 - i] = b32digits[value & 0x1f];
    buffer[0] |= flag << 4; // empty flag goes in top bit
}

// Return the value from a base 32 character
// Returns a negative value if char is not a valid base32 char
// ASCII only
static int b32(unsigned char c) {
    if (c < '0') return -1;
    if (c - '0' < 10) return c - '0';
    if (c < 'A') return -1;
    if (c - 'A' < 22) return c - 'A' + 10;
    if (c < 'a') return -1;
    if (c - 'a' < 22) return c - 'a' + 10;
    return -1;
}

static apr_uint64_t base32decode(unsigned char *s, int *flag) {
    apr_int64_t value = 0;
    while (*s == '"') s++; // Skip initial quotes
    *flag = (b32(*s) >> 4 ) & 1; // Pick up the flag from bit 5
    for (int v = b32(*s++) & 0xf; v >= 0; v = b32(*s++))
        value = (value << 5) + v;
    return value;
}

static void *create_dir_config(apr_pool_t *p, char *path)
{
    repro_conf *c = (repro_conf *)apr_pcalloc(p, sizeof(repro_conf));
    c->doc_path = path;
    return c;
}

// Returns a table read from a file, or NULL and an error message
static apr_table_t *read_pKVP_from_file(apr_pool_t *pool, const char *fname, char **err_message)

{
    *err_message = NULL;
    ap_configfile_t *cfg_file;
    apr_status_t s = ap_pcfg_openfile(&cfg_file, pool, fname);

    if (APR_SUCCESS != s) { // %pm means print status error string
        *err_message = apr_psprintf(pool, " %s - %pm", fname, &s);
        return NULL;
    }

    char buffer[MAX_STRING_LEN];
    apr_table_t *table = apr_table_make(pool, 8);
    // This can return ENOSPC if lines are too long
    while (APR_SUCCESS == (s = ap_cfg_getline(buffer, MAX_STRING_LEN, cfg_file))) {
        if ((strlen(buffer) == 0) || buffer[0] == '#')
            continue;
        const char *value = buffer;
        char *key = ap_getword_white(pool, &value);
        apr_table_add(table, key, value);
    }

    ap_cfg_closefile(cfg_file);
    if (s == APR_ENOSPC) {
        *err_message = apr_psprintf(pool, "maximum line length of %d exceeded", MAX_STRING_LEN);
        return NULL;
    }

    return table;
}

static void init_rsets(apr_pool_t *p, struct TiledRaster &raster)
{
    // Clean up pagesize defaults
    raster.pagesize.c = raster.size.c;
    raster.pagesize.z = 1;

    struct rset level;
    level.width = int(1 + (raster.size.x - 1) / raster.pagesize.x);
    level.height = int(1 + (raster.size.y - 1) / raster.pagesize.y);
    level.rx = (raster.bbox.xmax - raster.bbox.xmin) / raster.size.x;
    level.ry = (raster.bbox.ymax - raster.bbox.ymin) / raster.size.y;

    // How many levels do we have
    raster.n_levels = 2 + ilogb(max(level.height, level.width) - 1);
    raster.rsets = (struct rset *)apr_pcalloc(p, sizeof(rset) * raster.n_levels);

    // Populate rsets from the bottom, the way tile protcols count levels
    // These are MRF rsets, not all of them are visible
    struct rset *r = raster.rsets + raster.n_levels - 1;
    for (int i = 0; i < raster.n_levels; i++) {
        *r-- = level;
        // Prepare for the next level, assuming powers of two
        level.width = 1 + (level.width - 1) / 2;
        level.height = 1 + (level.height - 1) / 2;
        level.rx *= 2;
        level.ry *= 2;
    }

    // MRF has one tile at the top
    ap_assert(raster.rsets[0].height == 1 && raster.rsets[0].width == 1);
    ap_assert(raster.n_levels > raster.skip);
}

// Temporary switch locale to C, get four comma separated numbers in a bounding box, WMS style
static const char *getbbox(const char *line, bbox_t *bbox)
{
    const char *lcl = setlocale(LC_NUMERIC, NULL);
    const char *message = " format incorrect, expects four comma separated C locale numbers";
    char *l;
    setlocale(LC_NUMERIC, "C");

    do {
        bbox->xmin = strtod(line, &l); if (*l++ != ',') break;
        bbox->ymin = strtod(l, &l);    if (*l++ != ',') break;
        bbox->xmax = strtod(l, &l);    if (*l++ != ',') break;
        bbox->ymax = strtod(l, &l);
        message = NULL;
    } while (false);

    setlocale(LC_NUMERIC, lcl);
    return message;
}

static const char *ConfigRaster(apr_pool_t *p, apr_table_t *kvp, struct TiledRaster &raster)
{
    const char *line;
    line = apr_table_get(kvp, "Size");
    if (!line)
        return "Size directive is mandatory";
    const char *err_message;
    err_message = get_xyzc_size(&(raster.size), line);
    if (err_message) return apr_pstrcat(p, "Size", err_message, NULL);
    // Optional page size, defaults to 512x512
    raster.pagesize.x = raster.pagesize.y = 512;
    line = apr_table_get(kvp, "PageSize");
    if (line) {
        err_message = get_xyzc_size(&(raster.pagesize), line);
        if (err_message) return apr_pstrcat(p, "PageSize", err_message, NULL);
    }

    // Optional data type, defaults to unsigned byte
    raster.datatype = GetDT(apr_table_get(kvp, "DataType"));

    line = apr_table_get(kvp, "SkippedLevels");
    if (line)
        raster.skip = int(apr_atoi64(line));

    // Default projection is WM, meaning web mercator
    line = apr_table_get(kvp, "Projection");
    raster.projection = line ? apr_pstrdup(p, line) : "WM";

    // Bounding box: minx, miny, maxx, maxy
    raster.bbox.xmin = raster.bbox.ymin = 0.0;
    raster.bbox.xmax = raster.bbox.ymax = 1.0;
    line = apr_table_get(kvp, "BoundingBox");
    if (line)
        err_message = getbbox(line, &raster.bbox);
    if (err_message)
        return apr_pstrcat(p, "BoundingBox", err_message, NULL);

    init_rsets(p, raster);

    return NULL;
}

static char *read_empty_tile(cmd_parms *cmd, repro_conf *c, const char *line)
{
    // If we're provided a file name or a size, pre-read the empty tile in the 
    apr_file_t *efile;
    apr_off_t offset = 0;
    apr_status_t stat;
    char *last;

    c->empty.size = static_cast<int>(apr_strtoi64(line, &last, 0));
    // Might be an offset, or offset then file name
    if (last != line)
        apr_strtoff(&(offset), last, &last, 0);
    
    while (*last && isblank(*last)) last++;
    const char *efname = last;

    // Use the temp pool for the file open, it will close it for us
    if (!c->empty.size) { // Don't know the size, get it from the file
        apr_finfo_t finfo;
        stat = apr_stat(&finfo, efname, APR_FINFO_CSIZE, cmd->temp_pool);
        if (APR_SUCCESS != stat)
            return apr_psprintf(cmd->pool, "Can't stat %s %pm", efname, stat);
        c->empty.size = static_cast<int>(finfo.csize);
    }
    stat = apr_file_open(&efile, efname, READ_RIGHTS, 0, cmd->temp_pool);
    if (APR_SUCCESS != stat)
        return apr_psprintf(cmd->pool, "Can't open empty file %s, %pm", efname, stat);
    c->empty.buffer = static_cast<char *>(apr_palloc(cmd->pool, (apr_size_t)c->empty.size));
    stat = apr_file_seek(efile, APR_SET, &offset);
    if (APR_SUCCESS != stat)
        return apr_psprintf(cmd->pool, "Can't seek empty tile %s: %pm", efname, stat);
    apr_size_t size = (apr_size_t)c->empty.size;
    stat = apr_file_read(efile, c->empty.buffer, &size);
    if (APR_SUCCESS != stat)
        return apr_psprintf(cmd->pool, "Can't read from %s: %pm", efname, stat);
    apr_file_close(efile);
    return NULL;
}

// Allow for one or more RegExp guard
// One of them has to match if the request is to be considered
static const char *set_regexp(cmd_parms *cmd, repro_conf *c, const char *pattern)
{
    char *err_message = NULL;
    if (c->regexp == 0)
        c->regexp = apr_array_make(cmd->pool, 2, sizeof(ap_regex_t));
    ap_regex_t *m = (ap_regex_t *)apr_array_push(c->regexp);
    int error = ap_regcomp(m, pattern, 0);
    if (error) {
        int msize = 2048;
        err_message = (char *)apr_pcalloc(cmd->pool, msize);
        ap_regerror(error, m, err_message, msize);
        return apr_pstrcat(cmd->pool, "Reproject Regexp incorrect ", err_message, NULL);
    }
    return NULL;
}

// Is the projection GCS
static bool is_gcs(const char *projection) {
    return !apr_strnatcasecmp(projection, "GCS")
        || !apr_strnatcasecmp(projection, "EPSG:4326");
}

// Is the projection spherical mercator, include the Pseudo Mercator code
static bool is_wm(const char *projection) {
    return !apr_strnatcasecmp(projection, "WM")
        || !apr_strnatcasecmp(projection, "EPSG:3857")
        || !apr_strnatcasecmp(projection, "EPSG:3785");
}

// Is the projection WGS84 based mercator
static bool is_mercator(const char *projection) {
    return !apr_strnatcasecmp(projection, "Mercator")
        || !apr_strnatcasecmp(projection, "EPSG:3395");
}

// If projection is the same, the transformation is an affine scaling
#define IS_AFFINE_SCALING(cfg) (!apr_strnatcasecmp(cfg->inraster.projection, cfg->raster.projection))
#define IS_GCS2WM(cfg) (is_gcs(cfg->inraster.projection) && is_wm(cfg->raster.projection))
#define IS_WM2GCS(cfg) (is_wm(cfg->inraster.projection) && is_gcs(cfg->raster.projection))


//
// Tokenize a string into an array
//  
static apr_array_header_t* tokenize(apr_pool_t *p, const char *s, char sep = '/')
{
    apr_array_header_t* arr = apr_array_make(p, 10, sizeof(char *));
    while (sep == *s) s++;
    char *val;
    while (*s && (val = ap_getword(p, &s, sep))) {
        char **newelt = (char **)apr_array_push(arr);
        *newelt = val;
    }
    return arr;
}

static int etag_matches(request_rec *r, const char *ETag) {
    const char *ETagIn = apr_table_get(r->headers_in, "If-None-Match");
    return ETagIn != 0 && strstr(ETagIn, ETag);
}

// Returns the empty tile if defined
static int send_empty_tile(request_rec *r) {
    repro_conf *cfg = (repro_conf *)ap_get_module_config(r->per_dir_config, &reproject_module);
    if (etag_matches(r, cfg->eETag)) {
        apr_table_setn(r->headers_out, "ETag", cfg->eETag);
        return HTTP_NOT_MODIFIED;
    }

    if (!cfg->empty.buffer) return DECLINED;
    return send_image(r, cfg->empty);
}

// Returns a bad request error if condition is met
#define REQ_ERR_IF(X) if (X) {\
    return HTTP_BAD_REQUEST; \
}

// If the condition is met, sends the message to the error log and returns HTTP INTERNAL ERROR
#define SERR_IF(X, msg) if (X) { \
    ap_log_error(APLOG_MARK, APLOG_ERR, 0, r->server, msg);\
    return HTTP_INTERNAL_SERVER_ERROR; \
}

// Pick an input level based on desired output resolution
// TODO: Consider the Y resolution too
static int input_level(const TiledRaster &raster, double res, int over) {
    // The raster levels are in increasing resolution order, test until 
    for (int choice = 0; choice < raster.n_levels; choice++) {
        double cres = raster.rsets[choice].rx;
        cres -= cres / raster.pagesize.x / 2; // Add half pixel worth to avoid jitter noise
        if (cres < res) { // This is the best choice, we will return
            if (over) choice -= 1; // Use the lower resolution if oversampling
            if (choice < raster.skip)
                return raster.skip;
            return choice;
        }
    }
    // Use the highest resolution level
    return raster.n_levels -1;
}

// From a tile location, generate a bounding box of a raster
static void tile_to_bbox(const TiledRaster &raster, const sz *tile, bbox_t &bb) {
    double rx = raster.rsets[tile->l].rx;
    double ry = raster.rsets[tile->l].ry;

    // Compute the top left
    bb.xmin = raster.bbox.xmin + tile->x * rx * raster.pagesize.x;
    bb.ymax = raster.bbox.ymax - tile->y * ry * raster.pagesize.y;
    // Adjust for the bottom right
    bb.xmax = bb.xmin + rx * raster.pagesize.x;
    bb.ymin = bb.ymax - ry * raster.pagesize.y;
}

// From a bounding box, calculate the top-left and bottom-right tiles of a specific level of a raster
// Input level is absolute, the one set in output tiles is relative
static void bbox_to_tile(const TiledRaster &raster, int level, const bbox_t &bb, sz *tl_tile, sz *br_tile) {
    double rx = raster.rsets[level].rx;
    double ry = raster.rsets[level].ry;
    double x = (bb.xmin - raster.bbox.xmin) / (rx * raster.pagesize.x);
    double y = (raster.bbox.ymax - bb.ymax) / (ry * raster.pagesize.y);

    // Truncate is fine for these two, after adding quarter pixel to eliminate jitter
    // X and Y are in pages, so a pixel is 1/pagesize
    tl_tile->x = int(x + 0.25 / raster.pagesize.x);
    tl_tile->y = int(y + 0.25 / raster.pagesize.y);

    x = (bb.xmax - raster.bbox.xmin) / (rx * raster.pagesize.x);
    y = (raster.bbox.ymax - bb.ymin) / (ry * raster.pagesize.y);

    // Pad these quarter pixel to avoid jitter
    br_tile->x = int(x + 0.25 / raster.pagesize.x);
    br_tile->y = int(y + 0.25 / raster.pagesize.y);
    // Use a tile only if we get more than half pixel in
    if (x - br_tile->x > 0.5 / raster.pagesize.x) br_tile->x++;
    if (y - br_tile->y > 0.5 / raster.pagesize.y) br_tile->y++;
}


// Fetches and decodes all tiles between tl and br, writes output in buffer
// aligned as a single raster
// Returns APR_SUCCESS if everything is fine, otherwise an HTTP error code

static apr_status_t retrieve_source(request_rec *r, const  sz &tl, const sz &br, void **buffer)
{
    repro_conf *cfg = (repro_conf *)ap_get_module_config(r->per_dir_config, &reproject_module);
    const char *error_message;

    int ntiles = int((br.x - tl.x) * (br.y - tl.y));
    // Should have a reasonable number of input tiles, 64 is a good figure
    SERR_IF(ntiles > 64, "Too many input tiles required, maximum is 64");

    // Allocate a buffer for receiving responses
    receive_ctx rctx;
    rctx.maxsize = cfg->max_input_size;
    rctx.buffer = (char *)apr_palloc(r->pool, rctx.maxsize);

    ap_filter_t *rf = ap_add_output_filter("Receive", &rctx, r, r->connection);

    codec_params params;
    int pixel_size = DT_SIZE(cfg->inraster.datatype);

    // inraster->pagesize.c has to be set correctly
    int input_line_width = int(cfg->inraster.pagesize.x * cfg->inraster.pagesize.c * pixel_size);
    int pagesize = int(input_line_width * cfg->inraster.pagesize.y);

    params.line_stride = int((br.x - tl.x) * input_line_width);

    apr_size_t bufsize = pagesize * ntiles;
    if (*buffer == NULL) // Allocate the buffer if not provided, filled with zeros
        *buffer = apr_pcalloc(r->pool, bufsize);


    // Retrieve every required tile and decompress it in the right place
    for (int y = int(tl.y); y < br.y; y++) for (int x = int(tl.x); x < br.x; x++) {
        char *sub_uri = apr_pstrcat(r->pool,
            (tl.z == 0) ?
            apr_psprintf(r->pool, "%s/%d/%d/%d", cfg->source, int(tl.l), y, x) :
            apr_psprintf(r->pool, "%s/%d/%d/%d/%d", cfg->source, int(tl.z), int(tl.l), y, x),
            cfg->postfix, NULL);

        request_rec *rr = ap_sub_req_lookup_uri(sub_uri, r, r->output_filters);

        // Location of first byte of this input tile
        void *b = (char *)(*buffer) + pagesize * (y - tl.y) * (br.x - tl.x) 
                + input_line_width * (x - tl.x);

        // Set up user agent signature, prepend the info
        const char *user_agent = apr_table_get(r->headers_in, "User-Agent");
        user_agent = user_agent == NULL ? USER_AGENT :
            apr_pstrcat(r->pool, USER_AGENT ", ", user_agent, NULL);
        apr_table_setn(rr->headers_in, "User-Agent", user_agent);

        rctx.size = 0; // Reset the receive size
        int rr_status = ap_run_sub_req(rr);
        if (rr_status != APR_SUCCESS) {
            ap_remove_output_filter(rf);
            ap_log_rerror(APLOG_MARK, APLOG_ERR, rr_status, r, "Receive failed for %s", sub_uri);
            return rr_status; // Pass status along
        }

	storage_manager src = { rctx.buffer, rctx.size };
        apr_uint32_t sig;
        memcpy(&sig, rctx.buffer, sizeof(sig));

        switch (hton32(sig))
        {
        case JPEG_SIG:
            error_message = jpeg_stride_decode(params, cfg->inraster, src, b);
            break;
        case PNG_SIG:
            error_message = png_stride_decode(params, cfg->inraster, src, b);
            break;
        default:
            error_message = "Unsupported format received";
        }

        if (error_message != NULL) { // Something went wrong
            ap_log_rerror(APLOG_MARK, APLOG_ERR, 0, r, "%s :%s", error_message, sub_uri);
            return HTTP_NOT_FOUND;
        }
    }

    ap_remove_output_filter(rf);
    apr_table_clear(r->headers_out); // Clean up the headers set by subrequests

    return APR_SUCCESS;
}

// Interpolation line, contains the above line and the relative weight (never zero)
// These don't have to be bit fields, but it keeps them smaller
// w is weigth of next line *256, can be 0 but not 256.
// line is the higher line to be interpolated, always positive
struct iline {
    unsigned int w:8, line:24;
};

// Offset should be Out - In, center of first pixels, real world coordinates
// If this is negative, we got trouble?
static void init_ilines(double delta_in, double delta_out, double offset, iline *itable, int lines)
{
    for (int i = 0; i < lines; i++) {
        double pos = (offset + i * delta_out) / delta_in;
        // The high line
        itable[i].line = static_cast<int>(ceil(pos));
        // Weight of high line, under 256
        itable[i].w = static_cast<int>(floor(256.0 * (pos - floor(pos))));
    }
}

// Adjust an interpolation table to avoid addressing unavailable lines
// Max available is the max available line
static void adjust_itable(iline *table, int n, unsigned int max_avail) {
    // Adjust the end first
    while (n && table[--n].line > max_avail) {
        table[n].line = max_avail;
        table[n].w = 255; // Mostly the last available line
    }
    for (int i = 0; i < n && table[i].line <= 0; i++) {
        table[i].line = 1;
        table[i].w = 0; // Use line zero value
    }
}

// An 2D buffer
struct interpolation_buffer {
    void *buffer;       // Location of first value per line
    sz size;            // Describes the organization of the buffer
};


//
// Perform the actual interpolation using ilines, working type WT
//
template<typename T = apr_byte_t, typename WT = apr_int32_t> static void interpolate(
    const interpolation_buffer &src, interpolation_buffer &dst,
    iline *h = NULL, iline *v = NULL)
{
    ap_assert(src.size.c == dst.size.c); // Same number of colors
    T *data = reinterpret_cast<T *>(dst.buffer);
    T *s = reinterpret_cast<T *>(src.buffer);
    const int colors = static_cast<int>(dst.size.c);
    const int slw = static_cast<int>(src.size.x * colors);    // Source line size in pixels
    if (1 == colors) { // single band optimization
        for (int y = 0; y < dst.size.y; y++) {
            unsigned int vw = v[y].w;
            for (int x = 0; x < dst.size.x; x++) 
            {
                unsigned int hw = h[x].w;
                int idx = slw * v[y].line + h[x].line; // high left index
                WT lo = static_cast<WT>(s[idx - slw - 1]) * (256 - hw)
                    + static_cast<WT>(s[idx - slw]) * hw;
                WT hi = static_cast<WT>(s[idx]) * (256 - hw)
                    + static_cast<WT>(s[idx]) * hw;
                // Then interpolate the high and low using vertical weight
                WT value = hi * vw + lo * (256 - vw);
                // The value is divided by 256^2 because each interpolation is times 256
                // Make sure the working type is large enough to eliminate overflow
                *data++ = static_cast<T>(value / (256 * 256));
            }
        }
        return;
    }
    // More than one band
    for (int y = 0; y < dst.size.y; y++) {
        unsigned int vw = v[y].w;
        for (int x = 0; x < dst.size.x; x++)
        {
            unsigned int hw = h[x].w;
            int idx = slw * v[y].line + h[x].line * colors; // high left index
            for (int c = 0; c < colors; c++) {
                WT lo = static_cast<WT>(s[idx + c - slw]) * hw +
                    static_cast<WT>(s[idx + c - slw - colors]) * (256 - hw);
                WT hi = static_cast<WT>(s[idx + c]) * hw +
                    static_cast<WT>(s[idx + c - colors]) * (256 - hw);
                // Then interpolate the high and low using vertical weight
                WT value = hi * vw + lo * (256 - vw);
                // The value is divided by 256^2 because each interpolation is times 256
                // Make sure the working type is large enough to eliminate overflow
                *data++ = static_cast<T>(value / (256 * 256));
            }
        }
    }
}

//
// NearNb sampling, based on ilines
// Uses the weights to pick between two choices
//

template<typename T = apr_byte_t> static void interpolateNN(
    const interpolation_buffer &src, interpolation_buffer &dst,
    iline *h, iline *v)
{
    ap_assert(src.size.c == dst.size.c);
    T *data = reinterpret_cast<T *>(dst.buffer);
    T *s = reinterpret_cast<T *>(src.buffer);
    const int colors = static_cast<int>(dst.size.c);

    // Precompute the horizontal pick table, the vertical is only used once
    std::vector<int> hpick(dst.size.x);
    for (int i = 0; i < hpick.size(); i++)
        hpick[i] = colors * (h[i].line - ((h[i].w < 128) ? 1 : 0));

    if (colors == 1) { // faster code due to fewer tests
        for (int y = 0; y < static_cast<int>(dst.size.y); y++) {
            int vidx = src.size.x * (v[y].line - ((v[y].w < 128) ? 1 : 0));
            for (auto const &hid : hpick)
                *data++ = s[vidx + hid];
        }
        return;
    }

    for (int y = 0; y < static_cast<int>(dst.size.y); y++) {
        int vidx = colors * src.size.x * (v[y].line - ((v[y].w < 128) ? 1 : 0));
        for (auto const &hid : hpick)
            for (int c = 0; c < colors; c++)
                *data++ = s[vidx + hid + c];
    }
}

// Calls the interpolation for the right data type
void resample(const repro_conf *cfg, const interpolation_buffer &src, interpolation_buffer &dst,
    iline *h, iline *v)
{
    switch (cfg->raster.datatype) {
    case GDT_UInt16:
        if (cfg->nearNb)
            interpolateNN<apr_uint16_t>(src, dst, h, v);
        else
            interpolate<apr_uint16_t>(src, dst, h, v);
        break;
    case GDT_Int16:
        if (cfg->nearNb)
            interpolateNN<apr_int16_t>(src, dst, h, v);
        else
            interpolate<apr_int16_t>(src, dst, h, v);
        break;
    default: // Byte
        if (cfg->nearNb)
            interpolateNN(src, dst, h, v);
        else
            interpolate(src, dst, h, v);
    }
}

// Web mercator X to longitude in degrees
static double wm2lon(double eres, double x) {
    return 360 * eres * x;
}

// Web mercator Y to latitude in degrees
static double wm2lat(double eres, double y) {
    return 90 * (1 - 4 / pi * atan(exp(eres * pi * 2 * -y)));
}

// Convert WM bbox to GCS bbox in degrees
static void bbox_wm2gcs(double eres, const bbox_t &wm_bbox, bbox_t &gcs_bbox) {
    gcs_bbox.xmin = wm2lon(eres, wm_bbox.xmin);
    gcs_bbox.ymin = wm2lat(eres, wm_bbox.ymin);
    gcs_bbox.xmax = wm2lon(eres, wm_bbox.xmax);
    gcs_bbox.ymax = wm2lat(eres, wm_bbox.ymax);
}

static double lon2wm(double eres, double lon) {
    return lon / eres / 360;
}

// Goes out of bounds close to the poles, valid latitude range is under 85.052
static double lat2wm(double eres, double lat) {
    if (abs(lat) < 85.052)
        return log(tan(pi / 4 * (1 + lat / 90))) / eres / 2 / pi;
    return (lat > 85) ? (0.5 / eres) : (-0.5 / eres);
}

// Convert GCS bbox to WM
static void bbox_gcs2wm(double eres, const bbox_t &gcs_bbox, bbox_t &wm_bbox) {
    wm_bbox.xmin = lon2wm(eres, gcs_bbox.xmin);
    wm_bbox.ymin = lat2wm(eres, gcs_bbox.ymin);
    wm_bbox.xmax = lon2wm(eres, gcs_bbox.xmax);
    wm_bbox.ymax = lat2wm(eres, gcs_bbox.ymax);
}

// Functions to convert the output bounding box to input equivalent projection system
typedef void bb_eq_func(work &);

static void bb_scale(work &info) {
   // Input is the same projection as output
    info.out_equiv_bbox = info.out_bbox;
}

// For the output gcs, convert the bbox to equivalent WM
static void bb_wm2gcs(work &info) {
    bbox_gcs2wm(info.c->eres, info.out_bbox, info.out_equiv_bbox);
}

// For the output wm, convert the bbox to equivalent GCS
static void bb_gcs2wm(work &info) {
    bbox_wm2gcs(info.c->eres, info.out_bbox, info.out_equiv_bbox);
}

// Functions that compute the interpolation lines
typedef void iline_func(work &, iline *table);

// The x dimension is most of the time linear, convenience function
static void linear_x(work &info, iline *table) {
    const double out_rx = info.c->raster.rsets[info.out_tile.l].rx;
    const double in_rx = info.c->inraster.rsets[info.tl.l].rx;
    const double offset_x = info.out_equiv_bbox.xmin - info.in_bbox.xmin + 0.5 * (out_rx - in_rx);
    const int size_x = info.c->raster.pagesize.x;
    const int max_column = info.c->inraster.pagesize.x * (info.br.x - info.tl.x) - 1;

    init_ilines(in_rx, out_rx, offset_x, table, size_x);
    adjust_itable(table, size_x, max_column);
}

// The iline_func for linear scaling, does the same thing for x and for y
static void scale_iline(work &info, iline *table) {
    linear_x(info, table);
    // Y
    const double out_ry = info.c->raster.rsets[info.out_tile.l].ry;
    const double in_ry = info.c->inraster.rsets[info.tl.l].ry;
    const double offset_y = info.in_bbox.ymax - info.out_equiv_bbox.ymax + 0.5 * (out_ry - in_ry);
    iline *t_y = table + info.c->raster.pagesize.x;
    const int size_y = info.c->raster.pagesize.y;
    const int max_line = info.c->inraster.pagesize.y * (info.br.y - info.tl.y) - 1;

    init_ilines(in_ry, out_ry, offset_y, t_y, size_y);
    adjust_itable(t_y, size_y, max_line);
}

// The iline_func for gcs2wm
static void gcs2wm_iline(work &info, iline *table) {
    linear_x(info, table);
    // Y
    const double out_ry = info.c->raster.rsets[info.out_tile.l].ry;
    const double in_ry = info.c->inraster.rsets[info.tl.l].ry;
    const int size_y = info.c->raster.pagesize.y;
    iline *t_y = table + info.c->raster.pagesize.x;
    double offset_y = info.in_bbox.ymax - 0.5 * in_ry;
    const int max_line = info.c->inraster.pagesize.y * (info.br.y - info.tl.y) - 1;
    const double eres = info.c->eres;
    for (int i = 0; i < size_y; i++) {
        // Latitude of pixel center for this output line
        const double lat = wm2lat(eres, info.out_bbox.ymax - out_ry * (i + 0.5));
        // Input line center
        const double pos = (offset_y - lat) / in_ry;
        // Pick the higher line
        t_y[i].line = static_cast<int>(ceil(pos));
        t_y[i].w = static_cast<int>(floor(256 * (pos - floor(pos))));
    }
    adjust_itable(t_y, size_y, max_line);
}

// The iline_func for wm2gcs
static void wm2gcs_iline(work &info, iline *table) {
    linear_x(info, table);
    // Y
    const double out_ry = info.c->raster.rsets[info.out_tile.l].ry;
    const double in_ry = info.c->inraster.rsets[info.tl.l].ry;
    const int size_y = info.c->raster.pagesize.y;
    iline *t_y = table + info.c->raster.pagesize.x;
    double offset_y = info.in_bbox.ymax - 0.5 * in_ry;
    const int max_line = info.c->inraster.pagesize.y * (info.br.y - info.tl.y) - 1;
    const double eres = info.c->eres;
    for (int i = 0; i < size_y; i++) {
        // Northing of pixel center for this output line
        const double wm_y = lat2wm(eres, info.out_bbox.ymax - out_ry * (i + 0.5));
        // Input line center
        const double pos = (offset_y - wm_y) / in_ry;
        // Pick the higher line
        t_y[i].line = static_cast<int>(ceil(pos));
        t_y[i].w = static_cast<int>(floor(256 * (pos - floor(pos))));
    }
    adjust_itable(t_y, size_y, max_line);
}

static bool our_request(request_rec *r, repro_conf *cfg) {
    if (r->method_number != M_GET) return false;

    if (!cfg->regexp || cfg->code <= P_NONE || cfg->code >= P_COUNT) return false;
    char *url_to_match = r->args ? apr_pstrcat(r->pool, r->uri, "?", r->args, NULL) : r->uri;
    for (int i = 0; i < cfg->regexp->nelts; i++) {
        ap_regex_t *m = &APR_ARRAY_IDX(cfg->regexp, i, ap_regex_t);
        if (!ap_regexec(m, url_to_match, 0, NULL, 0)) return true; // Found
    }
    return false;
}

static int handler(request_rec *r)
{
    // Tables of reprojection code dependent functions, to dispatch on
    // Could be done with a switch, this is more compact and easier to extend
    static const bb_eq_func *box_equiv[P_COUNT] = { NULL, bb_scale, bb_gcs2wm, bb_wm2gcs };
    static const iline_func *calc_itables[P_COUNT] = { NULL, scale_iline, gcs2wm_iline, wm2gcs_iline };

    // TODO: use r->header_only to verify ETags, assuming the subrequests are faster in that mode
    repro_conf *cfg = (repro_conf *)ap_get_module_config(r->per_dir_config, &reproject_module);
    if (!our_request(r, cfg)) return DECLINED;

    apr_array_header_t *tokens = tokenize(r->pool, r->uri);
    if (tokens->nelts < 3) return DECLINED; // At least Level Row Column

    // Use a xyzc structure, with c being the level
    // Input order is M/Level/Row/Column, with M being optional
    struct sz tile;
    memset(&tile, 0, sizeof(tile));

    // Need at least three numerical arguments
    tile.x = apr_atoi64(*(char **)apr_array_pop(tokens)); REQ_ERR_IF(errno);
    tile.y = apr_atoi64(*(char **)apr_array_pop(tokens)); REQ_ERR_IF(errno);
    tile.l = apr_atoi64(*(char **)apr_array_pop(tokens)); REQ_ERR_IF(errno);

    // We can ignore the error on this one, defaults to zero
    // The parameter before the level can't start with a digit for an extra-dimensional MRF
    if (cfg->raster.size.z != 1 && tokens->nelts)
        tile.z = apr_atoi64(*(char **)apr_array_pop(tokens));

    // Don't allow access to negative values, send the empty tile instead
    if (tile.l < 0 || tile.x < 0 || tile.y < 0)
        return send_empty_tile(r);

    // Adjust the level to internal
    tile.l += cfg->raster.skip;

    // Outside of bounds tile returns a not-found error
    if (tile.l >= cfg->raster.n_levels ||
        tile.x >= cfg->raster.rsets[tile.l].width ||
        tile.y >= cfg->raster.rsets[tile.l].height)
        return send_empty_tile(r);

    // Need to have mod_receive available
    SERR_IF(!ap_get_output_filter_handle("Receive"), "mod_receive not installed");

    work info;
    info.out_tile = tile;
    double out_rx = cfg->raster.rsets[tile.l].rx;
    tile_to_bbox(cfg->raster, &(info.out_tile), info.out_bbox);

    // calculate the input projection equivalent bbox
    box_equiv[cfg->code](info);
    double out_equiv_rx = (info.out_equiv_bbox.xmax - info.out_equiv_bbox.xmin)
        / cfg->raster.pagesize.x;

    // Pick the input level
    int input_l = input_level(cfg->inraster, out_equiv_rx, cfg->oversample);
    bbox_to_tile(cfg->inraster, input_l, info.out_equiv_bbox, &info.tl, &info.br);
    info.tl.z = info.br.z = info.out_tile.z;
    info.tl.c = info.br.c = cfg->inraster.pagesize.c;
    info.tl.l = info.br.l = input_l - cfg->inraster.skip;
    tile_to_bbox(info.c->inraster, &info.tl, info.in_bbox);

    // Incoming tiles buffer
    void *buffer = NULL;
    apr_status_t status = retrieve_source(r, info.tl, info.br, &buffer);
    if (APR_SUCCESS != status) return status;
    // Convert back to absolute level for input tiles
    info.tl.l = info.br.l = input_l;

    // Outgoing raw tile buffer
    int pixel_size = cfg->raster.pagesize.c * DT_SIZE(cfg->raster.datatype);
    storage_manager raw;
    raw.size = cfg->raster.pagesize.x * cfg->raster.pagesize.y * pixel_size;
    raw.buffer = static_cast<char *>(apr_palloc(r->pool, raw.size));

    // Set up the input and output 2D interpolation buffers
    interpolation_buffer ib = { buffer, cfg->inraster.pagesize };
    // The input buffer contains multiple input pages
    ib.size.x *= (info.br.x - info.tl.x);
    ib.size.y *= (info.br.y - info.tl.y);
    interpolation_buffer ob = { raw.buffer, cfg->raster.pagesize };

    // Use a single vector to hold the interpolation tables for both x and y
//    std::vector<iline> table(ob.size.x + ob.size.y);
    iline *table = static_cast<iline *>(apr_palloc(r->pool, sizeof(iline)*(ob.size.x + ob.size.y)));

    // Compute the iline values, depending on the projection
    calc_itables[cfg->code](info, table);

    // Perform the resampling
    resample(cfg, ib, ob, table, table + ob.size.x);

    // A buffer for the output tile
    storage_manager dst;
    dst.size = cfg->max_output_size;
    dst.buffer = static_cast<char *>(apr_palloc(r->pool, dst.size));

    const char *error_message = "Unknown output format requested";

    if (NULL == cfg->mime_type || 0 == apr_strnatcmp(cfg->mime_type, "image/jpeg")) {
        jpeg_params params;
        params.quality = static_cast<int>(cfg->quality);
        error_message = jpeg_encode(params, cfg->raster, raw, dst);
    }
    else if (0 == apr_strnatcmp(cfg->mime_type, "image/png")) {
        png_params params;
        set_png_params(cfg->raster, &params);
        if (cfg->quality < 10) // Otherwise use the default of 6
            params.compression_level = static_cast<int>(cfg->quality);
        error_message = png_encode(params, cfg->raster, raw, dst);
    }

    if (error_message) {
        ap_log_rerror(APLOG_MARK, APLOG_ERR, 0, r, "%s from :%s", error_message, r->uri);
        // Something went wrong if compression fails
        return HTTP_INTERNAL_SERVER_ERROR;
    }

    return send_image(r, dst, cfg->mime_type);
}

static const char *read_config(cmd_parms *cmd, repro_conf *c, const char *src, const char *fname)
{
    char *err_message;
    const char *line;

    // Start with the source configuration
    apr_table_t *kvp = read_pKVP_from_file(cmd->temp_pool, src, &err_message);
    if (NULL == kvp) return err_message;

    err_message = const_cast<char*>(ConfigRaster(cmd->pool, kvp, c->inraster));
    if (err_message) return apr_pstrcat(cmd->pool, "Source ", err_message, NULL);

    // Then the real configuration file
    kvp = read_pKVP_from_file(cmd->temp_pool, fname, &err_message);
    if (NULL == kvp) return err_message;
    err_message = const_cast<char *>(ConfigRaster(cmd->pool, kvp, c->raster));
    if (err_message) return err_message;

    // Output mime type
    line = apr_table_get(kvp, "MimeType");
    c->mime_type = (line) ? apr_pstrdup(cmd->pool, line) : "image/jpeg";

    // Get the planet circumference in meters, for partial coverages
    line = apr_table_get(kvp, "Radius");
    // Stored as radius and the inverse of circumference
    double radius = (line) ? strtod(line, NULL) : 6378137.0;
    c->eres = 1.0 / (2 * pi * radius);

    // Sampling flags
    c->oversample = NULL != apr_table_get(kvp, "Oversample");
    c->nearNb = NULL != apr_table_get(kvp, "Nearest");

    line = apr_table_get(kvp, "ETagSeed");
    // Ignore the flag
    int flag;
    c->seed = line ? base32decode((unsigned char *)line, &flag) : 0;
    // Set the missing tile etag, with the extra bit set
    uint64tobase32(c->seed, c->eETag, 1);

    // EmptyTile, defaults to pass-through
    line = apr_table_get(kvp, "EmptyTile");
    if (line) {
        err_message = read_empty_tile(cmd, c, line);
        if (err_message) return err_message;
    }

    line = apr_table_get(kvp, "InputBufferSize");
    c->max_input_size = DEFAULT_INPUT_SIZE;
    if (line)
        c->max_input_size = (apr_size_t)apr_strtoi64(line, NULL, 0);

    line = apr_table_get(kvp, "OutputBufferSize");
    c->max_output_size = DEFAULT_INPUT_SIZE;
    if (line)
        c->max_output_size = (apr_size_t)apr_strtoi64(line, NULL, 0);

    line = apr_table_get(kvp, "SourcePath");
    if (!line)
        return "SourcePath directive is missing";
    c->source = apr_pstrdup(cmd->pool, line);

    line = apr_table_get(kvp, "SourcePostfix");
    if (line)
        c->postfix = apr_pstrdup(cmd->pool, line);

    c->quality = 75.0; // Default for JPEG
    line = apr_table_get(kvp, "Quality");
    if (line)
        c->quality = strtod(line, NULL);

    // Set the actuall reprojection function
    if (IS_AFFINE_SCALING(c))
        c->code = P_AFFINE;
    else if (IS_GCS2WM(c))
        c->code = P_GCS2WM;
    else if (IS_WM2GCS(c))
        c->code = P_WM2GCS;
    else
        return "Can't determine reprojection function";

    return NULL;
}

static const command_rec cmds[] =
{
    AP_INIT_TAKE2(
    "Reproject_ConfigurationFiles",
    (cmd_func) read_config, // Callback
    0, // Self-pass argument
    ACCESS_CONF, // availability
    "Source and output configuration files"
    ),

    AP_INIT_TAKE1(
    "Reproject_RegExp",
    (cmd_func) set_regexp,
    0, // Self-pass argument
    ACCESS_CONF, // availability
    "Regular expression that the URL has to match.  At least one is required."),

    { NULL }
};

static void register_hooks(apr_pool_t *p)
{
    ap_hook_handler(handler, NULL, NULL, APR_HOOK_MIDDLE);
}

module AP_MODULE_DECLARE_DATA reproject_module = {
    STANDARD20_MODULE_STUFF,
    create_dir_config,
    0, // No dir_merge
    0, // No server_config
    0, // No server_merge
    cmds, // configuration directives
    register_hooks // processing hooks
};
