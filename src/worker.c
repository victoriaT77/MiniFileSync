#include "common.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/mman.h>
#include <semaphore.h>
#include <time.h>

// Funcion requerida para la copia fisica de archivos usando E/S sin bufer de C (Llamadas al sistema)
int copiarArchivo(const char *origen, const char *destino, long *bytes_copiados) {
    int fd_origen = open(origen, O_RDONLY);
    if (fd_origen < 0) return -1;

    // Abrimos o creamos el destino con permisos de lectura/escritura (0644)
    int fd_destino = open(destino, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd_destino < 0) {
        close(fd_origen);
        return -1;
    }

    char buffer[BUFFER_SIZE];
    ssize_t leidos, escritos;
    *bytes_copiados = 0;

    // Bucle de lectura y escritura pura
    while ((leidos = read(fd_origen, buffer, BUFFER_SIZE)) > 0) {
        escritos = write(fd_destino, buffer, leidos);
        if (escritos != leidos) {
            close(fd_origen);
            close(fd_destino);
            return -1; // Error en la escritura (ej. disco lleno)
        }
        *bytes_copiados += escritos;
    }

    close(fd_origen);
    close(fd_destino);
    return 0; // Copia exitosa
}

// Funcion para enviar la notificacion al Logger a traves de la FIFO
void notificar_logger(const char *mensaje) {
    int fd_fifo = open(FIFO_NAME, O_WRONLY);
    if (fd_fifo >= 0) {
        write(fd_fifo, mensaje, strlen(mensaje));
        close(fd_fifo);
    }
}

// Funcion principal que ejecutara cada Worker hijo
void ejecutar_worker(int id, int pipe_lectura) {
    char ruta_origen[MAX_PATH];
    
    // Conectarse a la Memoria Compartida ya creada por el padre
    int shm_fd = shm_open(SHM_NAME, O_RDWR, 0666);
    struct stats *shared_stats = mmap(NULL, sizeof(struct stats), PROT_READ | PROT_WRITE, MAP_SHARED, shm_fd, 0);
    
    // Conectarse al Semaforo ya creado por el padre
    sem_t *sem = sem_open(SEM_NAME, 0);

    // Bucle continuo: el worker se bloquea en read() esperando que el Monitor le mande una ruta por el pipe
    while (read(pipe_lectura, ruta_origen, MAX_PATH) > 0) {
        // Si el monitor envia "FIN", el worker rompe el bucle y termina limpiamente
        if (strcmp(ruta_origen, "FIN") == 0) {
            break;
        }

        // Construir la ruta de destino dentro de la carpeta backup/
        // Ejemplo: si el origen es "origen/archivo.txt", se busca guardar en "backup/archivo.txt"
        // Para simplificar, extraemos el nombre base despues de la primera barra '/'
        char *nombre_base = strchr(ruta_origen, '/');
        char ruta_destino[MAX_PATH];
        if (nombre_base != NULL) {
            snprintf(ruta_destino, sizeof(ruta_destino), "backup%s", nombre_base);
        } else {
            snprintf(ruta_destino, sizeof(ruta_destino), "backup/%s", ruta_origen);
        }

        long bytes_este_archivo = 0;
        int resultado = copiarArchivo(ruta_origen, ruta_destino, &bytes_este_archivo);

        // Seccion Critica protegida por el Semaforo POSIX
        sem_wait(sem);
        if (resultado == 0) {
            shared_stats->archivos_copiados++;
            shared_stats->bytes_copiados += bytes_este_archivo;
        } else {
            shared_stats->errores++;
        }
        sem_post(sem); // Fin de la Seccion Critica

        // Preparar y enviar mensaje formateado con marca de tiempo al Logger
        time_t ahora = time(NULL);
        struct tm *t = localtime(&ahora);
        char msg_logger[MAX_PATH + 64];
        
        if (resultado == 0) {
            snprintf(msg_logger, sizeof(msg_logger), "[%02d:%02d:%02d] Worker %d copio %s\n", 
                     t->tm_hour, t->tm_min, t->tm_sec, id, ruta_destino);
        } else {
            snprintf(msg_logger, sizeof(msg_logger), "[%02d:%02d:%02d] Worker %d ERROR al copiar %s\n", 
                     t->tm_hour, t->tm_min, t->tm_sec, id, ruta_origen);
        }
        
        notificar_logger(msg_logger);
    }

    // Limpieza de recursos locales asignados al proceso hijo antes de morir
    munmap(shared_stats, sizeof(struct stats));
    close(shm_fd);
    sem_close(sem);
    close(pipe_lectura);
    exit(EXIT_SUCCESS);
}