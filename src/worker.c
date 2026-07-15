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

    int fd_destino = open(destino, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd_destino < 0) {
        close(fd_origen);
        return -1;
    }

    char buffer[BUFFER_SIZE];
    ssize_t leidos, escritos;
    *bytes_copiados = 0;

    while ((leidos = read(fd_origen, buffer, BUFFER_SIZE)) > 0) {
        escritos = write(fd_destino, buffer, leidos);
        if (escritos != leidos) {
            close(fd_origen);
            close(fd_destino);
            return -1;
        }
        *bytes_copiados += escritos;
    }

    close(fd_origen);
    close(fd_destino);
    return 0;
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
    
    int shm_fd = shm_open(SHM_NAME, O_RDWR, 0666);
    struct stats *shared_stats = mmap(NULL, sizeof(struct stats), PROT_READ | PROT_WRITE, MAP_SHARED, shm_fd, 0);
    
    sem_t *sem = sem_open(SEM_NAME, 0);

    while (read(pipe_lectura, ruta_origen, MAX_PATH) > 0) {
        if (strcmp(ruta_origen, "FIN") == 0) {
            break;
        }

        char *nombre_base = strrchr(ruta_origen, '/');
        char ruta_destino[MAX_PATH];
        
        if (nombre_base != NULL) {
            snprintf(ruta_destino, sizeof(ruta_destino), "backup%s", nombre_base);
        } else {
            snprintf(ruta_destino, sizeof(ruta_destino), "backup/%s", ruta_origen);
        }

        long bytes_este_archivo = 0;
        int resultado = copiarArchivo(ruta_origen, ruta_destino, &bytes_este_archivo);

        sem_wait(sem);
        if (resultado == 0) {
            shared_stats->archivos_copiados++;
            shared_stats->bytes_copiados += bytes_este_archivo;
        } else {
            shared_stats->errores++;
        }
        sem_post(sem);

        // OBTENCION DE FECHA Y HORA COMPLETAS (Corregido segun la Guia del Laboratorio)
        time_t ahora = time(NULL);
        struct tm *t = localtime(&ahora);
        char msg_logger[MAX_PATH + 128];
        
        // Explicacion tecnica: t->tm_year arranca en 1900, t->tm_mon arranca en 0 (Enero)
        int anio = t->tm_year + 1900;
        int mes = t->tm_mon + 1;
        int dia = t->tm_mday;

        // Extraemos solo el nombre del archivo final para que el Log se vea limpio
        char *solo_nombre = (nombre_base != NULL) ? (nombre_base + 1) : ruta_origen;

        if (resultado == 0) {
            snprintf(msg_logger, sizeof(msg_logger), "[%04d-%02d-%02d %02d:%02d:%02d] copiado %s (Worker %d)\n", 
                     anio, mes, dia, t->tm_hour, t->tm_min, t->tm_sec, solo_nombre, id);
        } else {
            snprintf(msg_logger, sizeof(msg_logger), "[%04d-%02d-%02d %02d:%02d:%02d] ERROR al copiar %s (Worker %d)\n", 
                     anio, mes, dia, t->tm_hour, t->tm_min, t->tm_sec, solo_nombre, id);
        }
        
        notificar_logger(msg_logger);
    }

    munmap(shared_stats, sizeof(struct stats));
    close(shm_fd);
    sem_close(sem);
    close(pipe_lectura);
    exit(EXIT_SUCCESS);
}