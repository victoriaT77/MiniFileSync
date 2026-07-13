#include "common.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>

void ejecutar_logger() {
    // Crear la FIFO (tubería con nombre) con permisos de lectura/escritura (0666)
    // Usamos unlink por si quedó colgada alguna de una ejecución previa
    unlink(FIFO_NAME); 
    if (mkfifo(FIFO_NAME, 0666) == -1) {
        perror("Error al crear la FIFO del Logger");
        exit(EXIT_FAILURE);
    }

    // Abrir la FIFO en modo lectura pura
    // open() se bloqueará aquí hasta que al menos un worker la abra para escribir
    int fd_fifo = open(FIFO_NAME, O_RDONLY);
    if (fd_fifo < 0) {
        perror("Error al abrir la FIFO en el Logger");
        exit(EXIT_FAILURE);
    }

    // Abrir o crear el archivo físico de log en modo "append" (añadir al final)
    // Usamos llamadas al sistema puras: open, write, close 
    int fd_log = open("sincronizacion.log", O_WRONLY | O_CREAT | O_APPEND, 0644);
    if (fd_log < 0) {
        perror("Error al crear el archivo sincronizacion.log");
        close(fd_fifo);
        exit(EXIT_FAILURE);
    }

    char buffer[BUFFER_SIZE];
    ssize_t bytes_leidos;

    // Bucle continuo de escucha
    // Cada vez que un worker escribe en la FIFO, este read() se despierta
    while ((bytes_leidos = read(fd_fifo, buffer, sizeof(buffer) - 1)) > 0) {
        buffer[bytes_leidos] = '\0'; // Asegurar fin de cadena

        // Si el monitor le manda la señal de "TERMINAR_LOGGER", cerramos todo de forma limpia
        if (strstr(buffer, "TERMINAR_LOGGER") != NULL) {
            break;
        }

        // Escribir físicamente en el archivo sincronizacion.log usando write()
        write(fd_log, buffer, bytes_leidos);
        
        // imprimir también en la consola del Logger si estuviera visible
        write(STDOUT_FILENO, buffer, bytes_leidos);
    }

    // Limpieza de recursos antes de salir
    close(fd_log);
    close(fd_fifo);
    unlink(FIFO_NAME); // Borra el archivo especial FIFO del sistema de archivos
    exit(EXIT_SUCCESS);
}