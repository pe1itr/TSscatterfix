#include "recovery.h"
#include <ctype.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

/* Small JSON reader for the documented linear model schema. Unknown metadata
 * is skipped recursively, with a depth limit. No runtime scripting dependency. */
typedef struct { const char *p; int depth; } Json;
static void ws(Json *j) { while (isspace((unsigned char)*j->p)) j->p++; }
static int token(Json *j, char c) { ws(j); if (*j->p != c) return 0; j->p++; return 1; }
static int string(Json *j, char *out, size_t capacity) {
    if (!token(j, '"')) return 0;
    size_t n = 0;
    while (*j->p && *j->p != '"') {
        unsigned char c = (unsigned char)*j->p++;
        if (c < 32 || c == '\\' || n+1 >= capacity) return 0;
        out[n++] = (char)c;
    }
    if (*j->p++ != '"') return 0;
    out[n] = 0; return 1;
}
static int number(Json *j, double *value) {
    ws(j); char *end;
    if (*j->p != '-' && !isdigit((unsigned char)*j->p)) return 0;
    *value = strtod(j->p, &end);
    if (end == j->p || !isfinite(*value)) return 0;
    j->p = end; return 1;
}
static int skip(Json *j) {
    ws(j); if (++j->depth > 16) return 0;
    int ok = 0;
    if (*j->p == '{' || *j->p == '[') {
        char close = *j->p++ == '{' ? '}' : ']'; ws(j);
        if (*j->p == close) { j->p++; ok = 1; }
        else do {
            if (close == '}') { char key[256]; if (!string(j,key,sizeof(key)) || !token(j,':')) break; }
            if (!skip(j)) break;
            if (token(j,close)) { ok = 1; break; }
        } while (token(j,','));
    } else if (*j->p == '"') { char value[4096]; ok = string(j,value,sizeof(value)); }
    else if (!strncmp(j->p,"null",4) || !strncmp(j->p,"true",4)) { j->p += 4; ok = 1; }
    else if (!strncmp(j->p,"false",5)) { j->p += 5; ok = 1; }
    else { double value; ok = number(j,&value); }
    --j->depth; return ok;
}
static int feature_index(const char *name) {
    for (int i = 0; i < SF_FEATURE_COUNT; ++i) if (!strcmp(name,sf_feature_names[i])) return i;
    return -1;
}
static int map(Json *j, double *values) {
    if (!token(j,'{')) return 0;
    if (token(j,'}')) return 1;
    unsigned seen = 0;
    do {
        char key[128]; double value;
        if (!string(j,key,sizeof(key)) || !token(j,':') || !number(j,&value)) return 0;
        int index = feature_index(key);
        if (index < 0 || (seen & (1u<<index))) return 0;
        seen |= 1u<<index; values[index] = value;
        if (token(j,'}')) return 1;
    } while (token(j,','));
    return 0;
}
int sf_rank_model_load(SfRankModel *model, const char *path) {
    if (!model || !path) return 0;
    FILE *file = fopen(path,"rb"); if (!file) return 0;
    char data[65537]; size_t n = fread(data,1,sizeof(data)-1,file);
    int bad = ferror(file) || !feof(file); fclose(file); if (bad) return 0;
    data[n] = 0; Json j = {data,0}; SfRankModel result = {0};
    for (int i = 0; i < SF_FEATURE_COUNT; ++i) result.scales[i] = 1;
    int order[SF_FEATURE_COUNT], count = 0, weight_count = 0; double weights[SF_FEATURE_COUNT+1];
    unsigned fields = 0;
    if (!token(&j,'{')) return 0;
    do {
        char key[128]; if (!string(&j,key,sizeof(key)) || !token(&j,':')) return 0;
        unsigned flag = !strcmp(key,"features") ? 1 : !strcmp(key,"weights") ? 2 : !strcmp(key,"means") ? 4 : !strcmp(key,"scales") ? 8 : !strcmp(key,"kind") ? 16 : 0;
        if (flag && (fields&flag)) return 0;
        fields |= flag;
        if (flag == 1) {
            if (!token(&j,'[')) return 0;
            unsigned seen = 0;
            do {
                char name[128]; if (count == SF_FEATURE_COUNT || !string(&j,name,sizeof(name))) return 0;
                int index = feature_index(name);
                if (index < 0 || (seen&(1u<<index))) return 0;
                seen |= 1u<<index; order[count++] = index;
            } while (token(&j,','));
            if (!token(&j,']')) return 0;
        } else if (flag == 2) {
            if (!token(&j,'[')) return 0;
            do {
                if (weight_count == SF_FEATURE_COUNT+1 || !number(&j,&weights[weight_count++])) return 0;
            } while (token(&j,','));
            if (!token(&j,']')) return 0;
        } else if (flag == 4 || flag == 8) {
            if (!map(&j,flag == 4 ? result.means : result.scales)) return 0;
        } else if (flag == 16) {
            char kind[128]; if (!string(&j,kind,sizeof(kind)) || strcmp(kind,"linear_regression_v1")) return 0;
        } else if (!skip(&j)) return 0;
        if (token(&j,'}')) break;
        if (!token(&j,',')) return 0;
    } while (1);
    ws(&j);
    if (*j.p || (fields&31) != 31 || weight_count != count+1) return 0;
    result.weights[0] = weights[0];
    for (int i = 0; i < count; ++i) result.weights[order[i]+1] = weights[i+1];
    result.enabled = 1; *model = result; return 1;
}
