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

// funciones importadas de los otros módulos del proyecto
int escanear_directorio(const char *directorio_origen, struct FileMetadata *lista, int max_archivos);
void ejecutar_worker(int id, int pipe_lectura);
void ejecutar_logger();

// Punteros globales para el control y mapeo de los recursos compartidos IPC
struct stats *shared_stats = NULL; // Estructura de estadísticas en Memoria Compartida
sem_t *sem = NULL;                 // Semáforo POSIX nombrado para exclusión mutua

/**
 * Verifica y asegura la existencia del directorio de destino 'backup/' en el disco duro.
 */
void asegurar_directorio_backup() {
    struct stat st = {0}; // recuperar metadatos del inodo
    // stat() verifica si el directorio ya existe en el sistema de archivos
    if (stat("backup", &st) == -1) {
        // mkdir() crea la carpeta con permisos fijos 0755 (rwxr-xr-x)
        mkdir("backup", 0755);
    }
}

/**
 * Inicializa y configura los mecanismos de comunicación y sincronización compartidos (SHM y Semáforos)
 */
void inicializar_shm_y_sem() {
    shm_unlink(SHM_NAME);// LLAMADA AL SISTEMA IPC: shm_unlink() elimina cualquier rastro de la memoria compartida colgada
    
    // LLAMADA AL SISTEMA IPC: shm_open() crea un objeto de memoria compartida en el espacio del Kernel
    int shm_fd = shm_open(SHM_NAME, O_CREAT | O_RDWR, 0666);
    if (shm_fd < 0) {
        perror("Error shm_open");
        exit(EXIT_FAILURE);
    }
    
    // ftruncate() define el tamaño físico exacto en bytes de la región SHM
    ftruncate(shm_fd, sizeof(struct stats));
    
    // mmap() mapea las páginas físicas asignadas por el Kernel al espacio virtual del proceso
    shared_stats = (struct stats *)mmap(NULL, sizeof(struct stats), PROT_READ | PROT_WRITE, MAP_SHARED, shm_fd, 0);
    if (shared_stats == MAP_FAILED) {
        perror("Error mmap");
        exit(EXIT_FAILURE);
    }

    // Inicialización a cero de los contadores compartidos dentro del layout de memoria en RAM
    shared_stats->archivos_copiados = 0;
    shared_stats->bytes_copiados = 0;
    shared_stats->errores = 0;

    // LLAMADA AL SISTEMA IPC: sem_unlink() limpia semáforos colgados con el mismo nombre
    sem_unlink(SEM_NAME);
    
    // LLAMADA AL SISTEMA IPC: sem_open() inicializa un semáforo binario con valor fijos '1' (Mutex)
    sem = sem_open(SEM_NAME, O_CREAT, 0666, 1);
    if (sem == SEM_FAILED) {
        perror("Error sem_open");
        exit(EXIT_FAILURE);
    }
}

/**
 * Transforma el proceso actual en un Demonio (Daemon) invisible de segundo plano desvinculado de la terminal.
 */
void convertirse_en_demonio() {
    // PRIMER FORK: Desvincular el proceso de la shell actual
    pid_t pid = fork();
    if (pid < 0) exit(EXIT_FAILURE);
    if (pid > 0) exit(EXIT_SUCCESS); // El padre original muere, devolviendo el prompt a la terminal

    // setsid() crea una nueva sesión y coloca al proceso como líder, perdiendo la tty
    if (setsid() < 0) exit(EXIT_FAILURE);

    //Ignorar señales de control de terminal para evitar interrupciones asíncronas
    signal(SIGCHLD, SIG_IGN); // El Kernel limpia automáticamente los procesos hijos terminados (evita zombis)
    signal(SIGHUP, SIG_IGN);  // Ignora la muerte del proceso padre o el cierre de la consola

    // SEGUNDO FORK: Garantiza que el demonio nunca pueda re-adquirir una terminal de control (tty)
    pid = fork();
    if (pid < 0) exit(EXIT_FAILURE);
    if (pid > 0) exit(EXIT_SUCCESS); // El primer hijo intermedio muere, dejando al nieto como demonio puro

    //  cierra los canales estándar de E/S
    close(STDIN_FILENO);  
    close(STDOUT_FILENO); 
    close(STDERR_FILENO); 
}

/**
 * Compara los metadatos de tamaño y tiempo de modificación de inodos.
 */
int archivo_requiere_sincronizacion(struct FileMetadata *actual, struct FileMetadata *anteriores, int total_anteriores) {
    for (int i = 0; i < total_anteriores; i++) {
        // Busca si el archivo ya existía previamente comparando sus cadenas de ruta
        if (strcmp(actual->ruta, anteriores[i].ruta) == 0) {
            // Evalúa si varió el tamaño o la fecha de modificación (st_mtime)
            if (actual->size != anteriores[i].size || actual->modification_time != anteriores[i].modification_time) {
                return 1; // El archivo cambió en sus metadatos, requiere ser copiado
            }
            return 0; // El archivo es idéntico, se descarta la copia para optimizar E/S
        }
    }
    return 1; // El archivo es completamente nuevo en el directorio, requiere copia
}


int main(int argc, char *argv[]) {
    // Validación inicial de los argumentos del comando en la línea de comandos
    if (argc < 2) {
        fprintf(stderr, "USO: %s <directorio_origen> [daemon]\n", argv[0]);
        return 1;
    }

    const char *directorio_origen = argv[1];
    int modo_daemon = (argc == 3 && strcmp(argv[2], "daemon") == 0);

    // Inicialización de las estructuras y entornos de almacenamiento del SO
    asegurar_directorio_backup();
    inicializar_shm_y_sem();

    // Lanzamiento del servicio independiente de logs 
    pid_t logger_pid = fork();
    if (logger_pid == 0) {
        ejecutar_logger(); // El hijo ejecuta de por vida el bucle de escucha de la FIFO
        exit(EXIT_SUCCESS); // por si retorna voluntariamente
    }

    // Si se especificó el argumento, independiza el monitor de la consola actual
    if (modo_daemon) {
        convertirse_en_demonio();
    }

    // Asignación de bloques de memoria dinámica en el Heap para almacenar el historial de inodos
    struct FileMetadata *historial_anterior = malloc(sizeof(struct FileMetadata) * MAX_ARCHIVOS);
    struct FileMetadata *historial_actual = malloc(sizeof(struct FileMetadata) * MAX_ARCHIVOS);
    int total_anteriores = 0;

    // BUCLE INFINITO DE MONITOREO   se ejecuta en background permanente
    while (1) {
        // Escaneo recursivo del directorio mediante llamadas de bajo nivel (readdir, lstat)
        int total_actual = escanear_directorio(directorio_origen, historial_actual, MAX_ARCHIVOS);
        struct FileMetadata archivos_por_copiar[MAX_ARCHIVOS];
        int total_por_copiar = 0;

        // Filtrado por inodos para aislar las tareas que sufrieron modificaciones
        for (int i = 0; i < total_actual; i++) {
            if (archivo_requiere_sincronizacion(&historial_actual[i], historial_anterior, total_anteriores)) {
                archivos_por_copiar[total_por_copiar++] = historial_actual[i];
            }
        }

        // Si se detectaron cambios incrementales, se activa la arquitectura de Workers distribuidos
        if (total_por_copiar > 0) {
            int pipes[NUM_WORKERS][2];         // Arreglo matricial de descriptores para canales Pipe anónimos
            pid_t workers_pids[NUM_WORKERS];    // Almacén de identificadores de procesos hijos (PIDs)

            // Creación de los canales de comunicación y bifurcaciones multiproceso
            for (int i = 0; i < NUM_WORKERS; i++) {
                // pipe() inicializa una tubería interna compartida unidireccional
                if (pipe(pipes[i]) == -1) {
                    perror("Error al crear pipe");
                }
                
                // fork() duplica el proceso actual creando un espacio de direcciones aislado
                workers_pids[i] = fork();
                if (workers_pids[i] == 0) {
                    // hijo (Worker): Cierra el descriptor de escritura [1] que no va a ocupar
                    close(pipes[i][1]);
                    // Optimización de Memoria Virtual: Libera duplicados innecesarios heredados en el heap del hijo
                    free(historial_anterior);
                    free(historial_actual);
                    // Pasa el control de ejecución a la rutina especializada pasándole su extremo de lectura [0]
                    ejecutar_worker(i + 1, pipes[i][0]);
                    exit(EXIT_SUCCESS); // Terminación voluntaria limpia del proceso hijo
                } else {
                    //padre  (Monitor): Cierra el descriptor de lectura [0] que no usará el emisor
                    close(pipes[i][0]);
                }
            }

            // Distribución equitativa y balanceada de carga entre los Workers creados
            for (int i = 0; i < total_por_copiar; i++) {
                int worker_asignado = i % NUM_WORKERS; // Operador residuo para alternar  (0, 1, 0, 1)
                char comando_tarea[MAX_PATH + 16];
                // Formatea la cadena bajo el protocolo exacto requerido: "COPIAR <ruta>"
                snprintf(comando_tarea, sizeof(comando_tarea), "COPIAR %s", archivos_por_copiar[i].ruta);
                // write() inyecta la orden en el búfer de la tubería anónima del Worker asignado
                write(pipes[worker_asignado][1], comando_tarea, MAX_PATH);
            }

            //Envía la orden de fin a los Workers para que salgan de sus bucles bloqueantes
            for (int i = 0; i < NUM_WORKERS; i++) {
                write(pipes[i][1], "FIN", MAX_PATH);
                close(pipes[i][1]); // Cierra definitivamente la escritura rompiendo el canal desde el emisor
            }

            // El Monitor detiene su ejecución hasta recolectar el código de salida de cada hijo en la tabla del Kernel
            for (int i = 0; i < NUM_WORKERS; i++) {
                waitpid(workers_pids[i], NULL, 0);
            }

            if (!modo_daemon) {
                //Bloquea el semáforo para evitar lecturas sucias si el Logger estuviese modificando algo
                sem_wait(sem);
                printf("\n--- ESTADISTICAS DE SINCRONIZACION ---\n");
                printf("Archivos copiados con exito: %ld\n", shared_stats->archivos_copiados);
                printf("Bytes transferidos: %ld bytes\n", shared_stats->bytes_copiados);
                printf("Errores registrados: %ld\n", shared_stats->errores);
                printf("--------------------------------------\n\n");
                sem_post(sem); // Libera el semáforo d
            }
        }

        // El estado actual capturado pasa a ser el histórico base para la siguiente iteración comparativa
        memcpy(historial_anterior, historial_actual, sizeof(struct FileMetadata) * total_actual);
        total_anteriores = total_actual;
        
        // sleep() suspende el proceso Monitor pasándolo a estado 'Dormido' por 5 segundos
        sleep(5);
    }

    free(historial_anterior);
    free(historial_actual);
    sem_close(sem);
    munmap(shared_stats, sizeof(struct stats));
    return 0;
}