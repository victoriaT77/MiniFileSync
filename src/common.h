#ifndef COMMON_H
#define COMMON_H

#include <stddef.h>

//Constantes globales para los mecanismos IPC
#define SHM_NAME      "/minisync_shm"     // Nombre para la memoria compartida
#define SEM_NAME      "/minisync_sem"     // Nombre para el semaforo POSIX
#define FIFO_NAME     "/tmp/minisync_fifo" // Ruta para la FIFO (Logger)
#define MAX_PATH      512                 // Tamano maximo para rutas de archivos
#define BUFFER_SIZE   4096                // Buffer para la copia de archivos

//Estructura de Estadisticas Compartida (Requerida por el PDF)
struct stats {
    long archivos_copiados;
    long bytes_copiados;
    long errores;
};

//almacenar los metadatos de un archivo en memoria
struct FileMetadata {
    char ruta[MAX_PATH];
    unsigned long inode;
    long size;
    long modification_time;
    int permissions;
};

#endif // COMMON_H