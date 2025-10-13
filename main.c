// slop

#include <arpa/inet.h>
#include <sys/select.h>
#include <fcntl.h>
#include <signal.h>
#include <stdbool.h>
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#include "objpar.h"
#include "tinyosc.h"

#define SCREEN_WIDTH 80
#define SCREEN_HEIGHT 40

// Screen buffer - store strings for UTF-8 support
char screen[SCREEN_HEIGHT][SCREEN_WIDTH][4];
float zbuffer[SCREEN_HEIGHT][SCREEN_WIDTH];

static volatile bool keepRunning = true;

static void sigintHandler(int x){
  keepRunning = false;
}

char* read_file(const char* filename, unsigned int* out_size) {
    FILE* file = fopen(filename, "rb");
    if (!file) {
        printf("Error: Could not open file '%s'\n", filename);
        return NULL;
    }
    
    fseek(file, 0, SEEK_END);
    unsigned int size = ftell(file);
    fseek(file, 0, SEEK_SET);
    
    char* buffer = (char*)malloc(size + 1);
    if (!buffer) {
        printf("Error: Could not allocate memory\n");
        fclose(file);
        return NULL;
    }
    
    fread(buffer, 1, size, file);
    buffer[size] = '\0';
    fclose(file);
    
    *out_size = size;
    return buffer;
}

void clear_screen() {
    for (int y = 0; y < SCREEN_HEIGHT; y++) {
        for (int x = 0; x < SCREEN_WIDTH; x++) {
            strcpy(screen[y][x], " ");
            zbuffer[y][x] = -1e10;
        }
    }
}

void display_screen() {
    printf("\033[2J\033[H");
    for (int y = 0; y < SCREEN_HEIGHT; y++) {
        for (int x = 0; x < SCREEN_WIDTH; x++) {
            // [31m for red.
            if (strcmp(screen[y][x], "█") == 0) {
                printf("\033[31m%s\033[0m", screen[y][x]); // Cyan
            } else {
                printf("%s", screen[y][x]);
            }
        }
        putchar('\n');
    }
}

void project(float x, float y, float z, int* sx, int* sy) {
    float scale = 15.0f;
    float distance = 4.0f;
    float factor = scale / (z + distance);
    *sx = (int)(x * factor) + SCREEN_WIDTH / 2;
    *sy = (int)(y * factor) + SCREEN_HEIGHT / 2;
}

void draw_line(int x0, int y0, float z0, int x1, int y1, float z1, const char* ch) {
    int dx = abs(x1 - x0);
    int dy = abs(y1 - y0);
    int sx = x0 < x1 ? 1 : -1;
    int sy = y0 < y1 ? 1 : -1;
    int err = dx - dy;
    
    float z = z0;
    float dz = (z1 - z0) / (float)(dx + dy + 1);
    
    while (1) {
        if (x0 >= 0 && x0 < SCREEN_WIDTH && y0 >= 0 && y0 < SCREEN_HEIGHT) {
            if (z > zbuffer[y0][x0]) {
                strcpy(screen[y0][x0], ch);
                zbuffer[y0][x0] = z;
            }
        }
        
        if (x0 == x1 && y0 == y1) break;
        
        int e2 = 2 * err;
        if (e2 > -dy) {
            err -= dy;
            x0 += sx;
        }
        if (e2 < dx) {
            err += dx;
            y0 += sy;
        }
        z += dz;
    }
}

void rotate_y(float* x, float* y, float* z, float angle) {
    float cos_a = cos(angle);
    float sin_a = sin(angle);
    float nx = *x * cos_a + *z * sin_a;
    float nz = -*x * sin_a + *z * cos_a;
    *x = nx;
    *z = nz;
}

void rotate_x(float* x, float* y, float* z, float angle) {
    float cos_a = cos(angle);
    float sin_a = sin(angle);
    float ny = *y * cos_a - *z * sin_a;
    float nz = *y * sin_a + *z * cos_a;
    *y = ny;
    *z = nz;
}

int main(int argc, char *argv[]) {
    printf("ASCII OBJ Renderer\n");
    printf("==================\n\n");
    
    if (argc < 2) {
        printf("Usage: %s <objfile.obj>\n", argv[0]);
        return 1;
    }
    
    const char* filename = argv[1];
    printf("Loading: %s\n", filename);
    
    unsigned int file_size;
    char* obj_data = read_file(filename, &file_size);
    if (!obj_data) return 1;
    
    unsigned int buffer_size = objpar_get_size(obj_data, file_size);
    if (buffer_size == 0) {
        printf("Error: Invalid OBJ file\n");
        free(obj_data);
        return 1;
    }
    
    void* buffer = malloc(buffer_size);
    if (!buffer) {
        printf("Error: Could not allocate buffer\n");
        free(obj_data);
        return 1;
    }
    
    struct objpar_data parsed_data;
    unsigned int result = objpar(obj_data, file_size, buffer, &parsed_data);
    
    if (!result) {
        printf("Failed to parse OBJ\n");
        free(buffer);
        free(obj_data);
        return 1;
    }

    printf("Vertices: %u, Faces: %u\n", parsed_data.position_count, parsed_data.face_count);
    printf("Position width: %u (should be 3)\n", parsed_data.position_width);
    printf("Face width: %u (should be 3)\n", parsed_data.face_width);
    
    if (parsed_data.position_width != 3) {
        printf("ERROR: Position width is not 3! Your OBJ file has invalid vertex data.\n");
        free(buffer);
        free(obj_data);
        return 1;
    }
    
    if (parsed_data.face_width != 3 && parsed_data.face_width != 4) {
        printf("ERROR: Face width must be 3 (triangles) or 4 (quads).\n");
        free(buffer);
        free(obj_data);
        return 1;
    }

    // osc init.
    char o_buffer[2048]; // 2kb buffer to read packet data into

    printf("starting quick write tests:\n");
    uint32_t len = 0;
    char blob[8] = {0x01, 0x23, 0x45, 0x67, 0xAB, 0xCD, 0xEF}; // not really sure what these do but documentation has them in here for some reason.
    len = tosc_writeMessage(o_buffer, sizeof(o_buffer), "/address", "fsibTFNI", 1.0f, "hai", -1, sizeof(blob), blob);
    tosc_printOscBuffer(o_buffer, len);
    printf("done.\n");
    signal(SIGINT, &sigintHandler);
    //
    const int fd = socket(AF_INET, SOCK_DGRAM, 0);
    fcntl(fd, F_SETFL, O_NONBLOCK);
    struct sockaddr_in sin;
    sin.sin_family = AF_INET;
    sin.sin_port = htons(9000);
    sin.sin_addr.s_addr = INADDR_ANY;
    bind(fd, (struct sockaddr *) &sin, sizeof(struct sockaddr_in));
    printf("tinyosc listening on port 9000.\n");
    //
    printf("Rendering... (Ctrl+C to exit)\n\n");
    //
    //TODO: i'm not gonna add the osc parsing yet because it seems a little difficult and i'm sleepy.
    float angle = 0.0f;
    while(keepRunning) {
        clear_screen();
        
        for (unsigned int i = 0; i < parsed_data.face_count; i++) {
            unsigned int face_offset = i * parsed_data.face_width * 3;
            unsigned int num_edges = parsed_data.face_width;
            
            for (unsigned int edge = 0; edge < num_edges; edge++) {
                unsigned int v0_idx = parsed_data.p_faces[face_offset + edge * 3] - 1;
                unsigned int v1_idx = parsed_data.p_faces[face_offset + ((edge + 1) % num_edges) * 3] - 1;
                
                float x0 = parsed_data.p_positions[v0_idx * parsed_data.position_width];
                float y0 = parsed_data.p_positions[v0_idx * parsed_data.position_width + 1];
                float z0 = parsed_data.p_positions[v0_idx * parsed_data.position_width + 2];
                
                float x1 = parsed_data.p_positions[v1_idx * parsed_data.position_width];
                float y1 = parsed_data.p_positions[v1_idx * parsed_data.position_width + 1];
                float z1 = parsed_data.p_positions[v1_idx * parsed_data.position_width + 2];
                
                rotate_y(&x0, &y0, &z0, angle);
                rotate_x(&x0, &y0, &z0, angle * 0.7f);
                rotate_y(&x1, &y1, &z1, angle);
                rotate_x(&x1, &y1, &z1, angle * 0.7f);
                
                int sx0, sy0, sx1, sy1;
                project(x0, y0, z0, &sx0, &sy0);
                project(x1, y1, z1, &sx1, &sy1);
                
                draw_line(sx0, sy0, z0, sx1, sy1, z1, "█");
            }
        }
        
        display_screen();
        angle += 0.01f;
        
        for (volatile int d = 0; d < 10000000; d++);
    }
    
    free(buffer);
    free(obj_data);
    
    return 0;
}
