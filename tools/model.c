/* Native CSV labelling and linear-model training utilities. */
#include "recovery.h"
#include "media.h"
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct { char **cells; size_t count; } Row;
typedef struct { Row *rows; size_t count; } Table;
static void row_free(Row *row) {
    for (size_t i = 0; i < row->count; ++i) free(row->cells[i]);
    free(row->cells); memset(row,0,sizeof(*row));
}
static void table_free(Table *table) {
    for (size_t i = 0; i < table->count; ++i) row_free(&table->rows[i]);
    free(table->rows); memset(table,0,sizeof(*table));
}
static int cell_add(Row *row, const char *value) {
    char **cells = realloc(row->cells,(row->count+1)*sizeof(*cells));
    if (!cells) return 0;
    row->cells = cells;
    char *copy = malloc(strlen(value)+1); if (!copy) return 0;
    strcpy(copy,value); row->cells[row->count++] = copy; return 1;
}
static int cell_set(Row *row, size_t column, const char *value) {
    while (row->count <= column) if (!cell_add(row,"")) return 0;
    char *copy = malloc(strlen(value)+1); if (!copy) return 0;
    strcpy(copy,value); free(row->cells[column]); row->cells[column] = copy; return 1;
}
static int row_read(FILE *file, Row *row) {
    char *buffer = malloc(1024*1024); if (!buffer) return -1;
    size_t n = 0; int quoted = 0, closed = 0, any = 0, c;
    while ((c = fgetc(file)) != EOF) {
        any = 1;
        if (quoted) {
            if (c == '"') {
                int next = fgetc(file);
                if (next != '"') { quoted = 0; closed = 1; if (next != EOF) ungetc(next,file); continue; }
            }
        } else {
            if (c == '"' && !n && !closed) { quoted = 1; continue; }
            if (c == ',' || c == '\n' || c == '\r') {
                buffer[n] = 0;
                if (!cell_add(row,buffer)) goto bad;
                n = 0; closed = 0;
                if (c != ',') {
                    if (c == '\r') { c = fgetc(file); if (c != '\n' && c != EOF) ungetc(c,file); }
                    free(buffer); return 1;
                }
                continue;
            }
            if (closed || c == '"') goto bad;
        }
        if (n == 1024*1024-1) goto bad;
        buffer[n++] = (char)c;
    }
    if (quoted || ferror(file)) goto bad;
    if (any) { buffer[n] = 0; if (!cell_add(row,buffer)) goto bad; }
    free(buffer); return any;
bad:
    free(buffer); row_free(row); return -1;
}
static int table_read(Table *table, const char *path) {
    FILE *file = fopen(path,"rb"); if (!file) return 0;
    Row row = {0}; int r;
    while ((r = row_read(file,&row)) > 0) {
        if (table->count == 100000) { row_free(&row); r = -1; break; }
        Row *rows = realloc(table->rows,(table->count+1)*sizeof(*rows));
        if (!rows) { row_free(&row); r = -1; break; }
        table->rows = rows; table->rows[table->count++] = row; memset(&row,0,sizeof(row));
    }
    fclose(file); return r == 0 && table->count > 0;
}
static int column(const Table *table, const char *name) {
    if (!table->count) return -1;
    for (size_t i = 0; i < table->rows[0].count; ++i) if (!strcmp(table->rows[0].cells[i],name)) return (int)i;
    return -1;
}
static const char *get(const Table *table, size_t row, const char *name) {
    int col = column(table,name);
    return col >= 0 && (size_t)col < table->rows[row].count ? table->rows[row].cells[col] : "";
}
static int numeric(const char *text, double *value) {
    char *end; errno = 0; *value = strtod(text,&end);
    while (isspace((unsigned char)*end)) ++end;
    return *text && !*end && !errno && isfinite(*value);
}
static void csv_write(FILE *file, const Row *row) {
    for (size_t i = 0; i < row->count; ++i) {
        if (i) fputc(',',file);
        fputc('"',file);
        for (const char *s = row->cells[i]; *s; ++s) { if (*s == '"') fputc('"',file); fputc(*s,file); }
        fputc('"',file);
    }
    fputc('\n',file);
}
static int table_write(const Table *table, const char *path) {
    FILE *file = fopen(path,"w"); if (!file) return 0;
    for (size_t i = 0; i < table->count; ++i) csv_write(file,&table->rows[i]);
    int ok = !ferror(file); if (fclose(file)) ok = 0; return ok;
}
static int labels(const char *features, const char *directory, const char *output, int idr_only) {
    Table table = {0}; FILE *file = NULL; DIR *dir = NULL; int ok = 0;
    if (!table_read(&table,features) || column(&table,"source") < 0 || column(&table,"access_unit") < 0) goto done;
    dir = opendir(directory); if (!dir) goto done;
    file = fopen(output,"w"); if (!file) goto done;
    fputs("source,access_unit,label,candidate_png,score,packet_confidence,slice_bytes,notes\n",file);
    for (size_t i = 1; i < table.count; ++i) {
        if (idr_only && strcmp(get(&table,i,"has_idr"),"1")) continue;
        double index;
        if (!numeric(get(&table,i,"access_unit"),&index) || index < 0 || index > 1e12) goto done;
        char suffix[64], png[1024] = "";
        snprintf(suffix,sizeof(suffix),"_au_%06llu.png",(unsigned long long)index);
        rewinddir(dir); struct dirent *entry;
        while ((entry = readdir(dir))) {
            size_t n = strlen(entry->d_name), m = strlen(suffix);
            if (!strncmp(entry->d_name,"candidate_",10) && n >= m && !strcmp(entry->d_name+n-m,suffix) &&
                (!png[0] || strcmp(entry->d_name,png) < 0)) snprintf(png,sizeof(png),"%s",entry->d_name);
        }
        Row row = {0};
        const char *values[] = {get(&table,i,"source"),get(&table,i,"access_unit"),"",png,get(&table,i,"score"),get(&table,i,"packet_confidence"),get(&table,i,"slice_bytes"),""};
        for (unsigned j = 0; j < sizeof(values)/sizeof(*values); ++j) if (!cell_add(&row,values[j])) { row_free(&row); goto done; }
        csv_write(file,&row); row_free(&row);
    }
    ok = !ferror(file);
done:
    if (file && fclose(file)) ok = 0;
    if (dir) closedir(dir);
    table_free(&table); return ok;
}
static int score_image(const SfImage *im) {
    double blue=0,black=0,gray=0,call=0,text=0,nb=0,nc=0,nt=0;
    for (int y=0;y<im->height;++y) for (int x=0;x<im->width;++x) {
        const uint8_t *p=im->pixels+(size_t)y*im->stride+3*x; int r=p[0],g=p[1],b=p[2];
        if (x >= (int)(im->width*.20) && x < (int)(im->width*.80) && y >= (int)(im->height*.02) && y < (int)(im->height*.98)) {
            nb++; blue += b>130 && r<90 && g<120; black += r<25 && g<25 && b<25;
            gray += abs(r-g)<12 && abs(g-b)<12 && r>=70 && r<=190;
        }
        if (x >= (int)(im->width*.18) && x < (int)(im->width*.82) && y >= (int)(im->height*.60) && y < (int)(im->height*.95)) {
            nc++; call += r>185 && g>185 && b>185;
        }
        if (x >= (int)(im->width*.20) && x < (int)(im->width*.80) && y >= (int)(im->height*.05) && y < (int)(im->height*.58)) {
            nt++; text += r>185 && g>185 && b>185;
        }
    }
    double raw=fmin(2,blue/fmax(1,nb)*2.8)+fmin(2.3,call/fmax(1,nc)*38)+fmin(1.2,text/fmax(1,nt)*18)-fmin(2,black/fmax(1,nb)*8)-fmin(1,gray/fmax(1,nb)*5);
    return (int)fmax(0,fmin(5,nearbyint(raw)));
}
static int autolabel(const char *labels_path, const char *directory, const char *output, int overwrite) {
    Table table = {0}; int ok = 0;
    if (!table_read(&table,labels_path)) goto done;
    int label_col = column(&table,"label"), notes_col = column(&table,"notes");
    if (label_col < 0 || column(&table,"candidate_png") < 0) goto done;
    if (notes_col < 0) { notes_col = (int)table.rows[0].count; if (!cell_add(&table.rows[0],"notes")) goto done; }
    for (size_t i=1;i<table.count;++i) {
        if (*get(&table,i,"label") && !overwrite) continue;
        const char *name = get(&table,i,"candidate_png"); if (!*name) continue;
        char path[4096]; if (snprintf(path,sizeof(path),"%s/%s",directory,name) >= (int)sizeof(path)) goto done;
        SfImage image = {0}; if (!sf_image_read(path,&image)) continue;
        char label[16]; snprintf(label,sizeof(label),"%d",score_image(&image)); sf_image_free(&image);
        const char *old_notes = get(&table,i,"notes"); size_t n = strlen(old_notes)+64;
        char *notes = malloc(n); if (!notes) goto done;
        snprintf(notes,n,"%s%sauto test-card colour heuristic",old_notes,*old_notes ? "; " : "");
        int changed = cell_set(&table.rows[i],(size_t)label_col,label) && cell_set(&table.rows[i],(size_t)notes_col,notes);
        free(notes); if (!changed) goto done;
    }
    ok = table_write(&table,output);
done:
    table_free(&table); return ok;
}
typedef struct { double x[SF_FEATURE_COUNT], y; } Sample;
static int train(const char **features, const char **labels_paths, int count, const char *output, int epochs, double lr, double l2) {
    Sample *samples = NULL; size_t n = 0; int ok = 0;
    for (int pair=0;pair<count;++pair) {
        Table f={0}, l={0};
        if (!table_read(&f,features[pair]) || !table_read(&l,labels_paths[pair]) || column(&f,"source")<0 || column(&f,"access_unit")<0 || column(&l,"source")<0 || column(&l,"access_unit")<0 || column(&l,"label")<0) { table_free(&f); table_free(&l); goto done; }
        for (size_t i=1;i<l.count;++i) {
            const char *label=get(&l,i,"label"); if (!*label) continue;
            double y; if (!numeric(label,&y)) { table_free(&f); table_free(&l); goto done; }
            for (size_t j=1;j<f.count;++j) if (!strcmp(get(&l,i,"source"),get(&f,j,"source")) && !strcmp(get(&l,i,"access_unit"),get(&f,j,"access_unit"))) {
                Sample sample = {{0},y};
                for (int k=0;k<SF_FEATURE_COUNT;++k) {
                    const char *v=get(&f,j,sf_feature_names[k]);
                    if (*v && !numeric(v,&sample.x[k])) { table_free(&f); table_free(&l); goto done; }
                }
                Sample *next = realloc(samples,(n+1)*sizeof(*samples));
                if (!next) { table_free(&f); table_free(&l); goto done; }
                samples=next; samples[n++]=sample; break;
            }
        }
        table_free(&f); table_free(&l);
    }
    if (n<3) { fprintf(stderr,"Need at least 3 matching labelled feature rows.\n"); goto done; }
    SfRankModel model={0};
    for (int k=0;k<SF_FEATURE_COUNT;++k) {
        for (size_t i=0;i<n;++i) model.means[k]+=samples[i].x[k]/n;
        for (size_t i=0;i<n;++i) { double d=samples[i].x[k]-model.means[k]; model.scales[k]+=d*d/(n-1); }
        model.scales[k]=sqrt(model.scales[k]); if (!model.scales[k]) model.scales[k]=1;
        if (!isfinite(model.means[k]) || !isfinite(model.scales[k])) goto done;
        for (size_t i=0;i<n;++i) samples[i].x[k]=(samples[i].x[k]-model.means[k])/model.scales[k];
    }
    for (int epoch=0;epoch<epochs;++epoch) {
        double grad[SF_FEATURE_COUNT+1]={0};
        for (size_t i=0;i<n;++i) {
            double pred=model.weights[0];
            for (int k=0;k<SF_FEATURE_COUNT;++k) pred+=model.weights[k+1]*samples[i].x[k];
            double error=pred-samples[i].y; grad[0]+=error;
            for (int k=0;k<SF_FEATURE_COUNT;++k) grad[k+1]+=error*samples[i].x[k];
        }
        for (int k=0;k<=SF_FEATURE_COUNT;++k) {
            model.weights[k]-=lr*(grad[k]/n+(k ? l2*model.weights[k] : 0));
            if (!isfinite(model.weights[k])) goto done;
        }
    }
    double mae=0,lo=samples[0].y,hi=lo;
    for (size_t i=0;i<n;++i) {
        double pred=model.weights[0]; for (int k=0;k<SF_FEATURE_COUNT;++k) pred+=model.weights[k+1]*samples[i].x[k];
        mae+=fabs(pred-samples[i].y)/n; lo=fmin(lo,samples[i].y); hi=fmax(hi,samples[i].y);
    }
    FILE *file=fopen(output,"w"); if (!file) goto done;
    fputs("{\n  \"kind\": \"linear_regression_v1\",\n  \"features\": [",file);
    for (int k=0;k<SF_FEATURE_COUNT;++k) fprintf(file,"%s\"%s\"",k ? ", " : "",sf_feature_names[k]);
    fputs("],\n  \"means\": {",file);
    for (int k=0;k<SF_FEATURE_COUNT;++k) fprintf(file,"%s\"%s\": %.17g",k ? ", " : "",sf_feature_names[k],model.means[k]);
    fputs("},\n  \"scales\": {",file);
    for (int k=0;k<SF_FEATURE_COUNT;++k) fprintf(file,"%s\"%s\": %.17g",k ? ", " : "",sf_feature_names[k],model.scales[k]);
    fputs("},\n  \"weights\": [",file);
    for (int k=0;k<=SF_FEATURE_COUNT;++k) fprintf(file,"%s%.17g",k ? ", " : "",model.weights[k]);
    fprintf(file,"],\n  \"label_min\": %.17g, \"label_max\": %.17g,\n  \"train_rows\": %zu, \"train_mae\": %.17g\n}\n",lo,hi,n,mae);
    ok=!ferror(file); if (fclose(file)) ok=0;
    if (ok) fprintf(stderr,"[train] rows=%zu mae=%.6f model=%s\n",n,mae,output);
done:
    free(samples); return ok;
}
static void usage(void) {
    fprintf(stderr,"Usage: scatterfix-model train|labels|autolabel [options]\n"
        "  train --features-csv F --labels-csv L --output-model M [--epochs 3000 --learning-rate .03 --l2 .001]\n"
        "        Repeat paired --features-csv/--labels-csv to combine datasets.\n"
        "  labels --features-csv F --candidate-dir D --output L [--idr-only]\n"
        "  autolabel --labels-csv L --candidate-dir D --output L2 [--overwrite]\n");
}
int main(int argc,char **argv) {
    if (argc<2) { usage(); return 2; }
    if (!strcmp(argv[1],"--help")) { usage(); return 0; }
    const char *features[256],*labels_paths[256],*output=NULL,*directory=NULL;
    int nf=0,nl=0,idr=0,overwrite=0,epochs=3000; double lr=.03,l2=.001;
    for (int i=2;i<argc;++i) {
        const char *arg=argv[i];
        if (!strcmp(arg,"--idr-only")) idr=1;
        else if (!strcmp(arg,"--overwrite")) overwrite=1;
        else {
            if (++i==argc) goto bad;
            const char *value=argv[i];
            if (!strcmp(arg,"--features-csv")) { if (nf==256) goto bad; features[nf++]=value; }
            else if (!strcmp(arg,"--labels-csv")) { if (nl==256) goto bad; labels_paths[nl++]=value; }
            else if (!strcmp(arg,"--output") || !strcmp(arg,"--output-model")) output=value;
            else if (!strcmp(arg,"--candidate-dir")) directory=value;
            else {
                double number; if (!numeric(value,&number)) goto bad;
                if (!strcmp(arg,"--epochs") && number>=1 && number<=1000000 && number==floor(number)) epochs=(int)number;
                else if (!strcmp(arg,"--learning-rate") && number>0 && number<=1) lr=number;
                else if (!strcmp(arg,"--l2") && number>=0) l2=number;
                else goto bad;
            }
        }
    }
    if (!output) goto bad;
    int ok;
    if (!strcmp(argv[1],"train") && nf && nf==nl) ok=train(features,labels_paths,nf,output,epochs,lr,l2);
    else if (!strcmp(argv[1],"labels") && nf==1 && directory) ok=labels(features[0],directory,output,idr);
    else if (!strcmp(argv[1],"autolabel") && nl==1 && directory) ok=autolabel(labels_paths[0],directory,output,overwrite);
    else goto bad;
    if (!ok) fprintf(stderr,"scatterfix-model: operation failed (check paths, CSV columns and numeric values)\n");
    return ok ? 0 : 1;
bad:
    usage(); return 2;
}
