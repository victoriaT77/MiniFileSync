#include "common.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <semaphore.h>
#include <signal.h>

#define MAX_ARCHIVOS 1000
#define NUM_WORKERS 2 

// Declaraciones de funciones de los otros modulos
int escanear_directorio(const char *directorio_origen, struct FileMetadata *lista, int max_archivos);
void ejecutar_worker(int id, int pipe_lectura);
void ejecutar_logger();

struct stats *shared_stats = NULL;
sem_t *sem = NULL;

void asegurar_directorio_backup() {
    struct stat st = {0};
    if (stat("backup", &st) == -1) {
        mkdir("backup", 0755);
    }
}

void inicializar_shm_y_sem() {
    shm_unlink(SHM_NAME);
    int shm_fd = shm_open(SHM_NAME, O_CREAT | O_RDWR, 0666);
    if (shm_fd < 0) {
        perror("Error shm_open");
        exit(EXIT_FAILURE);
    }
    ftruncate(shm_fd, sizeof(struct stats));
    
    shared_stats = (struct stats *)mmap(NULL, sizeof(struct stats), PROT_READ | PROT_WRITE, MAP_SHARED, shm_fd, 0);
    if (shared_stats == MAP_FAILED) {
        perror("Error mmap");
        exit(EXIT_FAILURE);
    }

    shared_stats->archivos_copiados = 0;
    shared_stats->bytes_copiados = 0;
    shared_stats->errores = 0;

    sem_unlink(SEM_NAME);
    sem = sem_open(SEM_NAME, O_CREAT, 0666, 1);
    if (sem == SEM_FAILED) {
        perror("Error sem_open");
        exit(EXIT_FAILURE);
    }
}

void convertirse_en_demonio() {
    pid_t pid = fork();
    if (pid < 0) exit(EXIT_FAILURE);
    if (pid > 0) exit(EXIT_SUCCESS); 

    if (setsid() < 0) exit(EXIT_FAILURE);

    signal(SIGCHLD, SIG_IGN);
    signal(SIGHUP, SIG_IGN);

    pid = fork();
    if (pid < 0) exit(EXIT_FAILURE);
    if (pid > 0) exit(EXIT_SUCCESS); 

    close(STDIN_FILENO);
    close(STDOUT_FILENO);
    close(STDERR_FILENO);
}

int archivo_requiere_sincronizacion(struct FileMetadata *actual, struct FileMetadata *anteriores, int total_anteriores) {
    for (int i = 0; i < total_anteriores; i++) {
        if (strcmp(actual->ruta, anteriores[i].ruta) == 0) {
            if (actual->size != anteriores[i].size || actual->modification_time != anteriores[i].modification_time) {
                return 1; 
            }
            return 0; 
        }
    }
    return 1; 
}

int main(int argc, char *argv[]) {
    if (argc < 2) {
        fprintf(stderr, "USO: %s <directorio_origen> [daemon]\n", argv[0]);
        return 1;
    }

    const char *directorio_origen = argv[1];
    int modo_daemon = (argc == 3 && strcmp(argv[2], "daemon") == 0);

    asegurar_directorio_backup();
    inicializar_shm_y_sem();

    pid_t logger_pid = fork();
    if (logger_pid == 0) {
        ejecutar_logger();
        exit(EXIT_SUCCESS);
    }

    if (modo_daemon) {
        convertirse_en_demonio();
    }

    struct FileMetadata *historial_anterior = malloc(sizeof(struct FileMetadata) * MAX_ARCHIVOS);
    struct FileMetadata *historial_actual = malloc(sizeof(struct FileMetadata) * MAX_ARCHIVOS);
    int total_anteriores = 0;

    while (1) {
        int total_actual = escanear_directorio(directorio_origen, historial_actual, MAX_ARCHIVOS);
        struct FileMetadata archivos_por_copiar[MAX_ARCHIVOS];
        int total_por_copiar = 0;

        for (int i = 0; i < total_actual; i++) {
            if (archivo_requiere_sincronizacion(&historial_actual[i], historial_anterior, total_anteriores)) {
                archivos_por_copiar[total_por_copiar++] = historial_actual[i];
            }
        }

        if (total_por_copiar > 0) {
            int pipes[NUM_WORKERS][2];
            pid_t workers_pids[NUM_WORKERS];

            for (int i = 0; i < NUM_WORKERS; i++) {
                if (pipe(pipes[i]) == -1) {
                    perror("Error al crear pipe");
                }
                
                workers_pids[i] = fork();
                if (workers_pids[i] == 0) {
                    close(pipes[i][1]);
                    free(historial_anterior);
                    free(historial_actual);
                    ejecutar_worker(i + 1, pipes[i][0]);
                    exit(EXIT_SUCCESS);
                } else {
                    close(pipes[i][0]);
                }
            }

            // Distribucion Round-Robin con comando explicito "COPIAR <ruta>"
            for (int i = 0; i < total_por_copiar; i++) {
                int worker_asignado = i % NUM_WORKERS;
                char comando_tarea[MAX_PATH + 16];
                snprintf(comando_tarea, sizeof(comando_tarea), "COPIAR %s", archivos_por_copiar[i].ruta);
                write(pipes[worker_asignado][1], comando_tarea, MAX_PATH);
            }

            for (int i = 0; i < NUM_WORKERS; i++) {
                write(pipes[i][1], "FIN", MAX_PATH);
                close(pipes[i][1]); 
            }

            for (int i = 0; i < NUM_WORKERS; i++) {
                waitpid(workers_pids[i], NULL, 0);
            }

            if (!modo_daemon) {
                sem_wait(sem);
                printf("\n--- ESTADISTICAS DE SINCRONIZACION ---\n");
                printf("Archivos copiados con exito: %ld\n", shared_stats->archivos_copiados);
                printf("Bytes transferidos: %ld bytes\n", shared_stats->bytes_copiados);
                printf("Errores registrados: %ld\n", shared_stats->errores);
                printf("--------------------------------------\n\n");
                sem_post(sem);
            }
        }

        memcpy(historial_anterior, historial_actual, sizeof(struct FileMetadata) * total_actual);
        total_anteriores = total_actual;
        sleep(5);
    }

    free(historial_anterior);
    free(historial_actual);
    sem_close(sem);
    munmap(shared_stats, sizeof(struct stats));
    return 0;
}