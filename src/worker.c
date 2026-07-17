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

/**
 * Realiza la copia fisica de un archivo byte a byte.
 */
int copiarArchivo(const char *origen, const char *destino, long *bytes_copiados) {
    // open() en modo O_RDONLY solicita un descriptor para lectura del archivo original
    int fd_origen = open(origen, O_RDONLY);
    if (fd_origen < 0) return -1; // Retorna error si el archivo no existe o no tiene permisos de lectura

    // open() con O_WRONLY|O_CREAT|O_TRUNC abre/crea el destino con permisos fijos 0644
    // (Lectura/Escritura para el dueño, Lectura para los demás miembros del sistema)
    int fd_destino = open(destino, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd_destino < 0) {
        close(fd_origen); // Liberación preventiva del descriptor de origen para no causar fuga de recursos
        return -1;
    }
    
    char buffer[BUFFER_SIZE]; // Bloque temporal en el Stack del proceso (Tamaño definido en common.h, ej. 4KB)
    ssize_t leidos, escritos;
    *bytes_copiados = 0;

    // BUCLE DE TRANSFERENCIA CRUDA: Lee bloques desde el archivo origen y los vuelca al destino
    // read() se bloquea hasta traer bytes del almacenamiento al espacio de memoria virtual
    while ((leidos = read(fd_origen, buffer, BUFFER_SIZE)) > 0) {
        // write() escribe el tamaño exacto leido para no corromper la consistencia de datos
        escritos = write(fd_destino, buffer, leidos);
        if (escritos != leidos) {
            close(fd_origen);
            close(fd_destino);
            return -1; // Control de fallos (ej. Espacio en disco lleno o error de hardware de Entrada/Salida)
        }
        *bytes_copiados += escritos; // Acumulador local de bytes copiados exitosamente
    }

    // LLAMADAS AL SISTEMA: close() devuelve los descriptores al pool del Kernel liberando la tabla del proceso
    close(fd_origen);
    close(fd_destino);
    return 0; // Finalización exitosa de la copia binaria
}

/**
 * Envia notificaciones de auditoria al proceso independiente Logger.
 * Utiliza IPC basado en una Tubería con Nombre (FIFO).
 */
void notificar_logger(const char *mensaje) {
    // open() abre la FIFO en modo solo escritura.
    // Esta llamada se mantendrá síncronamente bloqueada hasta que el Logger abra su extremo de lectura.
    int fd_fifo = open(FIFO_NAME, O_WRONLY);
    if (fd_fifo >= 0) {
        // write() inyecta la cadena estructurada directamente en el canal IPC
        write(fd_fifo, mensaje, strlen(mensaje));
        close(fd_fifo); // Cierre inmediato del descriptor para flush del flujo de datos de la tubería
    }
}

/**
 * Rutina principal del ciclo de vida de cada proceso hijo Worker.
 * Procesos independientes creados mediante fork() desde el Monitor.
 */
void ejecutar_worker(int id, int pipe_lectura) {
    char buffer_msg[MAX_PATH]; // Almacén temporal para recibir el comando enviado por el Monitor
    
    // LLAMADA AL SISTEMA IPC: shm_open conecta al objeto de memoria compartida global ya establecido por el padre
    int shm_fd = shm_open(SHM_NAME, O_RDWR, 0666);
    
    // mmap vincula las paginas de memoria fisica compartida al espacio de direcciones virtual del hijo
    struct stats *shared_stats = mmap(NULL, sizeof(struct stats), PROT_READ | PROT_WRITE, MAP_SHARED, shm_fd, 0);
    
    // LLAMADA AL SISTEMA IPC: sem_open enlaza al semáforo POSIX nombrado para el control de concurrencia
    sem_t *sem = sem_open(SEM_NAME, 0);

    // CICLO INFINITO IPC: El Worker se bloquea síncronamente en read() esperando órdenes del Monitor por el pipe anónimo
    while (read(pipe_lectura, buffer_msg, MAX_PATH) > 0) {
        // PROTOCOLO DE PARADA: Si el Monitor transmite la cadena exacta "FIN", el Worker rompe el ciclo limpio
        if (strcmp(buffer_msg, "FIN") == 0) {
            break;
        }

        char ruta_origen[MAX_PATH];
        
        // TRATAMIENTO DE COMANDOS: Desempaquetar y validar el formato de ejemplo exigido por la guía ("COPIAR <ruta>")
        if (strncmp(buffer_msg, "COPIAR ", 7) == 0) {
            strncpy(ruta_origen, buffer_msg + 7, MAX_PATH); // Extrae la ruta ignorando el prefijo del protocolo (7 bytes)
        } else {
            strncpy(ruta_origen, buffer_msg, MAX_PATH); // Respaldo preventivo por si llega la ruta directa
        }

        // AISLAMIENTO UNIVERSAL (Basename): strrchr localiza la última aparición del carácter '/'
        // Esto permite extraer solo el nombre final del archivo, logrando soportar rutas absolutas complejas
        char *nombre_base = strrchr(ruta_origen, '/');
        char ruta_destino[MAX_PATH];
        
        if (nombre_base != NULL) {
            // Si la ruta contiene barras (ej: /mnt/c/origen/informe.pdf), nombre_base apunta a "/informe.pdf"
            snprintf(ruta_destino, sizeof(ruta_destino), "backup%s", nombre_base);
        } else {
            // Si el archivo se pasó localmente sin rutas jerárquicas (ej: datos.txt)
            snprintf(ruta_destino, sizeof(ruta_destino), "backup/%s", ruta_origen);
        }

        long bytes_este_archivo = 0;
        // Invocación a la función de copia binaria de bajo nivel
        int resultado = copiarArchivo(ruta_origen, ruta_destino, &bytes_este_archivo);

        // SECCIÓN CRÍTICA (MUTEX): Exclusión mutua estricta para mitigar condiciones de carrera en la SHM
        // sem_wait decrementa el semáforo. Si vale 0, el Worker se suspende (estado Bloqueado)
        sem_wait(sem); 
        if (resultado == 0) {
            shared_stats->archivos_copiados++; // Modificación de variables globales mapeadas en RAM compartida
            shared_stats->bytes_copiados += bytes_este_archivo;
        } else {
            shared_stats->errores++; // Registro global de fallos de Entrada/Salida
        }
        // sem_post incrementa el semáforo a 1, despertando al siguiente proceso en cola de espera
        sem_post(sem); 
        // FIN DE LA SECCIÓN CRÍTICA

        // FORMATO DE AUDITORÍA: Captura del tiempo del sistema y conversión a estructura local desglosada
        time_t ahora = time(NULL);
        struct tm *t = localtime(&ahora);
        char msg_logger[MAX_PATH + 128];
        
        int anio = t->tm_year + 1900; // El contador de años de struct tm arranca en el estándar de 1900
        int mes = t->tm_mon + 1;     // El contador de meses arranca en índice base 0 (Enero = 0)
        int dia = t->tm_mday;

        // Limpieza estética para registrar únicamente el archivo final en el Log físico de auditoría
        char *solo_nombre = (nombre_base != NULL) ? (nombre_base + 1) : ruta_origen;

        // Formateo del string de registro respetando la especificación literal: [fecha-hora] copiado archivo
        if (resultado == 0) {
            snprintf(msg_logger, sizeof(msg_logger), "[%04d-%02d-%02d %02d:%02d:%02d] copiado %s (Worker %d)\n", 
                     anio, mes, dia, t->tm_hour, t->tm_min, t->tm_sec, solo_nombre, id);
        } else {
            snprintf(msg_logger, sizeof(msg_logger), "[%04d-%02d-%02d %02d:%02d:%02d] ERROR al copiar %s (Worker %d)\n", 
                     anio, mes, dia, t->tm_hour, t->tm_min, t->tm_sec, solo_nombre, id);
        }
        
        // Transmisión asíncrona hacia el Logger independiente
        notificar_logger(msg_logger);
    }

    // DESASIGNACIÓN DE RECURSOS: El proceso hijo limpia su espacio de memoria antes de finalizar voluntariamente
    munmap(shared_stats, sizeof(struct stats)); // Rompe el mapeo de memoria virtual del proceso hijo
    close(shm_fd);                              // Cierra el descriptor de la SHM
    sem_close(sem);                             // Cierra la referencia local al semáforo POSIX
    close(pipe_lectura);                        // Cierra el extremo de lectura de su pipe anónimo asignado
    
    // exit() termina el proceso hijo devolviendo control total al Kernel de Linux
    exit(EXIT_SUCCESS); 
}