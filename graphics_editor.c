/*
 * ╔══════════════════════════════════════════════════════════╗
 * ║        ASCII 2D GRAPHICS EDITOR  — gfx_canvas.c         ║
 * ║  Shapes: Circle · Rectangle · Line · Triangle           ║
 * ║  Canvas stored as 2-D char array; rendered with * and _ ║
 * ╚══════════════════════════════════════════════════════════╝
 *
 * UNIQUE DESIGN DECISIONS
 * ───────────────────────
 *  • Every shape is stored in an "object registry" (array of
 *    ShapeNode) with a sequential numeric ID, making add /
 *    delete / modify O(n) but easy to audit.
 *  • The canvas is NEVER drawn to during editing — it is
 *    re-rasterised from scratch each time "display" is called
 *    (painter's algorithm, back-to-front by insertion order).
 *  • A tiny REPL loop parses typed commands; no menus, no
 *    ncurses — pure portable C99.
 *  • Bresenham's circle & line algorithms used for accuracy.
 *  • Fill character for shape interiors : '_'
 *    Outline / border character          : '*'
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

/* ── canvas dimensions ─────────────────────────────────── */
#define ROWS  25
#define COLS  60

/* ── shape-type tags ───────────────────────────────────── */
typedef enum { SH_CIRCLE = 1, SH_RECT, SH_LINE, SH_TRIANGLE } ShapeKind;

/* ── generic parameter block (union-style flat struct) ─── */
typedef struct {
    int id;          /* assigned on insertion, 1-based      */
    ShapeKind kind;
    int filled;      /* 1 = fill interior with '_'          */
    /* circle : cx cy radius                                 */
    /* rect   : x1 y1 x2 y2                                 */
    /* line   : x1 y1 x2 y2                                 */
    /* triangle: x1 y1  x2 y2  x3 y3                        */
    int p[6];
} Shape;

/* ── registry ──────────────────────────────────────────── */
#define MAX_SHAPES 64
static Shape registry[MAX_SHAPES];
static int   reg_count = 0;

/* ── canvas buffer ─────────────────────────────────────── */
static char canvas[ROWS][COLS];

/* ══════════════════════════════════════════════════════════
 *  LOW-LEVEL CANVAS HELPERS
 * ══════════════════════════════════════════════════════════ */

static void canvas_clear(void) {
    for (int r = 0; r < ROWS; r++)
        for (int c = 0; c < COLS; c++)
            canvas[r][c] = ' ';
}

/* safe pixel setter — silently clips out-of-bounds */
static void put_pixel(int r, int c, char ch) {
    if (r >= 0 && r < ROWS && c >= 0 && c < COLS)
        canvas[r][c] = ch;
}

/* ══════════════════════════════════════════════════════════
 *  RASTERISERS  (work directly on the canvas buffer)
 * ══════════════════════════════════════════════════════════ */

/* ── Bresenham line ────────────────────────────────────── */
static void raster_line(int x1, int y1, int x2, int y2, char ch) {
    int dx = abs(x2 - x1), dy = abs(y2 - y1);
    int sx = (x1 < x2) ? 1 : -1;
    int sy = (y1 < y2) ? 1 : -1;
    int err = dx - dy;
    while (1) {
        put_pixel(y1, x1, ch);
        if (x1 == x2 && y1 == y2) break;
        int e2 = 2 * err;
        if (e2 > -dy) { err -= dy; x1 += sx; }
        if (e2 <  dx) { err += dx; y1 += sy; }
    }
}

/* ── Bresenham circle outline ──────────────────────────── */
static void circle_outline_points(int cx, int cy, int x, int y) {
    put_pixel(cy + y, cx + x, '*'); put_pixel(cy - y, cx + x, '*');
    put_pixel(cy + y, cx - x, '*'); put_pixel(cy - y, cx - x, '*');
    put_pixel(cy + x, cx + y, '*'); put_pixel(cy - x, cx + y, '*');
    put_pixel(cy + x, cx - y, '*'); put_pixel(cy - x, cx - y, '*');
}

static void raster_circle(int cx, int cy, int r, int filled) {
    if (filled) {
        /* horizontal scanlines inside circle */
        for (int row = cy - r; row <= cy + r; row++) {
            int dy  = row - cy;
            int len = (int)sqrt((double)(r * r - dy * dy));
            for (int col = cx - len; col <= cx + len; col++)
                put_pixel(row, col, '_');
        }
    }
    /* always draw outline on top */
    int x = 0, y = r, d = 3 - 2 * r;
    while (x <= y) {
        circle_outline_points(cx, cy, x, y);
        if (d < 0) d += 4 * x + 6;
        else { d += 4 * (x - y) + 10; y--; }
        x++;
    }
}

/* ── rectangle ─────────────────────────────────────────── */
static void raster_rect(int x1, int y1, int x2, int y2, int filled) {
    /* normalise */
    if (x1 > x2) { int t = x1; x1 = x2; x2 = t; }
    if (y1 > y2) { int t = y1; y1 = y2; y2 = t; }
    if (filled)
        for (int r = y1 + 1; r < y2; r++)
            for (int c = x1 + 1; c < x2; c++)
                put_pixel(r, c, '_');
    /* borders */
    raster_line(x1, y1, x2, y1, '*');
    raster_line(x1, y2, x2, y2, '*');
    raster_line(x1, y1, x1, y2, '*');
    raster_line(x2, y1, x2, y2, '*');
}

/* ── triangle ──────────────────────────────────────────── */
static void raster_triangle(int x1,int y1, int x2,int y2,
                             int x3,int y3, int filled) {
    if (filled) {
        /* scanline fill using edge sorting */
        int minY = y1, maxY = y1;
        if (y2 < minY) minY = y2; if (y2 > maxY) maxY = y2;
        if (y3 < minY) minY = y3; if (y3 > maxY) maxY = y3;
        for (int row = minY; row <= maxY; row++) {
            /* find intersections with the three edges */
            int xs[3]; int nx = 0;
            /* helper lambda via macro */
#define EDGE_X(ax,ay,bx,by) \
    if ((ay<=row&&row<by)||(by<=row&&row<ay)) { \
        xs[nx++]=(ax)+(int)(((double)((row)-(ay))/((by)-(ay)))*((bx)-(ax))); }
            EDGE_X(x1,y1,x2,y2)
            EDGE_X(x2,y2,x3,y3)
            EDGE_X(x3,y3,x1,y1)
#undef EDGE_X
            if (nx >= 2) {
                int lo = xs[0], hi = xs[1];
                if (lo > hi) { int t=lo; lo=hi; hi=t; }
                for (int c = lo; c <= hi; c++) put_pixel(row, c, '_');
            }
        }
    }
    raster_line(x1,y1,x2,y2,'*');
    raster_line(x2,y2,x3,y3,'*');
    raster_line(x3,y3,x1,y1,'*');
}

/* ══════════════════════════════════════════════════════════
 *  DISPATCH: rasterise one Shape onto the canvas
 * ══════════════════════════════════════════════════════════ */
static void render_shape(const Shape *s) {
    switch (s->kind) {
    case SH_CIRCLE:
        raster_circle(s->p[0], s->p[1], s->p[2], s->filled);
        break;
    case SH_RECT:
        raster_rect(s->p[0], s->p[1], s->p[2], s->p[3], s->filled);
        break;
    case SH_LINE:
        raster_line(s->p[0], s->p[1], s->p[2], s->p[3], '*');
        break;
    case SH_TRIANGLE:
        raster_triangle(s->p[0],s->p[1],s->p[2],s->p[3],
                        s->p[4],s->p[5], s->filled);
        break;
    }
}

/* ══════════════════════════════════════════════════════════
 *  REGISTRY: add / delete / modify
 * ══════════════════════════════════════════════════════════ */

/* returns pointer to new slot or NULL if full */
static Shape *reg_add(ShapeKind k, int filled, int params[]) {
    if (reg_count >= MAX_SHAPES) { puts("Registry full!"); return NULL; }
    Shape *s = &registry[reg_count++];
    s->id     = reg_count;           /* 1-based sequential ID */
    s->kind   = k;
    s->filled = filled;
    memcpy(s->p, params, sizeof(s->p));
    printf("  Added shape ID %d\n", s->id);
    return s;
}

/* delete by ID — fills gap by shifting tail down */
static int reg_delete(int id) {
    for (int i = 0; i < reg_count; i++) {
        if (registry[i].id == id) {
            memmove(&registry[i], &registry[i+1],
                    sizeof(Shape) * (reg_count - i - 1));
            reg_count--;
            printf("  Deleted shape ID %d\n", id);
            return 1;
        }
    }
    printf("  Shape ID %d not found.\n", id);
    return 0;
}

/* modify: find by ID, keep kind, replace params */
static int reg_modify(int id, int filled, int params[]) {
    for (int i = 0; i < reg_count; i++) {
        if (registry[i].id == id) {
            registry[i].filled = filled;
            memcpy(registry[i].p, params, sizeof(registry[i].p));
            printf("  Modified shape ID %d\n", id);
            return 1;
        }
    }
    printf("  Shape ID %d not found.\n", id);
    return 0;
}

/* ══════════════════════════════════════════════════════════
 *  DISPLAY  — re-rasterises from registry each call
 * ══════════════════════════════════════════════════════════ */
static void display_canvas(void) {
    canvas_clear();
    for (int i = 0; i < reg_count; i++)
        render_shape(&registry[i]);

    /* top border */
    putchar('+');
    for (int c = 0; c < COLS; c++) putchar('-');
    puts("+");

    for (int r = 0; r < ROWS; r++) {
        putchar('|');
        for (int c = 0; c < COLS; c++) putchar(canvas[r][c]);
        puts("|");
    }

    /* bottom border */
    putchar('+');
    for (int c = 0; c < COLS; c++) putchar('-');
    puts("+");
    printf("  Shapes in registry: %d\n", reg_count);
}

/* ══════════════════════════════════════════════════════════
 *  LIST registry contents
 * ══════════════════════════════════════════════════════════ */
static const char *kind_name(ShapeKind k) {
    switch (k) {
    case SH_CIRCLE:   return "circle";
    case SH_RECT:     return "rect";
    case SH_LINE:     return "line";
    case SH_TRIANGLE: return "triangle";
    default:          return "?";
    }
}

static void list_shapes(void) {
    if (reg_count == 0) { puts("  No shapes."); return; }
    for (int i = 0; i < reg_count; i++) {
        Shape *s = &registry[i];
        printf("  [%d] %-8s filled=%d  params=(%d %d %d %d %d %d)\n",
               s->id, kind_name(s->kind), s->filled,
               s->p[0],s->p[1],s->p[2],s->p[3],s->p[4],s->p[5]);
    }
}

/* ══════════════════════════════════════════════════════════
 *  HELP
 * ══════════════════════════════════════════════════════════ */
static void print_help(void) {
    puts("\n  ── Commands ──────────────────────────────────────────");
    puts("  circle  cx cy r  [fill]         add a circle");
    puts("  rect    x1 y1 x2 y2 [fill]      add a rectangle");
    puts("  line    x1 y1 x2 y2             add a line");
    puts("  tri     x1 y1 x2 y2 x3 y3 [fill] add a triangle");
    puts("  del     id                      delete a shape");
    puts("  mod     id [fill] p0..p5        modify a shape");
    puts("  list                            show registry");
    puts("  show                            render & display");
    puts("  demo                            load demo scene");
    puts("  clear                           clear registry");
    puts("  help                            this message");
    puts("  quit                            exit");
    puts("  (fill = 1 for filled, 0 for outline only)");
    puts("  ──────────────────────────────────────────────────────\n");
}

/* ══════════════════════════════════════════════════════════
 *  DEMO SCENE
 * ══════════════════════════════════════════════════════════ */
static void load_demo(void) {
    reg_count = 0;
    int p[6];

    /* filled circle centre */
    p[0]=29; p[1]=12; p[2]=5;  memset(p+3,0,12); reg_add(SH_CIRCLE,  1, p);
    /* outline rectangle frame */
    p[0]=2;  p[1]=2;  p[2]=57; p[3]=22; memset(p+4,0,8); reg_add(SH_RECT, 0, p);
    /* diagonal line */
    p[0]=2;  p[1]=2;  p[2]=57; p[3]=22; reg_add(SH_LINE, 0, p);
    /* filled triangle */
    p[0]=5;  p[1]=20; p[2]=15; p[3]=5; p[4]=25; p[5]=20; reg_add(SH_TRIANGLE, 1, p);
    /* small outline circle */
    p[0]=48; p[1]=6;  p[2]=4;  memset(p+3,0,12); reg_add(SH_CIRCLE, 0, p);

    puts("  Demo scene loaded (5 shapes). Type 'show' to display.");
}

/* ══════════════════════════════════════════════════════════
 *  REPL  (Read-Eval-Print Loop)
 * ══════════════════════════════════════════════════════════ */
int main(void) {
    char line[256];
    int  p[6];
    int  fill, id;

    puts("\n  ╔══════════════════════════════════════╗");
    puts("  ║  ASCII 2-D GRAPHICS EDITOR  v1.0    ║");
    puts("  ║  Symbols: outline=*  fill=_         ║");
    puts("  ╚══════════════════════════════════════╝");
    puts("  Type 'help' for commands, 'demo' to see a sample scene.\n");

    while (1) {
        printf("gfx> ");
        fflush(stdout);

        if (!fgets(line, sizeof(line), stdin)) break;
        /* strip newline */
        line[strcspn(line, "\n")] = '\0';

        /* ── tokenise first word ── */
        char cmd[32] = {0};
        sscanf(line, "%31s", cmd);

        if      (strcmp(cmd,"quit")==0 || strcmp(cmd,"q")==0) break;
        else if (strcmp(cmd,"help")==0)  print_help();
        else if (strcmp(cmd,"show")==0)  display_canvas();
        else if (strcmp(cmd,"list")==0)  list_shapes();
        else if (strcmp(cmd,"demo")==0)  load_demo();
        else if (strcmp(cmd,"clear")==0){ reg_count=0; puts("  Registry cleared."); }

        else if (strcmp(cmd,"circle")==0) {
            memset(p,0,sizeof(p));
            fill=0;
            sscanf(line,"%*s %d %d %d %d",&p[0],&p[1],&p[2],&fill);
            reg_add(SH_CIRCLE, fill, p);
        }
        else if (strcmp(cmd,"rect")==0) {
            memset(p,0,sizeof(p));
            fill=0;
            sscanf(line,"%*s %d %d %d %d %d",&p[0],&p[1],&p[2],&p[3],&fill);
            reg_add(SH_RECT, fill, p);
        }
        else if (strcmp(cmd,"line")==0) {
            memset(p,0,sizeof(p));
            sscanf(line,"%*s %d %d %d %d",&p[0],&p[1],&p[2],&p[3]);
            reg_add(SH_LINE, 0, p);
        }
        else if (strcmp(cmd,"tri")==0) {
            memset(p,0,sizeof(p));
            fill=0;
            sscanf(line,"%*s %d %d %d %d %d %d %d",
                   &p[0],&p[1],&p[2],&p[3],&p[4],&p[5],&fill);
            reg_add(SH_TRIANGLE, fill, p);
        }
        else if (strcmp(cmd,"del")==0) {
            id=0; sscanf(line,"%*s %d",&id);
            reg_delete(id);
        }
        else if (strcmp(cmd,"mod")==0) {
            id=0; fill=0; memset(p,0,sizeof(p));
            sscanf(line,"%*s %d %d %d %d %d %d %d %d",
                   &id,&fill,&p[0],&p[1],&p[2],&p[3],&p[4],&p[5]);
            reg_modify(id, fill, p);
        }
        else if (strlen(cmd) > 0) {
            printf("  Unknown command '%s'. Type 'help'.\n", cmd);
        }
    }

    puts("\n  Goodbye.");
    return 0;
}
