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

// Declaraciones de funciones que estan en los otros archivos compilados juntos
int escanear_directorio(const char *directorio_origen, struct FileMetadata *lista, int max_archivos);
void ejecutar_worker(int id, int pipe_lectura);
void ejecutar_logger();

// Variables globales para facilitar la limpieza ante senales
struct stats *shared_stats = NULL;
sem_t *sem = NULL;

// Asegurar que la carpeta backup/ exista antes de iniciar
void asegurar_directorio_backup() {
    struct stat st = {0};
    if (stat("backup", &st) == -1) {
        mkdir("backup", 0755);
    }
}

// Inicializar la memoria compartida de estadisticas
void inicializar_shm_y_sem() {
    // Crear y truncar la Memoria Compartida
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

    // Inicializar contadores a 0
    shared_stats->archivos_copiados = 0;
    shared_stats->bytes_copiados = 0;
    shared_stats->errores = 0;

    // Crear Semaforo POSIX
    sem_unlink(SEM_NAME);
    sem = sem_open(SEM_NAME, O_CREAT, 0666, 1);
    if (sem == SEM_FAILED) {
        perror("Error sem_open");
        exit(EXIT_FAILURE);
    }
}

// Convertir el proceso en un Demonio (Daemon)
void convertirse_en_demonio() {
    pid_t pid = fork();
    if (pid < 0) exit(EXIT_FAILURE);
    if (pid > 0) exit(EXIT_SUCCESS); // El padre original muere

    // Crear una nueva sesion
    if (setsid() < 0) exit(EXIT_FAILURE);

    // Ignorar señales de control de terminal
    signal(SIGCHLD, SIG_IGN);
    signal(SIGHUP, SIG_IGN);

    pid = fork();
    if (pid < 0) exit(EXIT_FAILURE);
    if (pid > 0) exit(EXIT_SUCCESS); // El primer hijo muere para asegurar que no se re-adquiera terminal

    // Cambiar al directorio raiz (o mantener el actual segun necesidades de prueba)
    // chdir("/");

    // Cerrar descriptores de Entrada/Salida estandar para no interferir con la terminal
    close(STDIN_FILENO);
    close(STDOUT_FILENO);
    close(STDERR_FILENO);
}

// Buscar si un archivo ya existia en el escaneo anterior y ver si cambio
int archivo_requiere_sincronizacion(struct FileMetadata *actual, struct FileMetadata *anteriores, int total_anteriores) {
    for (int i = 0; i < total_anteriores; i++) {
        if (strcmp(actual->ruta, anteriores[i].ruta) == 0) {
            // El archivo ya existia: verificar tamaño y fecha de modificacion (Sincronizacion Incremental)
            if (actual->size != anteriores[i].size || actual->modification_time != anteriores[i].modification_time) {
                return 1; // Cambio de metadatos, requiere copia
            }
            return 0; // Sigue igual, no se copia
        }
    }
    return 1; // Es un archivo nuevo, requiere copia
}

int main(int argc, char *argv[]) {
    // Validar argumentos del comando scan <directorio> (Adicionalmente opcion daemon)
    if (argc < 2) {
        fprintf(stderr, "USO: %s <directorio_origen> [daemon]\n", argv[0]);
        return 1;
    }

    const char *directorio_origen = argv[1];
    int modo_daemon = (argc == 3 && strcmp(argv[2], "daemon") == 0);

    asegurar_directorio_backup();
    inicializar_shm_y_sem();

    // 1. Lanzar el proceso LOGGER independiente
    pid_t logger_pid = fork();
    if (logger_pid == 0) {
        ejecutar_logger();
        exit(EXIT_SUCCESS);
    }

    // Si se especifico, convertir el Monitor en un demonio en segundo plano
    if (modo_daemon) {
        convertirse_en_demonio();
    }

    // Estructuras para almacenar los metadatos de los archivos (ciclo actual y ciclo previo)
    struct FileMetadata *historial_anterior = malloc(sizeof(struct FileMetadata) * MAX_ARCHIVOS);
    struct FileMetadata *historial_actual = malloc(sizeof(struct FileMetadata) * MAX_ARCHIVOS);
    int total_anteriores = 0;

    printf("[Monitor] Iniciando sincronizacion sobre '%s' cada 5 segundos...\n", directorio_origen);

    // Bucle infinito del monitor
    while (1) {
        // Escanear el directorio recursivamente
        int total_actual = escanear_directorio(directorio_origen, historial_actual, MAX_ARCHIVOS);

        // Detectar que archivos necesitan sincronizarse
        struct FileMetadata archivos_por_copiar[MAX_ARCHIVOS];
        int total_por_copiar = 0;

        for (int i = 0; i < total_actual; i++) {
            if (archivo_requiere_sincronizacion(&historial_actual[i], historial_anterior, total_anteriores)) {
                archivos_por_copiar[total_por_copiar++] = historial_actual[i];
            }
        }

        // Si hay trabajo por hacer, creamos los Workers y distribuimos la carga
        if (total_por_copiar > 0) {
            int pipes[NUM_WORKERS][2];
            pid_t workers_pids[NUM_WORKERS];

            // Crear los pipes e hilos/procesos workers
            for (int i = 0; i < NUM_WORKERS; i++) {
                if (pipe(pipes[i]) == -1) {
                    perror("Error al crear pipe");
                }
                
                workers_pids[i] = fork();
                if (workers_pids[i] == 0) {
                    // Hijo (Worker): cierra el extremo de escritura
                    close(pipes[i][1]);
                    // Liberamos memoria dinamica duplicada en el hijo
                    free(historial_anterior);
                    free(historial_actual);
                    ejecutar_worker(i + 1, pipes[i][0]);
                    exit(EXIT_SUCCESS);
                } else {
                    // Padre (Monitor): cierra el extremo de lectura del hijo
                    close(pipes[i][0]);
                }
            }

            // Distribuir de forma equitativa (Round-Robin) las rutas de los archivos modificados
            for (int i = 0; i < total_por_copiar; i++) {
                int worker_asignado = i % NUM_WORKERS;
                write(pipes[worker_asignado][1], archivos_por_copiar[i].ruta, MAX_PATH);
            }

            // Enviar senal de "FIN" a cada worker por su pipe para que terminen limpiamente
            for (int i = 0; i < NUM_WORKERS; i++) {
                write(pipes[i][1], "FIN", MAX_PATH);
                close(pipes[i][1]); // Cerrar extremo de escritura definitivamente
            }

            // Esperar a que terminen todos los workers creados en esta iteracion
            for (int i = 0; i < NUM_WORKERS; i++) {
                waitpid(workers_pids[i], NULL, 0);
            }

            // Mostrar el estado actual de las estadisticas en la consola (si no somos un demonio)
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

        // El estado actual pasa a ser el historial anterior para la siguiente comparacion
        memcpy(historial_anterior, historial_actual, sizeof(struct FileMetadata) * total_actual);
        total_anteriores = total_actual;

        // Dormir por 5 segundos segun la especificacion del diseño
        sleep(5);
    }

    // Liberacion de memoria dinamica (En caso de salir del bucle)
    free(historial_anterior);
    free(historial_actual);
    
    // Cerrar semaforo y memoria compartida
    sem_close(sem);
    munmap(shared_stats, sizeof(struct stats));
    return 0;
}