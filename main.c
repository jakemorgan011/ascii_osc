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

#define SCREEN_WIDTH 120
#define SCREEN_HEIGHT 35
#define LOG_LINES 8

// Screen buffer
char screen[SCREEN_HEIGHT][SCREEN_WIDTH][4];
float zbuffer[SCREEN_HEIGHT][SCREEN_WIDTH];
int text_mask[SCREEN_HEIGHT][SCREEN_WIDTH]; // 0 = 3D, 1 = text

// OSC message log
typedef struct {
    char address[32];
    char sound[32];
    int n;
    float cycle;
    float gain;
    int orbit;
    char timestamp[16];
    bool active;
} OscLog;

OscLog logs[LOG_LINES];
int log_index = 0;
int total_messages = 0;

static volatile bool keepRunning = true;

static void sigintHandler(int x) {
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
            text_mask[y][x] = 0;
        }
    }
}

void draw_text_into_scene(const char* text, int start_x, int y, int color_type) {
    int len = strlen(text);
    if (y < 0 || y >= SCREEN_HEIGHT || start_x >= SCREEN_WIDTH) return;
    
    // Shift everything to the right to make room for text
    for (int x = SCREEN_WIDTH - 1; x >= start_x + len; x--) {
        if (x - len >= 0) {
            strcpy(screen[y][x], screen[y][x - len]);
            zbuffer[y][x] = zbuffer[y][x - len];
            text_mask[y][x] = text_mask[y][x - len];
        }
    }
    
    // Insert the text
    for (int i = 0; i < len && (start_x + i) < SCREEN_WIDTH; i++) {
        if (start_x + i >= 0) {
            char buf[4];
            snprintf(buf, sizeof(buf), "%c", text[i]);
            strcpy(screen[y][start_x + i], buf);
            text_mask[y][start_x + i] = 1;
            zbuffer[y][start_x + i] = 1e10;
        }
    }
}

int get_text_offset(int x, int y) {
    return 0; // Not needed anymore since we're literally displacing
}

void display_screen() {
    printf("\033[2J\033[H");
    
    // Draw everything
    for (int y = 0; y < SCREEN_HEIGHT; y++) {
        for (int x = 0; x < SCREEN_WIDTH; x++) {
            if (text_mask[y][x] == 1) {
                // Black text
                printf("\033[33m%s\033[0m", screen[y][x]);
            } else if (strcmp(screen[y][x], "█") == 0) {
                // Red wireframe
                printf("\033[31m%s\033[0m", screen[y][x]);
            } else {
                printf("%s", screen[y][x]);
            }
        }
        putchar('\n');
    }
    
    // Bottom info
    printf("\033[37m");
    for (int i = 0; i < SCREEN_WIDTH; i++) printf("─");
    printf("\033[0m\n");
    printf("\033[31m▌\033[0m \033[37mOSC MESSAGES: %d\033[0m\n", total_messages);
}

void project(float x, float y, float z, int* sx, int* sy) {
    float scale = 20.0f;
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
            if (z > zbuffer[y0][x0] && text_mask[y0][x0] == 0) {
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

void add_osc_log(const char* address, tosc_message* msg) {
    if (!address || !msg) return;
    
    // First parse to get orbit number
    int target_orbit = 0;
    const char* format = tosc_getFormat(msg);
    if (!format) return;
    
    tosc_reset(msg);
    
    // Quick scan for orbit value
    for (int i = 0; format[i] != '\0' && i < 100; i++) {
        if (format[i] == 's') {
            const char* str = tosc_getNextString(msg);
            if (str && strcmp(str, "orbit") == 0 && i + 1 < 100 && format[i+1] == 'i') {
                target_orbit = tosc_getNextInt32(msg);
                break;
            }
        } else if (format[i] == 'i') {
            tosc_getNextInt32(msg);
        } else if (format[i] == 'f') {
            tosc_getNextFloat(msg);
        }
    }
    
    // Use orbit as the index (clamped to LOG_LINES)
    if (target_orbit >= LOG_LINES) target_orbit = LOG_LINES - 1;
    OscLog* log = &logs[target_orbit];
    
    strncpy(log->address, address, 31);
    log->address[31] = '\0';
    
    strcpy(log->sound, "???");
    log->n = 0;
    log->cycle = 0.0f;
    log->gain = 1.0f;
    log->orbit = target_orbit;
    log->active = true;
    
    tosc_reset(msg);
    
    // Parse all values
    int i = 0;
    while (format[i] != '\0' && i < 100) {
        if (format[i] == 's') {
            const char* str = tosc_getNextString(msg);
            if (str) {
                if (strcmp(str, "s") == 0 && i + 1 < 100 && format[i+1] == 's') {
                    const char* sound = tosc_getNextString(msg);
                    if (sound) {
                        strncpy(log->sound, sound, 31);
                        log->sound[31] = '\0';
                    }
                    i++;
                } else if (strcmp(str, "n") == 0 && i + 1 < 100 && format[i+1] == 'i') {
                    log->n = tosc_getNextInt32(msg);
                    i++;
                } else if (strcmp(str, "cycle") == 0 && i + 1 < 100 && format[i+1] == 'f') {
                    log->cycle = tosc_getNextFloat(msg);
                    i++;
                } else if (strcmp(str, "gain") == 0 && i + 1 < 100 && format[i+1] == 'f') {
                    log->gain = tosc_getNextFloat(msg);
                    i++;
                }
            }
        } else if (format[i] == 'i') {
            tosc_getNextInt32(msg);
        } else if (format[i] == 'f') {
            tosc_getNextFloat(msg);
        }
        i++;
    }
    
    snprintf(log->timestamp, 16, "%02d:%02d", 
             (total_messages / 60) % 60, 
             total_messages % 60);
    
    total_messages++;
}

int main(int argc, char *argv[]) {
    printf("ASCII OBJ + OSC Corrupted Renderer\n");
    printf("===================================\n\n");
    
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
    
    if (parsed_data.position_width != 3) {
        printf("ERROR: Position width is not 3!\n");
        free(buffer);
        free(obj_data);
        return 1;
    }
    
    if (parsed_data.face_width != 3 && parsed_data.face_width != 4) {
        printf("ERROR: Face width must be 3 or 4.\n");
        free(buffer);
        free(obj_data);
        return 1;
    }
    
    // OSC setup
    signal(SIGINT, &sigintHandler);
    const int fd = socket(AF_INET, SOCK_DGRAM, 0);
    fcntl(fd, F_SETFL, O_NONBLOCK);
    struct sockaddr_in sin;
    sin.sin_family = AF_INET;
    sin.sin_port = htons(9000);
    sin.sin_addr.s_addr = INADDR_ANY;
    bind(fd, (struct sockaddr*)&sin, sizeof(struct sockaddr_in));
    printf("Listening on port 9000\n");
    printf("Starting render...\n\n");
    
    // Initialize logs
    for (int i = 0; i < LOG_LINES; i++) {
        logs[i].active = false;
        logs[i].address[0] = '\0';
        logs[i].sound[0] = '\0';
        logs[i].orbit = i;
    }
    
    sleep(1);
    
    char osc_buffer[2048];
    float angle = 0.0f;
    
    while (keepRunning) {
        // Check for OSC messages
        int len = recvfrom(fd, osc_buffer, sizeof(osc_buffer), 0, NULL, NULL);
        if (len > 0) {
            tosc_message msg;
            if (tosc_parseMessage(&msg, osc_buffer, len) == 0) {
                add_osc_log(tosc_getAddress(&msg), &msg);
            }
        }
        
        clear_screen();
        
        // Render 3D model FIRST
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
        
        // Draw OSC messages OVER the 3D - one line per orbit
        for (int i = 0; i < LOG_LINES; i++) {
            if (logs[i].active) {
                int text_y = 3 + (i * 3);
                int x_pos = 5;
                
                // Orbit number
                char orbit_buf[8];
                snprintf(orbit_buf, sizeof(orbit_buf), "[%d]", logs[i].orbit);
                draw_text_into_scene(orbit_buf, x_pos, text_y, 1);
                x_pos += 5;
                
                // Sound name
                draw_text_into_scene(logs[i].sound, x_pos, text_y, 1);
                x_pos += 15;
                
                // n value
                char n_buf[16];
                snprintf(n_buf, sizeof(n_buf), "n:%d", logs[i].n);
                draw_text_into_scene(n_buf, x_pos, text_y, 1);
                x_pos += 8;
                
                // cycle as progress bar
                float cycle_frac = logs[i].cycle - (int)logs[i].cycle;
                int bar_length = 10;
                int filled = (int)(cycle_frac * bar_length);
                
                char cyc_buf[64] = "[";
                for (int b = 0; b < bar_length; b++) {
                    if (b < filled) {
                        strcat(cyc_buf, "#");
                    } else {
                        strcat(cyc_buf, "-");
                    }
                }
                strcat(cyc_buf, "]");
                draw_text_into_scene(cyc_buf, x_pos, text_y, 1);
                x_pos += 13;
                
                // gain
                char gain_buf[16];
                snprintf(gain_buf, sizeof(gain_buf), "g:%.2f", logs[i].gain);
                draw_text_into_scene(gain_buf, x_pos, text_y, 1);
            }
        }
        
        display_screen();
        angle += 0.02f;
        
        usleep(16666); // ~60 FPS
    }
    
    close(fd);
    free(buffer);
    free(obj_data);
    
    return 0;
}
